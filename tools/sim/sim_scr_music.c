/**
 * sim_scr_music.c —— scr_music.c 的模拟器 1:1 复刻（日主题）
 * 差异说明：模拟器无中文点阵字体，nino_font_cjk16() 桩映射到 montserrat_16，
 * 中文文案以英文占位（布局/颜色/尺寸/交互对象与板上代码一致）。
 */
#include "lvgl.h"

/* 板上 nino_font_cjk16() 的桩：模拟器无中文字体 */
static const lv_font_t *nino_font_cjk16(void) { return &lv_font_montserrat_16; }

#define SCR_WIDTH   1024
#define SCR_HEIGHT  600
#define TOP_BAR_H   56
#define MAIN_AREA_H 336
#define LIST_ITEM_H 44

/* 日主题色（theme_manager.c 实测值） */
#define C_BG        lv_color_hex(0xF5F5F5)
#define C_TEXT      lv_color_hex(0x333333)
#define C_PRIMARY   lv_color_hex(0x4A90D9)
#define C_CARD      lv_color_hex(0xFFFFFF)
#define C_BAR       lv_color_hex(0xE8E8E8)

/* 模块句柄（复刻 s_music 中要被 set_state 演示用到的部分） */
static lv_obj_t *track_label, *pos_label, *play_icon, *status_label, *vol_val;
static lv_obj_t *items[3];

/* ---- 顶栏：[Back]  ♪ Music   [Playing] ---- */
static void create_top_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, SCR_WIDTH, TOP_BAR_H);
    lv_obj_set_style_bg_color(bar, C_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 16, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(bar);
    lv_obj_set_size(back, 96, 40);
    lv_obj_set_style_bg_color(back, C_CARD, 0);
    lv_obj_set_style_radius(back, 20, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_t *bt = lv_label_create(back);
    lv_label_set_text(bt, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(bt, C_TEXT, 0);
    lv_obj_set_style_text_font(bt, nino_font_cjk16(), 0);
    lv_obj_center(bt);

    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, LV_SYMBOL_AUDIO " Music");
    lv_obj_set_style_text_color(title, C_TEXT, 0);
    lv_obj_set_style_text_font(title, nino_font_cjk16(), 0);

    status_label = lv_label_create(bar);
    lv_label_set_text(status_label, "Playing");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x27AE60), 0);
    lv_obj_set_style_text_font(status_label, nino_font_cjk16(), 0);
}

/* ---- 中部：曲名 + 时长 + 三按钮 + 音量 ---- */
static void create_main_area(lv_obj_t *parent)
{
    lv_obj_t *area = lv_obj_create(parent);
    lv_obj_set_size(area, SCR_WIDTH, MAIN_AREA_H);
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(area, 0, 0);
    lv_obj_set_style_radius(area, 0, 0);
    lv_obj_set_style_pad_all(area, 0, 0);
    lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(area, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(area, 14, 0);
    lv_obj_clear_flag(area, LV_OBJ_FLAG_SCROLLABLE);

    track_label = lv_label_create(area);
    lv_label_set_text(track_label, "Enviro Electronic");
    lv_obj_set_style_text_color(track_label, C_PRIMARY, 0);
    lv_obj_set_style_text_font(track_label, nino_font_cjk16(), 0);

    pos_label = lv_label_create(area);
    lv_label_set_text(pos_label, "Played 00:41");
    lv_obj_set_style_text_color(pos_label, C_TEXT, 0);
    lv_obj_set_style_text_font(pos_label, nino_font_cjk16(), 0);

    lv_obj_t *row = lv_obj_create(area);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 36, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *prev = lv_button_create(row);
    lv_obj_set_size(prev, 64, 64);
    lv_obj_set_style_radius(prev, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(prev, C_CARD, 0);
    lv_obj_set_style_shadow_width(prev, 0, 0);
    lv_obj_t *pi = lv_label_create(prev);
    lv_label_set_text(pi, LV_SYMBOL_PREV);
    lv_obj_set_style_text_color(pi, C_TEXT, 0);
    lv_obj_set_style_text_font(pi, &lv_font_montserrat_24, 0);
    lv_obj_center(pi);

    lv_obj_t *play = lv_button_create(row);
    lv_obj_set_size(play, 84, 84);
    lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(play, C_PRIMARY, 0);
    lv_obj_set_style_shadow_width(play, 0, 0);
    play_icon = lv_label_create(play);
    lv_label_set_text(play_icon, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_color(play_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(play_icon, &lv_font_montserrat_24, 0);
    lv_obj_center(play_icon);

    lv_obj_t *next = lv_button_create(row);
    lv_obj_set_size(next, 64, 64);
    lv_obj_set_style_radius(next, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(next, C_CARD, 0);
    lv_obj_set_style_shadow_width(next, 0, 0);
    lv_obj_t *ni = lv_label_create(next);
    lv_label_set_text(ni, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_color(ni, C_TEXT, 0);
    lv_obj_set_style_text_font(ni, &lv_font_montserrat_24, 0);
    lv_obj_center(ni);

    lv_obj_t *vol_row = lv_obj_create(area);
    lv_obj_set_size(vol_row, 420, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vol_row, 0, 0);
    lv_obj_set_style_pad_all(vol_row, 0, 0);
    lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(vol_row, 12, 0);
    lv_obj_clear_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *vi = lv_label_create(vol_row);
    lv_label_set_text(vi, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(vi, C_TEXT, 0);
    lv_obj_set_style_text_font(vi, &lv_font_montserrat_14, 0);

    lv_obj_t *slider = lv_slider_create(vol_row);
    lv_obj_set_width(slider, 300);
    lv_obj_set_height(slider, 12);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 30, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, C_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, C_PRIMARY, LV_PART_KNOB);

    vol_val = lv_label_create(vol_row);
    lv_label_set_text(vol_val, "30");
    lv_obj_set_style_text_color(vol_val, C_TEXT, 0);
    lv_obj_set_style_text_font(vol_val, nino_font_cjk16(), 0);
    lv_obj_set_width(vol_val, 40);
}

/* ---- 底部列表：3 个电台，第 1 个高亮（复刻 set_playlist + 高亮态） ---- */
static void create_list_area(lv_obj_t *parent)
{
    lv_obj_t *area = lv_obj_create(parent);
    lv_obj_set_size(area, SCR_WIDTH, SCR_HEIGHT - TOP_BAR_H - MAIN_AREA_H);
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(area, 0, 0);
    lv_obj_set_style_radius(area, 0, 0);
    lv_obj_set_style_pad_all(area, 0, 0);
    lv_obj_set_style_pad_hor(area, 60, 0);
    lv_obj_set_style_pad_row(area, 6, 0);
    lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(area, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(area, LV_DIR_VER);

    static const char *names[3] = {
        "Enviro Electronic", "Piano Jazz", "Ambient Space" };
    for (int i = 0; i < 3; i++) {
        bool active = (i == 0);                     /* 当前曲高亮 */
        lv_obj_t *item = lv_button_create(area);
        lv_obj_set_size(item, LV_PCT(100), LIST_ITEM_H);
        lv_obj_set_style_bg_color(item, active ? C_PRIMARY : C_CARD, 0);
        lv_obj_set_style_radius(item, 10, 0);
        lv_obj_set_style_shadow_width(item, 0, 0);
        lv_obj_set_style_pad_hor(item, 16, 0);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(item, 8, 0);
        char buf[96];
        snprintf(buf, sizeof(buf), "%d. %s", i + 1, names[i]);
        lv_obj_t *label = lv_label_create(item);
        lv_label_set_text(label, buf);
        lv_obj_set_style_text_color(label,
                                    active ? lv_color_hex(0xFFFFFF) : C_TEXT, 0);
        lv_obj_set_style_text_font(label, nino_font_cjk16(), 0);
        items[i] = item;
    }
}

void create_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, C_BG, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    create_top_bar(screen);
    create_main_area(screen);
    create_list_area(screen);
}
