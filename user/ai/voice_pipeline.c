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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "bsp_wifi.h"
#include "voice_rec.h"
#include "asr_client.h"
#include "dialog_manager.h"
#include "app_events.h"

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

    /* M1：等 LLM 全文到齐一次上字幕；token 流式刷新与 TTS 一起在 M2 接 */
    char *reply = dialog_ask(text, NULL, NULL);
    free(text);
    if (reply) {
        ui_text(reply);
        free(reply);
    } else {
        ui_text("……脑子突然一片空白，再说一次？");
    }
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
