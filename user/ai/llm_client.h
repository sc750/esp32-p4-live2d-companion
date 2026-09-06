/**
 * @file    llm_client.h
 * @brief   LLM 客户端（L4）——DeepSeek 等 OpenAI 兼容流式对话
 *
 * 只做一件事：messages JSON 进 → 流式 token 出。人设/历史归 dialog_manager。
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#ifndef LLM_CLIENT_H
#define LLM_CLIENT_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 收到一个"UTF-8 完整"的文本片段（1~N 个完整字符；多字节不会拦腰截断） */
typedef void (*llm_token_cb_t)(const char *text, void *ctx);

/** 初始化（读 Kconfig 配置；幂等） */
esp_err_t llm_client_init(void);

/**
 * @brief 流式对话一轮
 *
 * @param messages_json  已组好的 messages 数组 JSON 文本（不含外层花括号），
 *                       形如 [{"role":"system",...},{...}]；dialog_manager 负责
 * @param on_token       token 回调（UTF-8 完整片段；不回调空串）
 * @param ctx            回调上下文
 * @return ESP_OK 正常完成（含 [DONE]）；否则网络/HTTP/解析错误
 */
esp_err_t llm_chat_stream(const char *messages_json,
                          llm_token_cb_t on_token, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* LLM_CLIENT_H */
