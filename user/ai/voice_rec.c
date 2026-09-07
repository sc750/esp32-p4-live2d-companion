/**
 * @file    voice_rec.c
 * @brief   录音器实现——预分配 2MB PSRAM，块式采集，出 WAV
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#include "voice_rec.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

#include "bsp_audio.h"

#define TAG "voice_rec"

#define CHUNK_BYTES     (6400)      /* 100ms @ 16k/16bit/2ch */
#define REC_BUF_SIZE    (VOICE_REC_MAX_SEC * 64000)

static struct {
    bool inited;
    char *buf;                  /* PSRAM 2MB：[0,44)=WAV头，[44,44+pcm)=PCM */
    size_t pcm_len;             /* 已录 PCM 字节数 */
    bool recording;
} s_rec;

esp_err_t voice_rec_init(void)
{
    if (s_rec.inited) {
        return ESP_OK;
    }
    s_rec.buf = heap_caps_malloc(REC_BUF_SIZE, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_rec.buf, ESP_ERR_NO_MEM, TAG, "2MB 录音缓冲分配失败");
    s_rec.inited = true;
    ESP_LOGI(TAG, "录音器就绪（上限 %ds = %dKB PSRAM）",
             VOICE_REC_MAX_SEC, REC_BUF_SIZE / 1024);
    return ESP_OK;
}

esp_err_t voice_rec_begin(void)
{
    ESP_RETURN_ON_FALSE(s_rec.inited, ESP_ERR_INVALID_STATE, TAG, "not init");
    ESP_RETURN_ON_FALSE(!s_rec.recording, ESP_ERR_INVALID_STATE, TAG, "already rec");
    s_rec.pcm_len = 0;
    s_rec.recording = true;
    return ESP_OK;
}

void voice_rec_chunk(void)
{
    if (!s_rec.recording) {
        return;
    }
    if (s_rec.pcm_len + CHUNK_BYTES > REC_BUF_SIZE - 44) {
        return;                                 /* 到达 30s 上限，静默停写 */
    }
    if (bsp_audio_record(s_rec.buf + 44 + s_rec.pcm_len, CHUNK_BYTES) != ESP_OK) {
        ESP_LOGW(TAG, "录音块读取失败");
        return;
    }
    s_rec.pcm_len += CHUNK_BYTES;
}

static void wav_put_u32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);                           /* 小端机直写 */
}

static void wav_put_u16(uint8_t *p, uint16_t v)
{
    memcpy(p, &v, 2);
}

esp_err_t voice_rec_end_and_get(char **wav_out, size_t *len_out)
{
    ESP_RETURN_ON_FALSE(s_rec.recording, ESP_ERR_INVALID_STATE, TAG, "not rec");
    s_rec.recording = false;

    /* 最短 200ms：太短给 ASR 是浪费一次调用 */
    ESP_RETURN_ON_FALSE(s_rec.pcm_len >= 12800, ESP_ERR_INVALID_SIZE,
                        TAG, "录音太短 (%ums)", (unsigned)(s_rec.pcm_len / 64));

    /* 内部缓冲常驻复用；拷出有效段（44 头 + PCM）交调用方 */
    size_t total = 44 + s_rec.pcm_len;
    char *out = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(out, ESP_ERR_NO_MEM, TAG, "WAV 输出分配失败");

    uint8_t *h = (uint8_t *)out;
    memcpy(h, "RIFF", 4);
    wav_put_u32(h + 4, (uint32_t)(36 + s_rec.pcm_len));
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    wav_put_u32(h + 16, 16);                    /* fmt 块长 */
    wav_put_u16(h + 20, 1);                     /* PCM */
    wav_put_u16(h + 22, 2);                     /* 声道 */
    wav_put_u32(h + 24, 16000);                 /* 采样率 */
    wav_put_u32(h + 28, 16000 * 2 * 2);         /* 字节率 */
    wav_put_u16(h + 32, 2 * 2);                 /* 块对齐 */
    wav_put_u16(h + 34, 16);                    /* 位深 */
    memcpy(h + 36, "data", 4);
    wav_put_u32(h + 40, (uint32_t)s_rec.pcm_len);
    memcpy(out + 44, s_rec.buf, s_rec.pcm_len);

    *wav_out = out;
    *len_out = total;
    ESP_LOGI(TAG, "录音完成: %ums / %uKB",
             (unsigned)(s_rec.pcm_len / 64), (unsigned)(total / 1024));
    return ESP_OK;
}

void voice_rec_abort(void)
{
    if (!s_rec.recording) {
        return;
    }
    s_rec.recording = false;
    /* buf 留着复用（abort 不转移所有权） */
}

uint32_t voice_rec_elapsed_ms(void)
{
    return s_rec.pcm_len / 64;                  /* 64 字节/ms */
}
