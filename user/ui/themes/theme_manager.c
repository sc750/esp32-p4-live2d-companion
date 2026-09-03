/**
 * @file    theme_manager.c
 * @brief   昼夜主题管理实现
 *
 * 主题颜色在运行时初始化（因为 LVGL 的 lv_color_hex() 不是 C 常量表达式，
 * 不能用于静态初始化）。
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

/* 1. 自身公开头 */
#include "theme_manager.h"

/* 2. C 标准库 */

/* 3. 项目级 */

/* 4. 平台/厂商头 */
#include "esp_log.h"

static const char *TAG = "theme";

/* 日间和夜间主题的颜色表（运行时初始化） */
static theme_colors_t s_day_theme;
static theme_colors_t s_night_theme;

/* 当前使用的主题 */
static theme_type_t s_current_theme = THEME_DAY;

/**
 * @brief 初始化两套主题的颜色值
 *
 * 日间主题：浅色背景，适合白天使用
 * 夜间主题：深色背景，适合夜间使用，减少对眼睛的刺激
 */
static void init_theme_colors(void)
{
    /* ===== 日间主题（浅色） ===== */
    s_day_theme.bg_color        = lv_color_hex(0xF5F5F5);  /* 浅灰白背景 */
    s_day_theme.text_color      = lv_color_hex(0x333333);  /* 深灰文字 */
    s_day_theme.primary_color   = lv_color_hex(0x4A90D9);  /* 天蓝色（按钮/强调） */
    s_day_theme.accent_color    = lv_color_hex(0xFF6B9D);  /* 粉红色（特殊标记） */
    s_day_theme.caption_bg      = lv_color_hex(0x000000);  /* 字幕背景（黑色半透明） */
    s_day_theme.card_bg         = lv_color_hex(0xFFFFFF);  /* 白色卡片 */
    s_day_theme.status_bar_bg   = lv_color_hex(0xE8E8E8);  /* 浅灰色状态栏 */

    /* ===== 夜间主题（深色） ===== */
    s_night_theme.bg_color      = lv_color_hex(0x1A1A2E);  /* 深蓝黑背景 */
    s_night_theme.text_color    = lv_color_hex(0xE0E0E0);  /* 浅白文字 */
    s_night_theme.primary_color = lv_color_hex(0x7B68EE);  /* 紫蓝色（按钮/强调） */
    s_night_theme.accent_color  = lv_color_hex(0xFF6B9D);  /* 粉红色（不变） */
    s_night_theme.caption_bg    = lv_color_hex(0x000000);  /* 字幕背景（黑色半透明） */
    s_night_theme.card_bg       = lv_color_hex(0x16213E);  /* 深蓝卡片 */
    s_night_theme.status_bar_bg = lv_color_hex(0x0F3460);  /* 深蓝状态栏 */
}

esp_err_t theme_manager_init(theme_type_t initial_theme)
{
    /* 初始化颜色表 */
    init_theme_colors();

    /* 参数检查：主题值必须有效 */
    if (initial_theme >= THEME_COUNT) {
        ESP_LOGW(TAG, "无效主题 %d，使用默认日间主题", initial_theme);
        initial_theme = THEME_DAY;
    }

    s_current_theme = initial_theme;
    ESP_LOGI(TAG, "主题初始化完成: %s",
             s_current_theme == THEME_DAY ? "日间" : "夜间");
    return ESP_OK;
}

void theme_manager_set(theme_type_t theme)
{
    /* 参数检查 + 避免重复设置 */
    if (theme >= THEME_COUNT || theme == s_current_theme) {
        return;
    }

    s_current_theme = theme;
    ESP_LOGI(TAG, "主题切换: %s",
             theme == THEME_DAY ? "日间" : "夜间");
}

theme_type_t theme_manager_get(void)
{
    return s_current_theme;
}

const theme_colors_t *theme_manager_get_colors(void)
{
    /* 根据当前主题返回对应的颜色集 */
    return (s_current_theme == THEME_DAY) ? &s_day_theme : &s_night_theme;
}

void theme_manager_toggle(void)
{
    /* 昼→夜，夜→昼 */
    theme_type_t new_theme = (s_current_theme == THEME_DAY) ? THEME_NIGHT : THEME_DAY;
    theme_manager_set(new_theme);
}
