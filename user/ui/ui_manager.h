/**
 * @file    ui_manager.h
 * @brief   LVGL UI 管理器
 *
 * UI 管理器负责：
 *   1. 创建和管理所有页面（Home、Chat 等）
 *   2. 处理页面之间的切换
 *   3. 更新状态栏信息（Wi-Fi 状态、时间）
 *
 * 页面切换由状态机驱动：当状态变化时，自动切换到对应的页面。
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 页面 ID 枚举
 *
 * 每个页面对应系统的一个功能界面。
 */
typedef enum {
    UI_PAGE_HOME = 0,   /* 主页：显示角色待机画面 + 状态栏 + 字幕 */
    UI_PAGE_CHAT,       /* 对话页：显示对话字幕 + 语音波形 + 停止按钮 */
    UI_PAGE_MUSIC,      /* 音乐页：（Phase 4 实现）*/
    UI_PAGE_POMODORO,   /* 番茄钟页：（Phase 5 实现）*/
    UI_PAGE_DIARY,      /* 日记页：（Phase 4 实现）*/
    UI_PAGE_SETTINGS,   /* 设置页：（Phase 5 实现）*/
    UI_PAGE_COUNT       /* 页面总数（哨兵值） */
} ui_page_id_t;

/**
 * @brief 初始化 UI 管理器
 *
 * 完成以下工作：
 *   1. 初始化 LVGL 主题（默认日间主题）
 *   2. 创建 LVGL 主屏幕
 *   3. 创建 Home 和 Chat 页面
 *   4. 默认显示 Home 页面
 *   5. 注册状态变化回调（状态机切换时自动切换页面）
 *
 * @return ESP_OK 成功
 */
esp_err_t ui_manager_init(void);

/**
 * @brief 导航到指定页面
 *
 * 隐藏当前页面，显示目标页面。
 *
 * @param[in] page_id  目标页面 ID
 */
void ui_manager_navigate(ui_page_id_t page_id);

/**
 * @brief 获取当前页面 ID
 *
 * @return 当前显示的页面 ID
 */
ui_page_id_t ui_manager_get_current_page(void);

/**
 * @brief 更新状态栏信息
 *
 * 更新顶部状态栏的 Wi-Fi 图标和时间显示。
 *
 * @param[in] wifi_connected  Wi-Fi 是否已连接
 * @param[in] time_str        时间字符串（如 "14:30"）
 */
void ui_manager_update_status_bar(bool wifi_connected, const char *time_str);

/**
 * @brief 显示字幕文本
 *
 * 在 Chat 页面的字幕区显示文本。
 *
 * @param[in] text  要显示的文本内容
 */
void ui_manager_set_subtitle(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* UI_MANAGER_H */
