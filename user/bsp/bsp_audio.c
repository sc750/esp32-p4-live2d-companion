/**
 * @file    bsp_audio.c
 * @brief   音频 BSP 实现（ES8311 录放 + PA 托管）
 *
 * 实现对齐官方 bsp_extra_codec_* 模式：
 *   bsp_audio_codec_speaker_init() / bsp_audio_codec_microphone_init()
 *   （内部懒初始化共享 I2C1 与 I2S1，PA=GPIO53 由 codec 驱动托管）
 *   → esp_codec_dev_open(16k/16bit/2ch) → set_in_gain / set_out_vol
 *
 * @date    2026-09-03
 * @version 1.0.0
 */

#include "bsp_audio.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

/* 官方 BSP */
#include "bsp/esp-bsp.h"

static const char *TAG = "bsp_audio";

static esp_codec_dev_handle_t s_play_handle = NULL;   /* 扬声器（ES8311 DAC + PA） */
static esp_codec_dev_handle_t s_record_handle = NULL; /* 麦克风（ES8311 ADC） */
static bool s_is_init = false;
static int s_volume = BSP_AUDIO_DEFAULT_VOLUME;

esp_err_t bsp_audio_open(void)
{
    if (s_is_init) {
        return ESP_OK;
    }

    /* speaker_init/microphone_init 内部懒初始化 I2C1(GPIO7/8) + I2S1 */
    s_play_handle = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_FALSE(s_play_handle, ESP_FAIL, TAG, "speaker codec init failed");

    s_record_handle = bsp_audio_codec_microphone_init();
    ESP_RETURN_ON_FALSE(s_record_handle, ESP_FAIL, TAG, "microphone codec init failed");

    ESP_RETURN_ON_ERROR(bsp_audio_set_fs(BSP_AUDIO_DEFAULT_SAMPLE_RATE,
                                         BSP_AUDIO_DEFAULT_BIT_WIDTH,
                                         BSP_AUDIO_DEFAULT_CHANNEL),
                        TAG, "set fs failed");

    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_play_handle, s_volume),
                        TAG, "set out vol failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_record_handle, BSP_AUDIO_DEFAULT_ADC_GAIN),
                        TAG, "set in gain failed");

    s_is_init = true;
    ESP_LOGI(TAG, "音频初始化成功 (ES8311, %dHz/%dbit/%dch, vol=%d)",
             BSP_AUDIO_DEFAULT_SAMPLE_RATE, BSP_AUDIO_DEFAULT_BIT_WIDTH,
             BSP_AUDIO_DEFAULT_CHANNEL, s_volume);
    return ESP_OK;
}

bool bsp_audio_is_ready(void)
{
    return s_is_init;
}

esp_err_t bsp_audio_set_volume(int volume)
{
    ESP_RETURN_ON_FALSE(s_play_handle, ESP_ERR_INVALID_STATE, TAG, "audio not init");
    if (volume < 0 || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_play_handle, volume),
                        TAG, "set out vol failed");
    s_volume = volume;
    return ESP_OK;
}

esp_err_t bsp_audio_set_fs(uint32_t rate, uint32_t bits, uint32_t channels)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = rate,
        .channel = channels,
        .bits_per_sample = bits,
    };

    /* 官方模式：先 close 再 open，speaker 与 microphone 分别处理 */
    if (s_play_handle) {
        ESP_RETURN_ON_ERROR(esp_codec_dev_close(s_play_handle), TAG, "close play failed");
        ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_play_handle, &fs), TAG, "open play failed");
    }
    if (s_record_handle) {
        ESP_RETURN_ON_ERROR(esp_codec_dev_close(s_record_handle), TAG, "close record failed");
        ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_record_handle, &fs), TAG, "open record failed");
        ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_record_handle, BSP_AUDIO_DEFAULT_ADC_GAIN),
                            TAG, "set in gain failed");
    }
    return ESP_OK;
}

esp_err_t bsp_audio_play(const void *data, size_t len)
{
    ESP_RETURN_ON_FALSE(s_play_handle, ESP_ERR_INVALID_STATE, TAG, "audio not init");
    ESP_RETURN_ON_FALSE(data && len, ESP_ERR_INVALID_ARG, TAG, "bad arg");
    return esp_codec_dev_write(s_play_handle, (void *)data, len);
}

esp_err_t bsp_audio_record(void *data, size_t len)
{
    ESP_RETURN_ON_FALSE(s_record_handle, ESP_ERR_INVALID_STATE, TAG, "audio not init");
    ESP_RETURN_ON_FALSE(data && len, ESP_ERR_INVALID_ARG, TAG, "bad arg");
    return esp_codec_dev_read(s_record_handle, data, len);
}

esp_err_t bsp_audio_self_test(void)
{
    esp_err_t ret;

    /* ---- 扬声器：0.5s 1kHz 正弦提示音（16k/16bit/2ch = 32KB） ---- */
    const uint32_t rate = BSP_AUDIO_DEFAULT_SAMPLE_RATE;
    const size_t samples = rate / 2; /* 0.5s，单声道样点数 */
    int16_t *pcm = heap_caps_malloc(samples * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(pcm, ESP_ERR_NO_MEM, TAG, "no mem for test tone");

    for (size_t i = 0; i < samples; i++) {
        int16_t v = (int16_t)(sinf(2.0f * (float)M_PI * 1000.0f * (float)i / (float)rate) * 8000);
        pcm[2 * i] = v;      /* 左声道 */
        pcm[2 * i + 1] = v;  /* 右声道 */
    }
    ret = bsp_audio_play(pcm, samples * 2 * sizeof(int16_t));
    heap_caps_free(pcm);
    ESP_RETURN_ON_ERROR(ret, TAG, "play test tone failed");
    ESP_LOGI(TAG, "扬声器自检音播放完成");

    /* ---- 麦克风：读 100ms，计算峰值电平 ---- */
    const size_t rec_bytes = rate / 10 * 2 * sizeof(int16_t); /* 100ms 双声道 */
    int16_t *rec = heap_caps_malloc(rec_bytes, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(rec, ESP_ERR_NO_MEM, TAG, "no mem for mic test");

    ret = bsp_audio_record(rec, rec_bytes);
    if (ret == ESP_OK) {
        int32_t peak = 0;
        for (size_t i = 0; i < rec_bytes / sizeof(int16_t); i++) {
            int32_t a = rec[i] > 0 ? rec[i] : -rec[i];
            if (a > peak) {
                peak = a;
            }
        }
        ESP_LOGI(TAG, "麦克风自检: 100ms 采样峰值 %ld/32767%s",
                 (long)peak, peak > 50 ? "" : " (环境安静或无声源)");
    } else {
        ESP_LOGW(TAG, "麦克风读取出错 (%d)", ret);
    }
    heap_caps_free(rec);

    return ESP_OK;
}
