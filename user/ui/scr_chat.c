/**
 * @file    scr_chat.c
 * @brief   Chat Screen 实现
 *
 * 对话界面（R8 重设计，lvgl-simulator 模拟器验证布局）：
 *   - 状态栏 48px：[‹返回] 按钮 + 居中"圆点+状态文字"指示
 *   - Live2D 角色区 386px：角色从主页搬过来常驻（rig_lvgl_set_parent）
 *   - 字幕区 110px：黑底白字，nino_cjk_16 中文自动换行
 *   - 底部栏 56px：五根波形示意条（Phase 3 接真波形）+ [■ 停止] 按钮
 *
 * 设计要点（踩坑记录）：
 *   - lv_obj 默认主题自带 pad=20，固定高度栏必须显式 pad_all(0)，
 *     否则内容被挤出栏外（R8 前状态栏文字上溢的根因）
 *   - LVGL 9.4 没有 lv_obj_set_style_pad_gap（9.5 新增），
 *     列方向 flex 间隙要用 pad_row、行方向用 pad_column
 *   - Montserrat 字体无 CJK 字形，中文一律用 nino_cjk_16（自定义字库）
 *
 * @date    2026-09-01
 * @version 2.0.0
 */

/* 1. 自身公开头 */
#include "scr_chat.h"

/* 2. C 标准库 */
#include <string.h>

/* 3. 项目级 */
#include "theme_manager.h"
#include "app_events.h"
#include "app_state_machine.h"
#include "nino_font.h"          /* 自定义中文字体 nino_cjk_16 */

/* 4. 平台/厂商头 */
#include "esp_log.h"

static const char *TAG = "scr_chat";

/* 布局常量（总和必须 = 600，角色区用 flex_grow 弹性吃掉剩余高度） */
#define SCR_WIDTH       1024
#define SCR_HEIGHT      600
#define STATUS_BAR_H    48      /* 顶部状态栏 */
#define SUBTITLE_H      110     /* 字幕区 */
#define BOTTOM_BAR_H    56      /* 底部栏 */

/**
 * @brief Chat 页面的所有 UI 对象
 */
typedef struct {
    lv_obj_t *container;        /* 页面容器 */
    lv_obj_t *status_bar;       /* 顶部状态栏 */
    lv_obj_t *back_btn;         /* 返回按钮 */
    lv_obj_t *state_dot;        /* 状态圆点（颜色随对话阶段变） */
    lv_obj_t *state_label;      /* 状态指示文本（监听中/思考中/说话中） */
    lv_obj_t *live2d_area;      /* Live2D 角色区域（角色常驻，flex_grow 占满） */
    lv_obj_t *subtitle_area;    /* 字幕区域 */
    lv_obj_t *subtitle_label;   /* 字幕文本标签 */
    lv_obj_t *bottom_bar;       /* 底部栏 */
    lv_obj_t *waveform_area;    /* 波形示意条容器（Phase 3 接真波形） */
    lv_obj_t *stop_btn;         /* 停止按钮 */
} chat_ui_t;

static chat_ui_t s_chat_ui;

/* 前向声明 */
static void on_back_clicked(lv_event_t *e);
static void on_stop_clicked(lv_event_t *e);

/**
 * @brief 创建状态栏（48px：返回按钮 | 圆点+状态文字 | 平衡垫）
 *
 * 三段式 SPACE_BETWEEN：左右两个 76px 的等宽元素把中间状态挤到真居中。
 * 必须显式 pad_all(0)：lv_obj 默认主题 pad=20，会把 48px 高的栏撑爆。
 */
static void create_status_bar(lv_obj_t *parent)
{
    const theme_colors_t *colors = theme_manager_get_colors();

    s_chat_ui.status_bar = lv_obj_create(parent);
    lv_obj_set_size(s_chat_ui.status_bar, SCR_WIDTH, STATUS_BAR_H);
    lv_obj_set_style_bg_color(s_chat_ui.status_bar, colors->status_bar_bg, 0);
    lv_obj_set_style_bg_opa(s_chat_ui.status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_chat_ui.status_bar, 0, 0);
    lv_obj_set_style_radius(s_chat_ui.status_bar, 0, 0);
    lv_obj_set_style_pad_all(s_chat_ui.status_bar, 0, 0);       /* 干掉默认 pad=20 */
    lv_obj_set_style_pad_hor(s_chat_ui.status_bar, 16, 0);      /* 只留左右 16px */
    lv_obj_set_flex_flow(s_chat_ui.status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_chat_ui.status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_chat_ui.status_bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 返回按钮：左箭头（Montserrat 图标字形）+ "返回"（CJK 字形）
     * 两个 label 并排——CJK 字库里没有 FontAwesome 箭头，混在一个 label 会缺字 */
    s_chat_ui.back_btn = lv_btn_create(s_chat_ui.status_bar);
    lv_obj_set_size(s_chat_ui.back_btn, 76, 32);
    lv_obj_set_style_bg_color(s_chat_ui.back_btn, colors->primary_color, 0);
    lv_obj_set_style_radius(s_chat_ui.back_btn, 16, 0);
    lv_obj_set_style_pad_all(s_chat_ui.back_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_chat_ui.back_btn, 0, 0);    /* 关按钮默认阴影 */
    lv_obj_add_event_cb(s_chat_ui.back_btn, on_back_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_sym = lv_label_create(s_chat_ui.back_btn);
    lv_label_set_text(back_sym, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_sym, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(back_sym, &lv_font_montserrat_14, 0);
    lv_obj_align(back_sym, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *back_txt = lv_label_create(s_chat_ui.back_btn);
    lv_label_set_text(back_txt, "返回");
    lv_obj_set_style_text_color(back_txt, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(back_txt, nino_font_cjk16(), 0);
    lv_obj_align(back_txt, LV_ALIGN_RIGHT_MID, -12, 0);

    /* 状态指示：小圆点 + 状态文字，水平排列居中 */
    lv_obj_t *state_row = lv_obj_create(s_chat_ui.status_bar);
    lv_obj_set_size(state_row, 120, 32);
    lv_obj_set_style_bg_opa(state_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state_row, 0, 0);
    lv_obj_set_style_pad_all(state_row, 0, 0);
    lv_obj_set_flex_flow(state_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(state_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(state_row, 8, 0);   /* 点和文字间距 8px */
    lv_obj_clear_flag(state_row, LV_OBJ_FLAG_SCROLLABLE);

    s_chat_ui.state_dot = lv_obj_create(state_row);
    lv_obj_set_size(s_chat_ui.state_dot, 10, 10);
    lv_obj_set_style_radius(s_chat_ui.state_dot, 5, 0);
    lv_obj_set_style_bg_color(s_chat_ui.state_dot, colors->primary_color, 0);
    lv_obj_set_style_border_width(s_chat_ui.state_dot, 0, 0);
    lv_obj_clear_flag(s_chat_ui.state_dot, LV_OBJ_FLAG_SCROLLABLE);

    s_chat_ui.state_label = lv_label_create(state_row);
    lv_label_set_text(s_chat_ui.state_label, "监听中");
    lv_obj_set_style_text_color(s_chat_ui.state_label, colors->primary_color, 0);
    lv_obj_set_style_text_font(s_chat_ui.state_label, nino_font_cjk16(), 0);

    /* 平衡垫：与返回按钮同宽，让 SPACE_BETWEEN 下的中间元素真居中 */
    lv_obj_t *pad = lv_obj_create(s_chat_ui.status_bar);
    lv_obj_set_size(pad, 76, 1);
    lv_obj_set_style_bg_opa(pad, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pad, 0, 0);
    lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
}

/**
 * @brief 创建 Live2D 角色区域（386px，flex_grow 弹性占满剩余高度）
 *
 * 角色本体由 rig_lvgl 模块管理：进对话页时 rig_lvgl_set_parent() 把
 * 角色 image 搬进这个容器，回主页再搬回去——全程无闪烁。
 */
static void create_live2d_area(lv_obj_t *parent)
{
    s_chat_ui.live2d_area = lv_obj_create(parent);
    lv_obj_set_size(s_chat_ui.live2d_area, SCR_WIDTH, 100);
    lv_obj_set_flex_grow(s_chat_ui.live2d_area, 1);     /* 弹性吃掉剩余高度 */
    lv_obj_set_style_bg_opa(s_chat_ui.live2d_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chat_ui.live2d_area, 0, 0);
    lv_obj_set_style_radius(s_chat_ui.live2d_area, 0, 0);
    lv_obj_set_style_pad_all(s_chat_ui.live2d_area, 0, 0);
    lv_obj_clear_flag(s_chat_ui.live2d_area, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *scr_chat_get_live2d_area(void)
{
    return s_chat_ui.live2d_area;
}

/**
 * @brief 创建字幕区域（110px，黑底 80% + 白字中文自动换行）
 */
static void create_subtitle_area(lv_obj_t *parent)
{
    const theme_colors_t *colors = theme_manager_get_colors();

    s_chat_ui.subtitle_area = lv_obj_create(parent);
    lv_obj_set_size(s_chat_ui.subtitle_area, SCR_WIDTH, SUBTITLE_H);
    lv_obj_set_style_bg_color(s_chat_ui.subtitle_area, colors->caption_bg, 0);
    lv_obj_set_style_bg_opa(s_chat_ui.subtitle_area, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_chat_ui.subtitle_area, 0, 0);
    lv_obj_set_style_radius(s_chat_ui.subtitle_area, 0, 0);
    lv_obj_set_style_pad_hor(s_chat_ui.subtitle_area, 32, 0);
    lv_obj_set_style_pad_ver(s_chat_ui.subtitle_area, 14, 0);
    lv_obj_clear_flag(s_chat_ui.subtitle_area, LV_OBJ_FLAG_SCROLLABLE);

    /* 字幕文本（AI 回复逐字更新到这里），宽度留出左右 pad 防溢出 */
    s_chat_ui.subtitle_label = lv_label_create(s_chat_ui.subtitle_area);
    lv_label_set_text(s_chat_ui.subtitle_label, "你好呀！我是三玖，点击屏幕开始聊天吧～");
    lv_obj_set_style_text_color(s_chat_ui.subtitle_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_chat_ui.subtitle_label, nino_font_cjk16(), 0);
    lv_label_set_long_mode(s_chat_ui.subtitle_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_chat_ui.subtitle_label, SCR_WIDTH - 64);
    lv_obj_center(s_chat_ui.subtitle_label);
}

/**
 * @brief 创建底部栏（56px：波形示意条 | 停止按钮）
 */
static void create_bottom_bar(lv_obj_t *parent)
{
    const theme_colors_t *colors = theme_manager_get_colors();

    s_chat_ui.bottom_bar = lv_obj_create(parent);
    lv_obj_set_size(s_chat_ui.bottom_bar, SCR_WIDTH, BOTTOM_BAR_H);
    lv_obj_set_style_bg_color(s_chat_ui.bottom_bar, colors->card_bg, 0);
    lv_obj_set_style_bg_opa(s_chat_ui.bottom_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_chat_ui.bottom_bar, 0, 0);
    /* 只画上边框细线，与字幕区形成层次分隔 */
    lv_obj_set_style_border_side(s_chat_ui.bottom_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(s_chat_ui.bottom_bar, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_border_width(s_chat_ui.bottom_bar, 1, 0);
    lv_obj_set_style_pad_hor(s_chat_ui.bottom_bar, 32, 0);
    lv_obj_set_style_pad_ver(s_chat_ui.bottom_bar, 0, 0);
    lv_obj_set_flex_flow(s_chat_ui.bottom_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_chat_ui.bottom_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_chat_ui.bottom_bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 波形示意：五根圆角竖条，高度中间高两边低（Phase 3 换真波形动画） */
    s_chat_ui.waveform_area = lv_obj_create(s_chat_ui.bottom_bar);
    lv_obj_set_size(s_chat_ui.waveform_area, 160, 36);
    lv_obj_set_style_bg_opa(s_chat_ui.waveform_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chat_ui.waveform_area, 0, 0);
    lv_obj_set_style_pad_all(s_chat_ui.waveform_area, 0, 0);
    lv_obj_set_flex_flow(s_chat_ui.waveform_area, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_chat_ui.waveform_area, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_chat_ui.waveform_area, 8, 0);
    lv_obj_clear_flag(s_chat_ui.waveform_area, LV_OBJ_FLAG_SCROLLABLE);

    static const int16_t bar_hs[5] = {12, 22, 32, 22, 12};   /* 各条高度 */
    for (int i = 0; i < 5; i++) {
        lv_obj_t *bar = lv_obj_create(s_chat_ui.waveform_area);
        lv_obj_set_size(bar, 8, bar_hs[i]);
        lv_obj_set_style_radius(bar, 4, 0);
        lv_obj_set_style_bg_color(bar, colors->primary_color, 0);
        lv_obj_set_style_bg_opa(bar, 140 + i * 15, 0);       /* 中间实两边淡 */
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 停止按钮：白色小方块图标 + "停止"（打断 AI 说话 / 退出对话） */
    s_chat_ui.stop_btn = lv_btn_create(s_chat_ui.bottom_bar);
    lv_obj_set_size(s_chat_ui.stop_btn, 108, 36);
    lv_obj_set_style_bg_color(s_chat_ui.stop_btn, lv_color_hex(0xE74C3C), 0);
    lv_obj_set_style_radius(s_chat_ui.stop_btn, 8, 0);
    lv_obj_set_style_pad_all(s_chat_ui.stop_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_chat_ui.stop_btn, 0, 0);
    lv_obj_add_event_cb(s_chat_ui.stop_btn, on_stop_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *stop_sq = lv_obj_create(s_chat_ui.stop_btn);
    lv_obj_set_size(stop_sq, 10, 10);
    lv_obj_set_style_bg_color(stop_sq, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(stop_sq, 2, 0);
    lv_obj_set_style_border_width(stop_sq, 0, 0);
    lv_obj_align(stop_sq, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_clear_flag(stop_sq, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *stop_txt = lv_label_create(s_chat_ui.stop_btn);
    lv_label_set_text(stop_txt, "停止");
    lv_obj_set_style_text_color(stop_txt, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(stop_txt, nino_font_cjk16(), 0);
    lv_obj_align(stop_txt, LV_ALIGN_RIGHT_MID, -16, 0);
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
 *
 * 页面容器默认隐藏，由 ui_manager_navigate() 按状态机切换显示。
 * 容器 pad_row(0)：LVGL 9.4 没有 pad_gap API，列方向间隙用 pad_row 归零。
 */
lv_obj_t *scr_chat_create(lv_obj_t *parent)
{
    s_chat_ui.container = lv_obj_create(parent);
    lv_obj_set_size(s_chat_ui.container, SCR_WIDTH, SCR_HEIGHT);
    lv_obj_set_style_bg_opa(s_chat_ui.container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chat_ui.container, 0, 0);
    lv_obj_set_style_radius(s_chat_ui.container, 0, 0);
    lv_obj_set_style_pad_all(s_chat_ui.container, 0, 0);
    lv_obj_set_style_pad_row(s_chat_ui.container, 0, 0);    /* 栏间隙归零（9.4 无 pad_gap） */
    lv_obj_set_flex_flow(s_chat_ui.container, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_chat_ui.container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_chat_ui.container, LV_OBJ_FLAG_HIDDEN);   /* 默认隐藏 */

    /* 按从上到下的顺序创建子元素 */
    create_status_bar(s_chat_ui.container);       /* 1. 状态栏 48px */
    create_live2d_area(s_chat_ui.container);      /* 2. 角色区（flex 弹性） */
    create_subtitle_area(s_chat_ui.container);    /* 3. 字幕区 110px */
    create_bottom_bar(s_chat_ui.container);       /* 4. 底栏 56px */

    ESP_LOGI(TAG, "Chat 页面创建完成");
    return s_chat_ui.container;
}

void scr_chat_set_subtitle(const char *text)
{
    if (s_chat_ui.subtitle_label != NULL && text != NULL) {
        lv_label_set_text(s_chat_ui.subtitle_label, text);
    }
}

void scr_chat_set_state(const char *text, lv_color_t color)
{
    if (s_chat_ui.state_dot != NULL) {
        lv_obj_set_style_bg_color(s_chat_ui.state_dot, color, 0);
    }
    if (s_chat_ui.state_label != NULL && text != NULL) {
        lv_obj_set_style_text_color(s_chat_ui.state_label, color, 0);
        lv_label_set_text(s_chat_ui.state_label, text);
    }
}
