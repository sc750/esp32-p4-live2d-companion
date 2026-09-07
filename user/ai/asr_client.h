/**
 * @file    asr_client.h
 * @brief   ASR 客户端（L4）——小米 mimo-v2.5-asr 语音识别
 *
 * 走 OpenAI chat.completions 兼容格式：wav 全文件 base64 塞进
 * input_audio Data URL，非流式收文本。
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#ifndef ASR_CLIENT_H
#define ASR_CLIENT_H

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化（读 Kconfig；幂等） */
esp_err_t asr_client_init(void);

/**
 * @brief 识别一段 WAV 音频
 *
 * @param wav       完整 WAV 数据（含 44B 头；16k/16bit，声道数不限）
 * @param wav_len   数据长度（base64 后 ≤10MB，即原始 ≤7.5MB ≈ 16k 立体声 58s）
 * @param text_out  成功时输出识别文本（堆上，调用方 free）
 * @return ESP_OK 识别成功；其他=网络/HTTP/空结果
 */
esp_err_t asr_recognize(const char *wav, size_t wav_len, char **text_out);

#ifdef __cplusplus
}
#endif

#endif /* ASR_CLIENT_H */
