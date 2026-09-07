/**
 * @file    scr_home.c
 * @brief   Home Screen 实现
 *
 * 主界面（R9 起：全应用唯一可见页面，"对话=主页状态层"）：
 *   - 顶部状态栏：Wi-Fi 状态 + 当前时间
 *   - 中间区域：Live2D 角色（唯一交互目标：五种触摸表情）
 *   - 底部字幕栏：AI 回复 / 三玖闲聊轮播文案
 *
 * 点屏幕空白无反应（R9）；Phase 3 语音接入后空白点击=开始说话。
 *
 * @date    2026-09-01
 * @version 2.0.0
 */

/* 1. 自身公开头 */
#include "scr_home.h"

/* 2. C 标准库 */
#include <string.h>
#include <stdio.h>

/* 3. 项目级 */
#include "theme_manager.h"    /* 主题管理：获取当前主题颜色 */
#include "nino_font.h"        /* 自定义中文字体 nino_cjk_16 */
/* 注：app_state_machine/app_events 已随 R9 拆除"点空白触发对话"移除 */

/* 4. 平台/厂商头 */
#include "esp_log.h"

static const char *TAG = "scr_home";

/* 屏幕尺寸常量（ESP32-P4 开发板的显示屏分辨率） */
#define SCR_WIDTH       1024   /* 屏幕宽度（像素） */
#define SCR_HEIGHT      600    /* 屏幕高度（像素） */
#define STATUS_BAR_H    40     /* 顶部状态栏高度（像素） */
#define SUBTITLE_BAR_H  80     /* 底部字幕栏高度（像素） */

/**
 * @brief Home 页面的所有 UI 对象
 *
 * 用一个结构体统一管理，方便后续更新和操作。
 */
typedef struct {
    lv_obj_t *container;        /* 页面容器：包含所有子元素 */
    lv_obj_t *status_bar;       /* 顶部状态栏 */
    lv_obj_t *wifi_switch;      /* Wi-Fi 开关滑块（R12：拨右=开/绿） */
    lv_obj_t *wifi_label;       /* Wi-Fi 状态文案（黄=未连/绿=连接中/已连） */
    lv_obj_t *time_label;       /* 时间显示标签 */
    lv_obj_t *live2d_area;      /* Live2D 角色区域 */
    lv_obj_t *subtitle_bar;     /* 底部字幕栏 */
    lv_obj_t *dialog_dot;       /* 对话状态点（M0 状态层最小版，IDLE 隐藏） */
    lv_obj_t *subtitle_label;   /* 字幕文本标签 */
    lv_obj_t *mic_btn;          /* 按住说话按钮（M1） */
} home_ui_t;

/* Home 页面的 UI 对象实例（静态全局，本模块独占） */
static home_ui_t s_home_ui;

/* Wi-Fi 开关切捔回调（编排层经 set_wifi_toggle_cb 注入，UI 不碰 BSP） */
static void (*s_wifi_toggle_cb)(bool turn_on, void *ctx) = NULL;
static void *s_wifi_toggle_ctx = NULL;

/* 按住说话回调（编排层注入：true=按下开始录，false=松开停止） */
static void (*s_voice_hold_cb)(bool holding, void *ctx) = NULL;
static void *s_voice_hold_ctx = NULL;

/* 前向声明：麦克风按钮事件 */
static void on_mic_pressed(lv_event_t *e);
static void on_mic_released(lv_event_t *e);

/**
 * @brief Wi-Fi 开关拨动回调（用户操作触发；程序设 CHECKED 不触发事件）
 */
static void on_wifi_switch_changed(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool turn_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "Wi-Fi 开关拨动: %s", turn_on ? "ON" : "OFF");
    if (s_wifi_toggle_cb) {
        s_wifi_toggle_cb(turn_on, s_wifi_toggle_ctx);
    }
}

/**
 * @brief 创建顶部状态栏（R12：Wi-Fi 滑块开关 + 三态文案 | 时间）
 *
 * 布局：SPACE_BETWEEN —— 左侧[开关+状态文案]组合，右侧时间。
 * 必须显式 pad_all(0)：lv_obj 默认主题 pad=20 会把 40px 高的栏撑爆。
 */
static void create_status_bar(lv_obj_t *parent)
{
    /* 创建状态栏容器 */
    s_home_ui.status_bar = lv_obj_create(parent);
    lv_obj_set_size(s_home_ui.status_bar, SCR_WIDTH, STATUS_BAR_H);
    /* 使用 Flexbox 布局：子元素水平排列，两端对齐 */
    lv_obj_set_flex_flow(s_home_ui.status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_home_ui.status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* R8：lv_obj 默认 pad=20 会撑爆 40px 高的栏，归零后只留左右内边距 */
    lv_obj_set_style_pad_all(s_home_ui.status_bar, 0, 0);
    lv_obj_set_style_pad_hor(s_home_ui.status_bar, 16, 0);
    const theme_colors_t *colors = theme_manager_get_colors();
    lv_obj_set_style_bg_color(s_home_ui.status_bar, colors->status_bar_bg, 0);
    lv_obj_set_style_bg_opa(s_home_ui.status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_home_ui.status_bar, 0, 0);
    lv_obj_set_style_radius(s_home_ui.status_bar, 0, 0);

    /* ---- 左组：Wi-Fi 滑块 + 状态文案（透明容器 8px 间距） ---- */
    lv_obj_t *wifi_group = lv_obj_create(s_home_ui.status_bar);
    lv_obj_set_size(wifi_group, 220, STATUS_BAR_H);
    lv_obj_set_style_bg_opa(wifi_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_group, 0, 0);
    lv_obj_set_style_pad_all(wifi_group, 0, 0);
    lv_obj_set_flex_flow(wifi_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi_group, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wifi_group, 10, 0);
    lv_obj_clear_flag(wifi_group, LV_OBJ_FLAG_SCROLLABLE);

    /* 滑块开关：椭圆框，拨右=开。选中态指示器绿、未选灰色 */
    s_home_ui.wifi_switch = lv_switch_create(wifi_group);
    lv_obj_set_size(s_home_ui.wifi_switch, 52, 26);
    lv_obj_set_style_bg_color(s_home_ui.wifi_switch, lv_color_hex(0x27AE60),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_home_ui.wifi_switch, lv_color_hex(0xB5B5B5),
                              LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_home_ui.wifi_switch, on_wifi_switch_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* 三态文案：未连接=黄 / 正在连接中=绿 / WiFi 已连接=绿 */
    s_home_ui.wifi_label = lv_label_create(wifi_group);
    lv_label_set_text(s_home_ui.wifi_label, "WiFi 未连接");
    lv_obj_set_style_text_color(s_home_ui.wifi_label, lv_color_hex(0xF1C40F), 0);
    lv_obj_set_style_text_font(s_home_ui.wifi_label, nino_font_cjk16(), 0);

    /* ---- 右侧：时间 ---- */
    s_home_ui.time_label = lv_label_create(s_home_ui.status_bar);
    lv_label_set_text(s_home_ui.time_label, "--:--");
    lv_obj_set_style_text_color(s_home_ui.time_label, colors->text_color, 0);
    lv_obj_set_style_text_font(s_home_ui.time_label, nino_font_cjk16(), 0);
}

/**
 * @brief 创建 Live2D 角色区域（Phase 2 前为占位符）
 *
 * 占位区域显示提示文字，告诉用户这里将显示 Live2D 角色。
 * 点击此区域会触发对话流程。
 */
static void create_live2d_area(lv_obj_t *parent)
{
    /* 计算 Live2D 区域高度：总高度 - 状态栏 - 字幕栏 */
    s_home_ui.live2d_area = lv_obj_create(parent);
    lv_obj_set_size(s_home_ui.live2d_area, SCR_WIDTH,
                    SCR_HEIGHT - STATUS_BAR_H - SUBTITLE_BAR_H);
    /* 透明背景，无边框 */
    lv_obj_set_style_bg_opa(s_home_ui.live2d_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_home_ui.live2d_area, 0, 0);
    lv_obj_set_style_radius(s_home_ui.live2d_area, 0, 0);
    lv_obj_set_style_pad_all(s_home_ui.live2d_area, 0, 0);
    /* 使用 Flexbox 居中排列子元素 */
    lv_obj_set_flex_flow(s_home_ui.live2d_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_home_ui.live2d_area, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 角色区域不滚动（拖动手势留给头部跟随，M03 R5b） */
    lv_obj_clear_flag(s_home_ui.live2d_area, LV_OBJ_FLAG_SCROLLABLE);

    /* R9（对话=主页状态层）：点击空白不再触发对话——交互只围绕角色本体
     * （角色 lv_image 自身 CLICKABLE，五种触摸表情由 rig_lvgl 手势识别处理）。
     * Phase 3 语音链路就绪后，空白点击将作为"开始说话"触发器回归。 */
}

lv_obj_t *scr_home_get_live2d_area(void)
{
    return s_home_ui.live2d_area;
}

/**
 * @brief 创建底部字幕栏
 *
 * 半透明黑色背景，用于显示 AI 的回复文本。
 * 字幕会随着对话内容动态更新。
 */
static void create_subtitle_bar(lv_obj_t *parent)
{
    s_home_ui.subtitle_bar = lv_obj_create(parent);
    lv_obj_set_size(s_home_ui.subtitle_bar, SCR_WIDTH, SUBTITLE_BAR_H);

    /* 设置黑色背景（R8：改为不透明，半透明叠透明容器的效果发灰） */
    const theme_colors_t *colors = theme_manager_get_colors();
    lv_obj_set_style_bg_color(s_home_ui.subtitle_bar, colors->caption_bg, 0);
    lv_obj_set_style_bg_opa(s_home_ui.subtitle_bar, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_home_ui.subtitle_bar, 0, 0);  /* 无边框 */
    lv_obj_set_style_radius(s_home_ui.subtitle_bar, 0, 0);        /* 无圆角 */
    lv_obj_set_style_pad_all(s_home_ui.subtitle_bar, 0, 0);

    /* 三段式布局：[平衡垫] [状态点+字幕] [按住说话] —— 字幕保持居中 */
    lv_obj_set_flex_flow(s_home_ui.subtitle_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_home_ui.subtitle_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_home_ui.subtitle_bar, 10, 0);

    /* 平衡垫（与麦克风按钮同宽，把中间字幕挤到真居中）
     * 顺序坑：必须先 remove_style_all 再 set_size——剥样式会把尺寸属性
     * 一起剥掉，反着来尺寸会回退成主题默认值（M1 模拟器实测） */
    lv_obj_t *pad = lv_obj_create(s_home_ui.subtitle_bar);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, 136, 1);

    /* 对话状态点（M1 美化）：剥光默认主题，纯色小圆（否则自带边框
     * 内边距衬得 10px 圆点怪模怪样） */
    s_home_ui.dialog_dot = lv_obj_create(s_home_ui.subtitle_bar);
    lv_obj_remove_style_all(s_home_ui.dialog_dot);
    lv_obj_set_size(s_home_ui.dialog_dot, 12, 12);
    lv_obj_set_style_radius(s_home_ui.dialog_dot, 6, 0);
    lv_obj_set_style_bg_color(s_home_ui.dialog_dot, lv_color_hex(0x27AE60), 0);
    lv_obj_set_style_bg_opa(s_home_ui.dialog_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_home_ui.dialog_dot, LV_OBJ_FLAG_HIDDEN);

    /* 字幕文本（M1 修复溢出）：固定宽 984 在三段式布局里会挤爆 1024
     * 屏宽（文字被顶出屏幕）→ 收窄到 700 并改 WRAP 换行 + 居中对齐 */
    s_home_ui.subtitle_label = lv_label_create(s_home_ui.subtitle_bar);
    lv_label_set_text(s_home_ui.subtitle_label, "你好！点击角色开始聊天 ~");
    lv_obj_set_style_text_color(s_home_ui.subtitle_label,
                                lv_color_hex(0xFFFFFF), 0);  /* 白色文字 */
    lv_obj_set_style_text_font(s_home_ui.subtitle_label,
                               nino_font_cjk16(), 0);
    lv_label_set_long_mode(s_home_ui.subtitle_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_home_ui.subtitle_label, 700);
    lv_obj_set_style_text_align(s_home_ui.subtitle_label,
                                LV_TEXT_ALIGN_CENTER, 0);

    /* 按住说话按钮（M1）：喇叭图标 + 文字，按下开始录音、松开送识别 */
    s_home_ui.mic_btn = lv_btn_create(s_home_ui.subtitle_bar);
    lv_obj_set_size(s_home_ui.mic_btn, 136, 44);
    const theme_colors_t *mic_colors = theme_manager_get_colors();
    lv_obj_set_style_bg_color(s_home_ui.mic_btn, mic_colors->primary_color, 0);
    lv_obj_set_style_radius(s_home_ui.mic_btn, 22, 0);
    lv_obj_set_style_pad_all(s_home_ui.mic_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_home_ui.mic_btn, 0, 0);
    lv_obj_add_event_cb(s_home_ui.mic_btn, on_mic_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_home_ui.mic_btn, on_mic_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_home_ui.mic_btn, on_mic_released, LV_EVENT_PRESS_LOST, NULL);

    lv_obj_t *mic_sym = lv_label_create(s_home_ui.mic_btn);
    lv_label_set_text(mic_sym, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(mic_sym, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(mic_sym, &lv_font_montserrat_14, 0);
    lv_obj_align(mic_sym, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *mic_txt = lv_label_create(s_home_ui.mic_btn);
    lv_label_set_text(mic_txt, "按住说话");
    lv_obj_set_style_text_color(mic_txt, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(mic_txt, nino_font_cjk16(), 0);
    lv_obj_align(mic_txt, LV_ALIGN_RIGHT_MID, -12, 0);
}

/** 按住说话：按下沿 → 开始录音 */
static void on_mic_pressed(lv_event_t *e)
{
    (void)e;
    if (s_voice_hold_cb) {
        s_voice_hold_cb(true, s_voice_hold_ctx);
    }
}

/** 按住说话：松开沿（含滑出按钮的 PRESS_LOST）→ 停止录音 */
static void on_mic_released(lv_event_t *e)
{
    (void)e;
    if (s_voice_hold_cb) {
        s_voice_hold_cb(false, s_voice_hold_ctx);
    }
}

/**
 * @brief 创建 Home 页面
 *
 * 创建页面容器和所有子元素，默认隐藏。
 * 需要调用 ui_manager_navigate(UI_PAGE_HOME) 才会显示。
 */
lv_obj_t *scr_home_create(lv_obj_t *parent)
{
    /* 创建页面容器（默认隐藏） */
    s_home_ui.container = lv_obj_create(parent);
    lv_obj_set_size(s_home_ui.container, SCR_WIDTH, SCR_HEIGHT);
    lv_obj_set_style_bg_color(s_home_ui.container,
                              theme_manager_get_colors()->bg_color, 0);
    lv_obj_set_style_bg_opa(s_home_ui.container, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_home_ui.container, LV_OPA_TRANSP, 0);  /* 透明背景 */
    lv_obj_set_style_border_width(s_home_ui.container, 0, 0);
    lv_obj_set_style_radius(s_home_ui.container, 0, 0);
    lv_obj_set_style_pad_all(s_home_ui.container, 0, 0);
    lv_obj_set_flex_flow(s_home_ui.container, LV_FLEX_FLOW_COLUMN);  /* 纵向排列 */
    lv_obj_add_flag(s_home_ui.container, LV_OBJ_FLAG_HIDDEN);        /* 默认隐藏 */
    /* 页面容器不滚动（全屏拖动会滚背景，M03 R5b 修复） */
    lv_obj_clear_flag(s_home_ui.container, LV_OBJ_FLAG_SCROLLABLE);

    /* 按顺序创建子元素（从上到下） */
    create_status_bar(s_home_ui.container);      /* 1. 顶部状态栏 */
    create_live2d_area(s_home_ui.container);      /* 2. 中间 Live2D 区域 */
    create_subtitle_bar(s_home_ui.container);     /* 3. 底部字幕栏 */

    ESP_LOGI(TAG, "Home 页面创建完成");
    return s_home_ui.container;
}

/**
 * @brief 更新状态栏
 *
 * 根据 Wi-Fi 连接状态更新图标和颜色：
 *   - 已连接：显示蓝色 Wi-Fi 图标
 *   - 未连接：显示白色 Wi-Fi 图标 + "Disconnected"
 */
void scr_home_set_wifi_state(scr_wifi_state_t state)
{
    if (s_home_ui.wifi_label == NULL) {
        return;
    }
    const char *text;
    lv_color_t color;
    switch (state) {
        case SCR_WIFI_CONNECTING:
            text = "正在连接中";
            color = lv_color_hex(0x27AE60);     /* 绿 */
            break;
        case SCR_WIFI_CONNECTED:
            text = "WiFi 已连接";
            color = lv_color_hex(0x27AE60);     /* 绿 */
            break;
        case SCR_WIFI_DISCONNECTED:
        default:
            text = "WiFi 未连接";
            color = lv_color_hex(0xF1C40F);     /* 黄 */
            break;
    }
    /* 滑块随状态联动：连接中/已连=拨右侧绿色；未连=左侧灰 */
    if (s_home_ui.wifi_switch != NULL) {
        if (state == SCR_WIFI_DISCONNECTED) {
            lv_obj_remove_state(s_home_ui.wifi_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(s_home_ui.wifi_switch, LV_STATE_CHECKED);
        }
    }
    lv_label_set_text(s_home_ui.wifi_label, text);
    lv_obj_set_style_text_color(s_home_ui.wifi_label, color, 0);
}

void scr_home_set_dialog_state(dialog_state_t state)
{
    if (s_home_ui.dialog_dot == NULL) {
        return;
    }
    lv_color_t color;
    switch (state) {
        case DIALOG_STATE_LISTENING:  color = lv_color_hex(0x4A90D9); break;  /* 蓝=听 */
        case DIALOG_STATE_THINKING:   color = lv_color_hex(0xE67E22); break;  /* 橙=想 */
        case DIALOG_STATE_SPEAKING:   color = lv_color_hex(0x27AE60); break;  /* 绿=说 */
        default:                    color = lv_color_hex(0x27AE60); break;
    }
    lv_obj_set_style_bg_color(s_home_ui.dialog_dot, color, 0);
    if (state == DIALOG_STATE_IDLE) {
        lv_obj_add_flag(s_home_ui.dialog_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_home_ui.dialog_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

void scr_home_set_time(const char *time_str, bool synced)
{
    if (s_home_ui.time_label == NULL || time_str == NULL) {
        return;
    }
    lv_label_set_text(s_home_ui.time_label, time_str);
    /* R12：断线期间晶振续走的时间为"非权威"——灰色提示，重连校准后恢复 */
    lv_obj_set_style_text_color(s_home_ui.time_label,
                                synced ? theme_manager_get_colors()->text_color
                                       : lv_color_hex(0x999999), 0);
}

void scr_home_set_wifi_toggle_cb(void (*cb)(bool turn_on, void *ctx), void *ctx)
{
    s_wifi_toggle_cb = cb;
    s_wifi_toggle_ctx = ctx;
}

void scr_home_set_voice_hold_cb(void (*cb)(bool holding, void *ctx), void *ctx)
{
    s_voice_hold_cb = cb;
    s_voice_hold_ctx = ctx;
}

void scr_home_set_subtitle(const char *text)
{
    if (s_home_ui.subtitle_label != NULL && text != NULL) {
        lv_label_set_text(s_home_ui.subtitle_label, text);
    }
}
