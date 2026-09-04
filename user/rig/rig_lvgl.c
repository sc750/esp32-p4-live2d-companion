/**
 * @file    rig_lvgl.c
 * @brief   rig → LVGL 桥接实现（L5）
 *
 * surface 内存布局与 LVGL ARGB8888 一致（rig_render 输出即用），
 * 包装为 lv_image_dsc_t 静态描述符，逐帧只需 invalidate。
 *
 * @date    2026-09-04
 * @version 1.0.0
 */

#include "rig_lvgl.h"

#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lv_adapter.h"

#include "rig_mem.h"
#include "rig_render.h"

#define TAG "rig_lvgl"

typedef struct {
    rig_surface_t   surface;
    lv_image_dsc_t  dsc;
    lv_obj_t       *img;
} rig_lvgl_ctx_t;

static rig_lvgl_ctx_t s_ctx;

lv_obj_t *rig_lvgl_create(lv_obj_t *parent, const rig_model_t *m, int fit_h)
{
    ESP_RETURN_ON_FALSE(parent && m && m->loaded, NULL, TAG, "bad arg");

    /* LVGL UI 互斥：adapter 任务在跑，创建/渲染需持锁 */
    esp_lv_adapter_lock(-1);

    if (rig_surface_init(&s_ctx.surface, m->canvas_w, m->canvas_h) != ESP_OK) {
        esp_lv_adapter_unlock();
        return NULL;
    }
    rig_render_model(&s_ctx.surface, m);

    memset(&s_ctx.dsc, 0, sizeof(s_ctx.dsc));
    s_ctx.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_ctx.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    s_ctx.dsc.header.w = (uint32_t)m->canvas_w;
    s_ctx.dsc.header.h = (uint32_t)m->canvas_h;
    s_ctx.dsc.data = s_ctx.surface.buf;
    s_ctx.dsc.data_size = (uint32_t)m->canvas_w * m->canvas_h * 4;

    s_ctx.img = lv_image_create(parent);
    if (!s_ctx.img) {
        rig_surface_deinit(&s_ctx.surface);
        esp_lv_adapter_unlock();
        return NULL;
    }
    lv_image_set_src(s_ctx.img, &s_ctx.dsc);

    /* 对象盒 = 屏幕可用高度，内容 CONTAIN 等比适配（自动缩放+居中）。
     * 注意：lv_image_set_scale 不改变对象布局尺寸，直接 scale+align
     * 会导致顶部溢出屏幕（R3 实测教训）。 */
    lv_obj_set_size(s_ctx.img, m->canvas_w, fit_h);
    lv_image_set_inner_align(s_ctx.img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(s_ctx.img, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_ctx.img, LV_OBJ_FLAG_CLICKABLE);  /* 允许后续触摸交互 */

    esp_lv_adapter_unlock();
    ESP_LOGI(TAG, "角色 lv_image 就绪 (%dx%d → fit_h=%d)",
             m->canvas_w, m->canvas_h, fit_h);
    return s_ctx.img;
}
