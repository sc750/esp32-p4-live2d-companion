/**
 * @file    xfyun_iat.h
 * @brief   讯飞流式听写（iat）WebSocket 客户端（L4）
 *
 * 协议：wss://iat-api.xfyun.cn/v2/iat，hmac-sha256 URL 鉴权，
 * 首帧 common+business+data(status=0)，末帧 data(status=2)，
 * 每帧音频 ≤1280B（base64 后 ≤13000），帧间隔建议 40ms。
 * 音频要求：16k/8k、16bit、单声道 PCM raw。
 * 会话上限 60s；>10s 不发数据服务端断连。
 * 文档：https://www.xfyun.cn/doc/asr/voicedictation/API.html
 *
 * @date    2026-09-07
 * @version 1.0.0
 */

#ifndef XFYUN_IAT_H
#define XFYUN_IAT_H

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化（读 Kconfig 的 APPID/APIKEY/APISECRET；幂等） */
esp_err_t xfyun_iat_init(void);

/** 是否已配置讯飞凭据（APPID 非空） */
bool xfyun_iat_configured(void);

/**
 * @brief 整段识别：一段单声道 16k/16bit PCM → 文本
 *
 * 内部按协议 1280B/40ms 分帧上传（音频时长决定了上传耗时≈音频时长，
 * 真流式"边录边传"为下一步优化），识别在服务端边传边算，末帧后
 * ~300ms 出全文。
 *
 * @param pcm_mono  单声道 16k/16bit PCM（裸数据，不带 WAV 头）
 * @param len       字节数（≤60s 音频）
 * @param text_out  成功时输出识别文本（堆上，调用方 free）
 * @return ESP_OK 识别成功；其他=握手/协议/识别错误
 */
esp_err_t xfyun_iat_recognize(const char *pcm_mono, size_t len, char **text_out);

#ifdef __cplusplus
}
#endif

#endif /* XFYUN_IAT_H */
