/**
 * @file    app_events.c
 * @brief   事件类型名称映射实现
 *
 * 本文件提供事件类型枚举值到可读字符串的转换。
 * 主要用于日志输出，方便调试时快速定位是哪个事件。
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

/* 1. 自身公开头 */
#include "app_events.h"

/* 2. C 标准库 */

/* 3. 项目级 */

/* 4. 平台/厂商头 */
#include "esp_log.h"

static const char *TAG = "app_events";

/**
 * @brief 获取事件类型的字符串名称
 *
 * 用法示例：
 *   ESP_LOGI(TAG, "收到事件: %s", app_event_type_name(EVENT_TOUCH_DOWN));
 *   输出: "收到事件: TOUCH_DOWN"
 *
 * @param[in] type  事件类型枚举值
 * @return 事件名称字符串，未知事件返回 "UNKNOWN"
 */
const char *app_event_type_name(app_event_type_t type)
{
    /*
     * 静态字符串数组：用枚举值作为下标，直接索引到对应的名称字符串。
     * 这种写法比 switch-case 更简洁，且查找速度是 O(1)。
     *
     * [EVENT_xxx] 是 C99 的"指定初始化器"语法：
     *   表示 names[EVENT_xxx] = "xxx"，中间的元素自动填 NULL。
     */
    static const char *names[] = {
        /* === 触摸事件 === */
        [EVENT_TOUCH_DOWN]        = "TOUCH_DOWN",        /* 手指按下屏幕 */
        [EVENT_TOUCH_UP]          = "TOUCH_UP",          /* 手指离开屏幕 */
        [EVENT_TOUCH_MOVE]        = "TOUCH_MOVE",        /* 手指在屏幕上滑动 */
        [EVENT_SCREEN_TAP]        = "SCREEN_TAP",        /* 点击屏幕（角色区域） */
        [EVENT_SCREEN_LONG_PRESS] = "SCREEN_LONG_PRESS", /* 长按屏幕超过1秒 */

        /* === 语音对话事件 === */
        [EVENT_ASR_PARTIAL]       = "ASR_PARTIAL",       /* 语音识别中间结果（边说边出字） */
        [EVENT_ASR_FINAL]         = "ASR_FINAL",         /* 语音识别最终结果（说完了） */
        [EVENT_LLM_TOKEN]         = "LLM_TOKEN",         /* 大模型返回一个字/词 */
        [EVENT_LLM_DONE]          = "LLM_DONE",          /* 大模型推理完成 */
        [EVENT_LLM_TOOL_CALL]     = "LLM_TOOL_CALL",     /* 大模型调用外部工具 */
        [EVENT_TTS_CHUNK]         = "TTS_CHUNK",         /* TTS 返回一小段音频 */
        [EVENT_TTS_DONE]          = "TTS_DONE",          /* TTS 播放完毕 */

        /* === 定时器事件 === */
        [EVENT_POMODORO_TICK]     = "POMODORO_TICK",     /* 番茄钟每秒滴答 */
        [EVENT_POMODORO_DONE]     = "POMODORO_DONE",     /* 番茄钟25分钟到 */
        [EVENT_DIARY_TIME]        = "DIARY_TIME",        /* 到了写日记的时间 */
        [EVENT_SLEEP_TIMEOUT]     = "SLEEP_TIMEOUT",     /* 超时未操作，准备息屏 */

        /* === 系统事件 === */
        [EVENT_WIFI_CONNECTED]    = "WIFI_CONNECTED",    /* Wi-Fi连接成功 */
        [EVENT_WIFI_DISCONNECTED] = "WIFI_DISCONNECTED", /* Wi-Fi断开 */
        [EVENT_LOW_MEMORY]        = "LOW_MEMORY",        /* 内存不足告警 */
        [EVENT_THEME_CHANGE]      = "THEME_CHANGE",      /* 昼/夜主题切换 */

        /* === Live2D 角色事件 === */
        [EVENT_L2D_EXPRESSION]    = "L2D_EXPRESSION",    /* 切换角色表情 */
        [EVENT_L2D_MOTION]        = "L2D_MOTION",        /* 触发角色动作 */

        /* === 页面导航事件 === */
        [EVENT_NAV_HOME]          = "NAV_HOME",          /* 导航到主页 */
        [EVENT_NAV_CHAT]          = "NAV_CHAT",          /* 导航到对话页 */
        [EVENT_NAV_MUSIC]         = "NAV_MUSIC",         /* 导航到音乐页 */
        [EVENT_NAV_POMODORO]      = "NAV_POMODORO",      /* 导航到番茄钟页 */
        [EVENT_NAV_DIARY]         = "NAV_DIARY",         /* 导航到日记页 */
        [EVENT_NAV_SETTINGS]      = "NAV_SETTINGS",      /* 导航到设置页 */
    };

    /* 边界检查：确保枚举值在有效范围内 */
    if (type >= 0 && type < EVENT_TYPE_COUNT && names[type] != NULL) {
        return names[type];  /* 返回对应的字符串名称 */
    }
    return "UNKNOWN";  /* 未知事件类型，返回 "UNKNOWN" */
}
