/**
 * @file    rig_lvgl.h
 * @brief   rig → LVGL 桥接（L5）——角色 surface 包装为 lv_image 显示
 *
 * @date    2026-09-04
 * @version 1.0.0
 */

#ifndef RIG_LVGL_H
#define RIG_LVGL_H

#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
#include "rig_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 在 parent 上创建角色显示控件并渲染第一帧
 *
 * 内部持 rig_lv_adapter 锁；渲染目标 = 模型画布同尺寸 ARGB8888 surface，
 * 以 lv_image 呈现（保持宽高比缩放到目标高度，底部居中）。
 *
 * @param parent    LVGL 父对象（如 lv_layer_top()）
 * @param m         已加载模型
 * @param fit_h     目标适配高度（像素，如屏幕高 600）
 * @return lv_obj_t* image 对象；失败 NULL
 */
lv_obj_t *rig_lvgl_create(lv_obj_t *parent, const rig_model_t *m, int fit_h);

/**
 * @brief 启动渲染任务（Core 1，~30fps）
 * @param fps 目标帧率（1~60，0/非法按 30）
 */
esp_err_t rig_lvgl_start(int fps);

#ifdef __cplusplus
}
#endif

#endif /* RIG_LVGL_H */
