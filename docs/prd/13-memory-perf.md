# M13: 内存管理与性能优化

> **优先级**: P0 | **预估工时**: 1-2 周 | **依赖**: M01

---

## 1. 模块概述

针对 ESP32-P4 的 32 MB PSRAM，设计大块池化分配器和页面 composite 缓存系统，抑制长时间运行的内存碎片化，确保系统 24+ 小时稳定运行。

---

## 2. 问题分析

### 2.1 PSRAM 碎片化问题

ESP32-P4 使用 SPIRAM (QSPI/OPI)，长时间运行后：
- malloc/free 频繁分配释放不同大小的块
- 产生外部碎片，大块连续内存无法分配
- Live2D 纹理加载、HTTP 缓冲等需要大块内存
- 碎片化导致 OOM，系统崩溃

### 2.2 解决思路

```
┌─────────────────────────────────────────────────┐
│              三层内存管理架构                      │
│                                                   │
│  Layer 1: 大块池化分配器 (Pool Allocator)         │
│  ├── 固定大小的块池                               │
│  ├── 适用于 Live2D 纹理、帧缓冲等大块分配         │
│  └── 零碎片                                      │
│                                                   │
│  Layer 2: 页面 Composite 缓存 (Page Cache)        │
│  ├── 将 PSRAM 划分为固定大小页面 (如 64KB)        │
│  ├── 页面可组合为大块                              │
│  ├── 适用于动态大小的 HTTP/LLM 缓冲               │
│  └── 低碎片                                      │
│                                                   │
│  Layer 3: 标准 malloc (fallback)                  │
│  ├── ESP-IDF 默认分配器                           │
│  ├── 适用于小块/临时分配                          │
│  └── 需要定期监控碎片率                           │
└─────────────────────────────────────────────────┘
```

---

## 3. 功能需求

| ID | 需求 | 优先级 |
|----|------|--------|
| MEM-01 | 大块池化分配器 | P0 |
| MEM-02 | 页面 Composite 缓存 | P0 |
| MEM-03 | 内存使用统计与监控 | P0 |
| MEM-04 | 碎片率检测与告警 | P1 |
| MEM-05 | 内存泄漏检测 | P1 |
| MEM-06 | OOM 保护 (优雅降级) | P0 |
| MEM-07 | PSRAM vs 内部 SRAM 分配策略 | P0 |

---

## 4. 大块池化分配器

### 4.1 设计

```c
// pool_allocator.h

// 池配置: 4 种大小的块池
typedef struct {
    size_t block_size;      // 每块大小
    int    block_count;     // 块数量
    void  *pool_start;      // 池起始地址
    uint32_t *bitmap;       // 占位图 (0=空闲, 1=已用)
    int    free_count;      // 空闲块数
} pool_t;

// 四个池:
// Pool A: 4 KB  × 64 块  = 256 KB   (小缓冲)
// Pool B: 16 KB × 32 块  = 512 KB   (中缓冲)
// Pool C: 64 KB × 16 块  = 1 MB     (大缓冲)
// Pool D: 256 KB × 8 块  = 2 MB     (超大缓冲/纹理)
// 总计: ~3.75 MB

static pool_t pools[] = {
    { .block_size = 4096,     .block_count = 64  },
    { .block_size = 16384,    .block_count = 32  },
    { .block_size = 65536,    .block_count = 16  },
    { .block_size = 262144,   .block_count = 8   },
};

// 分配: O(1) 查找空闲块
void* pool_alloc(pool_t *pool);

// 释放: O(1)
void  pool_free(pool_t *pool, void *ptr);

// 全局分配接口: 自动选择合适的池
void* smart_alloc(size_t size);
void  smart_free(void *ptr);
```

### 4.2 位图分配算法

```c
void* pool_alloc(pool_t *pool) {
    if (pool->free_count == 0) return NULL;
    
    // 查找第一个空闲位
    for (int i = 0; i < pool->block_count / 32; i++) {
        if (pool->bitmap[i] != 0xFFFFFFFF) {
            // 找到有空闲位的 word
            uint32_t word = pool->bitmap[i];
            int bit = __builtin_ctz(~word);  // 找到第一个 0 位
            pool->bitmap[i] |= (1 << bit);
            pool->free_count--;
            
            int block_index = i * 32 + bit;
            return (uint8_t*)pool->pool_start + block_index * pool->block_size;
        }
    }
    return NULL;
}
```

---

## 5. 页面 Composite 缓存

### 5.1 设计

```c
// page_cache.h

#define PAGE_SIZE       (64 * 1024)    // 64 KB 每页
#define MAX_PAGES       128            // 最多 128 页 = 8 MB
#define MAX_SEGMENTS    32             // 最多 32 个连续段

typedef struct {
    int page_index;             // 起始页
    int page_count;             // 连续页数
    size_t logical_size;        // 逻辑大小
    bool in_use;
    uint32_t last_access;
} segment_t;

typedef struct {
    void *pages_base;           // PSRAM 页面基地址
    bool  page_used[MAX_PAGES]; // 页面使用标记
    segment_t segments[MAX_SEGMENTS];
    int segment_count;
} page_cache_t;

// 分配连续页面 (支持非连续合并)
void* page_cache_alloc(page_cache_t *cache, size_t size);

// 释放
void  page_cache_free(page_cache_t *cache, void *ptr);

// LRU 淘汰 (当空间不足时)
void  page_cache_evict(page_cache_t *cache, size_t needed);
```

---

## 6. 内存监控

### 6.1 统计信息

```c
typedef struct {
    size_t psram_total;         // PSRAM 总量
    size_t psram_used;          // PSRAM 已用
    size_t psram_free;          // PSRAM 空闲
    size_t psram_largest_free;  // 最大连续空闲块
    
    size_t internal_used;       // 内部 SRAM 已用
    
    int    pool_hit_rate;       // 池分配命中率 (%)
    int    fragmentation_pct;   // 碎片率 (%)
    
    size_t task_stacks[16];     // 各任务栈使用
} memory_stats_t;

// 获取内存统计
memory_stats_t memory_get_stats(void);

// 打印内存报告 (调试用)
void memory_print_report(void);
```

### 6.2 碎片率计算

```
碎片率 = 1 - (largest_free_block / total_free) × 100%

碎片率 < 20%:  正常
碎片率 20-50%: 警告 (记录日志)
碎片率 > 50%:  严重 (触发内存整理)
```

### 6.3 内存泄漏检测

```c
// 在 debug 模式下跟踪分配
#ifdef CONFIG_MEM_DEBUG
typedef struct {
    void *ptr;
    size_t size;
    const char *file;
    int line;
    uint32_t timestamp;
} alloc_record_t;

#define TRACKED_MALLOC(size) tracked_malloc(size, __FILE__, __LINE__)
#define TRACKED_FREE(ptr)    tracked_free(ptr, __FILE__, __LINE__)

void tracked_malloc(size_t size, const char *file, int line);
void tracked_free(void *ptr, const char *file, int line);
void memory_leak_report(void);  // 列出未释放的分配
#else
#define TRACKED_MALLOC(size) malloc(size)
#define TRACKED_FREE(ptr)    free(ptr)
#endif
```

---

## 7. OOM 保护

```c
// 当分配失败时的降级策略
void* oom_protected_alloc(size_t size) {
    // 1. 尝试从池分配
    void *ptr = smart_alloc(size);
    if (ptr) return ptr;
    
    // 2. 尝试 LRU 淘汰页面缓存
    ptr = page_cache_alloc(&g_page_cache, size);
    if (ptr) return ptr;
    
    // 3. 尝试释放非关键缓存
    release_non_critical_caches();
    ptr = malloc(size);
    if (ptr) return ptr;
    
    // 4. 记录 OOM 事件, 通知 UI
    event_bus_post(EVENT_LOW_MEMORY, &(event_data_t){ .size = size });
    
    // 5. 如果是关键分配, 重启系统
    if (is_critical_alloc(size)) {
        esp_restart();
    }
    
    return NULL;
}
```

---

## 8. PSRAM vs SRAM 分配策略

| 内存类型 | 适用场景 | 原因 |
|---------|---------|------|
| **PSRAM** | 帧缓冲、纹理、HTTP 缓冲、LLM 上下文 | 容量大 (32MB) |
| **内部 SRAM** (768KB) | LVGL 核心结构、FreeRTOS 任务栈、中断缓冲 | 带宽高、延迟低 |

```c
// 内部分配: 需要高速访问的热数据
void *hot_data = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

// PSRAM 分配: 大块冷数据
void *cold_data = heap_caps_malloc(1024 * 1024, MALLOC_CAP_SPIRAM);
```

---

## 9. PSRAM 带宽优化

### 9.1 Cache 配置

```ini
# sdkconfig.defaults
CONFIG_SPIRAM_SPEED_200M=y         # PSRAM 时钟 200MHz
CONFIG_CACHE_L2_CACHE_256KB=y      # L2 Cache 256KB
CONFIG_CACHE_L2_CACHE_LINE_128B=y  # Cache line 128 字节
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y # 指令从 PSRAM XIP
CONFIG_SPIRAM_RODATA=y             # 只读数据从 PSRAM
```

### 9.2 DMA 对齐

所有 PSRAM 中用于 DMA 传输的缓冲区必须按 L1+L2 cache line 对齐：

```c
// 使用 esp_dma_capable_calloc 确保对齐
esp_dma_mem_info_t dma_mem = {
    .extra_heap_caps = MALLOC_CAP_SPIRAM,
};
void *buf;
esp_dma_capable_calloc(1, size, &dma_mem, &buf);
```

---

## 10. 对外接口

```c
// memory_manager.h

esp_err_t memory_manager_init(void);

// 智能分配
void* mem_alloc(size_t size);
void* mem_alloc_dma(size_t size);     // DMA 对齐分配
void  mem_free(void *ptr);

// 监控
memory_stats_t mem_get_stats(void);
void           mem_print_report(void);
void           mem_check_leaks(void);

// OOM 回调
typedef void (*oom_callback_t)(size_t requested_size, void *ctx);
void mem_register_oom_callback(oom_callback_t cb, void *ctx);
```

---

## 11. 测试用例

| ID | 测试项 | 预期结果 |
|----|--------|---------|
| TST-MEM-01 | 池分配/释放 | 10000 次分配释放无泄漏 |
| TST-MEM-02 | 页面缓存分配 | 大小不等的分配均成功 |
| TST-MEM-03 | 碎片率测试 | 运行 24h 后碎片率 <30% |
| TST-MEM-04 | OOM 保护 | 内存不足时优雅降级, 不崩溃 |
| TST-MEM-05 | 内存泄漏检测 | 工具正确报告泄漏 |
| TST-MEM-06 | PSRAM 带宽 | MIPI-DSI + Live2D 无 underrun |
| TST-MEM-07 | 长时间运行 (24h) | 内存使用稳定, 无增长 |
