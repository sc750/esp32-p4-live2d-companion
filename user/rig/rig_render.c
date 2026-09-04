/**
 * @file    rig_render.c
 * @brief   rig 软件渲染实现（L4）——z 序图层 blit + 每·通道 alpha 合成
 *
 * 性能注记（v1 静态帧）：
 *   - 逐像素分支：a==0 跳过 / a>=255 直拷 / 中间 alpha 定点混合
 *   - atlas 与 dst 均在 PSRAM；P4 250MHz PSRAM 下 179K 像素毫秒级
 *
 * @date    2026-09-04
 * @version 1.0.0
 */

#include "rig_render.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"

#include "rig_mem.h"

#define TAG "rig"

/* ---- 32bit ARGB8888 打包访问（LVGL 布局：小端字节 B,G,R,A = 0xAARRGGBB） ---- */
typedef union {
    uint32_t v;
    struct __attribute__((packed)) {
        uint8_t b, g, r, a;
    };
} rig_px_t;

esp_err_t rig_surface_init(rig_surface_t *s, int w, int h)
{
    ESP_RETURN_ON_FALSE(s && w > 0 && h > 0, ESP_ERR_INVALID_ARG, TAG, "bad arg");
    s->w = w;
    s->h = h;
    s->buf = rig_mem_alloc((size_t)w * h * 4);
    ESP_RETURN_ON_FALSE(s->buf, ESP_ERR_NO_MEM, TAG, "surface alloc %dx%d 失败", w, h);
    rig_surface_clear(s);
    return ESP_OK;
}

void rig_surface_deinit(rig_surface_t *s)
{
    if (s && s->buf) {
        rig_mem_free(s->buf);
        s->buf = NULL;
    }
}

void rig_surface_clear(const rig_surface_t *s)
{
    if (s && s->buf) {
        memset(s->buf, 0, (size_t)s->w * s->h * 4);
    }
}

/** 按像素 alpha 合成（dst_over 不适用；标准 src-over） */
static inline void blit_row(rig_px_t *dst, const uint8_t *src, int n)
{
    for (int i = 0; i < n; i++, src += 4, dst++) {
        const uint8_t a = src[3];
        if (a == 0) {
            continue;
        }
        if (a >= 255) {
            dst->r = src[0];
            dst->g = src[1];
            dst->b = src[2];
            dst->a = 255;
            continue;
        }
        /* out = src*a + dst*(255-a)，四舍五入 */
        const uint16_t ia = 255 - a;
        dst->r = (uint8_t)((src[0] * a + dst->r * ia + 127) >> 8);
        dst->g = (uint8_t)((src[1] * a + dst->g * ia + 127) >> 8);
        dst->b = (uint8_t)((src[2] * a + dst->b * ia + 127) >> 8);
        dst->a = (uint8_t)(a + ((dst->a * ia + 127) >> 8));
    }
}

/** z 升序索引表（层数 ≤16，插入排序足够） */
static void build_z_order(const rig_model_t *m, uint8_t *order)
{
    for (int i = 0; i < m->layer_count; i++) {
        order[i] = (uint8_t)i;
    }
    for (int i = 1; i < m->layer_count; i++) {
        const uint8_t key = order[i];
        const int16_t kz = m->layers[key].z;
        int j = i - 1;
        while (j >= 0 && m->layers[order[j]].z > kz) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
}

void rig_render_model(const rig_surface_t *dst, const rig_model_t *m)
{
    rig_render_pose(dst, m, NULL);
}

void rig_render_pose(const rig_surface_t *dst, const rig_model_t *m,
                     const rig_pose_t *pose)
{
    if (!dst || !dst->buf || !m || !m->loaded) {
        return;
    }

    uint8_t order[RIG_MAX_LAYERS];
    build_z_order(m, order);

    for (int oi = 0; oi < m->layer_count; oi++) {
        const uint8_t li = order[oi];
        const rig_layer_t *L = &m->layers[li];

        /* 补丁层可见性（pose 提供时默认隐藏，由动画显式点亮） */
        if (pose && !pose->visible[li]) {
            continue;
        }
        int16_t dx = pose ? pose->dx[li] : 0;
        int16_t dy = pose ? pose->dy[li] : 0;

        /* 画布位置 = base + 姿态偏移；裁剪到 surface 边界 */
        int x0 = L->base_x + dx;
        int y0 = L->base_y + dy;
        int x1 = x0 + L->atlas_w;
        int y1 = y0 + L->atlas_h;
        int sx = 0, sy = 0;                     /* atlas 内裁剪起点 */
        if (x0 < 0) { sx = -x0; x0 = 0; }
        if (y0 < 0) { sy = -y0; y0 = 0; }
        if (x1 > dst->w) { x1 = dst->w; }
        if (y1 > dst->h) { y1 = dst->h; }
        if (x0 >= x1 || y0 >= y1) {
            continue;
        }
        const int cw = x1 - x0;

        const uint8_t *src = m->atlas +
            ((size_t)(L->atlas_y + sy) * m->atlas_w + L->atlas_x + sx) * 4;
        rig_px_t *d = (rig_px_t *)(dst->buf + ((size_t)y0 * dst->w + x0) * 4);

        for (int row = y0; row < y1; row++) {
            blit_row(d, src, cw);
            d += dst->w;
            src += (size_t)m->atlas_w * 4;
        }
    }
}
