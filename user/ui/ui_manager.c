/**
 * @file    ui_manager.c
 * @brief   UI 管理器实现
 *
 * LVGL 图形库由 BSP 内部初始化和驱动（通过 espp::Display 类）。
 * 本模块只负责创建页面和管理页面切换。
 *
 * 页面切换逻辑：
 *   状态机 STATE_IDLE    → 显示 Home 页面
 *   状态机 STATE_LISTENING/THINKING/SPEAKING → 显示 Chat 页面
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

/* 1. 自身公开头 */
#include "ui_manager.h"

/* 2. C 标准库 */
#include <string.h>

/* 3. 项目级 */
#include "scr_home.h"            /* Home 页面 */
#include "scr_chat.h"            /* Chat 页面 */
#include "theme_manager.h"       /* 主题管理 */
#include "app_state_machine.h"   /* 状态机 */
#include "event_bus.h"           /* 事件总线 */

/* 4. 平台/厂商头 */
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "ui_mgr";

/**
 * @brief 页面信息结构体
 *
 * 记录每个页面的 LVGL 容器对象和状态。
 */
typedef struct {
    lv_obj_t *container;    /* 页面的 LVGL 容器对象（所有 UI 元素都放在里面） */
    ui_page_id_t page_id;   /* 页面 ID */
    bool is_created;        /* 页面是否已创建 */
} ui_page_t;

/* 页面数组：最多支持 UI_PAGE_COUNT 个页面 */
static ui_page_t s_pages[UI_PAGE_COUNT];

/* 当前正在显示的页面 ID */
/* No page is visible until ui_manager_init() explicitly navigates to Home. */
static ui_page_id_t s_current_page = UI_PAGE_COUNT;

/* LVGL 主屏幕对象（所有页面的父容器） */
static lv_obj_t *s_scr_main = NULL;

/**
 * @brief 状态变化回调：当状态机状态改变时，自动切换页面
 *
 * 例如：从 IDLE 切换到 LISTENING 时，自动从 Home 页面切换到 Chat 页面。
 */
static void on_state_change(app_state_t old_state, app_state_t new_state,
                            void *ctx)
{
    (void)ctx;       /* 未使用 */
    (void)old_state; /* 未使用 */

    /* 根据新状态决定显示哪个页面 */
    switch (new_state) {
        case STATE_IDLE:
            ui_manager_navigate(UI_PAGE_HOME);   /* 空闲 → 显示主页 */
            break;
        case STATE_LISTENING:
        case STATE_THINKING:
        case STATE_SPEAKING:
            ui_manager_navigate(UI_PAGE_CHAT);   /* 对话中 → 显示对话页 */
            break;
        default:
            break;  /* 其他状态不切换页面 */
    }
}

/**
 * @brief 初始化 UI 管理器
 *
 * 创建 LVGL 主屏幕和所有页面，注册状态变化回调。
 */
esp_err_t ui_manager_init(void)
{
    /* Use an opaque theme background so the first frame is visible. */
    theme_manager_init(THEME_DAY);
    ESP_LOGI(TAG, "正在初始化 UI 管理器...");

    /* 初始化主题（默认使用日间主题） */
    theme_manager_init(THEME_DAY);

    /* 创建 LVGL 主屏幕（所有页面都放在这个屏幕上） */
    s_scr_main = lv_obj_create(NULL);                    /* 创建空屏幕 */
    lv_obj_set_style_bg_opa(s_scr_main, LV_OPA_TRANSP, 0); /* 背景透明 */
    lv_obj_set_flex_flow(s_scr_main, LV_FLEX_FLOW_COLUMN); /* 纵向排列子对象 */
    lv_obj_set_style_pad_all(s_scr_main, 0, 0);         /* 无内边距 */
    lv_screen_load(s_scr_main);                          /* 加载到屏幕 */

    /* 初始化所有页面 */
    memset(s_pages, 0, sizeof(s_pages));  /* 清空页面数组 */

    /* 创建 Home 页面（主页：状态栏 + Live2D 占位 + 字幕栏） */
    s_pages[UI_PAGE_HOME].container = scr_home_create(s_scr_main);
    s_pages[UI_PAGE_HOME].page_id = UI_PAGE_HOME;
    s_pages[UI_PAGE_HOME].is_created = true;

    /* 创建 Chat 页面（对话页：字幕 + 波形 + 停止按钮） */
    s_pages[UI_PAGE_CHAT].container = scr_chat_create(s_scr_main);
    s_pages[UI_PAGE_CHAT].page_id = UI_PAGE_CHAT;
    s_pages[UI_PAGE_CHAT].is_created = true;

    /* 默认显示首页 */
    ui_manager_navigate(UI_PAGE_HOME);

    /* 注册状态变化回调：状态机切换时自动切换页面 */
    app_state_machine_on_change(on_state_change, NULL);

    ESP_LOGI(TAG, "UI 管理器初始化完成");
    lv_obj_set_style_bg_color(s_scr_main, theme_manager_get_colors()->bg_color, 0);
    lv_obj_set_style_bg_opa(s_scr_main, LV_OPA_COVER, 0);
    return ESP_OK;
}

/**
 * @brief 导航到指定页面
 *
 * 实现方式：隐藏当前页面，显示目标页面。
 * LVGL 的"隐藏"是通过 LV_OBJ_FLAG_HIDDEN 标志实现的。
 */
void ui_manager_navigate(ui_page_id_t page_id)
{
    /* 参数检查：页面 ID 有效，且不是当前页面 */
    if (page_id >= UI_PAGE_COUNT || page_id == s_current_page) {
        return;
    }

    /* 隐藏当前页面（如果存在） */
    if (s_current_page < UI_PAGE_COUNT &&
        s_pages[s_current_page].container != NULL) {
        lv_obj_add_flag(s_pages[s_current_page].container, LV_OBJ_FLAG_HIDDEN);
    }

    /* 显示目标页面（如果存在） */
    if (s_pages[page_id].container != NULL) {
        lv_obj_clear_flag(s_pages[page_id].container, LV_OBJ_FLAG_HIDDEN);
    }

    /* 更新当前页面记录 */
    s_current_page = page_id;
    ESP_LOGD(TAG, "页面切换到: %d", page_id);
}

ui_page_id_t ui_manager_get_current_page(void)
{
    return s_current_page;
}

void ui_manager_update_status_bar(bool wifi_connected, const char *time_str)
{
    /* 转发给 Home 页面的状态栏更新函数 */
    scr_home_update_status_bar(wifi_connected, time_str);
}

void ui_manager_set_subtitle(const char *text)
{
    /* 转发给 Chat 页面的字幕更新函数 */
    scr_chat_set_subtitle(text);
}
