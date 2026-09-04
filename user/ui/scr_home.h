/**
 * @file    scr_home.h
 * @brief   Home Screen（主界面）接口
 *
 * 主界面布局（1024×600 像素）：
 *   ┌─────────────────────────────────┐
 *   │  状态栏（40px）Wi-Fi图标 + 时间  │  ← 顶部
 *   ├─────────────────────────────────┤
 *   │                                 │
 *   │     Live2D 角色区域（占位）      │  ← 中间（Phase 2 实现）
 *   │     点击此区域开始对话          │
 *   │                                 │
 *   ├─────────────────────────────────┤
 *   │  字幕栏（80px）显示对话文本      │  ← 底部
 *   └─────────────────────────────────┘
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

#ifndef SCR_HOME_H
#define SCR_HOME_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建 Home Screen
 *
 * 创建所有 UI 元素（状态栏、Live2D 占位区、字幕栏），
 * 但页面默认是隐藏的，需要调用 ui_manager_navigate() 才会显示。
 *
 * @param[in] parent  父对象（通常是 LVGL 主屏幕）
 * @return 页面容器对象
 */
lv_obj_t *scr_home_create(lv_obj_t *parent);

/**
 * @brief 获取 Live2D 角色区域对象（角色渲染容器，M03 起启用）
 * @return 区域对象；scr_home_create 未调用时返回 NULL
 */
lv_obj_t *scr_home_get_live2d_area(void);

/**
 * @brief 更新状态栏
 *
 * 更新顶部状态栏的 Wi-Fi 图标和时间显示。
 *
 * @param[in] wifi_connected  true=显示已连接，false=显示未连接
 * @param[in] time_str        时间字符串（如 "14:30"），NULL 则不更新
 */
void scr_home_update_status_bar(bool wifi_connected, const char *time_str);

/**
 * @brief 更新底部字幕
 *
 * @param[in] text  要显示的文本（如 "你好！点击角色开始聊天 ~"）
 */
void scr_home_set_subtitle(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* SCR_HOME_H */
