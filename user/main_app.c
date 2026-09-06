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
 * @brief Wi-Fi 开关切捔回调（状态栏滑块 → BSP 连接/断开）
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
            strcmp(now_buf, last_time) != 0) {
            ui_bridge_set_time(now_buf);
            memcpy(last_time, now_buf, sizeof(now_buf));
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
