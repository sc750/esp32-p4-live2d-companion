/**
 * @file    minimax_tts.h
 * @brief   MiniMax T2A v2 语音合成客户端（L4）——整段 PCM 合成
 *
 * 协议（官方 OpenAPI 2025-09 核对）：
 *   POST {url}  Bearer 鉴权
 *   body: {model, text, stream:false, voice_setting{voice_id,speed,vol,pitch},
 *          audio_setting{sample_rate:24000, format:"pcm", channel:1}}
 *   resp: {data.audio = hex 编码 PCM, base_resp.status_code=0 为成功}
 *
 * 选型理由（M6）：非流式整段合成 + 按句流水线——每句 2~3s 合成时间
 * 与上一句播放重叠；pcm 直出免 mp3 解码；口型在播放侧按 PCM 驱动更准。
 *
 * @date    2026-09-08
 * @version 1.0.0
 */

#ifndef MINIMAX_TTS_H
#define MINIMAX_TTS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化（读 Kconfig；幂等） */
esp_err_t minimax_tts_init(void);

/** 是否已配置 MiniMax key（非空即视为启用） */
bool minimax_tts_configured(void);

/**
 * @brief 整段合成：文本 → 24kHz/16bit/mono PCM
 *
 * @param text       要念的文本（UTF-8）
 * @param pcm_out    成功时输出 PCM 缓冲（堆上，调用方 free）
 * @param samples_out 输出样本数（16bit mono）
 * @return ESP_OK 成功；其他=网络/HTTP/业务错误（base_resp.status_code!=0）
 */
esp_err_t minimax_tts_synthesize(const char *text,
                                 int16_t **pcm_out, size_t *samples_out);

/**
 * @brief MP3 裸流 → PCM 整段解码（feed 式，共享给 doubao_tts 复用）
 *
 * @param mp3        MP3 字节流（24kHz/mono/16bit 假定，采样率由流内帧头决定）
 * @param mp3_len    字节数
 * @param pcm_out    成功时输出 PCM 缓冲（PSRAM，调用方 free）
 * @param samples_out 输出样本数（16bit mono）
 */
esp_err_t minimax_tts_mp3_decode(const uint8_t *mp3, size_t mp3_len,
                                 int16_t **pcm_out, size_t *samples_out);

#ifdef __cplusplus
}
#endif

#endif /* MINIMAX_TTS_H */
