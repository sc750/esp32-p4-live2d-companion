/**
 * @file    memory_extract.h
 * @brief   对话摘要提取（L4）——攒轮 → LLM 提取长期记忆 → memory_store 入库
 *
 * 设计：不建独立任务。每轮对话完成后 dialog_manager 调 note_round()，
 * 素材攒在 RAM；满 EXTRACT_ROUNDS 轮时就地同步提取——此时上一轮回复已
 * 交给 TTS 任务播放、用户尚未开始下一轮，网络天然空闲，规避
 * mbedTLS 硬件加速器并发崩溃（PRD 2.2 硬约束），也无需额外互斥。
 *
 * @date    2026-09-09
 * @version 1.0.0
 */

#ifndef MEMORY_EXTRACT_H
#define MEMORY_EXTRACT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化（分配素材缓冲；幂等）
 */
esp_err_t memory_extract_init(void);

/**
 * @brief 记录一轮问答素材；攒满阈值自动触发同步提取（可能阻塞数秒）
 *
 * 调用时机：一轮对话完整回复产出之后（dialog_manager 内部）。
 * 提取失败（网络/解析）只丢弃素材，绝不影响对话主流程。
 *
 * @param user   本轮用户输入
 * @param reply  本轮完整回复
 */
void memory_extract_note_round(const char *user, const char *reply);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_EXTRACT_H */
