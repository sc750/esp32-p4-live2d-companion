/**
 * @file    rig_rig.h
 * @brief   rig 参数动画引擎（L4）——呼吸/眨眼/口型/头部跟随
 *
 * 参数→图层姿态（rig_pose）每帧求值；渲染任务以固定节拍调用
 * rig_rig_tick 取姿态后交给 rig_render_pose。
 *
 * @date    2026-09-04
 * @version 1.0.0
 */

#ifndef RIG_RIG_H
#define RIG_RIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "rig_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 口型级别（对应 rigbin 里的 mouth_half / mouth_open 补丁） */
typedef enum {
    RIG_MOUTH_CLOSED = 0,
    RIG_MOUTH_HALF   = 1,
    RIG_MOUTH_OPEN   = 2,
} rig_mouth_t;

/** 初始化（按图层名绑定 eyes_closed/mouth_half/mouth_open，缺失则相应功能降级） */
esp_err_t rig_rig_init(const rig_model_t *m);

/**
 * @brief 每帧求值姿态
 * @param now_ms 毫秒时基（任意单调起点）
 */
void rig_rig_tick(uint32_t now_ms, rig_pose_t *out_pose);

/**
 * @brief 触摸跟随目标（期望的头部位移，px，通常按屏幕坐标比例算出）
 * @param dx 期望水平位移（如 (touch_x - 屏宽/2) * 幅度/屏宽）
 * @param dy 期望垂直位移（同比例）
 * @param active false=结束触摸，回到随机游走
 */
void rig_rig_set_touch(int16_t dx, int16_t dy, bool active);

/** 外部口型驱动（TTS 接入后由音频层调用；未驱动时内部演示循环接管） */
void rig_rig_set_mouth(rig_mouth_t level);

#ifdef __cplusplus
}
#endif

#endif /* RIG_RIG_H */
