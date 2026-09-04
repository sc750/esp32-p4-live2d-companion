/**
 * @file    rig_render.h
 * @brief   rig 软件渲染接口（L4）——图层 blit + alpha 合成
 *
 * 输出像素格式 = LVGL ARGB8888 内存布局（小端字节序 B,G,R,A），
 * 渲染结果可直接作为 lv_image_dsc_t 数据源，零二次转换。
 *
 * @date    2026-09-04
 * @version 1.0.0
 */

#ifndef RIG_RENDER_H
#define RIG_RENDER_H

#include <stdint.h>
#include "esp_err.h"
#include "rig_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 渲染目标表面（ARGB8888，LVGL 内存布局） */
typedef struct {
    uint8_t *buf;   /* 不拥有内存，由调用方/rig_surface_init 分配 */
    int      w, h;
} rig_surface_t;

/** 分配 surface 后备缓冲（PSRAM，rig_mem） */
esp_err_t rig_surface_init(rig_surface_t *s, int w, int h);

/** 释放 surface 后备缓冲 */
void rig_surface_deinit(rig_surface_t *s);

/** 清屏为全透明 */
void rig_surface_clear(const rig_surface_t *s);

/**
 * @brief 渲染模型全部图层（按 z 升序，基准位姿，v1 无变换）
 *
 * 线程模型：调用方保证单线程独占（渲染任务内调用）。
 */
void rig_render_model(const rig_surface_t *dst, const rig_model_t *m);

#ifdef __cplusplus
}
#endif

#endif /* RIG_RENDER_H */
