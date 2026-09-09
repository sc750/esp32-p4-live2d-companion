/**
 * @file    main_app.c
 * @brief   用户应用入口实现
 *
 * Phase 1: 验证显示通路正常
 *
 * @date    2026-09-02
 * @version 1.0.0
 */

#include "user_app.h"

#include <stdio.h>
#include <string.h>
#include "bsp_init.h"
#include "bsp_wifi.h"
#include "event_bus.h"
#include "memory_manager.h"
#include "app_config.h"
#include "app_state_machine.h"
#include "ui_manager.h"
#include "ui_bridge.h"
#include "time_sync.h"
#include "dialog_manager.h"
#include "memory_store.h"
#include "chat_console.h"
#include "voice_pipeline.h"
#include "esp_timer.h"
#include "rig_model.h"
#include "rig_lvgl.h"
#include "rig_rig.h"
#include "rig_chatter.h"
#include "scr_home.h"
#include "ui_bridge.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

/**
 * @brief 闲聊回调：chatter 挑好一句话 → 更新主页字幕 + 让口型动起来
 *
 * 上下文：主循环任务（rig_chatter_tick 的调用方）。字幕更新走 ui_bridge
 * （内部拿 adapter 锁），口型 rig_rig_speak 只写两个 u32（与渲染任务
 * 之间为良性单写竞态，RISC-V 32 位对齐写原子）。
 */
static void on_chatter_line(const char *text, uint32_t speak_ms, void *ctx)
{
    (void)ctx;
    ui_bridge_set_subtitle(text);
    rig_rig_speak(speak_ms);
}

/**
 * @brief Wi-Fi 开关切换回调（状态栏滑块 → BSP 连接/断开）
 *
 * UI 层不直接碰 BSP：scr_home 只拨开关，这里做真正的连接/断开动作。
 * ON = 按 Kconfig 配置连接（并打开自动重连闸门）；OFF = 断开并停用重连。
 */
static void on_wifi_toggle(bool turn_on, void *ctx)
{
    (void)ctx;
    if (turn_on) {
        bsp_wifi_connect_from_config();
    } else {
        bsp_wifi_disconnect();
    }
}

/* ---- M0：串口对话链路（LLM 流式 → 字幕） ---- */
static char s_stream_buf[4096];         /* 流式回复累积（含 '\0'） */
static size_t s_stream_len = 0;
static int64_t s_last_flush_us = 0;

/** LLM token：攒进流式缓冲，≥100ms 才刷一次字幕（LVGL 重绘节流） */
static void on_llm_token(const char *text, void *ctx)
{
    (void)ctx;
    size_t tl = strlen(text);
    if (s_stream_len + tl >= sizeof(s_stream_buf) - 1) {
        return;                         /* 超长保护 */
    }
    memcpy(s_stream_buf + s_stream_len, text, tl);
    s_stream_len += tl;
    s_stream_buf[s_stream_len] = '\0';

    int64_t now = esp_timer_get_time();
    if (now - s_last_flush_us >= 100 * 1000) {
        s_last_flush_us = now;
        ui_bridge_set_subtitle(s_stream_buf);
    }
}

/** 按住说话按钮：按下沿开录、松开沿停录（LVGL 任务只发信号不阻塞） */
static void on_voice_hold(bool holding, void *ctx)
{
    (void)ctx;
    if (holding) {
        voice_pipeline_hold_start();
    } else {
        voice_pipeline_hold_stop();
    }
}

/** 语音管线的 UI 反馈 → 桥接到字幕/状态点 */
static void on_voice_state(int state, void *ctx)
{
    (void)ctx;
    ui_bridge_set_dialog_state(state);
}

static void on_voice_subtitle(const char *text, void *ctx)
{
    (void)ctx;
    ui_bridge_set_subtitle(text);
}

/** 串口一行：语音录音命令或对话文本（console 任务上下文，阻塞式跑完） */
static void on_chat_line(const char *text, void *ctx)
{
    (void)ctx;
    ESP_LOGI("main", "对话输入: %s", text);

    /* "rec 3" = 录 3 秒并走完整语音管线（M1 管线调试口） */
    if (strncmp(text, "rec ", 4) == 0) {
        int sec = atoi(text + 4);
        if (sec < 1 || sec > 30) {
            ui_bridge_set_subtitle("rec 用法：rec 1~30（秒）");
            return;
        }
        char hint[64];
        snprintf(hint, sizeof(hint), "录音 %ds 中，请说话……", sec);
        ui_bridge_set_subtitle(hint);
        esp_err_t err = voice_pipeline_record_ms((uint32_t)sec * 1000);
        if (err != ESP_OK) {
            ESP_LOGW("main", "rec 失败: %s", esp_err_to_name(err));
        }
        return;
    }

    if (!bsp_wifi_is_connected()) {
        ui_bridge_set_subtitle("……WiFi 还没连上呢，等一下再聊。");
        ESP_LOGW("main", "WiFi 未连接，跳过本轮对话");
        return;
    }
    s_stream_len = 0;
    s_stream_buf[0] = '\0';
    ui_bridge_set_dialog_state(DIALOG_STATE_THINKING);    /* 橙点=想 */

    char *reply = dialog_ask(text, on_llm_token, NULL);
    if (reply) {
        s_last_flush_us = esp_timer_get_time();
        ui_bridge_set_subtitle(reply);                  /* 终稿全覆盖一次 */
        ESP_LOGI("main", "回复: %.100s", reply);        /* 远程验收用（截前100字节） */
        voice_pipeline_speak(reply);                    /* M2：播报回复（声音+口型） */
        free(reply);
        ESP_LOGI("main", "回复完成");
    } else {
        ui_bridge_set_subtitle("……网络好像不太对劲，再试一次？");
        ui_bridge_set_dialog_state(DIALOG_STATE_IDLE);
    }
}

void user_app_run(void)
{
    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "  ESP32-P4 Live2D AI Companion");
    ESP_LOGI(TAG, "  Phase 2: 渲染管线");
    ESP_LOGI(TAG, "==========================================");

    /* 1. BSP 初始化 */
    ESP_ERROR_CHECK(bsp_init_all());

    /* 2. 核心服务初始化 */
    ESP_ERROR_CHECK(event_bus_init());
    ESP_ERROR_CHECK(mem_manager_init());
    ESP_ERROR_CHECK(app_config_init());

    /* 3. 状态机初始化 */
    ESP_ERROR_CHECK(app_state_machine_init());

    /* 4. UI 初始化 */
    ESP_ERROR_CHECK(ui_manager_init());

    /* 4b. Wi-Fi 开关接线（R12）：状态栏滑块 → BSP 连接/断开，
     * 并把 BSP 当前状态（开机即连）同步到 UI，滑块位置一步到位 */
    scr_home_set_wifi_toggle_cb(on_wifi_toggle, NULL);
    ui_bridge_set_wifi_state((int)bsp_wifi_get_state());
    ui_bridge_set_time("--:--", false);     /* 开机未校时：灰色占位 */

    /* 4c. AI 对话链路（Phase 3）：LLM + 人设 + 串口输入 + 语音管线 */
    ESP_ERROR_CHECK(memory_store_init());       /* Phase4：长期记忆先就位（读回 SPIFFS） */
    ESP_ERROR_CHECK(dialog_manager_init());
    ESP_ERROR_CHECK(llm_client_init());
    ESP_ERROR_CHECK(voice_pipeline_init());
    voice_ui_cb_t voice_ui = {
        .on_state = on_voice_state,
        .on_subtitle = on_voice_subtitle,
        .ctx = NULL,
    };
    voice_pipeline_set_ui(&voice_ui);
    scr_home_set_voice_hold_cb(on_voice_hold, NULL);
    chat_console_start(on_chat_line, NULL);

    /* 5. 角色加载 + 动画渲染（M03 R5b：入住 live2d_area + 触摸表情） */
    static rig_model_t s_model;
    if (rig_model_load_default(&s_model) == ESP_OK &&
        rig_rig_init(&s_model) == ESP_OK) {
        lv_obj_t *area = scr_home_get_live2d_area();
        /* fit_h=480 与 pack 的 max_height 一致——LVGL 1:1 绘制，无二次缩放 */
        rig_lvgl_create(area, &s_model, 480);
        rig_lvgl_start(30);

        /* 闲聊轮播（R10）：定时给字幕区投喂三玖语录 + 口型联动 */
        ESP_ERROR_CHECK(rig_chatter_init());
        rig_chatter_set_callback(on_chatter_line, NULL);
    } else {
        ESP_LOGW(TAG, "角色模型加载失败，继续启动");
    }

    /* 6. 内存报告 */
    mem_print_report();

    ESP_LOGI(TAG, "系统就绪！进入主循环...");

    /* 主循环：闲聊节拍 + Wi-Fi 状态/时钟同步 + 周期内存报告 */
    char last_time[8] = "";
    int last_wifi_state = -1;           /* -1 = 首轮强制刷一次 */
    bool last_time_synced = false;      /* 上轮 NTP 同步态（灰/黑切换用） */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* 闲聊轮播节拍（内部自带 3~8 分钟随机间隔，1s 粒度足够） */
        rig_chatter_tick();

        /* SNTP：联网即启动（幂等）；Wi-Fi 状态或分钟变化才刷状态栏，
         * 避免 1Hz 重绘 */
        const int wifi_state = (int)bsp_wifi_get_state();
        if (wifi_state == (int)BSP_WIFI_CONNECTED) {
            time_sync_start();
        }
        if (wifi_state != last_wifi_state) {
            ui_bridge_set_wifi_state(wifi_state);
            last_wifi_state = wifi_state;
        }
        char now_buf[8] = "--:--";
        if (time_sync_get_hhmm(now_buf, sizeof(now_buf)) &&
            (strcmp(now_buf, last_time) != 0 || !last_time_synced)) {
            /* 时间变了 或 刚从"未同步"转正——变灰/恢复色也要刷一次 */
            ui_bridge_set_time(now_buf, time_sync_is_synced());
            memcpy(last_time, now_buf, sizeof(now_buf));
        }
        last_time_synced = time_sync_is_synced();

        /* 每 60 秒打印内存报告 */
        static int tick_count = 0;
        tick_count++;
        if (tick_count >= 60) {
            tick_count = 0;
            mem_print_report();
        }
    }
}
