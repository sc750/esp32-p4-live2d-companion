/**
 * @file    scr_chat.c
 * @brief   Chat Screen 实现
 *
 * 对话界面用于 AI 语音对话流程：
 *   - 状态栏显示当前状态（监听中/思考中/说话中）+ 返回按钮
 *   - 中间区域显示 Live2D 角色（Phase 2 实现）
 *   - 字幕区显示对话文本（AI 的回复会逐字出现）
 *   - 底部有语音波形指示器（占位）和停止按钮
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

/* 1. 自身公开头 */
#include "scr_chat.h"

/* 2. C 标准库 */
#include <string.h>

/* 3. 项目级 */
#include "theme_manager.h"
#include "app_events.h"
#include "app_state_machine.h"
#include "event_bus.h"

/* 4. 平台/厂商头 */
#include "esp_log.h"

static const char *TAG = "scr_chat";

/* 屏幕尺寸常量 */
#define SCR_WIDTH       1024
#define SCR_HEIGHT      600
#define STATUS_BAR_H    40
#define SUBTITLE_H      120   /* 字幕区高度 */
#define BOTTOM_BAR_H    60

/**
 * @brief Chat 页面的所有 UI 对象
 */
typedef struct {
    lv_obj_t *container;        /* 页面容器 */
    lv_obj_t *status_bar;       /* 顶部状态栏 */
    lv_obj_t *state_label;      /* 状态指示文本（如 "Listening..."） */
    lv_obj_t *back_btn;         /* 返回按钮 */
    lv_obj_t *live2d_area;      /* Live2D 角色区域（占位） */
    lv_obj_t *subtitle_area;    /* 字幕区域 */
    lv_obj_t *subtitle_label;   /* 字幕文本标签 */
    lv_obj_t *bottom_bar;       /* 底部栏 */
    lv_obj_t *waveform_area;    /* 波形指示器（占位） */
    lv_obj_t *stop_btn;         /* 停止按钮 */
} chat_ui_t;

static chat_ui_t s_chat_ui;

/* 前向声明 */
static void on_back_clicked(lv_event_t *e);
static void on_stop_clicked(lv_event_t *e);

/**
 * @brief 创建状态栏（含状态文本 + 返回按钮）
 */
static void create_status_bar(lv_obj_t *parent)
{
    s_chat_ui.status_bar = lv_obj_create(parent);
    lv_obj_set_size(s_chat_ui.status_bar, SCR_WIDTH, STATUS_BAR_H);
    lv_obj_set_flex_flow(s_chat_ui.status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_chat_ui.status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const theme_colors_t *colors = theme_manager_get_colors();
    lv_obj_set_style_bg_color(s_chat_ui.status_bar, colors->status_bar_bg, 0);
    lv_obj_set_style_bg_opa(s_chat_ui.status_bar, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_chat_ui.status_bar, 0, 0);
    lv_obj_set_style_radius(s_chat_ui.status_bar, 0, 0);
    lv_obj_set_style_pad_hor(s_chat_ui.status_bar, 16, 0);

    /* 状态指示文本（显示 "Listening..." / "Thinking..." / "Speaking..."） */
    s_chat_ui.state_label = lv_label_create(s_chat_ui.status_bar);
    lv_label_set_text(s_chat_ui.state_label, "[Audio] Listening...");
    lv_obj_set_style_text_color(s_chat_ui.state_label, colors->primary_color, 0);

    /* 返回按钮（点击后返回主页） */
    s_chat_ui.back_btn = lv_btn_create(s_chat_ui.status_bar);
    lv_obj_set_size(s_chat_ui.back_btn, 60, 30);
    lv_obj_set_style_bg_color(s_chat_ui.back_btn, colors->primary_color, 0);
    lv_obj_set_style_radius(s_chat_ui.back_btn, 15, 0);
    lv_obj_add_event_cb(s_chat_ui.back_btn, on_back_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(s_chat_ui.back_btn);
    lv_label_set_text(btn_label, "< Back");
    lv_obj_set_style_text_color(btn_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_14, 0);
    lv_obj_center(btn_label);  /* 文字居中 */
}

/**
 * @brief 创建 Live2D 角色区域（Phase 2 前为占位符）
 */
static void create_live2d_area(lv_obj_t *parent)
{
    int live2d_h = SCR_HEIGHT - STATUS_BAR_H - SUBTITLE_H - BOTTOM_BAR_H;

    s_chat_ui.live2d_area = lv_obj_create(parent);
    lv_obj_set_size(s_chat_ui.live2d_area, SCR_WIDTH, live2d_h / 2);
    lv_obj_set_style_bg_opa(s_chat_ui.live2d_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chat_ui.live2d_area, 0, 0);
    lv_obj_set_style_radius(s_chat_ui.live2d_area, 0, 0);
    lv_obj_set_flex_flow(s_chat_ui.live2d_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_chat_ui.live2d_area, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 占位提示 */
    lv_obj_t *placeholder = lv_label_create(s_chat_ui.live2d_area);
    lv_label_set_text(placeholder, "[IMG]\nLive2D Character (Phase 2)");
    lv_obj_set_style_text_color(placeholder, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_align(placeholder, LV_TEXT_ALIGN_CENTER, 0);
}

/**
 * @brief 创建字幕区域
 */
static void create_subtitle_area(lv_obj_t *parent)
{
    s_chat_ui.subtitle_area = lv_obj_create(parent);
    lv_obj_set_size(s_chat_ui.subtitle_area, SCR_WIDTH, SUBTITLE_H);

    const theme_colors_t *colors = theme_manager_get_colors();
    lv_obj_set_style_bg_color(s_chat_ui.subtitle_area, colors->caption_bg, 0);
    lv_obj_set_style_bg_opa(s_chat_ui.subtitle_area, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_chat_ui.subtitle_area, 0, 0);
    lv_obj_set_style_radius(s_chat_ui.subtitle_area, 0, 0);
    lv_obj_set_style_pad_all(s_chat_ui.subtitle_area, 20, 0);

    /* 字幕文本（AI 的回复会逐字更新到这里） */
    s_chat_ui.subtitle_label = lv_label_create(s_chat_ui.subtitle_area);
    lv_label_set_text(s_chat_ui.subtitle_label, "等待语音输入...");
    lv_obj_set_style_text_color(s_chat_ui.subtitle_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_chat_ui.subtitle_label, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(s_chat_ui.subtitle_label, LV_LABEL_LONG_WRAP);  /* 自动换行 */
    lv_obj_set_width(s_chat_ui.subtitle_label, SCR_WIDTH - 40);
}

/**
 * @brief 创建底部栏（波形指示器 + 停止按钮）
 */
static void create_bottom_bar(lv_obj_t *parent)
{
    s_chat_ui.bottom_bar = lv_obj_create(parent);
    lv_obj_set_size(s_chat_ui.bottom_bar, SCR_WIDTH, BOTTOM_BAR_H);

    const theme_colors_t *colors = theme_manager_get_colors();
    lv_obj_set_style_bg_color(s_chat_ui.bottom_bar, colors->bg_color, 0);
    lv_obj_set_style_border_width(s_chat_ui.bottom_bar, 0, 0);
    lv_obj_set_style_radius(s_chat_ui.bottom_bar, 0, 0);
    lv_obj_set_flex_flow(s_chat_ui.bottom_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_chat_ui.bottom_bar, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_chat_ui.bottom_bar, 20, 0);

    /* 波形指示器占位（Phase 3 实现实际波形动画） */
    s_chat_ui.waveform_area = lv_obj_create(s_chat_ui.bottom_bar);
    lv_obj_set_size(s_chat_ui.waveform_area, 300, 40);
    lv_obj_set_style_bg_color(s_chat_ui.waveform_area, colors->card_bg, 0);
    lv_obj_set_style_bg_opa(s_chat_ui.waveform_area, LV_OPA_50, 0);
    lv_obj_set_style_radius(s_chat_ui.waveform_area, 8, 0);
    lv_obj_set_style_border_width(s_chat_ui.waveform_area, 0, 0);

    lv_obj_t *wave_label = lv_label_create(s_chat_ui.waveform_area);
    lv_label_set_text(wave_label, "[Audio] Waveform");
    lv_obj_set_style_text_color(wave_label, colors->text_color, 0);
    lv_obj_center(wave_label);

    /* 停止按钮（红色，用于打断 AI 说话） */
    s_chat_ui.stop_btn = lv_btn_create(s_chat_ui.bottom_bar);
    lv_obj_set_size(s_chat_ui.stop_btn, 120, 40);
    lv_obj_set_style_bg_color(s_chat_ui.stop_btn, lv_color_hex(0xE74C3C), 0);  /* 红色 */
    lv_obj_set_style_radius(s_chat_ui.stop_btn, 8, 0);
    lv_obj_add_event_cb(s_chat_ui.stop_btn, on_stop_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *stop_label = lv_label_create(s_chat_ui.stop_btn);
    lv_label_set_text(stop_label, "[Stop] Stop");
    lv_obj_set_style_text_color(stop_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(stop_label, &lv_font_montserrat_14, 0);
    lv_obj_center(stop_label);
}

/**
 * @brief 返回按钮回调：发送 NAV_HOME 事件，回到主页
 */
static void on_back_clicked(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "点击返回按钮，回到主页");
    app_state_machine_send_event(EVENT_NAV_HOME, NULL);
}

/**
 * @brief 停止按钮回调：打断当前对话，回到空闲状态
 */
static void on_stop_clicked(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "点击停止按钮，打断对话");
    app_state_machine_send_event(EVENT_SCREEN_TAP, NULL);
}

/**
 * @brief 创建 Chat 页面
 */
lv_obj_t *scr_chat_create(lv_obj_t *parent)
{
    /* 创建页面容器（默认隐藏） */
    s_chat_ui.container = lv_obj_create(parent);
    lv_obj_set_size(s_chat_ui.container, SCR_WIDTH, SCR_HEIGHT);
    lv_obj_set_style_bg_opa(s_chat_ui.container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chat_ui.container, 0, 0);
    lv_obj_set_style_radius(s_chat_ui.container, 0, 0);
    lv_obj_set_style_pad_all(s_chat_ui.container, 0, 0);
    lv_obj_set_flex_flow(s_chat_ui.container, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_chat_ui.container, LV_OBJ_FLAG_HIDDEN);  /* 默认隐藏 */

    /* 按从上到下的顺序创建子元素 */
    create_status_bar(s_chat_ui.container);       /* 1. 状态栏 */
    create_live2d_area(s_chat_ui.container);       /* 2. Live2D 区域 */
    create_subtitle_area(s_chat_ui.container);     /* 3. 字幕区域 */
    create_bottom_bar(s_chat_ui.container);        /* 4. 底部栏 */

    ESP_LOGI(TAG, "Chat 页面创建完成");
    return s_chat_ui.container;
}

void scr_chat_set_subtitle(const char *text)
{
    if (s_chat_ui.subtitle_label != NULL && text != NULL) {
        lv_label_set_text(s_chat_ui.subtitle_label, text);
    }
}

void scr_chat_set_state(const char *state_text)
{
    if (s_chat_ui.state_label != NULL && state_text != NULL) {
        lv_label_set_text(s_chat_ui.state_label, state_text);
    }
}
