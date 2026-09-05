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
    lv_obj_t *wifi_icon;        /* Wi-Fi 状态图标（文字替代） */
    lv_obj_t *time_label;       /* 时间显示标签 */
    lv_obj_t *live2d_area;      /* Live2D 角色区域（Phase 2 实现） */
    lv_obj_t *subtitle_bar;     /* 底部字幕栏 */
    lv_obj_t *subtitle_label;   /* 字幕文本标签 */
    lv_obj_t *tap_hint;         /* 点击提示文字 */
} home_ui_t;

/* Home 页面的 UI 对象实例（静态全局，本模块独占） */
static home_ui_t s_home_ui;

/**
 * @brief 创建顶部状态栏
 *
 * 布局：左右两端对齐（Wi-Fi 在左，时间在右）。
 * 背景半透明灰色，文字白色。
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
    /* 半透明背景 */
    const theme_colors_t *colors = theme_manager_get_colors();
    lv_obj_set_style_bg_color(s_home_ui.status_bar, colors->status_bar_bg, 0);
    /* R8：lv_obj 默认主题 pad=20 会把 40px 高的栏撑爆（文字上溢），
     * 必须显式 pad_all(0) 后只留左右内边距 */
    lv_obj_set_style_pad_all(s_home_ui.status_bar, 0, 0);
    lv_obj_set_style_pad_hor(s_home_ui.status_bar, 16, 0);  /* 左右内边距 16px */
    lv_obj_set_style_bg_opa(s_home_ui.status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_home_ui.status_bar, 0, 0);  /* 无边框 */
    lv_obj_set_style_radius(s_home_ui.status_bar, 0, 0);  /* 无圆角 */

    /* Wi-Fi 图标（中文状态文字，nino_cjk_16 字体含 ASCII 可混排） */
    s_home_ui.wifi_icon = lv_label_create(s_home_ui.status_bar);
    lv_label_set_text(s_home_ui.wifi_icon, "WiFi 未连接");
    lv_obj_set_style_text_color(s_home_ui.wifi_icon, colors->text_color, 0);
    lv_obj_set_style_text_font(s_home_ui.wifi_icon, nino_font_cjk16(), 0);

    /* 时间标签（默认显示 00:00） */
    s_home_ui.time_label = lv_label_create(s_home_ui.status_bar);
    lv_label_set_text(s_home_ui.time_label, "00:00");
    lv_obj_set_style_text_color(s_home_ui.time_label, colors->text_color, 0);
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

    /* 居中排列子元素 */
    lv_obj_set_flex_flow(s_home_ui.subtitle_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_home_ui.subtitle_bar, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 字幕文本标签（中文用 nino_cjk_16 字体） */
    s_home_ui.subtitle_label = lv_label_create(s_home_ui.subtitle_bar);
    lv_label_set_text(s_home_ui.subtitle_label, "你好！点击角色开始聊天 ~");
    lv_obj_set_style_text_color(s_home_ui.subtitle_label,
                                lv_color_hex(0xFFFFFF), 0);  /* 白色文字 */
    lv_obj_set_style_text_font(s_home_ui.subtitle_label,
                               nino_font_cjk16(), 0);
    /* 超长文本自动截断显示省略号 */
    lv_label_set_long_mode(s_home_ui.subtitle_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_home_ui.subtitle_label, SCR_WIDTH - 40);
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
void scr_home_update_status_bar(bool wifi_connected, const char *time_str)
{
    if (s_home_ui.wifi_icon == NULL) {
        return;  /* 状态栏还未创建 */
    }

    const theme_colors_t *colors = theme_manager_get_colors();

    if (wifi_connected) {
        /* 已连接：主题蓝色高亮 */
        lv_label_set_text(s_home_ui.wifi_icon, "WiFi 已连接");
        lv_obj_set_style_text_color(s_home_ui.wifi_icon,
                                    colors->primary_color, 0);
    } else {
        /* 未连接：默认文字色 */
        lv_label_set_text(s_home_ui.wifi_icon, "WiFi 未连接");
        lv_obj_set_style_text_color(s_home_ui.wifi_icon,
                                    colors->text_color, 0);
    }

    /* 更新时间显示 */
    if (time_str != NULL && s_home_ui.time_label != NULL) {
        lv_label_set_text(s_home_ui.time_label, time_str);
    }
}

void scr_home_set_subtitle(const char *text)
{
    if (s_home_ui.subtitle_label != NULL && text != NULL) {
        lv_label_set_text(s_home_ui.subtitle_label, text);
    }
}
