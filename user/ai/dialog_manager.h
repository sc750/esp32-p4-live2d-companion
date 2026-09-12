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
#include "cJSON.h"
#include "llm_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化（加载人设与历史缓冲；幂等） */
esp_err_t dialog_manager_init(void);

/** LLM 流内完成一个可朗读短句时调用；回调必须快速返回。 */
typedef void (*dialog_sentence_cb_t)(const char *sentence, void *ctx);

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

/**
 * @brief 流式对话，并在句末实时回调，供 TTS 与后续 LLM 输出并行。
 *
 * 句末按中英文句号、问号、感叹号和换行判定；返回值与 dialog_ask 相同。
 */
char *dialog_ask_stream(const char *user_text, llm_token_cb_t on_token,
                        dialog_sentence_cb_t on_sentence, void *ctx);

/**
 * @brief 组装网关对话上下文（步骤 4：LLM 迁网关，设备保持状态源）
 * @return cJSON 树 {"sys":"<组装好的 system prompt>",
 *                  "history":[{"role","content"}...按时间序]}；
 *         调用方 cJSON_Delete。失败返回 NULL。
 */
cJSON *dialog_build_gw_context(void);

/**
 * @brief 网关轮次落账：与 dialog_ask_stream 的收尾一致
 *        （历史入环 + 摘要提取投喂 + 日记素材计数），但不调 LLM。
 * @note  网关回传完整回复后由管线任务调用；user/reply 被拷贝。
 */
esp_err_t dialog_commit_gw_round(const char *user_text, const char *reply);

#ifdef __cplusplus
}
#endif

#endif /* DIALOG_MANAGER_H */
