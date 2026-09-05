/**
 * @file    scr_chat.h
 * @brief   Chat Screen（对话界面）接口
 *
 * 对话界面布局（1024×600 像素，R8 重设计，lvgl-simulator 模拟器验证）：
 *   ┌─────────────────────────────────┐
 *   │  状态栏 48px：[‹返回]  ● 监听中  │  ← 顶部
 *   ├─────────────────────────────────┤
 *   │                                 │
 *   │  Live2D 角色区 386px（角色常驻） │  ← 中间（flex 弹性占满）
 *   │                                 │
 *   ├─────────────────────────────────┤
 *   │  字幕区 110px（黑底白字，中文）   │  ← AI 回复
 *   ├─────────────────────────────────┤
 *   │  底栏 56px：波形示意  [■ 停止]   │  ← 底部
 *   └─────────────────────────────────┘
 *
 * @date    2026-09-01
 * @version 2.0.0  R8: 重设计布局 + CJK 字体 + 角色常驻（新增 get_live2d_area）
 */

#ifndef SCR_CHAT_H
#define SCR_CHAT_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 对话页 Live2D 区域高度 = 600 - 状态栏48 - 字幕110 - 底栏56（角色搬家用） */
#define SCR_CHAT_LIVE2D_FIT_H   (386)

/**
 * @brief 创建 Chat Screen
 *
 * @param[in] parent  父对象（通常是 LVGL 主屏幕）
 * @return 页面容器对象
 */
lv_obj_t *scr_chat_create(lv_obj_t *parent);

/**
 * @brief 获取 Live2D 角色区域对象
 *
 * 角色从主页"搬家"过来时，rig_lvgl_set_parent() 需要这个容器。
 *
 * @return 区域对象；scr_chat_create 未调用时返回 NULL
 */
lv_obj_t *scr_chat_get_live2d_area(void);

/**
 * @brief 设置对话字幕文本
 *
 * 在字幕区显示 AI 的回复文本（支持流式更新、自动换行）。
 *
 * @param[in] text  要显示的文本（NULL 则不更新）
 */
void scr_chat_set_subtitle(const char *text);

/**
 * @brief 设置对话状态指示
 *
 * 状态栏中央显示"圆点 + 状态文字"（如 ●监听中 / ●思考中 / ●说话中）。
 *
 * @param[in] text  状态文本（NULL 则不更新文字）
 * @param[in] color 状态圆点颜色
 */
void scr_chat_set_state(const char *text, lv_color_t color);

#ifdef __cplusplus
}
#endif

#endif /* SCR_CHAT_H */
