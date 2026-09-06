/**
 * @file    dialog_manager.h
 * @brief   对话管理器（L4）——三玖人设 + 多轮历史 + 完整回复拼装
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#ifndef DIALOG_MANAGER_H
#define DIALOG_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"
#include "llm_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化（加载人设与历史缓冲；幂等） */
esp_err_t dialog_manager_init(void);

/**
 * @brief 发起一轮对话（阻塞直到 LLM 回复完成）
 *
 * 内部完成：user_text 入历史 → 组 messages（system+历史+本轮）→
 * 流式调 LLM（每个 UTF-8 完整片段经 on_token 实时回调，供 UI 流式
 * 刷新）→ 完整回复入历史。
 *
 * @param user_text 用户输入（一问）
 * @param on_token  流式 token 回调（可为 NULL）
 * @param ctx       回调上下文
 * @return 完整回复文本（堆上，调用方 free）；失败返回 NULL
 */
char *dialog_ask(const char *user_text, llm_token_cb_t on_token, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DIALOG_MANAGER_H */
