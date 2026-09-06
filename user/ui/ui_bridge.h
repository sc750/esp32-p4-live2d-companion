/**
 * @file    ui_bridge.h
 * @brief   UI Bridge（L5 桥）——任意任务安全的主页 UI 更新入口
 *
 * 为什么存在：rig_chatter（L4，主循环上下文）要更新主页字幕（LVGL 对象，
 * adapter 锁保护），但 L4 不许直接碰 LVGL/锁——由本桥封装"拿锁+调 UI"，
 * 对齐 PRD 的 UI Bridge 层设想（M02 §7）。Phase 3 语音链路的字幕/波形
 * 更新也走这里。
 *
 * 线程模型：可在任意任务上下文调用（内部自取 adapter 递归锁）；
 * 文本指针须在 LVGL 渲染期间持续有效——传 static const 或调用方自管生命周期。
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#ifndef UI_BRIDGE_H
#define UI_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 更新主页字幕（任意任务安全；文本须为常量或调用方保证生命周期） */
void ui_bridge_set_subtitle(const char *text);

/** 更新 Wi-Fi 状态显示（R12：滑块+三态文案；state 取 scr_wifi_state_t 值） */
void ui_bridge_set_wifi_state(int state);

/** 更新状态栏时间（"HH:MM"；synced=false 断线漂移中 → 文字变灰） */
void ui_bridge_set_time(const char *time_str, bool synced);

#ifdef __cplusplus
}
#endif

#endif /* UI_BRIDGE_H */
