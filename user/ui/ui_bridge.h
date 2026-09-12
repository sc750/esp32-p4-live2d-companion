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

/** 更新对话状态点（state 取 dialog_state_t 值，任意任务安全） */
void ui_bridge_set_dialog_state(int state);

/** 更新 Wi-Fi 状态显示（R12：滑块+三态文案；state 取 scr_wifi_state_t 值） */
void ui_bridge_set_wifi_state(int state);

/** 更新状态栏时间（"HH:MM"；synced=false 断线漂移中 → 文字变灰） */
void ui_bridge_set_time(const char *time_str, bool synced);

/**
 * @brief 切换到指定页面（page_id 取 ui_page_id_t 值，任意任务安全）
 *
 * 存在的理由：ui_manager_navigate() 自己不持 adapter 锁（直接改 LVGL 的
 * HIDDEN 标志），从主循环任务直接调会与 LVGL 渲染任务竞争 → 由本桥补锁。
 */
void ui_bridge_navigate(int page_id);

/**
 * @brief 刷新音乐页播放状态（任意任务安全）
 *
 * 内部自带"值未变不绘制"与"页面隐藏直接返回"两道闸门，
 * 可放心按秒节拍调用。
 */
void ui_bridge_set_music_state(bool playing, bool paused, int cur_idx, int pos_sec);

/** 同步音乐页音量滑块位置（进页面时调一次；改值不发控制命令） */
void ui_bridge_set_music_volume(int vol);

/**
 * @brief 用曲名数组建/重建音乐页播放列表（任意任务安全）
 *
 * 存在的理由：set_playlist 内部会建/删 LVGL 对象，必须持 adapter 锁——
 * 编排层不许绕过本桥直接调 scr_music_set_playlist。
 *
 * @param[in] names  曲名指针数组（元素须指向常驻内存，页面不拷贝）
 * @param[in] count  曲目数（0 或 names=NULL 表示清空列表）
 */
void ui_bridge_set_music_playlist(const char *const *names, int count);

#ifdef __cplusplus
}
#endif

#endif /* UI_BRIDGE_H */
