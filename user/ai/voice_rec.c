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
#include <math.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

#include "bsp_audio.h"

#define TAG "voice_rec"

#define CHUNK_BYTES     (6400)      /* 100ms @ 16k/16bit/2ch（BSP 采集格式，不变） */
#define MONO_CHUNK_BYTES (3200)     /* 左声道抽取后：100ms @ 16k/16bit/1ch */
#define REC_BUF_SIZE    (VOICE_REC_MAX_SEC * 32000)
#define AGC_TARGET_PEAK  (12000)    /* 约 -8.7dBFS，给语音峰值留出余量 */
#define AGC_MAX_GAIN_X1000 (8000)   /* 最多 8 倍，避免把底噪放成爆音 */
#define AGC_MIN_PEAK     (200)      /* 低于此值通常是静音，不作增益 */

/* 立体声采集暂存（内部 RAM，I2S 块直读） */
static int16_t s_staging[CHUNK_BYTES / sizeof(int16_t)];

static struct {
    bool inited;
    char *buf;                  /* PSRAM 2MB：[0,44)=WAV头，[44,44+pcm)=PCM */
    size_t pcm_len;             /* 已录 PCM 字节数 */
    bool recording;
    int32_t peak;               /* 本段最大采样幅值，用于判断麦克风是否真有输入 */
    uint64_t energy;            /* 所有声道样本的平方和，用于计算 RMS */
    size_t sample_count;
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

    /* 音频 BSP 从启动起固定为官方已验证的 16k/16bit/双声道。
     * TTS 的 24k/单声道 PCM 在应用层转换，录音时绝不重开共享 codec。 */
    s_rec.pcm_len = 0;
    s_rec.peak = 0;
    s_rec.energy = 0;
    s_rec.sample_count = 0;
    s_rec.recording = true;
    return ESP_OK;
}

void voice_rec_chunk(void)
{
    if (!s_rec.recording) {
        return;
    }
    if (s_rec.pcm_len + MONO_CHUNK_BYTES > REC_BUF_SIZE - 44) {
        return;                                 /* 到达 30s 上限，静默停写 */
    }
    /* 读立体声块到暂存（BSP 固定 16k/16bit/2ch 采集，绝不重开 codec） */
    if (bsp_audio_record(s_staging, CHUNK_BYTES) != ESP_OK) {
        ESP_LOGW(TAG, "录音块读取失败");
        return;
    }
    /* M3 单声道化：ES8311 是单声道麦克风，右声道纯冗余——
     * 只保留左声道，上传体积减半（ASR 上传时间近似减半） */
    int16_t *dst = (int16_t *)(s_rec.buf + 44 + s_rec.pcm_len);
    const size_t mono_samples = CHUNK_BYTES / 2 / sizeof(int16_t);
    for (size_t i = 0; i < mono_samples; i++) {
        int32_t sample = s_staging[i * 2];      /* 偶数样本 = 左声道 */
        dst[i] = (int16_t)sample;
        int32_t magnitude = sample >= 0 ? sample : -sample;
        if (magnitude > s_rec.peak) {
            s_rec.peak = magnitude;
        }
        s_rec.energy += (uint64_t)((int64_t)sample * sample);
    }
    s_rec.sample_count += mono_samples;
    s_rec.pcm_len += MONO_CHUNK_BYTES;
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
    ESP_RETURN_ON_FALSE(s_rec.pcm_len >= 6400, ESP_ERR_INVALID_SIZE,
                        TAG, "录音太短 (%ums)", (unsigned)(s_rec.pcm_len / 32));

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
    wav_put_u16(h + 22, 1);                     /* 单声道（M3） */
    wav_put_u32(h + 24, 16000);                 /* 采样率 */
    wav_put_u32(h + 28, 16000 * 1 * 2);         /* 字节率 */
    wav_put_u16(h + 32, 1 * 2);                 /* 块对齐 */
    wav_put_u16(h + 34, 16);                    /* 位深 */
    memcpy(h + 36, "data", 4);
    wav_put_u32(h + 40, (uint32_t)s_rec.pcm_len);
    memcpy(out + 44, s_rec.buf + 44, s_rec.pcm_len);

    /* 板载麦克风在远距离说话时原始幅度很低，云端 ASR 会将其判为静音。
     * 用整段峰值做受限 AGC；采集格式不变，只提升有效语音的量化幅度。 */
    uint32_t gain_x1000 = 1000;
    if (s_rec.peak >= AGC_MIN_PEAK && s_rec.peak < AGC_TARGET_PEAK) {
        gain_x1000 = (uint32_t)((uint64_t)AGC_TARGET_PEAK * 1000 / s_rec.peak);
        if (gain_x1000 > AGC_MAX_GAIN_X1000) {
            gain_x1000 = AGC_MAX_GAIN_X1000;
        }
    }
    if (gain_x1000 > 1000) {
        int16_t *out_pcm = (int16_t *)(out + 44);
        size_t sample_count = s_rec.pcm_len / sizeof(*out_pcm);
        for (size_t i = 0; i < sample_count; i++) {
            int32_t scaled = (int32_t)((int64_t)out_pcm[i] * gain_x1000 / 1000);
            if (scaled > INT16_MAX) {
                scaled = INT16_MAX;
            } else if (scaled < INT16_MIN) {
                scaled = INT16_MIN;
            }
            out_pcm[i] = (int16_t)scaled;
        }
    }

    *wav_out = out;
    *len_out = total;
    uint32_t rms = s_rec.sample_count
                   ? (uint32_t)sqrt((double)s_rec.energy / s_rec.sample_count) : 0;
    ESP_LOGI(TAG, "录音完成: %ums / %uKB, peak=%ld, rms=%lu, agc=%lux",
             (unsigned)(s_rec.pcm_len / 32), (unsigned)(total / 1024),
             (long)s_rec.peak, (unsigned long)rms,
             (unsigned long)gain_x1000 / 1000);
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
    return s_rec.pcm_len / 32;                  /* 32 字节/ms（单声道） */
}
