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
#include "freertos/queue.h"

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

#define TTS_SENTENCE_MAX  512
#define TTS_QUEUE_DEPTH   8
#define TTS_READY_DEPTH    4
typedef struct {
    char text[TTS_SENTENCE_MAX];
} tts_sentence_t;

/** 一段已经完整转换为 codec PCM 的短语，所有权在播放任务释放。 */
typedef struct {
    uint8_t *pcm;
    size_t len;
    size_t cap;
    int16_t resample_tail[3];
    size_t resample_tail_count;
} tts_pcm_phrase_t;

static QueueHandle_t s_tts_queue;
static QueueHandle_t s_tts_ready_queue;
static volatile uint32_t s_tts_pending;
static volatile bool s_tts_input_done;
static volatile bool s_tts_stream_active;
static SemaphoreHandle_t s_speak_lock;
static SemaphoreHandle_t s_tts_request_lock;
static char s_reply_text[2048];
static size_t s_reply_len;
static int64_t s_reply_last_flush_us;

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

/** LLM token 只做字幕累积；节流避免每个 token 都触发一次 LVGL 重绘。 */
static void on_reply_token(const char *text, void *ctx)
{
    (void)ctx;
    size_t len = strlen(text);
    if (s_reply_len + len >= sizeof(s_reply_text)) {
        return;
    }
    memcpy(s_reply_text + s_reply_len, text, len);
    s_reply_len += len;
    s_reply_text[s_reply_len] = '\0';
    int64_t now = esp_timer_get_time();
    if (now - s_reply_last_flush_us >= FLUSH_MIN_US) {
        s_reply_last_flush_us = now;
        ui_text(s_reply_text);
    }
}

/** 句末回调在 LLM 网络任务上下文执行，只投递，绝不在这里请求 TTS。 */
static void on_reply_sentence(const char *sentence, void *ctx)
{
    (void)ctx;
    if (!sentence[0] || !s_tts_queue) {
        return;
    }
    tts_sentence_t item = {0};
    strlcpy(item.text, sentence, sizeof(item.text));
    __atomic_fetch_add(&s_tts_pending, 1, __ATOMIC_RELAXED);
    if (xQueueSend(s_tts_queue, &item, pdMS_TO_TICKS(50)) == pdPASS) {
        ESP_LOGI(TAG, "LLM 句末入队: %.80s", item.text);
    } else {
        __atomic_fetch_sub(&s_tts_pending, 1, __ATOMIC_RELAXED);
        ESP_LOGW(TAG, "TTS 短句队列已满，本句未播报");
    }
}

/** 把 24k/mono 输入转为 16k/stereo，并追加到短语的完整 PCM 缓冲。 */
static void prefetch_audio_cb(const int16_t *pcm, size_t samples, void *ctx)
{
    tts_pcm_phrase_t *phrase = (tts_pcm_phrase_t *)ctx;
    size_t max_frames = (phrase->resample_tail_count + samples) / 3 * 2;
    size_t bytes = max_frames * 2 * sizeof(int16_t);
    if (bytes == 0) {
        return;
    }
    size_t required = phrase->len + bytes;
    if (required > phrase->cap) {
        size_t new_cap = phrase->cap ? phrase->cap : 32 * 1024;
        while (new_cap < required) {
            new_cap *= 2;
        }
        uint8_t *new_pcm = heap_caps_realloc(phrase->pcm, new_cap, MALLOC_CAP_SPIRAM);
        if (!new_pcm) {
            ESP_LOGW(TAG, "TTS 短语 PCM 扩容失败: %uB", (unsigned)new_cap);
            return;
        }
        phrase->pcm = new_pcm;
        phrase->cap = new_cap;
    }

    size_t src = 0;
    size_t out_frames = 0;
    int16_t *out = (int16_t *)(phrase->pcm + phrase->len);
    while (src < samples) {
        while (phrase->resample_tail_count < 3 && src < samples) {
            phrase->resample_tail[phrase->resample_tail_count++] = pcm[src++];
        }
        if (phrase->resample_tail_count < 3) {
            break;
        }
        int16_t first = phrase->resample_tail[0];
        int16_t middle = (int16_t)(((int32_t)phrase->resample_tail[1] +
                                    phrase->resample_tail[2]) / 2);
        out[out_frames * 2] = first;
        out[out_frames * 2 + 1] = first;
        out_frames++;
        out[out_frames * 2] = middle;
        out[out_frames * 2 + 1] = middle;
        out_frames++;
        phrase->resample_tail_count = 0;
    }
    phrase->len += out_frames * 2 * sizeof(*out);
}

static void update_mouth_from_pcm(const int16_t *pcm, size_t samples)
{
    int64_t acc = 0;
    for (size_t i = 0; i < samples; i++) {
        acc += (int64_t)pcm[i] * pcm[i];
    }
    double rms = sqrt((double)acc / (samples ? samples : 1));
    rig_rig_set_mouth(rms > 2500 ? RIG_MOUTH_OPEN :
                      rms > 600 ? RIG_MOUTH_HALF : RIG_MOUTH_CLOSED);
}

/** 播放器只消费完整短语，不会因网络速度低于播放速度而欠载。 */
static void tts_play_task(void *arg)
{
    (void)arg;
    tts_pcm_phrase_t *phrase = NULL;
    while (1) {
        if (xQueueReceive(s_tts_ready_queue, &phrase, portMAX_DELAY) != pdPASS) {
            continue;
        }
        xSemaphoreTake(s_speak_lock, portMAX_DELAY);
        ui_state(DIALOG_STATE_SPEAKING);
        ESP_LOGI(TAG, "TTS playback start: buffered=%uB (complete phrase)",
                 (unsigned)phrase->len);
        size_t offset = 0;
        while (offset < phrase->len) {
            size_t chunk = phrase->len - offset;
            if (chunk > 4096) {
                chunk = 4096;
            }
            update_mouth_from_pcm((const int16_t *)(phrase->pcm + offset),
                                  chunk / sizeof(int16_t));
            esp_err_t err = bsp_audio_play(phrase->pcm + offset, chunk);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "I2S playback write failed: %s", esp_err_to_name(err));
                break;
            }
            offset += chunk;
        }
        ESP_LOGI(TAG, "TTS playback stats: underflows=0, send_timeouts=0, complete phrase");
        free(phrase->pcm);
        free(phrase);
        rig_rig_set_mouth(RIG_MOUTH_CLOSED);
        if (__atomic_load_n(&s_tts_pending, __ATOMIC_RELAXED) > 0) {
            __atomic_fetch_sub(&s_tts_pending, 1, __ATOMIC_RELAXED);
        }
        xSemaphoreGive(s_speak_lock);
    }
}

/** 下载下一短语时，播放器可同时播放上一短语。 */
static void tts_stream_task(void *arg)
{
    (void)arg;
    tts_sentence_t item;
    while (1) {
        if (xQueueReceive(s_tts_queue, &item, portMAX_DELAY) != pdPASS) {
            continue;
        }
        tts_pcm_phrase_t *phrase = calloc(1, sizeof(*phrase));
        if (!phrase) {
            ESP_LOGW(TAG, "TTS 短语控制块分配失败");
            __atomic_fetch_sub(&s_tts_pending, 1, __ATOMIC_RELAXED);
            continue;
        }
        xSemaphoreTake(s_tts_request_lock, portMAX_DELAY);
        esp_err_t err = tts_synthesize(item.text, "傲娇但温柔的少女语气，语速自然",
                                       prefetch_audio_cb, phrase);
        xSemaphoreGive(s_tts_request_lock);
        if (err != ESP_OK || phrase->len == 0) {
            ESP_LOGW(TAG, "TTS 短语下载失败: %s", esp_err_to_name(err));
            free(phrase->pcm);
            free(phrase);
            __atomic_fetch_sub(&s_tts_pending, 1, __ATOMIC_RELAXED);
            continue;
        }
        if (xQueueSend(s_tts_ready_queue, &phrase, portMAX_DELAY) != pdPASS) {
            free(phrase->pcm);
            free(phrase);
            __atomic_fetch_sub(&s_tts_pending, 1, __ATOMIC_RELAXED);
        }
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

    s_reply_len = 0;
    s_reply_text[0] = '\0';
    __atomic_store_n(&s_tts_pending, 0, __ATOMIC_RELAXED);
    s_tts_input_done = false;
    s_tts_stream_active = true;
    s_reply_last_flush_us = esp_timer_get_time();
    char *reply = dialog_ask_stream(text, on_reply_token, on_reply_sentence, NULL);
    free(text);
    if (reply) {
        ui_text(reply);
        s_tts_input_done = true;
        /* 等语音任务播完已入队短句；LLM 和第一个 TTS 已在此前并行。 */
        while (__atomic_load_n(&s_tts_pending, __ATOMIC_RELAXED) > 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        rig_rig_set_mouth(RIG_MOUTH_CLOSED);
        rig_rig_set_mouth(RIG_MOUTH_AUTO);
        s_tts_stream_active = false;
        ui_state(DIALOG_STATE_IDLE);
        free(reply);
    } else {
        s_tts_stream_active = false;
        ui_text("……脑子突然一片空白，再说一次？");
        ui_state(DIALOG_STATE_IDLE);
    }
}

/* ---------- TTS 播报（M2） ---------- */

/*
 * 播放架构（M2 教训：在 SSE 回调里同步 bsp_audio_play 会阻塞网络读取，
 * 服务端流式推送超时掐流——实测整条流只剩 1 块 160ms 音频）。
 * 这块 C6 Wi-Fi 到云端的 SSE 音频块存在长间隙，且长句的平均下发速度
 * 可能低于实时播放速度。语音按短语切分，每段必须完整预取后才稳定播出；
 * 下一段在上一段播放期间继续下载，避免网络抖动直接传到扬声器。
 */
#define SPK_RING_SIZE         (512 * 1024) /* 约 8 秒 16kHz/双声道 PCM16 */
#define SPK_RECV_TIMEOUT_MS   40

static struct {
    StreamBufferHandle_t ring;
    uint8_t *ring_mem;              /* 环形缓冲存储（PSRAM） */
    StaticStreamBuffer_t ring_ctl;  /* 静态创建的控制块 */
    volatile bool synth_done;       /* TTS 拉流结束 */
    volatile bool player_done;      /* 播放任务排空退出 */
    volatile bool player_started;   /* 预缓冲足够后才启动播放 */
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
    ESP_LOGI(TAG, "TTS playback start: buffered=%uB (complete phrase)",
             (unsigned)xStreamBufferBytesAvailable(s_spk.ring));
    while (1) {
        size_t got = xStreamBufferReceive(s_spk.ring, buf, sizeof(buf),
                                          pdMS_TO_TICKS(SPK_RECV_TIMEOUT_MS));
        if (got > 0) {
            esp_err_t err = bsp_audio_play(buf, got);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "I2S playback write failed: %s", esp_err_to_name(err));
            }
        } else if (s_spk.synth_done && xStreamBufferBytesAvailable(s_spk.ring) == 0) {
            break;                  /* SSE 收尾且缓冲已排空 */
        } else {
            s_spk.underflows++;     /* 网络抖动：等后续 SSE，不退出播放器 */
        }
    }
    s_spk.player_done = true;
    s_spk.player_started = false;
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
 * @brief 把一个短语播报出来（完整预取后播放）
 *
 * 绿点（SPEAKING）亮起 → 播完闭嘴回待机。硬件 codec 固定使用
 * 16k/双声道；TTS 的 24k/单声道输入由 on_tts_audio 转换。
 */
void voice_pipeline_speak(const char *text)
{
    if (!text || !text[0] || !s_speak_lock) {
        return;
    }
    xSemaphoreTake(s_speak_lock, portMAX_DELAY);
    ui_state(DIALOG_STATE_SPEAKING);
    /* 环形缓冲必须放 PSRAM——xStreamBufferCreate 默认走内部 SRAM
     * （总量仅 ~143KB，256KB 必然失败），静态创建 + PSRAM 存储 */
    s_spk.ring_mem = heap_caps_malloc(SPK_RING_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_spk.ring_mem) {
        ESP_LOGW(TAG, "播放缓冲（PSRAM）分配失败，本句跳过播报");
        if (!s_tts_stream_active) {
            ui_state(DIALOG_STATE_IDLE);
        }
        xSemaphoreGive(s_speak_lock);
        return;
    }
    s_spk.ring = xStreamBufferCreateStatic(SPK_RING_SIZE, 1,
                                           s_spk.ring_mem, &s_spk.ring_ctl);
    s_spk.synth_done = false;
    s_spk.player_done = false;
    s_spk.player_started = false;
    s_spk.underflows = 0;
    s_spk.send_failures = 0;
    s_spk.max_feed_gap_ms = 0;
    s_spk.last_feed_us = 0;
    s_spk.resample_tail_count = 0;
    xSemaphoreTake(s_tts_request_lock, portMAX_DELAY);
    esp_err_t err = tts_synthesize(text, "傲娇但温柔的少女语气，语速自然",
                                   on_tts_audio, NULL);
    xSemaphoreGive(s_tts_request_lock);
    s_spk.synth_done = true;
    if (xStreamBufferBytesAvailable(s_spk.ring) > 0) {
        s_spk.player_started = true;
        if (xTaskCreate(player_task, "spk_play", 4 * 1024, NULL, 5, NULL) != pdPASS) {
            s_spk.player_started = false;
        }
    } else if (err == ESP_OK && xStreamBufferBytesAvailable(s_spk.ring) == 0) {
        ESP_LOGW(TAG, "TTS 没有可播放的音频，跳过本句播报");
    }
    int wait = 0;
    while (s_spk.player_started && !s_spk.player_done && wait < 400) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait++;
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
    if (!s_tts_stream_active ||
        (s_tts_input_done &&
         __atomic_load_n(&s_tts_pending, __ATOMIC_RELAXED) <= 1 &&
         uxQueueMessagesWaiting(s_tts_queue) == 0)) {
        ui_state(DIALOG_STATE_IDLE);
    }
    xSemaphoreGive(s_speak_lock);
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
    s_tts_queue = xQueueCreate(TTS_QUEUE_DEPTH, sizeof(tts_sentence_t));
    ESP_RETURN_ON_FALSE(s_tts_queue, ESP_ERR_NO_MEM, TAG, "tts queue alloc failed");
    s_tts_ready_queue = xQueueCreate(TTS_READY_DEPTH, sizeof(tts_pcm_phrase_t *));
    ESP_RETURN_ON_FALSE(s_tts_ready_queue, ESP_ERR_NO_MEM, TAG, "tts ready queue alloc failed");
    s_speak_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_speak_lock, ESP_ERR_NO_MEM, TAG, "speaker lock alloc failed");
    s_tts_request_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_tts_request_lock, ESP_ERR_NO_MEM, TAG, "tts request lock alloc failed");
    s_vp.inited = true;
    if (xTaskCreate(pipeline_task, "voice_pipe", 8 * 1024, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建管线任务失败");
        return ESP_FAIL;
    }
    if (xTaskCreate(tts_stream_task, "tts_stream", 6 * 1024, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建流式 TTS 任务失败");
        return ESP_FAIL;
    }
    if (xTaskCreate(tts_play_task, "tts_play", 6 * 1024, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建 TTS 播放任务失败");
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
