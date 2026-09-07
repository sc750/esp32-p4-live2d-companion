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

#include <stdbool.h>
#include "lvgl.h"
#include "app_events.h"   /* dialog_state_t：UI 与语音管线共享词汇 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Wi-Fi 状态（UI 视角三态，R12 状态栏开关按钮用）
 * 与 bsp_wifi_state_t 的映射由编排层（main_app）完成，UI 不直接依赖 BSP。
 */
typedef enum {
    SCR_WIFI_DISCONNECTED = 0,  /* 未连接：文案黄色 */
    SCR_WIFI_CONNECTING,        /* 正在连接中：文案绿色 */
    SCR_WIFI_CONNECTED,         /* WiFi 已连接：文案绿色 */
} scr_wifi_state_t;

/**
 * @brief 创建 Home Screen
 *
 * 创建所有 UI 元素（状态栏、Live2D 角色区、字幕栏），
 * 但页面默认是隐藏的，需要调用 ui_manager_navigate() 才会显示。
 *
 * @param[in] parent  父对象（通常是 LVGL 主屏幕）
 * @return 页面容器对象
 */
lv_obj_t *scr_home_create(lv_obj_t *parent);

/**
 * @brief 注册按住说话回调（M1；编排层注入，UI 不碰音频）
 *
 * 用户按下麦克风按钮 → cb(true)；松开/滑出 → cb(false)。
 *
 * @param[in] cb   回调（NULL=注销）
 * @param[in] ctx  回调上下文
 */
void scr_home_set_voice_hold_cb(void (*cb)(bool holding, void *ctx), void *ctx);

/**
 * @brief 获取 Live2D 角色区域对象（角色渲染容器，M03 起启用）
 * @return 区域对象；scr_home_create 未调用时返回 NULL
 */
lv_obj_t *scr_home_get_live2d_area(void);

/**
 * @brief 更新 Wi-Fi 状态显示（R12：滑块开关 + 三态文案）
 *
 * 滑块位置随状态同步：CONNECTING/CONNECTED 在右侧，DISCONNECTED 在左。
 *
 * @param[in] state  Wi-Fi 状态
 */
void scr_home_set_wifi_state(scr_wifi_state_t state);

/**
 * @brief 更新对话状态点（颜色 + 显隐）
 * @param[in] state  对话状态
 */
void scr_home_set_dialog_state(dialog_state_t state);

/**
 * @brief 更新状态栏时间显示（"HH:MM"；未同步时由调用方传 "--:--"）
 *
 * R12：synced=false（NTP 断流，晶振续走的"非权威时间"）时文字变灰，
 * 提示仅供参考；重连校时成功后恢复正常色。
 *
 * @param[in] time_str  时间字符串，NULL 则不更新
 * @param[in] synced    true=NTP 已校准；false=断线漂移中
 */
void scr_home_set_time(const char *time_str, bool synced);

/**
 * @brief 注册 Wi-Fi 开关切捔回调（编排层注入，UI 不直接碰 BSP）
 *
 * 用户拨动状态栏滑块时调用：on=true 请求连接，false 请求断开。
 *
 * @param[in] cb   回调（NULL=注销）
 * @param[in] ctx  回调上下文
 */
void scr_home_set_wifi_toggle_cb(void (*cb)(bool turn_on, void *ctx), void *ctx);

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
