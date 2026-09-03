/**
 * @file    theme_manager.h
 * @brief   昼夜主题管理
 *
 * 系统支持两种主题：
 *   - 日间主题（浅色）：白色背景、深色文字、天蓝色强调
 *   - 夜间主题（深色）：深蓝背景、浅色文字、紫蓝色强调
 *
 * 主题切换时，所有 UI 元素的颜色会自动更新。
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

#ifndef THEME_MANAGER_H
#define THEME_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 主题类型枚举
 */
typedef enum {
    THEME_DAY = 0,      /* 日间主题（浅色背景） */
    THEME_NIGHT = 1,    /* 夜间主题（深色背景） */
    THEME_COUNT         /* 主题总数（哨兵值） */
} theme_type_t;

/**
 * @brief 主题颜色集
 *
 * 每种主题定义 7 种颜色，覆盖 UI 中用到的所有场景。
 */
typedef struct {
    lv_color_t bg_color;        /* 页面背景色 */
    lv_color_t text_color;      /* 普通文本颜色 */
    lv_color_t primary_color;   /* 主题色（按钮、强调） */
    lv_color_t accent_color;    /* 强调色（特殊标记） */
    lv_color_t caption_bg;      /* 字幕栏背景色（半透明黑） */
    lv_color_t card_bg;         /* 卡片/面板背景色 */
    lv_color_t status_bar_bg;   /* 状态栏背景色 */
} theme_colors_t;

/**
 * @brief 初始化主题管理器
 *
 * @param[in] initial_theme  初始主题（THEME_DAY 或 THEME_NIGHT）
 * @return ESP_OK 成功
 */
esp_err_t theme_manager_init(theme_type_t initial_theme);

/**
 * @brief 切换到指定主题
 *
 * @param[in] theme  目标主题
 */
void theme_manager_set(theme_type_t theme);

/**
 * @brief 获取当前主题类型
 *
 * @return THEME_DAY 或 THEME_NIGHT
 */
theme_type_t theme_manager_get(void);

/**
 * @brief 获取当前主题的颜色集
 *
 * @return 颜色集指针（根据当前主题返回日间或夜间颜色）
 */
const theme_colors_t *theme_manager_get_colors(void);

/**
 * @brief 切换到另一个主题（昼↔夜互切）
 */
void theme_manager_toggle(void);

#ifdef __cplusplus
}
#endif

#endif /* THEME_MANAGER_H */
