/**
 * @file    doubao_tts.h
 * @brief   豆包（火山引擎）语音合成大模型 TTS 客户端（L4）——HTTP chunked 流式
 *
 * 协议（官方文档 2026-09 核对，V3 单向流式 HTTP Chunked 版）：
 *   POST {url}，鉴权走请求头 X-Api-App-Id + X-Api-Access-Key（或新版 X-Api-Key）
 *   + X-Api-Resource-Id（豆包语音合成 2.0 = seed-tts-2.0）
 *   body: {user{uid}, req_params{text, speaker, audio_params{format,sample_rate}}}
 *   resp: 换行分隔的 JSON 行：音频块 {"code":0,"data":"<base64 mp3>"}，
 *         文本事件 data=null + sentence 对象；code!=0 为业务错误
 *
 * 选型理由（2026-09-12 方案 A）：首包 <300ms（官方）/ ~800ms（实测三方），
 * 合成速率 1.16x 实时——边合成边推流，配合 speak 环形缓冲实现真流式播报。
 *
 * @date    2026-09-12
 * @version 1.0.0
 */

#ifndef DOUBAO_TTS_H
#define DOUBAO_TTS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 音频块回调：24kHz/16bit/mono PCM（与 MiMo SSE 后端同规格） */
typedef void (*doubao_audio_cb_t)(const int16_t *pcm, size_t samples, void *ctx);

/** 初始化（读 Kconfig；幂等） */
esp_err_t doubao_tts_init(void);

/** 是否已配置（AppID 与 Key 均非空即视为启用） */
bool doubao_tts_configured(void);

/**
 * @brief 流式合成：文本 → 逐块回调 24kHz/16bit/mono PCM（边合成边回）
 *
 * 内部：chunked 收 base64 mp3 块 → 本地 MP3 解码（跨块保持解码器会话）
 * → 逐块回调。合成速率 ~1.16x 实时，调用方（speak 环形缓冲）可边收边播。
 *
 * @param text     要念的文本（UTF-8，一次一批 ~30-60 字）
 * @param on_audio 音频块回调（块间为连续 PCM 流，跨块无缝）
 * @param ctx      回调上下文
 * @return ESP_OK 成功；其他=网络/HTTP/业务错误（code!=0）
 */
esp_err_t doubao_tts_synthesize_stream(const char *text,
                                       doubao_audio_cb_t on_audio, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DOUBAO_TTS_H */
