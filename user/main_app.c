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
#include "memory_extract.h"
#include "diary_service.h"
#include "ai_http.h"
#include "music_service.h"
#include "chat_console.h"
#include "voice_pipeline.h"
#include "gw_client.h"        /* 语音网关 WS 客户端（步骤 1） */
#include "esp_timer.h"
#include "rig_model.h"
#include "rig_lvgl.h"
#include "rig_rig.h"
#include "rig_chatter.h"
#include "scr_home.h"
#include "scr_music.h"
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
        music_notify_voice_start();     /* Phase4：按住说话即停音乐（比 TTS 抢占更早介入） */
        voice_pipeline_hold_start();
    } else {
        voice_pipeline_hold_stop();
    }
}

/** 语音管线的 UI 反馈 → 桥接到字幕/状态点；
 * 对话进行中让闲聊轮播闭嘴（防随机语料覆盖对话字幕，2026-09-12 实测） */
static void on_voice_state(int state, void *ctx)
{
    (void)ctx;
    ui_bridge_set_dialog_state(state);
    /* 对话结束后 5s 恢复闲聊投喂（正常间隔 3~8 分钟）；
     * 对话/播报期间保持忙静默（闲聊不抢字幕） */
    rig_chatter_set_busy_soon(state == DIALOG_STATE_IDLE ? false : true);
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

    /* Phase4：任何一次对话交互开始前先停音乐。
     * 必要性：音乐流（持续占 SDIO 带宽）与随后的 LLM HTTPS 请求并发时，
     * 实测会把 esp_hosted 的 SDIO 收发内存池打满（"mempool OOM"），
     * 双方一起超时——先停音乐把带宽让给对话，是最省事的根治。 */
    music_notify_voice_start();

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

/* ---- Phase 5：音乐页（屏幕音乐控制入口） ---- */

/** 主页音符按钮 → 进音乐页；顺手把真实音量同步到滑块（串口改过也不失配） */
static void on_music_entry(void *ctx)
{
    (void)ctx;
    ui_bridge_navigate(UI_PAGE_MUSIC);              /* 持锁切页（ui_manager_navigate 自身不带锁） */
    ui_bridge_set_music_volume(music_get_volume()); /* 音量滑块对齐真实值 */
}

/** 音乐页返回按钮 → 回主页（音乐继续播：播放任务独立于页面，不受切页影响） */
static void on_music_back(void *ctx)
{
    (void)ctx;
    ui_bridge_navigate(UI_PAGE_HOME);               /* 切回主页 */
}

/**
 * @brief 音乐页播控命令 → music_service
 *
 * 安全性：LVGL 事件回调运行在 LVGL 任务上下文，这里调的每个接口都是
 * 非阻塞的（music_play_index/next/prev 只入命令队列，pause/resume 只置
 * 标志，set_volume 透传 codec），不会把 UI 任务卡住。
 */
static void on_music_ctrl(int cmd, int arg, void *ctx)
{
    (void)ctx;
    /* 显式点播 = 用户此刻要听音乐：先掐掉未播完的 TTS（2026-09-12 上板
     * 实测：遗留 TTS 短语队列会把音乐反复停掉，听感"一卡一卡"） */
    switch (cmd) {                                              /* UI 枚举 → 业务调用 */
        case SCR_MUSIC_CMD_PLAY:   voice_pipeline_tts_cancel(); music_play_index(arg); break;   /* 播第 arg 首 */
        case SCR_MUSIC_CMD_PAUSE:  music_pause();        break;     /* 暂停 */
        case SCR_MUSIC_CMD_RESUME: voice_pipeline_tts_cancel(); music_resume();  break; /* 从暂停继续 */
        case SCR_MUSIC_CMD_NEXT:   voice_pipeline_tts_cancel(); music_next();    break;   /* 下一首/下一台 */
        case SCR_MUSIC_CMD_PREV:   voice_pipeline_tts_cancel(); music_prev();    break;   /* 上一首/上一台 */
        case SCR_MUSIC_CMD_VOL:    music_set_volume(arg); break;    /* 音量 0~100 */
        default: break;                                             /* 未知命令忽略 */
    }
}

/** 用音乐服务的曲目表填充音乐页列表（须在 music_service_init 之后调） */
static void music_ui_load_playlist(void)
{
    static const char *names[SCR_MUSIC_LIST_MAX];   /* 曲名快照（static：不占任务栈） */
    int n = music_count();                          /* 当前曲目数 */
    if (n > SCR_MUSIC_LIST_MAX) {                   /* 超出页面列表上限 */
        ESP_LOGW("main", "曲目 %d 首超出音乐页上限，只显示前 %d",    /* 告警 */
                 n, SCR_MUSIC_LIST_MAX);
        n = SCR_MUSIC_LIST_MAX;                     /* 截断 */
    }
    for (int i = 0; i < n; i++) {                   /* 逐个取曲名 */
        names[i] = music_name_at(i);                /* 指向曲目表内部（PSRAM 常驻，无需拷贝） */
    }
    ui_bridge_set_music_playlist(names, n);         /* 持锁交给页面建列表项（渲染任务已在跑） */
    ESP_LOGI("main", "音乐页播放列表: %d 项", n);    /* 日志 */
}

/** 音乐页接线（入口/返回/播控 + 列表）——打包一个调用，避免 user_app_run 编排膨胀 */
static void wire_music_ui(void)
{
    scr_home_set_music_entry_cb(on_music_entry, NULL);  /* 主页音符按钮 */
    scr_music_set_back_cb(on_music_back, NULL);         /* 页面返回按钮 */
    scr_music_set_control_cb(on_music_ctrl, NULL);      /* 播控命令出口 */
    music_ui_load_playlist();                           /* 填充播放列表 */
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
    ESP_ERROR_CHECK(ai_http_init());            /* Phase4：网络互斥锁先行（TLS 并发闸门） */
    ESP_ERROR_CHECK(memory_store_init());       /* Phase4：长期记忆先就位（读回 SPIFFS） */
    ESP_ERROR_CHECK(memory_extract_init());     /* Phase4：摘要提取缓冲 */
    ESP_ERROR_CHECK(diary_service_init());      /* Phase4：日记服务（22:00 定时） */
    ESP_ERROR_CHECK(music_service_init());      /* Phase4：音乐服务（SD 挂载+播放任务） */
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

    /* 4d. 音乐页接线（Phase 5）：须在 music_service_init 之后（列表要读曲目表） */
    wire_music_ui();

    /* 4e. 语音网关 WS 客户端（网关阶段步骤 1）：仅通道，不影响现有链路 */
    ESP_ERROR_CHECK(gw_client_init());

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

        /* 音乐页状态刷新（Phase5）：只在音乐页可见时做——主页时省掉每秒拿锁。
         * 页面内部还有"值未变不重绘"的闸门，所以这里按 1s 节拍喂是安全的。 */
        if (ui_manager_get_current_page() == UI_PAGE_MUSIC) {
            ui_bridge_set_music_state(music_is_playing(), music_is_paused(),
                                      music_current_index(), music_position_sec());
        }

        /* 每 60 秒打印内存报告 */
        static int tick_count = 0;
        tick_count++;
        if (tick_count >= 60) {
            tick_count = 0;
            mem_print_report();
        }
    }
}
