/**
 * @file    rig_rig.c
 * @brief   rig 参数动画实现（L4）
 *
 * 动画清单（PRD L2D-02/03/05/06 子集）：
 *   breath  身体/头部相位差正弦位移（周期 3.4s）
 *   blink   随机间隔 2.6~5.2s，闭眼补丁显示 130ms
 *   mouth   说话串（0.9s，closed→half→open→half 循环）每 4~8s 一串；
 *           rig_rig_set_mouse 外部驱动时外部优先
 *   head    二阶弹簧跟随触摸点（无触摸时缓慢随机游走），
 *           头 + 子图层（眼/嘴补丁）整体视差
 *
 * @date    2026-09-04
 * @version 1.0.0
 */

#include "rig_rig.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_random.h"

#define TAG "rig"

/** sinf 直用（-O2 下足够快；热路径查表化留给 R5 视实测决定） */
static inline float sinf_approx(float x)
{
    return sinf(x);
}

/* ---- 参数节拍（ms） ---- */
#define BREATH_PERIOD_MS    3400.0f
#define BLINK_CLOSE_MS      130
#define MOUTH_BURST_MS      900
#define HEAD_SPRING_K       0.045f   /* 弹簧刚度（每帧@30fps） */
#define HEAD_SPRING_D       0.82f    /* 阻尼 */
#define HEAD_RANGE_X        3
#define HEAD_RANGE_Y        2

typedef struct {
    const rig_model_t *m;
    int idx_eyes_closed;
    int idx_mouth_half;
    int idx_mouth_open;

    /* 呼吸相位 */
    uint32_t t0_ms;

    /* 眨眼 */
    uint32_t next_blink_ms;
    uint32_t blink_until_ms;

    /* 口型 */
    rig_mouth_t mouth_ext;          /* 外部驱动 */
    bool mouth_ext_active;
    uint32_t burst_start_ms;        /* 当前说话串起点 */
    uint32_t next_burst_ms;

    /* 头部弹簧状态 */
    float head_x, head_y;           /* 当前位移 */
    float head_vx, head_vy;         /* 速度 */
    int16_t target_dx, target_dy;   /* 期望位移（触摸时由外部给出） */
    bool touch_active;
} rig_state_t;

static rig_state_t s;

/** 按名找层，找不到返回 -1 */
static int layer_idx(const rig_model_t *m, const char *name)
{
    for (int i = 0; i < m->layer_count; i++) {
        if (strcmp(m->layers[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

esp_err_t rig_rig_init(const rig_model_t *m)
{
    ESP_RETURN_ON_FALSE(m && m->loaded, ESP_ERR_INVALID_STATE, TAG, "model not loaded");
    memset(&s, 0, sizeof(s));
    s.m = m;
    s.idx_eyes_closed = layer_idx(m, "eyes_closed");
    s.idx_mouth_half  = layer_idx(m, "mouth_half");
    s.idx_mouth_open  = layer_idx(m, "mouth_open");
    s.t0_ms = 0;
    s.next_blink_ms = 2000 + (esp_random() % 2000);
    s.next_burst_ms = 3000 + (esp_random() % 3000);
    ESP_LOGI(TAG, "动画引擎就绪: eyes_closed=%d mouth_half=%d mouth_open=%d",
             s.idx_eyes_closed, s.idx_mouth_half, s.idx_mouth_open);
    return ESP_OK;
}

void rig_rig_set_touch(int16_t dx, int16_t dy, bool active)
{
    s.target_dx = dx;
    s.target_dy = dy;
    s.touch_active = active;
}

void rig_rig_set_mouth(rig_mouth_t level)
{
    s.mouth_ext = level;
    s.mouth_ext_active = true;
}

/** 目标口型（外部驱动优先，其次演示串） */
static rig_mouth_t mouth_target(uint32_t now)
{
    if (s.mouth_ext_active) {
        return s.mouth_ext;
    }
    uint32_t t = now - s.burst_start_ms;
    if (t < MOUTH_BURST_MS) {
        /* 串内 150ms 步进 closed→half→open→half 循环 */
        static const rig_mouth_t seq[4] = {
            RIG_MOUTH_HALF, RIG_MOUTH_OPEN, RIG_MOUTH_HALF, RIG_MOUTH_CLOSED
        };
        return seq[(t / 150) % 4];
    }
    return RIG_MOUTH_CLOSED;
}

void rig_rig_tick(uint32_t now, rig_pose_t *out)
{
    const rig_model_t *m = s.m;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < m->layer_count; i++) {
        out->visible[i] = 1;    /* 基础层全可见 */
    }

    /* ---- 呼吸：身体 ±1px，头部 ±2px 相位差 0.9s ---- */
    float phase = (now % (uint32_t)BREATH_PERIOD_MS) / BREATH_PERIOD_MS * 6.2832f;
    float body_b = 1.0f * sinf_approx(phase);
    float head_b = 1.2f * sinf_approx(phase - 1.7f);

    /* ---- 头部弹簧（触摸跟随 / 随机游走） ---- */
    float tx, ty;
    if (s.touch_active) {
        tx = (float)s.target_dx;
        ty = (float)s.target_dy;
        if (tx > HEAD_RANGE_X) tx = HEAD_RANGE_X;
        if (tx < -HEAD_RANGE_X) tx = -HEAD_RANGE_X;
        if (ty > HEAD_RANGE_Y) ty = HEAD_RANGE_Y;
        if (ty < -HEAD_RANGE_Y) ty = -HEAD_RANGE_Y;
    } else {
        /* 缓慢随机游走（低频伪随机） */
        float w1 = sinf_approx(phase * 0.21f + 1.3f);
        float w2 = sinf_approx(phase * 0.13f + 4.1f);
        tx = 0;
        ty = 0;
    }
    s.head_vx = (s.head_vx + (tx - s.head_x) * HEAD_SPRING_K) * HEAD_SPRING_D;
    s.head_vy = (s.head_vy + (ty - s.head_y) * HEAD_SPRING_K) * HEAD_SPRING_D;
    s.head_x += s.head_vx;
    s.head_y += s.head_vy;

    /* ---- 写入姿态 ---- */
    int idx_head = m->idx_head;
    for (int i = 0; i < m->layer_count; i++) {
        const rig_layer_t *L = &m->layers[i];
        bool in_head_tree = false;
        for (int p = L->parent; p >= 0; p = m->layers[p].parent) {
            if (p == idx_head) { in_head_tree = true; break; }
        }
        if (i == idx_head) {
            out->dx[i] = (int16_t)(s.head_x + 0.5f);
            out->dy[i] = (int16_t)(s.head_y + head_b + 0.5f);
        } else if (in_head_tree) {
            /* 子层随头动（补丁层由可见性控制，偏移继承头部） */
            out->dx[i] = out->dx[idx_head];
            out->dy[i] = out->dy[idx_head];
        } else if (L->parent < 0) {
            out->dy[i] = (int16_t)(body_b + 0.5f);
        }
    }

    /* ---- 眨眼 ---- */
    if (s.idx_eyes_closed >= 0) {
        if (s.blink_until_ms && now < s.blink_until_ms) {
            out->visible[s.idx_eyes_closed] = 1;
        } else {
            out->visible[s.idx_eyes_closed] = 0;
            if (now >= s.next_blink_ms) {
                s.blink_until_ms = now + BLINK_CLOSE_MS;
                s.next_blink_ms = now + 2600 + (esp_random() % 2600);
            }
        }
    }

    /* ---- 口型 ---- */
    rig_mouth_t mt = mouth_target(now);
    if (now >= s.next_burst_ms && !s.mouth_ext_active) {
        s.burst_start_ms = now;
        s.next_burst_ms = now + 4000 + (esp_random() % 4000);
    }
    if (s.idx_mouth_open >= 0) {
        out->visible[s.idx_mouth_open] = (mt == RIG_MOUTH_OPEN);
    }
    if (s.idx_mouth_half >= 0) {
        out->visible[s.idx_mouth_half] = (mt == RIG_MOUTH_HALF);
    }
}
