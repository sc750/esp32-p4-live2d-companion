/**
 * @file    gw_client.h
 * @brief   语音网关 WS 客户端（步骤 1：通道打通）——连接/心跳/文本收发
 *
 * 职责边界（步骤 1）：
 *   - 维持一条到网关的 WebSocket 长连接（自动重连，周期见 GW_RECONNECT_MS）
 *   - 连接建立即发 hello 握手；应用层文本消息收发
 *   - 收到网关消息仅打日志（后续步骤接入对话/音频流水线）
 *
 * 不负责：音频收发（步骤 2/3）、对话编排（步骤 4）、VAD（步骤 5）。
 *
 * 线程模型：esp_websocket_client 自带任务；对外接口线程安全（内部转发）。
 *
 * @date    2026-09-12
 * @version 1.0.0
 */

#ifndef GW_CLIENT_H
#define GW_CLIENT_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化并启动 WS 客户端（幂等；自动连接 + 断线重连） */
esp_err_t gw_client_init(void);

/** 是否已连接到网关 */
bool gw_client_is_connected(void);

/**
 * @brief 发送一条文本消息到网关（JSON 由调用方组好）
 * @return ESP_OK 已提交发送；ESP_ERR_INVALID_STATE 未连接；其他=发送失败
 */
esp_err_t gw_client_send_text(const char *json_text);

#ifdef __cplusplus
}
#endif

#endif /* GW_CLIENT_H */
