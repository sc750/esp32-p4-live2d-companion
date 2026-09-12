/**
 * @file    scr_music.c
 * @brief   音乐页实现（全屏控制面板）
 *
 * 层次：本模块只画 UI + 收触摸事件，动作全部经回调交编排层执行；
 * 播放状态由编排层按秒节拍喂进来（scr_music_set_state）。
 * 详见 scr_music.h 的分层约定说明。
 *
 * 两个省电/防抖要点：
 *   1. set_state 先比对缓存，值没变不碰 LVGL（主循环 1s 一次，不比对等于每秒全量重绘）
 *   2. 页面处于 HIDDEN 时直接返回（LVGL 不渲染隐藏对象，但改属性仍会标脏）
 *
 * @date    2026-09-10
 * @version 1.0.0
 */

/* 1. 自身公开头 */
#include "scr_music.h"

/* 2. C 标准库 */
#include <stdio.h>              /* snprintf（格式化已播时长/音量） */
#include <stdint.h>             /* intptr_t（列表项索引经 user_data 传递） */
#include <string.h>             /* strchr（从 "1. 曲名" 中剥出曲名） */

/* 3. 项目级 */
#include "theme_manager.h"      /* 主题色：卡片/文字/主题强调色 */
#include "nino_font.h"          /* 自定义中文字体 nino_cjk_16 */

/* 4. 平台/厂商头 */
#include "esp_log.h"

static const char *TAG = "scr_music";

/* ---------- 布局常量（对照 ESP32-P4 开发板 1024×600 屏） ---------- */
#define SCR_WIDTH       1024    /* 屏幕宽度 */
#define SCR_HEIGHT      600     /* 屏幕高度 */
#define TOP_BAR_H       56      /* 顶栏高度（返回/标题/状态） */
#define MAIN_AREA_H     336     /* 中部控制区高度（曲名+时长+按钮+音量） */
#define LIST_ITEM_H     44      /* 列表项高度 */
#define LIST_MAX        SCR_MUSIC_LIST_MAX  /* 列表最多建项数（头文件公开，编排层按它截断） */

/* ---------- 模块状态 ---------- */

/** 音乐页所有 LVGL 对象 + 上次状态缓存 */
static struct {
    lv_obj_t *container;                /* 页面根容器（ui_manager 控制显隐） */
    /* --- 顶栏 --- */
    lv_obj_t *back_btn;                 /* 返回按钮 */
    lv_obj_t *status_label;             /* 状态文案：播放中/已暂停/已停止 */
    /* --- 中部 --- */
    lv_obj_t *track_label;              /* 当前曲名（大字） */
    lv_obj_t *pos_label;                /* 已播时长 "已播 MM:SS" */
    lv_obj_t *play_icon;                /* 播放/暂停按钮上的图标（随状态切换） */
    lv_obj_t *vol_slider;               /* 音量滑块 */
    lv_obj_t *vol_val_label;            /* 音量数值文案 */
    /* --- 底部列表 --- */
    lv_obj_t *list_area;                /* 列表容器（纵向 flex，可滚动） */
    lv_obj_t *items[LIST_MAX];          /* 列表项对象（按钮） */
    lv_obj_t *item_labels[LIST_MAX];    /* 列表项文字 */
    int item_count;                     /* 当前已建项数 */
    /* --- 上次状态缓存（无变化不重绘） --- */
    bool last_playing;                  /* 上次是否在播 */
    bool last_paused;                   /* 上次是否暂停 */
    int  last_idx;                      /* 上次当前曲索引 */
    int  last_pos;                      /* 上次已播秒数 */
    bool last_valid;                    /* 缓存是否已初始化过 */
    int  cur_idx;                       /* 当前曲索引（点主按钮时决定 PLAY 哪首） */
    /* --- 回调（编排层注入） --- */
    void (*back_cb)(void *ctx);                     /* 返回按钮 */
    void *back_ctx;                                 /* 返回上下文 */
    void (*ctrl_cb)(int cmd, int arg, void *ctx);   /* 播控命令 */
    void *ctrl_ctx;                                 /* 播控上下文 */
    bool syncing_vol;                   /* 程序设音量滑块中（抑制回调，防自激） */
} s_music;

/* ---------- 内部工具 ---------- */

/** 发一条播控命令（统一走回调，未注入则忽略） */
static void emit_cmd(scr_music_cmd_t cmd, int arg)
{
    if (s_music.ctrl_cb) {                              /* 有回调才发 */
        s_music.ctrl_cb((int)cmd, arg, s_music.ctrl_ctx);
    }
}

/* ---------- 事件回调 ---------- */

/** 返回按钮：交编排层切回主页（音乐继续播，由音乐任务独立维持） */
static void on_back_clicked(lv_event_t *e)
{
    (void)e;                                            /* 未用事件参数 */
    ESP_LOGI(TAG, "返回主页");                           /* 诊断日志 */
    if (s_music.back_cb) {                              /* 有回调才发 */
        s_music.back_cb(s_music.back_ctx);
    }
}

/** 播放/暂停主按钮：按当前状态决定发 RESUME / PAUSE / PLAY */
static void on_play_clicked(lv_event_t *e)
{
    (void)e;                                            /* 未用事件参数 */
    if (s_music.last_paused) {                          /* 暂停中 */
        emit_cmd(SCR_MUSIC_CMD_RESUME, 0);              /* 继续播 */
    } else if (s_music.last_playing) {                  /* 正在播 */
        emit_cmd(SCR_MUSIC_CMD_PAUSE, 0);               /* 暂停 */
    } else {                                            /* 停止态 */
        int idx = (s_music.cur_idx >= 0) ? s_music.cur_idx : 0;     /* 有当前曲就续播它 */
        emit_cmd(SCR_MUSIC_CMD_PLAY, idx);              /* 否则从头一台开始 */
    }
}

/** 上一首/下一首 */
static void on_prev_clicked(lv_event_t *e)
{
    (void)e;                                            /* 未用事件参数 */
    emit_cmd(SCR_MUSIC_CMD_PREV, 0);                    /* 交编排层切台 */
}

static void on_next_clicked(lv_event_t *e)
{
    (void)e;                                            /* 未用事件参数 */
    emit_cmd(SCR_MUSIC_CMD_NEXT, 0);                    /* 交编排层切台 */
}

/** 音量滑块拖动：只在用户操作时发命令（程序设值时被 syncing_vol 抑制） */
static void on_vol_changed(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);          /* 取滑块对象 */
    int vol = (int)lv_slider_get_value(slider);         /* 读当前值 0~100 */
    char buf[8];                                        /* 数值文案缓冲 */
    snprintf(buf, sizeof(buf), "%d", vol);              /* 格式化（最大 3 位） */
    lv_label_set_text(s_music.vol_val_label, buf);      /* 更新数值显示 */
    if (s_music.syncing_vol) {                          /* 程序设值引起 */
        return;                                         /* 不发命令，防自激 */
    }
    emit_cmd(SCR_MUSIC_CMD_VOL, vol);                   /* 用户拖动 → 下发音量 */
}

/** 列表项点击：播放该项（索引经 user_data 传入） */
static void on_item_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e); /* 还原列表索引 */
    ESP_LOGI(TAG, "列表点选第 %d 项", idx);              /* 诊断日志 */
    emit_cmd(SCR_MUSIC_CMD_PLAY, idx);                  /* 播放该曲 */
}

/* ---------- 各区块创建 ---------- */

/**
 * @brief 建顶栏：[返回]  ♪ 音乐            [状态]
 *
 * 坑（同 scr_home）：lv_obj 默认 pad=20 会把 56px 高的栏撑爆，必须显式归零。
 */
static void create_top_bar(lv_obj_t *parent)
{
    const theme_colors_t *colors = theme_manager_get_colors();      /* 主题色 */
    lv_obj_t *bar = lv_obj_create(parent);                          /* 顶栏容器 */
    lv_obj_set_size(bar, SCR_WIDTH, TOP_BAR_H);                     /* 通栏固定高 */
    lv_obj_set_style_bg_color(bar, colors->status_bar_bg, 0);       /* 状态栏底色 */
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);                  /* 不透明 */
    lv_obj_set_style_border_width(bar, 0, 0);                       /* 无边框 */
    lv_obj_set_style_radius(bar, 0, 0);                             /* 无圆角 */
    lv_obj_set_style_pad_all(bar, 0, 0);                            /* 内边距归零（否则撑爆） */
    lv_obj_set_style_pad_hor(bar, 16, 0);                           /* 只留左右内边距 */
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);                    /* 横向排布 */
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,         /* 两端对齐 */
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);                 /* 不滚动 */

    /* --- 左：返回按钮 --- */
    /* 图标/文字拆成两个 label（2026-09-12 上板实测豆腐块）：nino_cjk_16
     * 是纯 CJK 字库，不含 LVGL 的 FontAwesome 符号区，LV_SYMBOL_LEFT
     * 混排进去就是"□ 返回"。符号走 Montserrat（内置含符号区），与
     * scr_home 麦克风按钮同一套做法。 */
    s_music.back_btn = lv_button_create(bar);                       /* 建按钮 */
    lv_obj_set_size(s_music.back_btn, 96, 40);                      /* 尺寸 */
    lv_obj_set_style_bg_color(s_music.back_btn, colors->card_bg, 0);    /* 卡片底色 */
    lv_obj_set_style_radius(s_music.back_btn, 20, 0);               /* 圆角胶囊 */
    lv_obj_set_style_shadow_width(s_music.back_btn, 0, 0);          /* 去阴影 */
    lv_obj_set_style_pad_all(s_music.back_btn, 0, 0);               /* 内边距归零（flex 居中接管） */
    lv_obj_set_flex_flow(s_music.back_btn, LV_FLEX_FLOW_ROW);       /* 图标+文字横排 */
    lv_obj_set_flex_align(s_music.back_btn, LV_FLEX_ALIGN_CENTER,   /* 整体居中 */
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_music.back_btn, 6, 0);            /* 图标与文字间距 */
    lv_obj_add_event_cb(s_music.back_btn, on_back_clicked,          /* 点击 → 返回 */
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_icon = lv_label_create(s_music.back_btn);        /* 左箭头图标 */
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);                   /* ◀（Montserrat 符号区） */
    lv_obj_set_style_text_color(back_icon, colors->text_color, 0);  /* 图标色 */
    lv_obj_set_style_text_font(back_icon, &lv_font_montserrat_16, 0);   /* 含符号的内置字体 */
    lv_obj_t *back_txt = lv_label_create(s_music.back_btn);         /* 按钮文字 */
    lv_label_set_text(back_txt, "返回");                             /* 纯文字（CJK 字库覆盖内） */
    lv_obj_set_style_text_color(back_txt, colors->text_color, 0);   /* 文字色 */
    lv_obj_set_style_text_font(back_txt, nino_font_cjk16(), 0);     /* 中文字体 */

    /* --- 中：标题（容器横排：音符图标 + 音乐） --- */
    lv_obj_t *title_box = lv_obj_create(bar);                       /* 标题容器（占一个 flex 位） */
    lv_obj_set_size(title_box, 64, 20);                             /* 显式尺寸（9.4 不用 LV_SIZE_CONTENT） */
    lv_obj_set_style_bg_opa(title_box, LV_OPA_TRANSP, 0);           /* 透明 */
    lv_obj_set_style_border_width(title_box, 0, 0);                 /* 无边框 */
    lv_obj_set_style_pad_all(title_box, 0, 0);                      /* 内边距归零 */
    lv_obj_set_flex_flow(title_box, LV_FLEX_FLOW_ROW);              /* 图标+文字横排 */
    lv_obj_set_flex_align(title_box, LV_FLEX_ALIGN_CENTER,          /* 整体居中 */
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title_box, 6, 0);                   /* 间距 */
    lv_obj_clear_flag(title_box, LV_OBJ_FLAG_SCROLLABLE);           /* 不滚动 */
    lv_obj_t *title_icon = lv_label_create(title_box);              /* 音符图标 */
    lv_label_set_text(title_icon, LV_SYMBOL_AUDIO);                 /* ♪（Montserrat 符号区） */
    lv_obj_set_style_text_color(title_icon, colors->text_color, 0); /* 图标色 */
    lv_obj_set_style_text_font(title_icon, &lv_font_montserrat_16, 0);  /* 含符号的内置字体 */
    lv_obj_t *title_txt = lv_label_create(title_box);               /* 标题文字 */
    lv_label_set_text(title_txt, "音乐");                            /* 纯文字（CJK 字库覆盖内） */
    lv_obj_set_style_text_color(title_txt, colors->text_color, 0);  /* 文字色 */
    lv_obj_set_style_text_font(title_txt, nino_font_cjk16(), 0);    /* 中文字体 */

    /* --- 右：播放状态 --- */
    s_music.status_label = lv_label_create(bar);                    /* 状态文字 */
    lv_label_set_text(s_music.status_label, "已停止");               /* 初始停止态 */
    lv_obj_set_style_text_color(s_music.status_label, colors->text_color, 0);   /* 文字色 */
    lv_obj_set_style_text_font(s_music.status_label, nino_font_cjk16(), 0);     /* 中文字体 */
}

/**
 * @brief 建中部控制区：曲名 + 已播时长 + 三大按钮 + 音量滑块
 */
static void create_main_area(lv_obj_t *parent)
{
    const theme_colors_t *colors = theme_manager_get_colors();      /* 主题色 */
    lv_obj_t *area = lv_obj_create(parent);                         /* 控制区容器 */
    lv_obj_set_size(area, SCR_WIDTH, MAIN_AREA_H);                  /* 通栏固定高 */
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0);                /* 透明背景 */
    lv_obj_set_style_border_width(area, 0, 0);                      /* 无边框 */
    lv_obj_set_style_radius(area, 0, 0);                            /* 无圆角 */
    lv_obj_set_style_pad_all(area, 0, 0);                           /* 内边距归零 */
    lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);                /* 纵向排布 */
    lv_obj_set_flex_align(area, LV_FLEX_ALIGN_CENTER,               /* 主轴居中 */
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(area, 14, 0);                          /* 行间距 */
    lv_obj_clear_flag(area, LV_OBJ_FLAG_SCROLLABLE);                /* 不滚动 */

    /* --- 曲名（当前未播时给提示语） --- */
    s_music.track_label = lv_label_create(area);                    /* 曲名标签 */
    lv_label_set_text(s_music.track_label, "未在播放");              /* 初始占位 */
    lv_obj_set_style_text_color(s_music.track_label, colors->primary_color, 0);  /* 主题色突出 */
    lv_obj_set_style_text_font(s_music.track_label, nino_font_cjk16(), 0);       /* 中文字体 */

    /* --- 已播时长 --- */
    s_music.pos_label = lv_label_create(area);                      /* 时长标签 */
    lv_label_set_text(s_music.pos_label, " ");                      /* 占位（避免布局跳动） */
    lv_obj_set_style_text_color(s_music.pos_label, colors->text_color, 0);       /* 文字色 */
    lv_obj_set_style_text_font(s_music.pos_label, nino_font_cjk16(), 0);         /* 中文字体 */

    /* --- 按钮行：[⏮] [▶/⏸] [⏭] --- */
    /* 尺寸必须显式给（LVGL 9.4 坑，2026-09-12 上板实测）：flex 容器的
     * LV_SIZE_CONTENT 主轴宽度只按"最大子控件"算（本行=84），不算间距
     * 和其余子项——⏮/⏭ 会被裁剪。64+36+84+36+64=284，高=最大子项84。 */
    lv_obj_t *row = lv_obj_create(area);                            /* 按钮行容器 */
    lv_obj_set_size(row, 284, 84);                                  /* 显式尺寸（见上注释） */
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);                 /* 透明 */
    lv_obj_set_style_border_width(row, 0, 0);                       /* 无边框 */
    lv_obj_set_style_pad_all(row, 0, 0);                            /* 内边距归零 */
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);                    /* 横向排布 */
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,                /* 居中 */
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 36, 0);                        /* 按钮间距 */
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);                 /* 不滚动 */

    /* 上一首（圆形次级按钮） */
    lv_obj_t *prev = lv_button_create(row);                         /* 建按钮 */
    lv_obj_set_size(prev, 64, 64);                                  /* 尺寸 */
    lv_obj_set_style_radius(prev, LV_RADIUS_CIRCLE, 0);             /* 正圆 */
    lv_obj_set_style_bg_color(prev, colors->card_bg, 0);            /* 卡片底色 */
    lv_obj_set_style_shadow_width(prev, 0, 0);                      /* 去阴影 */
    lv_obj_add_event_cb(prev, on_prev_clicked, LV_EVENT_CLICKED, NULL);     /* 点击 */
    lv_obj_t *prev_icon = lv_label_create(prev);                    /* 图标 */
    lv_label_set_text(prev_icon, LV_SYMBOL_PREV);                   /* ⏮ */
    lv_obj_set_style_text_color(prev_icon, colors->text_color, 0);  /* 图标色 */
    lv_obj_set_style_text_font(prev_icon, &lv_font_montserrat_24, 0);   /* 大图标 */
    lv_obj_center(prev_icon);                                       /* 居中 */

    /* 播放/暂停（主按钮，更大更醒目） */
    lv_obj_t *play = lv_button_create(row);                         /* 建按钮 */
    lv_obj_set_size(play, 84, 84);                                  /* 尺寸 */
    lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);             /* 正圆 */
    lv_obj_set_style_bg_color(play, colors->primary_color, 0);      /* 主题色 */
    lv_obj_set_style_shadow_width(play, 0, 0);                      /* 去阴影 */
    lv_obj_add_event_cb(play, on_play_clicked, LV_EVENT_CLICKED, NULL);     /* 点击 */
    s_music.play_icon = lv_label_create(play);                      /* 图标（随状态切换） */
    lv_label_set_text(s_music.play_icon, LV_SYMBOL_PLAY);           /* 初始 ▶ */
    lv_obj_set_style_text_color(s_music.play_icon, lv_color_hex(0xFFFFFF), 0);  /* 白图标 */
    lv_obj_set_style_text_font(s_music.play_icon, &lv_font_montserrat_24, 0);   /* 大图标 */
    lv_obj_center(s_music.play_icon);                               /* 居中 */

    /* 下一首 */
    lv_obj_t *next = lv_button_create(row);                         /* 建按钮 */
    lv_obj_set_size(next, 64, 64);                                  /* 尺寸 */
    lv_obj_set_style_radius(next, LV_RADIUS_CIRCLE, 0);             /* 正圆 */
    lv_obj_set_style_bg_color(next, colors->card_bg, 0);            /* 卡片底色 */
    lv_obj_set_style_shadow_width(next, 0, 0);                      /* 去阴影 */
    lv_obj_add_event_cb(next, on_next_clicked, LV_EVENT_CLICKED, NULL);     /* 点击 */
    lv_obj_t *next_icon = lv_label_create(next);                    /* 图标 */
    lv_label_set_text(next_icon, LV_SYMBOL_NEXT);                   /* ⏭ */
    lv_obj_set_style_text_color(next_icon, colors->text_color, 0);  /* 图标色 */
    lv_obj_set_style_text_font(next_icon, &lv_font_montserrat_24, 0);   /* 大图标 */
    lv_obj_center(next_icon);                                       /* 居中 */

    /* --- 音量行：[音量] ──●── [30] --- */
    lv_obj_t *vol_row = lv_obj_create(area);                        /* 音量行容器 */
    lv_obj_set_size(vol_row, 420, 20);                              /* 固定宽，高=最大子项（9.4 同坑预防） */
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_TRANSP, 0);             /* 透明 */
    lv_obj_set_style_border_width(vol_row, 0, 0);                   /* 无边框 */
    lv_obj_set_style_pad_all(vol_row, 0, 0);                        /* 内边距归零 */
    lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);                /* 横向排布 */
    lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_CENTER,            /* 居中 */
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(vol_row, 12, 0);                    /* 元素间距 */
    lv_obj_clear_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);             /* 不滚动 */

    lv_obj_t *vol_icon = lv_label_create(vol_row);                  /* 音量图标 */
    lv_label_set_text(vol_icon, LV_SYMBOL_VOLUME_MAX);              /* 喇叭图标 */
    lv_obj_set_style_text_color(vol_icon, colors->text_color, 0);   /* 图标色 */
    lv_obj_set_style_text_font(vol_icon, &lv_font_montserrat_14, 0);    /* 小图标 */

    s_music.vol_slider = lv_slider_create(vol_row);                 /* 音量滑块 */
    lv_obj_set_width(s_music.vol_slider, 300);                      /* 固定宽度 */
    lv_obj_set_height(s_music.vol_slider, 12);                      /* 轨道粗细 */
    lv_slider_set_range(s_music.vol_slider, 0, 100);                /* 0~100 */
    lv_slider_set_value(s_music.vol_slider, 30, LV_ANIM_OFF);       /* 初值 30（编排层会同步真实值） */
    lv_obj_set_style_bg_color(s_music.vol_slider, colors->primary_color,
                              LV_PART_INDICATOR);                   /* 已选段主题色 */
    lv_obj_set_style_bg_color(s_music.vol_slider, colors->primary_color,
                              LV_PART_KNOB);                        /* 滑块主题色 */
    lv_obj_add_event_cb(s_music.vol_slider, on_vol_changed,         /* 拖动 → 下发音量 */
                        LV_EVENT_VALUE_CHANGED, NULL);

    s_music.vol_val_label = lv_label_create(vol_row);               /* 音量数值 */
    lv_label_set_text(s_music.vol_val_label, "30");                 /* 与滑块初值一致 */
    lv_obj_set_style_text_color(s_music.vol_val_label, colors->text_color, 0);  /* 文字色 */
    lv_obj_set_style_text_font(s_music.vol_val_label, nino_font_cjk16(), 0);    /* 字体 */
    lv_obj_set_width(s_music.vol_val_label, 40);                    /* 固定宽防抖动 */
}

/**
 * @brief 建底部播放列表容器（列表项由 set_playlist 动态建）
 */
static void create_list_area(lv_obj_t *parent)
{
    s_music.list_area = lv_obj_create(parent);                      /* 列表容器 */
    lv_obj_set_size(s_music.list_area, SCR_WIDTH,
                    SCR_HEIGHT - TOP_BAR_H - MAIN_AREA_H);          /* 占满剩余高度 */
    lv_obj_set_style_bg_opa(s_music.list_area, LV_OPA_TRANSP, 0);   /* 透明 */
    lv_obj_set_style_border_width(s_music.list_area, 0, 0);         /* 无边框 */
    lv_obj_set_style_radius(s_music.list_area, 0, 0);               /* 无圆角 */
    lv_obj_set_style_pad_all(s_music.list_area, 0, 0);              /* 内边距归零 */
    lv_obj_set_style_pad_hor(s_music.list_area, 60, 0);             /* 左右留白 */
    lv_obj_set_style_pad_row(s_music.list_area, 6, 0);              /* 项间距 */
    lv_obj_set_flex_flow(s_music.list_area, LV_FLEX_FLOW_COLUMN);   /* 纵向排布 */
    lv_obj_set_flex_align(s_music.list_area, LV_FLEX_ALIGN_START,   /* 从顶部开始排 */
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_music.list_area, LV_DIR_VER);           /* 曲目多时纵向滚动 */
}

/* ---------- 对外接口 ---------- */

lv_obj_t *scr_music_create(lv_obj_t *parent)
{
    const theme_colors_t *colors = theme_manager_get_colors();      /* 主题色 */

    s_music.container = lv_obj_create(parent);                      /* 页面根容器 */
    lv_obj_set_size(s_music.container, SCR_WIDTH, SCR_HEIGHT);      /* 整屏 */
    lv_obj_set_style_bg_color(s_music.container, colors->bg_color, 0);  /* 背景色 */
    lv_obj_set_style_bg_opa(s_music.container, LV_OPA_COVER, 0);    /* 不透明（遮住主页） */
    lv_obj_set_style_border_width(s_music.container, 0, 0);         /* 无边框 */
    lv_obj_set_style_radius(s_music.container, 0, 0);               /* 无圆角 */
    lv_obj_set_style_pad_all(s_music.container, 0, 0);              /* 内边距归零 */
    lv_obj_set_flex_flow(s_music.container, LV_FLEX_FLOW_COLUMN);   /* 纵向三段式 */
    lv_obj_add_flag(s_music.container, LV_OBJ_FLAG_HIDDEN);         /* 默认隐藏 */
    lv_obj_clear_flag(s_music.container, LV_OBJ_FLAG_SCROLLABLE);   /* 不滚动 */

    create_top_bar(s_music.container);                              /* 1. 顶栏 */
    create_main_area(s_music.container);                            /* 2. 中部控制区 */
    create_list_area(s_music.container);                            /* 3. 底部列表 */

    s_music.cur_idx = -1;                                           /* 初始无当前曲 */
    ESP_LOGI(TAG, "音乐页创建完成");                                 /* 日志 */
    return s_music.container;                                       /* 返回容器 */
}

void scr_music_set_back_cb(void (*cb)(void *ctx), void *ctx)
{
    s_music.back_cb = cb;                                           /* 存回调 */
    s_music.back_ctx = ctx;                                         /* 存上下文 */
}

void scr_music_set_control_cb(void (*cb)(int cmd, int arg, void *ctx), void *ctx)
{
    s_music.ctrl_cb = cb;                                           /* 存回调 */
    s_music.ctrl_ctx = ctx;                                         /* 存上下文 */
}

void scr_music_set_playlist(const char *const *names, int count)
{
    if (s_music.list_area == NULL) {                                /* 页面未建 */
        return;                                                     /* 忽略 */
    }
    /* ---- 1. 清空旧项（LVGL 对象删掉，指针置空防悬挂） ---- */
    for (int i = 0; i < s_music.item_count; i++) {                  /* 遍历已建项 */
        if (s_music.items[i]) {                                     /* 存在才删 */
            lv_obj_delete(s_music.items[i]);                        /* 释放对象（含子标签） */
            s_music.items[i] = NULL;                                /* 指针置空 */
            s_music.item_labels[i] = NULL;                          /* 标签一并置空 */
        }
    }
    s_music.item_count = 0;                                         /* 计数归零 */

    if (names == NULL || count <= 0) {                              /* 空列表 */
        return;                                                     /* 无项可建 */
    }
    if (count > LIST_MAX) {                                         /* 超出静态上限 */
        ESP_LOGW(TAG, "曲目 %d 首超出列表上限 %d，只显示前 %d",       /* 告警 */
                 count, LIST_MAX, LIST_MAX);
        count = LIST_MAX;                                           /* 截断 */
    }

    const theme_colors_t *colors = theme_manager_get_colors();      /* 主题色 */
    /* ---- 2. 逐个建项（按钮 + 序号曲名），索引挂在 user_data 上 ---- */
    for (int i = 0; i < count; i++) {                               /* 遍历曲目 */
        lv_obj_t *item = lv_button_create(s_music.list_area);       /* 列表项按钮 */
        lv_obj_set_size(item, LV_PCT(100), LIST_ITEM_H);            /* 通栏固定高 */
        lv_obj_set_style_bg_color(item, colors->card_bg, 0);        /* 未选中：卡片底色 */
        lv_obj_set_style_radius(item, 10, 0);                       /* 圆角 */
        lv_obj_set_style_shadow_width(item, 0, 0);                  /* 去阴影 */
        lv_obj_set_style_pad_hor(item, 16, 0);                      /* 左右内边距 */
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);               /* 图标+文字横排 */
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START,            /* 左对齐 */
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(item, 8, 0);                    /* 元素间距 */
        lv_obj_add_event_cb(item, on_item_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);                   /* 点击 → 播第 i 项 */

        char buf[96];                                               /* 序号+曲名缓冲 */
        snprintf(buf, sizeof(buf), "%d. %s", i + 1, names[i]);      /* 组装文案 */
        lv_obj_t *label = lv_label_create(item);                    /* 文字标签 */
        lv_label_set_text(label, buf);                              /* 设置文案 */
        lv_obj_set_style_text_color(label, colors->text_color, 0);  /* 常规文字色 */
        lv_obj_set_style_text_font(label, nino_font_cjk16(), 0);    /* 中文字体 */

        s_music.items[i] = item;                                    /* 记录对象 */
        s_music.item_labels[i] = label;                             /* 记录标签 */
    }
    s_music.item_count = count;                                     /* 记项数 */
    ESP_LOGI(TAG, "列表已建 %d 项", count);                          /* 日志 */
}

void scr_music_set_state(bool playing, bool paused, int cur_idx, int pos_sec)
{
    if (s_music.container == NULL) {                                /* 页面未建 */
        return;                                                     /* 忽略 */
    }
    if (lv_obj_has_flag(s_music.container, LV_OBJ_FLAG_HIDDEN)) {   /* 页面不可见 */
        s_music.last_valid = false;                                 /* 缓存作废（回页时全量刷） */
        return;                                                     /* 不做任何绘制 */
    }
    /* ---- 与上次比对：完全一致就不碰 LVGL（主循环 1s 一次，不比对等于每秒全量重绘） ---- */
    bool same = s_music.last_valid &&                              /* 有缓存且 */
                playing == s_music.last_playing &&                 /* 播放态同 */
                paused == s_music.last_paused &&                   /* 暂停态同 */
                cur_idx == s_music.last_idx &&                     /* 当前曲同 */
                pos_sec == s_music.last_pos;                       /* 秒数同 */
    s_music.cur_idx = cur_idx;                                     /* 无论是否重绘都记当前索引（主按钮要用） */
    if (same) {                                                    /* 无需更新 */
        return;                                                    /* 直接返回 */
    }

    const theme_colors_t *colors = theme_manager_get_colors();      /* 主题色 */
    bool idx_changed = (!s_music.last_valid) || (cur_idx != s_music.last_idx);   /* 当前曲是否变了 */

    /* ---- 1. 状态文案 + 主按钮图标 ---- */
    const char *status;                                             /* 状态文案 */
    lv_color_t status_color;                                        /* 状态配色 */
    if (paused) {                                                   /* 暂停中 */
        status = "已暂停";                                           /* 文案 */
        status_color = lv_color_hex(0xE67E22);                      /* 橙 */
        lv_label_set_text(s_music.play_icon, LV_SYMBOL_PLAY);       /* 图标切回 ▶ */
    } else if (playing) {                                           /* 正在播 */
        status = "播放中";                                           /* 文案 */
        status_color = lv_color_hex(0x27AE60);                      /* 绿 */
        lv_label_set_text(s_music.play_icon, LV_SYMBOL_PAUSE);      /* 图标切成 ⏸ */
    } else {                                                        /* 已停止 */
        status = "已停止";                                           /* 文案 */
        status_color = lv_color_hex(0x999999);                      /* 灰 */
        lv_label_set_text(s_music.play_icon, LV_SYMBOL_PLAY);       /* 图标 ▶ */
    }
    lv_label_set_text(s_music.status_label, status);                /* 更新状态文案 */
    lv_obj_set_style_text_color(s_music.status_label, status_color, 0); /* 更新状态配色 */

    /* ---- 2. 曲名（只在换曲时取名字，避免每秒查列表） ---- */
    if (idx_changed) {                                              /* 当前曲变了 */
        const char *name = NULL;                                    /* 曲名 */
        if (cur_idx >= 0 && cur_idx < s_music.item_count) {         /* 索引有效 */
            lv_obj_t *lbl = s_music.item_labels[cur_idx];           /* 对应列表项文字 */
            if (lbl) {                                              /* 存在才取 */
                const char *full = lv_label_get_text(lbl);          /* 形如 "1. 曲名" */
                const char *dot = strchr(full, '.');                /* 跳过编号 */
                name = (dot && dot[1] == ' ') ? dot + 2 : full;     /* 取编号之后的曲名 */
            }
        }
        lv_label_set_text(s_music.track_label, name ? name : "未在播放");    /* 更新曲名 */
    }

    /* ---- 3. 已播时长（缓冲给足：%d 全域保守，避免 format-truncation） ---- */
    if (pos_sec != s_music.last_pos || idx_changed || !s_music.last_valid) {    /* 秒数或曲目变了 */
        char timebuf[48];                                           /* 时长文案缓冲 */
        if (pos_sec < 0) {                                          /* 异常值兜底 */
            pos_sec = 0;                                            /* 归零 */
        }
        snprintf(timebuf, sizeof(timebuf), "已播 %02d:%02d",        /* 分:秒 */
                 pos_sec / 60, pos_sec % 60);
        lv_label_set_text(s_music.pos_label, timebuf);              /* 更新时长 */
    }

    /* ---- 4. 列表高亮（只在换曲时重刷所有项，避免每秒遍历） ---- */
    if (idx_changed) {                                              /* 当前曲变了 */
        for (int i = 0; i < s_music.item_count; i++) {              /* 遍历列表项 */
            lv_obj_t *item = s_music.items[i];                      /* 取项 */
            lv_obj_t *lbl = s_music.item_labels[i];                 /* 取文字 */
            if (!item || !lbl) {                                    /* 防御 */
                continue;                                           /* 跳过 */
            }
            bool active = (i == cur_idx);                           /* 是否当前曲 */
            lv_obj_set_style_bg_color(item,                         /* 项底色 */
                                      active ? colors->primary_color : colors->card_bg, 0);
            lv_obj_set_style_text_color(lbl,                        /* 文字色 */
                                        active ? lv_color_hex(0xFFFFFF) : colors->text_color, 0);
        }
    }

    /* ---- 5. 写回缓存 ---- */
    s_music.last_playing = playing;                                 /* 记播放态 */
    s_music.last_paused = paused;                                   /* 记暂停态 */
    s_music.last_idx = cur_idx;                                     /* 记当前曲 */
    s_music.last_pos = pos_sec;                                     /* 记秒数 */
    s_music.last_valid = true;                                      /* 缓存有效 */
}

void scr_music_set_volume(int vol)
{
    if (s_music.vol_slider == NULL) {                               /* 页面未建 */
        return;                                                     /* 忽略 */
    }
    if (vol < 0) {                                                  /* 下限夹紧 */
        vol = 0;                                                    /* 归零 */
    } else if (vol > 100) {                                         /* 上限夹紧 */
        vol = 100;                                                  /* 拉满 */
    }
    s_music.syncing_vol = true;                                     /* 抑制回调（程序设值不发命令） */
    lv_slider_set_value(s_music.vol_slider, vol, LV_ANIM_OFF);      /* 设滑块位置 */
    char buf[8];                                                    /* 数值缓冲 */
    snprintf(buf, sizeof(buf), "%d", vol);                          /* 格式化 */
    lv_label_set_text(s_music.vol_val_label, buf);                  /* 同步数值文案 */
    s_music.syncing_vol = false;                                    /* 恢复回调 */
}
