/**
 * @file    memory_manager.c
 * @brief   PSRAM 池化分配器实现
 *
 * 核心算法：位图分配器
 * - 每个内存池有一个"位图"（bitmap），每个 bit 对应一个内存块
 * - bit = 0 表示空闲，bit = 1 表示已使用
 * - 分配时找到第一个 0 bit，把它变成 1
 * - 释放时找到对应 bit，把它变成 0
 * - 时间复杂度：O(1)（通过 __builtin_ctz 快速找到第一个 0 bit）
 *
 * @date    2026-09-01
 * @version 1.0.0
 * @hw      ESP32-P4 32MB PSRAM
 */

/* 1. 自身公开头 */
#include "memory_manager.h"

/* 2. C 标准库 */
#include <string.h>

/* 3. 项目级 */

/* 4. 平台/厂商头 */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "mem_mgr";

/* ======================== 内存池数据结构 ======================== */

/** 内存池级别数量：共 4 级，从小到大 */
#define POOL_LEVEL_COUNT  4

/**
 * @brief 单个内存池的描述信息
 *
 * 每个池包含：
 *   - 一块连续的 PSRAM 内存
 *   - 一个位图数组，记录每个块的使用状态
 *   - 一个互斥锁，保证多任务同时分配/释放时不出错
 */
typedef struct {
    size_t block_size;          /* 每个块的大小（字节）：4KB / 16KB / 64KB / 256KB */
    int block_count;            /* 这个池里有多少个块 */
    void *pool_start;           /* 池的起始地址（PSRAM 中的一段连续内存） */
    uint32_t *bitmap;           /* 位图数组：每个 bit 对应一个块，0=空闲 1=已用 */
    int free_count;             /* 当前剩余的空闲块数量 */
    SemaphoreHandle_t mutex;   /* 互斥锁：保护多任务并发访问 */
} pool_t;

/**
 * 四个内存池的配置：
 *   Pool A (4KB):   256KB 总容量，用于小缓冲区（如网络收发包）
 *   Pool B (16KB):  512KB 总容量，用于中等缓冲区（如音频帧）
 *   Pool C (64KB):  1MB 总容量，用于大缓冲区（如 HTTP 响应）
 *   Pool D (256KB): 2MB 总容量，用于超大缓冲区（如纹理贴图）
 */
static pool_t s_pools[POOL_LEVEL_COUNT] = {
    { .block_size = 4096,     .block_count = 64  },  /* Pool A: 4KB × 64 = 256KB */
    { .block_size = 16384,    .block_count = 32  },  /* Pool B: 16KB × 32 = 512KB */
    { .block_size = 65536,    .block_count = 16  },  /* Pool C: 64KB × 16 = 1MB */
    { .block_size = 262144,   .block_count = 8   },  /* Pool D: 256KB × 8 = 2MB */
};

/* OOM 回调函数和上下文（内存不足时调用） */
static oom_callback_t s_oom_callback = NULL;
static void *s_oom_ctx = NULL;

/* 统计数据 */
static int s_pool_alloc_count = 0;  /* 总分配次数 */
static int s_pool_hit_count = 0;    /* 从池分配成功的次数 */

/**
 * @brief 从内存池中分配一个块（内部函数）
 *
 * 分配算法（位图 O(1) 查找）：
 *   1. 检查是否有空闲块
 *   2. 遍历位图数组，找到第一个不是全 1 的 word（32 bit）
 *   3. 用 __builtin_ctz(~word) 快速找到第一个 0 bit 的位置
 *   4. 把该 bit 置为 1（标记为已使用）
 *   5. 根据 bit 位置计算对应的内存地址
 *
 * @param[in,out] pool  要分配的内存池
 * @return 分配到的内存指针，池满返回 NULL
 */
static void *pool_alloc_block(pool_t *pool)
{
    /* 没有空闲块了，直接返回 NULL */
    if (pool->free_count == 0) {
        return NULL;
    }

    /* 加锁：防止多个任务同时分配导致冲突 */
    xSemaphoreTake(pool->mutex, portMAX_DELAY);

    /* 遍历位图数组，每个 word 有 32 个 bit，对应 32 个块 */
    int words = (pool->block_count + 31) / 32;  /* 向上取整计算需要多少个 word */
    for (int i = 0; i < words; i++) {
        /* 如果这个 word 不是全 1（即不是所有块都被占用），说明里面有空闲块 */
        if (pool->bitmap[i] != 0xFFFFFFFF) {
            uint32_t word = pool->bitmap[i];
            /* __builtin_ctz(~word)：计算 ~word 中前导零的个数
             * 即找到第一个值为 0 的 bit 的位置（从低位开始数） */
            int bit = __builtin_ctz(~word);
            /* 把该 bit 置为 1，标记这个块为已使用 */
            pool->bitmap[i] |= (1U << bit);
            pool->free_count--;  /* 空闲块数 -1 */

            /* 计算这个块的实际内存地址 */
            int block_idx = i * 32 + bit;  /* 这是第几个块 */
            void *ptr = (uint8_t *)pool->pool_start + block_idx * pool->block_size;

            xSemaphoreGive(pool->mutex);  /* 释放锁 */
            return ptr;  /* 返回分配到的内存地址 */
        }
    }

    xSemaphoreGive(pool->mutex);  /* 释放锁 */
    return NULL;  /* 理论上不会到这里（因为前面检查了 free_count） */
}

/**
 * @brief 释放内存池中的一个块（内部函数）
 *
 * 释放算法：
 *   1. 根据指针计算出它是第几个块
 *   2. 找到对应的位图 bit
 *   3. 把该 bit 清零（标记为空闲）
 */
static void pool_free_block(pool_t *pool, void *ptr)
{
    if (ptr == NULL) {
        return;  /* NULL 安全：传入 NULL 什么都不做 */
    }

    /* 计算这个块在池中的索引号 */
    ptrdiff_t offset = (uint8_t *)ptr - (uint8_t *)pool->pool_start;
    int block_idx = (int)(offset / pool->block_size);

    /* 检查索引是否有效（防止越界） */
    if (block_idx < 0 || block_idx >= pool->block_count) {
        ESP_LOGE(TAG, "非法释放: 指针 %p 不属于此内存池", ptr);
        return;
    }

    /* 加锁 */
    xSemaphoreTake(pool->mutex, portMAX_DELAY);

    int word_idx = block_idx / 32;  /* 这个块在位图的第几个 word */
    int bit = block_idx % 32;       /* 在这个 word 的第几个 bit */

    /* 检查是否重复释放（double free） */
    if (!(pool->bitmap[word_idx] & (1U << bit))) {
        ESP_LOGE(TAG, "重复释放: 块 %d 已经是空闲的", block_idx);
        xSemaphoreGive(pool->mutex);
        return;
    }

    /* 把 bit 清零，标记为空闲 */
    pool->bitmap[word_idx] &= ~(1U << bit);
    pool->free_count++;  /* 空闲块数 +1 */

    xSemaphoreGive(pool->mutex);
}

/**
 * @brief 查找指针属于哪个内存池
 *
 * 遍历所有池，检查指针是否在某个池的地址范围内。
 */
static pool_t *find_pool(void *ptr)
{
    for (int i = 0; i < POOL_LEVEL_COUNT; i++) {
        void *start = s_pools[i].pool_start;
        size_t total = (size_t)s_pools[i].block_size * s_pools[i].block_count;
        /* 如果 ptr 在 [start, start+total) 范围内，说明属于这个池 */
        if (ptr >= start && ptr < (uint8_t *)start + total) {
            return &s_pools[i];
        }
    }
    return NULL;  /* 不属于任何池 */
}

/**
 * @brief 根据请求大小找到最合适的内存池
 *
 * 选择策略：找第一个块大小 ≥ 请求大小的池。
 * 例如请求 8KB，会选 Pool B（16KB），因为 Pool A（4KB）太小放不下。
 */
static pool_t *find_best_pool(size_t size)
{
    for (int i = 0; i < POOL_LEVEL_COUNT; i++) {
        if (size <= s_pools[i].block_size && s_pools[i].free_count > 0) {
            return &s_pools[i];  /* 找到合适的池，且还有空闲块 */
        }
    }
    return NULL;  /* 所有池都不合适（太大或已满） */
}

/* ======================== 公开 API 实现 ======================== */

/**
 * @brief 初始化内存管理器
 *
 * 为每个池分配内存（PSRAM）和位图（内部 SRAM），
 * 初始化位图为全 0（所有块空闲），创建互斥锁。
 */
esp_err_t mem_manager_init(void)
{
    ESP_LOGI(TAG, "正在初始化内存管理器...");

    for (int i = 0; i < POOL_LEVEL_COUNT; i++) {
        pool_t *pool = &s_pools[i];

        /* 计算这个池需要的总内存和位图大小 */
        size_t total_size = pool->block_size * pool->block_count;
        size_t bitmap_size = ((pool->block_count + 31) / 32) * sizeof(uint32_t);

        /* 从 PSRAM 分配池内存（大块，放 PSRAM 中） */
        pool->pool_start = heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM);
        if (pool->pool_start == NULL) {
            ESP_LOGE(TAG, "Pool %d 内存分配失败（需要 %zu 字节）", i, total_size);
            return ESP_ERR_NO_MEM;
        }

        /* 从内部 SRAM 分配位图（小块，访问频繁，放高速内存中） */
        pool->bitmap = heap_caps_malloc(bitmap_size,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (pool->bitmap == NULL) {
            ESP_LOGE(TAG, "Pool %d 位图分配失败", i);
            return ESP_ERR_NO_MEM;
        }

        /* 初始化位图为全 0：所有块都是空闲的 */
        memset(pool->bitmap, 0, bitmap_size);
        pool->free_count = pool->block_count;  /* 空闲块数 = 总块数 */
        pool->mutex = xSemaphoreCreateMutex();  /* 创建互斥锁 */

        ESP_LOGI(TAG, "Pool %d: %zuKB × %d = %zuKB",
                 i, pool->block_size / 1024, pool->block_count,
                 total_size / 1024);
    }

    s_pool_alloc_count = 0;
    s_pool_hit_count = 0;

    ESP_LOGI(TAG, "内存管理器初始化完成");
    return ESP_OK;
}

/**
 * @brief 智能内存分配
 *
 * 分配策略：
 *   1. 先尝试从内存池分配（快，无碎片）
 *   2. 池分配失败则回退到标准 malloc（慢，可能碎片化）
 *   3. 如果 malloc 也失败，调用 OOM 回调
 */
void *mem_alloc(size_t size)
{
    if (size == 0) {
        return NULL;  /* 请求 0 字节，直接返回 NULL */
    }

    s_pool_alloc_count++;  /* 统计：总分配次数 +1 */

    /* 第一步：尝试从内存池分配 */
    pool_t *pool = find_best_pool(size);
    if (pool != NULL) {
        void *ptr = pool_alloc_block(pool);
        if (ptr != NULL) {
            s_pool_hit_count++;  /* 统计：池分配成功次数 +1 */
            return ptr;
        }
    }

    /* 第二步：回退到标准 malloc（从 PSRAM 分配） */
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (ptr == NULL) {
        /* 第三步：内存分配失败，触发 OOM 回调 */
        ESP_LOGW(TAG, "内存分配失败: 请求 %zu 字节", size);
        if (s_oom_callback != NULL) {
            s_oom_callback(size, s_oom_ctx);
        }
    }
    return ptr;
}

/**
 * @brief DMA 对齐的内存分配
 *
 * 分配可以用于 DMA 传输的内存。
 * MIPI-DSI 显示、I2S 音频等硬件需要 DMA 直接访问内存，
 * 这类内存必须满足特殊的对齐要求。
 */
void *mem_alloc_dma(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    /* 分配 PSRAM 中的内存，带 DMA 对齐属性 */
    void *ptr = heap_caps_malloc(size,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (ptr == NULL) {
        ESP_LOGW(TAG, "DMA 内存分配失败: 请求 %zu 字节", size);
        return NULL;
    }
    return ptr;
}

/**
 * @brief 释放内存
 *
 * 自动判断指针属于哪个内存池，归还到对应的池中。
 * 如果不属于任何池，就调用标准 free() 释放。
 */
void mem_mgr_free(void *ptr)
{
    if (ptr == NULL) {
        return;  /* NULL 安全 */
    }

    /* 检查是否属于某个内存池 */
    pool_t *pool = find_pool(ptr);
    if (pool != NULL) {
        pool_free_block(pool, ptr);  /* 影还到池中 */
        return;
    }

    /* 不属于任何池，使用标准释放 */
    heap_caps_free(ptr);
}

/**
 * @brief 获取内存统计信息
 *
 * 读取 PSRAM 的使用情况，计算碎片率和池命中率。
 */
mem_stats_t mem_get_stats(void)
{
    mem_stats_t stats = {0};  /* 初始化为全 0 */

    /* 读取 PSRAM 使用情况 */
    stats.psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    stats.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    stats.psram_used = stats.psram_total - stats.psram_free;
    stats.psram_largest_free = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    /* 读取内部 SRAM 使用情况 */
    stats.internal_used = heap_caps_get_total_size(MALLOC_CAP_INTERNAL) -
                          heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    /* 计算碎片率 = 1 - (最大连续空闲块 / 总空闲量) × 100%
     * 碎片率越高，说明内存越碎片化 */
    if (stats.psram_free > 0) {
        stats.fragmentation_pct = (int)(100 - (stats.psram_largest_free * 100 /
                                               stats.psram_free));
    }

    /* 计算内存池命中率 = 池分配成功次数 / 总分配次数 × 100% */
    if (s_pool_alloc_count > 0) {
        stats.pool_hit_rate_pct = (int)(s_pool_hit_count * 100 / s_pool_alloc_count);
    }

    return stats;
}

/**
 * @brief 打印内存报告到串口日志
 *
 * 每 60 秒打印一次，用于监控内存使用趋势。
 * 如果碎片率超过 50%，会打印严重告警。
 */
void mem_print_report(void)
{
    mem_stats_t stats = mem_get_stats();

    ESP_LOGI(TAG, "========== 内存报告 ==========");
    ESP_LOGI(TAG, "PSRAM: 总计=%zuKB, 已用=%zuKB, 空闲=%zuKB",
             stats.psram_total / 1024, stats.psram_used / 1024,
             stats.psram_free / 1024);
    ESP_LOGI(TAG, "PSRAM 最大连续空闲块: %zuKB",
             stats.psram_largest_free / 1024);
    ESP_LOGI(TAG, "内部 SRAM 已用: %zuKB", stats.internal_used / 1024);
    /* --- Phase4 诊断：SDIO 传输缓冲所在堆的余量（2026-09-10 追加） ---
     * esp_hosted 的 SDIO 收发缓冲走 heap_caps_* 显式指定 caps 分配，成败只看
     * 这两块堆，与上面的 PSRAM 总量/内部 SRAM 总量都不是一回事。上板曾出现
     * `eh_sdio: dma_alloc(4608) failed; dropping read` 后数据面永久失联，
     * 故把"空闲 + 最大连续块"常态化打印，便于随时判定是不是又把内部 RAM 挤爆。 */
    ESP_LOGI(TAG, "内部DMA堆: 空闲=%zuKB, 最大连续=%zuKB",                /* 内部可 DMA 堆 */
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024,          /* 空闲量 */
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024); /* 最大连续块 */
    ESP_LOGI(TAG, "PSRAM-DMA堆: 空闲=%zuKB, 最大连续=%zuKB",              /* PSRAM 可 DMA 堆 */
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA) / 1024,            /* 空闲量 */
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA) / 1024);  /* 最大连续块 */
    ESP_LOGI(TAG, "碎片率: %d%%", stats.fragmentation_pct);
    ESP_LOGI(TAG, "内存池命中率: %d%%", stats.pool_hit_rate_pct);

    /* 碎片率告警 */
    if (stats.fragmentation_pct > 50) {
        ESP_LOGW(TAG, "⚠ 严重: 碎片率超过 50%%！需要整理内存！");
    } else if (stats.fragmentation_pct > 20) {
        ESP_LOGW(TAG, "⚠ 警告: 碎片率超过 20%%，注意监控");
    }
    ESP_LOGI(TAG, "================================");
}

void mem_register_oom_callback(oom_callback_t cb, void *ctx)
{
    s_oom_callback = cb;  /* 注册 OOM 回调函数 */
    s_oom_ctx = ctx;      /* 保存上下文 */
}
