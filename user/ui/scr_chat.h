/**
 * @file    scr_chat.h
 * @brief   Chat Screen（对话界面）接口
 *
 * 对话界面布局（1024×600 像素）：
 *   ┌─────────────────────────────────┐
 *   │  状态栏（40px）状态 + 返回按钮   │  ← 顶部
 *   ├─────────────────────────────────┤
 *   │     Live2D 角色区域（占位）      │  ← 上半部分
 *   ├─────────────────────────────────┤
 *   │  字幕区（120px）对话文本         │  ← 下半部分
 *   ├─────────────────────────────────┤
 *   │  底部栏（60px）波形 + 停止按钮   │  ← 底部
 *   └─────────────────────────────────┘
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

#ifndef SCR_CHAT_H
#define SCR_CHAT_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建 Chat Screen
 *
 * @param[in] parent  父对象（通常是 LVGL 主屏幕）
 * @return 页面容器对象
 */
lv_obj_t *scr_chat_create(lv_obj_t *parent);

/**
 * @brief 设置对话字幕文本
 *
 * 在字幕区显示 AI 的回复文本（支持流式更新）。
 *
 * @param[in] text  要显示的文本
 */
void scr_chat_set_subtitle(const char *text);

/**
 * @brief 设置对话状态指示
 *
 * 在状态栏显示当前对话状态（如 "Listening..."、"Thinking..."）。
 *
 * @param[in] state_text  状态文本
 */
void scr_chat_set_state(const char *state_text);

#ifdef __cplusplus
}
#endif

#endif /* SCR_CHAT_H */
