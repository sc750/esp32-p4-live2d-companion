/**
 * @file    memory_manager.h
 * @brief   PSRAM 池化分配器与内存监控
 *
 * ESP32-P4 有 32MB PSRAM（外部伪静态RAM），如果频繁 malloc/free
 * 不同大小的内存块，时间长了会产生"碎片"——有很多小空闲块，
 * 但没有一个足够大的连续空间来分配大内存，导致 OOM 崩溃。
 *
 * 解决方案：预分配几个固定大小的"内存池"，每个池里都是相同大小的块。
 * 分配时从合适的池里取一块，释放时还回去，永远不会产生碎片。
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

#ifndef MEMORY_MANAGER_H
#define MEMORY_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 内存统计信息结构体
 *
 * 记录系统当前的内存使用情况，用于监控和调试。
 */
typedef struct {
    size_t psram_total;         /* PSRAM 总容量（字节） */
    size_t psram_used;          /* PSRAM 已使用量（字节） */
    size_t psram_free;          /* PSRAM 剩余量（字节） */
    size_t psram_largest_free;  /* PSRAM 中最大的连续空闲块（字节） */
    size_t internal_used;       /* 内部 SRAM 已使用量（768KB，高速） */
    int pool_hit_rate_pct;      /* 内存池命中率（百分比）：从池分配成功的比例 */
    int fragmentation_pct;      /* 碎片率（百分比）：越高说明内存越碎片化 */
} mem_stats_t;

/**
 * @brief OOM（内存不足）回调函数类型
 *
 * 当分配内存失败时，这个函数会被调用，你可以在这里做降级处理。
 *
 * @param[in] requested_size  本次请求分配的大小（字节）
 * @param[in] ctx             用户上下文
 */
typedef void (*oom_callback_t)(size_t requested_size, void *ctx);

/**
 * @brief 初始化内存管理器
 *
 * 创建 4 级内存池，总计约 3.75MB：
 *   Pool A: 4KB × 64 块 = 256KB  （用于小缓冲区）
 *   Pool B: 16KB × 32 块 = 512KB （用于中等缓冲区）
 *   Pool C: 64KB × 16 块 = 1MB   （用于大缓冲区）
 *   Pool D: 256KB × 8 块 = 2MB   （用于超大缓冲区/纹理）
 *
 * @return ESP_OK 成功
 */
esp_err_t mem_manager_init(void);

/**
 * @brief 智能内存分配
 *
 * 根据请求大小，自动选择最合适的内存池：
 *   - 如果请求 ≤ 4KB，从 Pool A 分配
 *   - 如果请求 ≤ 16KB，从 Pool B 分配
 *   - 如果请求 ≤ 64KB，从 Pool C 分配
 *   - 如果请求 ≤ 256KB，从 Pool D 分配
 *   - 如果所有池都满了，回退到标准 malloc
 *
 * @param[in] size  请求的内存大小（字节）
 * @return 分配到的内存指针，失败返回 NULL
 */
void *mem_alloc(size_t size);

/**
 * @brief DMA 对齐的内存分配
 *
 * 分配可以用于 DMA 传输的内存（PSRAM + 特殊对齐）。
 * 用于 MIPI-DSI 显示、I2S 音频等需要 DMA 的场景。
 *
 * @param[in] size  请求的内存大小（字节）
 * @return 分配到的内存指针，失败返回 NULL
 */
void *mem_alloc_dma(size_t size);

/**
 * @brief 释放内存
 *
 * 自动判断指针属于哪个内存池，归还到对应的池中。
 * 如果不属于任何池，就调用标准 free() 释放。
 * 传入 NULL 是安全的（什么也不做）。
 *
 * @param[in] ptr  之前由 mem_alloc/mem_alloc_dma 分配的指针
 */
void mem_mgr_free(void *ptr);

/**
 * @brief 获取内存统计信息
 *
 * 读取当前的 PSRAM 使用情况、碎片率等信息。
 *
 * @return 内存统计结构体（包含各项数据）
 */
mem_stats_t mem_get_stats(void);

/**
 * @brief 打印内存报告到串口日志
 *
 * 输出格式示例：
 *   PSRAM: total=32768KB, used=10240KB, free=22528KB
 *   PSRAM largest free block: 18000KB
 *   Fragmentation: 5%
 *   Pool hit rate: 85%
 */
void mem_print_report(void);

/**
 * @brief 注册 OOM 回调
 *
 * 当内存分配失败时，指定的函数会被调用。
 * 你可以在回调中释放非关键缓存、通知 UI 显示警告等。
 *
 * @param[in] cb   回调函数
 * @param[in] ctx  传给回调函数的上下文数据
 */
void mem_register_oom_callback(oom_callback_t cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_MANAGER_H */
