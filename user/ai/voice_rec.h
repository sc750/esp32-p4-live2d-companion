/**
 * @file    voice_rec.h
 * @brief   按住说话录音器（L4）——PSRAM 线性缓冲，录音→完整 WAV
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#ifndef VOICE_REC_H
#define VOICE_REC_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 最长录音时长（秒）。16k/16bit/2ch=64KB/s → 上限 2MB PSRAM */
#define VOICE_REC_MAX_SEC   (30)

/** 初始化（预分配 2MB 录音缓冲；幂等） */
esp_err_t voice_rec_init(void);

/**
 * @brief 开始录音（从当前位置覆盖写）
 * @return ESP_OK 就绪；ESP_ERR_INVALID_STATE 上一段没取走
 */
esp_err_t voice_rec_begin(void);

/** 录一个 100ms 块（64KB/s × 0.1s = 6400B；阻塞式） */
void voice_rec_chunk(void);

/** 停止录音并取出完整 WAV（堆上，调用方 free；*len 含 44B 头） */
esp_err_t voice_rec_end_and_get(char **wav_out, size_t *len_out);

/** 放弃当前录音（释放内部缓冲所有权，下次 begin 重置） */
void voice_rec_abort(void);

/** 当前已录时长 ms */
uint32_t voice_rec_elapsed_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_REC_H */
