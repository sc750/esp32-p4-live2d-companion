/**
 * @file    tts_client.h
 * @brief   TTS 客户端（L4）——小米 mimo-v2.5-tts 流式语音合成
 *
 * chat.completions 格式 + SSE 流式：合成文本放 role:assistant、
 * 风格指令放 role:user，audio.format=pcm16 → 24kHz PCM16LE mono 分块。
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#ifndef TTS_CLIENT_H
#define TTS_CLIENT_H

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 收到一块 24kHz/16bit/mono PCM（数据在回调期间有效，用完即弃） */
typedef void (*tts_audio_cb_t)(const int16_t *pcm, size_t samples, void *ctx);

/** 初始化（读 Kconfig；幂等） */
esp_err_t tts_client_init(void);

/**
 * @brief 流式合成一段文本（阻塞至播放数据全部回调完）
 *
 * @param text     要念的文本（LLM 回复）
 * @param style    风格指令（可 NULL；如"害羞但温柔的少女语气"）
 * @param on_audio PCM 块回调
 * @param ctx      回调上下文
 * @return ESP_OK 合成完成；其他=网络/HTTP/无音频
 */
esp_err_t tts_synthesize(const char *text, const char *style,
                         tts_audio_cb_t on_audio, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TTS_CLIENT_H */
