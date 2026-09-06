/**
 * @file    ai_http.h
 * @brief   AI 云端 HTTP 客户端（L3）——OpenAI 兼容 POST + SSE 流式解析
 *
 * 三件套（DeepSeek LLM / MiMo ASR / MiMo TTS）全是 OpenAI chat.completions
 * 兼容格式，本模块是它们共用的唯一 HTTP 通道：
 *   - ai_http_post_json   非流式：POST 后收完整 body（ASR 用）
 *   - ai_http_post_sse    流式：POST 后逐行解析 "data: {...}"（LLM/TTS 用）
 *
 * 设计约束（PRD 硬件约束 + conventions）：
 *   - mbedTLS 共享硬件加速器 → 全工程同一时刻只允许一个 HTTPS 请求
 *     （调用方保证串行；对话流水线本身就是顺序的）
 *   - 行缓冲 PSRAM 动态分配（TTS 流式单行 base64 可达数 KB）
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#ifndef AI_HTTP_H
#define AI_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SSE 每收到一行 data: 载荷回调（已剥 "data: " 前缀；"[DONE]" 原样传入） */
typedef void (*ai_sse_cb_t)(const char *data_line, void *ctx);

/**
 * @brief POST JSON 并按 SSE 逐行回调（流式）
 *
 * @param url        完整 URL（如 base_url + "/chat/completions"）
 * @param api_key    Bearer token
 * @param json_body  请求体（JSON 字符串）
 * @param on_data    data 行回调；收到 "[DONE]" 或流结束后停止
 * @param ctx        回调上下文
 * @param recv_timeout_s  两次收到数据之间的超时秒数（流式思考期可较长）
 * @return ESP_OK 正常结束；ESP_ERR_TIMEOUT 超时；其他=连接/HTTP 错误
 */
esp_err_t ai_http_post_sse(const char *url, const char *api_key,
                           const char *json_body, ai_sse_cb_t on_data,
                           void *ctx, int recv_timeout_s);

/**
 * @brief POST JSON 并收完整响应 body（非流式）
 *
 * @param resp_buf  响应缓冲（调用方提供，自动补 '\0'）
 * @param resp_size 缓冲大小
 * @return ESP_OK（含 HTTP 4xx/5xx——状态码调用方自查体内容）；
 *         ESP_ERR_TIMEOUT/连接错误
 */
esp_err_t ai_http_post_json(const char *url, const char *api_key,
                            const char *json_body,
                            char *resp_buf, size_t resp_size,
                            int recv_timeout_s);

#ifdef __cplusplus
}
#endif

#endif /* AI_HTTP_H */
