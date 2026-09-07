/**
 * @file    voice_pipeline.c
 * @brief   语音管线编排实现——录音→ASR→LLM→字幕
 *
 * 状态机（一次一轮，s_busy 防重入）：
 *   按下 → 录音（蓝点+“在听”） → 松开/30s上限 → ASR（“识别中”）
 *        → 识别文本 → LLM（橙点，token 流式刷字幕） → 绿点→灭
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#include "voice_pipeline.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "bsp_wifi.h"
#include "bsp_audio.h"
#include "voice_rec.h"
#include "asr_client.h"
#include "tts_client.h"
#include "dialog_manager.h"
#include "rig_rig.h"
#include "app_events.h"
#include <math.h>
#include "freertos/stream_buffer.h"

#define TAG "voice"

/* 事件位：按住说话的按下/松开沿 */
#define EVT_HOLD_START  BIT0
#define EVT_HOLD_STOP   BIT1

/* 流式字幕刷新节流（M2 接 token 流时启用） */
#define FLUSH_MIN_US    (100 * 1000)

static struct {
    bool inited;
    EventGroupHandle_t evt;
    voice_ui_cb_t ui;
    bool busy;                  /* 一轮管线进行中 */
} s_vp;

/* ---------- UI 快捷 ---------- */
static void ui_state(int st)
{
    if (s_vp.ui.on_state) {
        s_vp.ui.on_state(st, s_vp.ui.ctx);
    }
}

static void ui_text(const char *t)
{
    if (s_vp.ui.on_subtitle) {
        s_vp.ui.on_subtitle(t, s_vp.ui.ctx);
    }
}

/* ---------- 录音段 ---------- */

/** 录到 stop 事件或 ms 上限；返回时录音已结束 */
static void rec_until_stop_or(uint32_t max_ms)
{
    voice_rec_begin();
    ui_state(DIALOG_STATE_LISTENING);
    ui_text("在听呢……（说完松手）");
    uint32_t start = (uint32_t)(esp_timer_get_time() / 1000);
    while ((uint32_t)(esp_timer_get_time() / 1000) - start < max_ms) {
        EventBits_t bits = xEventGroupWaitBits(s_vp.evt, EVT_HOLD_STOP,
                                               pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(100));
        voice_rec_chunk();      /* 100ms 一块，落在等待超时的缝隙里 */
        if (bits & EVT_HOLD_STOP) {
            break;
        }
    }
}

/** ASR + LLM + 字幕（录音结束后调用；wav 所有权随之转移进来并释放） */
static void process_wav(char *wav, size_t wav_len)
{
    ui_state(DIALOG_STATE_THINKING);
    ui_text("……让我听听你说了啥");

    char *text = NULL;
    esp_err_t err = asr_recognize(wav, wav_len, &text);
    free(wav);
    if (err != ESP_OK || text == NULL) {
        ui_text("……没听清呢，再说一遍？");
        ui_state(DIALOG_STATE_IDLE);
        return;
    }
    ESP_LOGI(TAG, "识别: %s", text);

    /* M1：等 LLM 全文到齐；M2 接 TTS——回复即播报（绿点+声音+口型） */
    char *reply = dialog_ask(text, NULL, NULL);
    free(text);
    if (reply) {
        ui_text(reply);
        voice_pipeline_speak(reply);        /* 说话 + 口型，播完回 IDLE */
        free(reply);
    } else {
        ui_text("……脑子突然一片空白，再说一次？");
        ui_state(DIALOG_STATE_IDLE);
    }
}

/* ---------- TTS 播报（M2） ---------- */

/*
 * 播放架构（M2 教训：在 SSE 回调里同步 bsp_audio_play 会阻塞网络读取，
 * 服务端流式推送超时掐流——实测整条流只剩 1 块 160ms 音频）。
 * 这块 C6 Wi-Fi 到云端的 SSE 音频块间隙可达 1.6 秒，远高于一小块
 * PCM 的播放时长。语句级 TTS 因此优先完整预取：先收完一句，再稳定播完。
 * 牺牲少量首包延迟，换掉每个网络抖动都变成耳朵里爆豆的体验。
 */
#define SPK_RING_SIZE         (2 * 1024 * 1024) /* 约 32 秒 16kHz/双声道 PCM16 */
#define SPK_RECV_TIMEOUT_MS   20

static struct {
    StreamBufferHandle_t ring;
    uint8_t *ring_mem;              /* 环形缓冲存储（PSRAM） */
    StaticStreamBuffer_t ring_ctl;  /* 静态创建的控制块 */
    volatile bool synth_done;       /* TTS 拉流结束 */
    volatile bool player_done;      /* 播放任务排空退出 */
    volatile uint32_t underflows;   /* 诊断：播放时缓冲耗尽次数 */
    volatile uint32_t send_failures;
    uint32_t max_feed_gap_ms;
    int64_t last_feed_us;
    int16_t resample_tail[3];       /* 24k 源 PCM 不足三帧时跨 SSE 块续上 */
    size_t resample_tail_count;
} s_spk;

static void player_task(void *arg)
{
    static char buf[4096];
    (void)arg;
    ESP_LOGI(TAG, "TTS playback start: buffered=%uB (full sentence)",
             (unsigned)xStreamBufferBytesAvailable(s_spk.ring));
    while (1) {
        size_t got = xStreamBufferReceive(s_spk.ring, buf, sizeof(buf),
                                          pdMS_TO_TICKS(SPK_RECV_TIMEOUT_MS));
        if (got > 0) {
            esp_err_t err = bsp_audio_play(buf, got);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "I2S playback write failed: %s", esp_err_to_name(err));
            }
        } else if (xStreamBufferBytesAvailable(s_spk.ring) == 0) {
            /* 播放任务只在 TTS 拉流完成后启动，空缓冲就是本句结束。 */
            break;
        }
    }
    s_spk.player_done = true;
    vTaskDelete(NULL);
}

/** TTS 音频块：24k/mono → 16k/stereo，塞进播放缓冲 + 按块 RMS 驱动口型 */
static void on_tts_audio(const int16_t *pcm, size_t samples, void *ctx)
{
    (void)ctx;
    int64_t now_us = esp_timer_get_time();
    if (s_spk.last_feed_us) {
        uint32_t gap_ms = (uint32_t)((now_us - s_spk.last_feed_us) / 1000);
        if (gap_ms > s_spk.max_feed_gap_ms) {
            s_spk.max_feed_gap_ms = gap_ms;
        }
    }
    s_spk.last_feed_us = now_us;

    /* Codec 固定跑 16k/双声道，避免 TTS 和 ADC 反复重开同一条 I2S。
     * 每 3 个 24k 单声道样本变成 2 个 16k 双声道帧：第 2 帧做线性插值。 */
    size_t max_frames = (s_spk.resample_tail_count + samples) / 3 * 2;
    int16_t *codec_pcm = heap_caps_malloc(max_frames * 2 * sizeof(*codec_pcm),
                                          MALLOC_CAP_SPIRAM);
    if (codec_pcm == NULL) {
        s_spk.send_failures++;
        ESP_LOGW(TAG, "TTS 重采样缓冲分配失败");
        return;
    }
    size_t src = 0;
    size_t out_frames = 0;
    while (src < samples) {
        while (s_spk.resample_tail_count < 3 && src < samples) {
            s_spk.resample_tail[s_spk.resample_tail_count++] = pcm[src++];
        }
        if (s_spk.resample_tail_count < 3) {
            break;
        }
        int16_t first = s_spk.resample_tail[0];
        int16_t middle = (int16_t)(((int32_t)s_spk.resample_tail[1] +
                                    s_spk.resample_tail[2]) / 2);
        codec_pcm[out_frames * 2] = first;
        codec_pcm[out_frames * 2 + 1] = first;
        out_frames++;
        codec_pcm[out_frames * 2] = middle;
        codec_pcm[out_frames * 2 + 1] = middle;
        out_frames++;
        s_spk.resample_tail_count = 0;
    }

    /* StreamBuffer may accept only part of a block. Silently discarding the
     * remainder creates missing PCM samples and sounds exactly like stutter. */
    size_t bytes = out_frames * 2 * sizeof(*codec_pcm);
    size_t sent = 0;
    while (sent < bytes) {
        size_t wrote = xStreamBufferSend(s_spk.ring, (const uint8_t *)codec_pcm + sent,
                                         bytes - sent, pdMS_TO_TICKS(2000));
        if (wrote == 0) {
            s_spk.send_failures++;
            ESP_LOGW(TAG, "TTS ring write timeout: %u/%uB dropped",
                     (unsigned)(bytes - sent), (unsigned)bytes);
            break;
        }
        sent += wrote;
    }
    free(codec_pcm);
    int64_t acc = 0;
    for (size_t i = 0; i < samples; i++) {
        acc += (int64_t)pcm[i] * pcm[i];
    }
    double rms = sqrt((double)acc / (samples ? samples : 1));
    rig_mouth_t m = (rms > 2500) ? RIG_MOUTH_OPEN
                    : (rms > 600) ? RIG_MOUTH_HALF : RIG_MOUTH_CLOSED;
    rig_rig_set_mouth(m);
}

/**
 * @brief 把一句话播报出来（阻塞：TTS 流式拉取 + 播放任务连播）
 *
 * 绿点（SPEAKING）亮起 → 播完闭嘴回待机。硬件 codec 固定使用
 * 16k/双声道；TTS 的 24k/单声道输入由 on_tts_audio 转换。
 */
void voice_pipeline_speak(const char *text)
{
    ui_state(DIALOG_STATE_SPEAKING);
    /* 环形缓冲必须放 PSRAM——xStreamBufferCreate 默认走内部 SRAM
     * （总量仅 ~143KB，256KB 必然失败），静态创建 + PSRAM 存储 */
    s_spk.ring_mem = heap_caps_malloc(SPK_RING_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_spk.ring_mem) {
        ESP_LOGW(TAG, "播放缓冲（PSRAM）分配失败，本句跳过播报");
        ui_state(DIALOG_STATE_IDLE);
        return;
    }
    s_spk.ring = xStreamBufferCreateStatic(SPK_RING_SIZE, 1,
                                           s_spk.ring_mem, &s_spk.ring_ctl);
    s_spk.synth_done = false;
    s_spk.player_done = false;
    s_spk.underflows = 0;
    s_spk.send_failures = 0;
    s_spk.max_feed_gap_ms = 0;
    s_spk.last_feed_us = 0;
    s_spk.resample_tail_count = 0;
    esp_err_t err = tts_synthesize(text, "傲娇但温柔的少女语气，语速自然",
                                   on_tts_audio, NULL);
    s_spk.synth_done = true;
    if (xStreamBufferBytesAvailable(s_spk.ring) > 0 &&
        xTaskCreate(player_task, "spk_play", 4 * 1024, NULL, 5, NULL) == pdPASS) {
        /* 等播放任务排空。2MB 缓冲最多约 32 秒，留出足够收尾余量。 */
        int wait = 0;
        while (!s_spk.player_done && wait < 400) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait++;
        }
    } else if (err == ESP_OK) {
        ESP_LOGW(TAG, "TTS 没有可播放的音频，跳过本句播报");
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TTS 播报失败: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "TTS playback stats: underflows=%u, send_timeouts=%u, max_feed_gap=%ums",
             (unsigned)s_spk.underflows, (unsigned)s_spk.send_failures,
             (unsigned)s_spk.max_feed_gap_ms);
    vStreamBufferDelete(s_spk.ring);
    free(s_spk.ring_mem);
    s_spk.ring = NULL;
    rig_rig_set_mouth(RIG_MOUTH_CLOSED);
    rig_rig_set_mouth(RIG_MOUTH_AUTO);      /* 释放口型控制，idle 串接管 */
    ui_state(DIALOG_STATE_IDLE);
}

/* ---------- 两个入口 ---------- */

static void pipeline_run_hold(void)
{
    if (s_vp.busy) {
        ui_text("……等等，我还在上一句里呢");
        return;
    }
    if (!bsp_wifi_is_connected()) {
        ui_text("……WiFi 断了，聊不了天啦");
        return;
    }
    s_vp.busy = true;
    rec_until_stop_or(VOICE_REC_MAX_SEC * 1000);
    char *wav = NULL;
    size_t len = 0;
    if (voice_rec_end_and_get(&wav, &len) != ESP_OK) {
        ui_text("……声音太短啦，按住多说一会儿");
        ui_state(DIALOG_STATE_IDLE);
        s_vp.busy = false;
        return;
    }
    process_wav(wav, len);
    s_vp.busy = false;
}

static void pipeline_task(void *arg)
{
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(s_vp.evt, EVT_HOLD_START,
                                               pdTRUE, pdFALSE,
                                               portMAX_DELAY);
        if (bits & EVT_HOLD_START) {
            pipeline_run_hold();
        }
    }
}

esp_err_t voice_pipeline_init(void)
{
    if (s_vp.inited) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(voice_rec_init(), TAG, "rec init failed");
    ESP_RETURN_ON_ERROR(asr_client_init(), TAG, "asr init failed");
    ESP_RETURN_ON_ERROR(tts_client_init(), TAG, "tts init failed");
    s_vp.evt = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_vp.evt, ESP_ERR_NO_MEM, TAG, "evt alloc failed");
    s_vp.inited = true;
    if (xTaskCreate(pipeline_task, "voice_pipe", 8 * 1024, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建管线任务失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "语音管线就绪（按住说话 / rec <秒> 调试）");
    return ESP_OK;
}

void voice_pipeline_set_ui(const voice_ui_cb_t *cb)
{
    if (cb) {
        s_vp.ui = *cb;
    }
}

void voice_pipeline_hold_start(void)
{
    if (s_vp.inited) {
        xEventGroupSetBits(s_vp.evt, EVT_HOLD_START);
    }
}

void voice_pipeline_hold_stop(void)
{
    if (s_vp.inited) {
        xEventGroupSetBits(s_vp.evt, EVT_HOLD_STOP);
    }
}

esp_err_t voice_pipeline_record_ms(uint32_t ms)
{
    if (!s_vp.inited || s_vp.busy) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bsp_wifi_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_vp.busy = true;
    /* 同步版：录 ms 毫秒（100ms 块）后直接走管线 */
    voice_rec_begin();
    ui_state(DIALOG_STATE_LISTENING);
    ui_text("在听呢……");
    uint32_t start = (uint32_t)(esp_timer_get_time() / 1000);
    while ((uint32_t)(esp_timer_get_time() / 1000) - start < ms) {
        voice_rec_chunk();
        vTaskDelay(pdMS_TO_TICKS(1));   /* record 本身阻塞 100ms，无需长延时 */
    }
    char *wav = NULL;
    size_t len = 0;
    esp_err_t err = voice_rec_end_and_get(&wav, &len);
    if (err != ESP_OK) {
        ui_text("……声音太短啦");
        ui_state(DIALOG_STATE_IDLE);
        s_vp.busy = false;
        return err;
    }
    process_wav(wav, len);
    s_vp.busy = false;
    return ESP_OK;
}
