/**
 * @file    ui_bridge.c
 * @brief   UI Bridge 实现——"拿 adapter 锁 + 更新主页控件"的薄封装
 *
 * R10 起字幕常驻主页（独立 Chat 页已拆），所有跨任务 UI 更新
 * 统一从这座桥走，避免每个调用方自己记"要先拿锁"的规矩。
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

/* 1. 自身公开头 */
#include "ui_bridge.h"

/* 2. C 标准库 */
#include <string.h>

/* 3. 项目级 */
#include "scr_home.h"

/* 4. 平台/厂商头 */
#include "esp_lv_adapter.h"

void ui_bridge_set_subtitle(const char *text)
{
    if (text == NULL) {
        return;
    }
    esp_lv_adapter_lock(-1);            /* 递归锁：LVGL 任务内调用也安全 */
    scr_home_set_subtitle(text);
    esp_lv_adapter_unlock();
}

void ui_bridge_set_wifi_state(int state)
{
    esp_lv_adapter_lock(-1);
    scr_home_set_wifi_state((scr_wifi_state_t)state);
    esp_lv_adapter_unlock();
}

void ui_bridge_set_time(const char *time_str)
{
    if (time_str == NULL) {
        return;
    }
    esp_lv_adapter_lock(-1);
    scr_home_set_time(time_str);
    esp_lv_adapter_unlock();
}
