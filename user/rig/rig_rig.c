/**
 * @file    rig_rig.c
 * @brief   rig 参数动画实现（L4）
 *
 * 动画清单：
 *   breath    身体/头部相位差正弦位移（周期 3.4s），让角色"活着"
 *   blink     随机间隔 2.6~5.2s，闭眼补丁显示 130ms
 *   mouth     idle 说话串（0.9s，half→open→half→closed 循环）每 4~8s 一串；
 *             rig_rig_set_mouth 外部驱动时外部优先（TTS 接入后用）
 *   expr      触摸表情状态机（R6 新增）：rig_rig_trigger() 点播，
 *             持续期内接管眼/嘴/腮红可见性 + 头部俯仰/摇晃，
 *             并抑制眨眼和 idle 口型串，到期自动回待机
 *
 * 历史包袱清理（R6）：
 *   头部二阶弹簧触摸跟随已删除——2D 分层 rig 平移头部做"转头"
 *   效果太假（用户实测反馈），头部位移现在只来自 呼吸 + 表情俯仰/摇晃。
 *
 * @date    2026-09-04
 * @version 2.0.0
 */

#include "rig_rig.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_random.h"
#include "esp_timer.h"

#define TAG "rig"

/** sinf 直用（-O2 下足够快；热路径查表化留给后续视实测决定） */
static inline float sinf_approx(float x)
{
    return sinf(x);
}

/* ---- 待机动画节拍 ---- */
#define BREATH_PERIOD_MS    3400.0f     /* 呼吸周期：3.4s 一个来回 */
#define BLINK_CLOSE_MS      130         /* 眨眼闭眼时长 */
#define MOUTH_BURST_MS      900         /* idle 说话串长度 */

/**
 * 表情定义表：每种触摸反应 = 眼/嘴/腮红图层组合 + 头部动作
 *
 * 字段说明：
 *   eyes_layer   眼睛变体补丁层名，NULL=不动眼睛（保留眨眼底层）
 *   mouth_layer  嘴变体补丁层名，NULL=不动嘴
 *   blush        是否叠画腮红层（害羞系专属待遇）
 *   head_dy      头部附加 Y 偏移：正=低头（害羞），负=抬头（惊讶），0=不动
 *   sway_amp     左右摇晃幅度 px，0=不摇（蹭头/闹脾气用）
 *   sway_ms      摇晃一个左右来回的周期 ms（amp=0 时忽略）
 *   duration_ms  一次性表情的持续时长；按住类表情给短时长（~300ms），
 *                靠调用方每帧重复触发 rig_rig_trigger() 续命
 */
typedef struct {
    const char *eyes_layer;
    const char *mouth_layer;
    bool        blush;
    int16_t     head_dy;
    int16_t     sway_amp;
    uint16_t    sway_ms;
    uint32_t    duration_ms;
} rig_expr_def_t;

static const rig_expr_def_t s_expr_defs[RIG_EXPR_MAX] = {
    /* eyes_layer    mouth_layer   blush  head_dy sway_amp sway_ms duration */
    [RIG_EXPR_NONE]     = { NULL,         NULL,         false,    0,  0,    0,      0 },
    /* 害羞低头：笑眼 + 腮红 + 头下移 4px，2 秒后恢复 */
    [RIG_EXPR_SHY]      = { "eyes_smile", NULL,         true,   +4,  0,    0,   2000 },
    /* 蹭头：闭眼享受 + 腮红 + 头下移 + 左右轻晃；按住期间由触摸层续期 */
    [RIG_EXPR_PAT]      = { "eyes_closed", NULL,        true,   +4,  2,  900,   300 },
    /* 惊讶：圆眼 + 头上抬 2px（缩了一下），1.2 秒 */
    [RIG_EXPR_SURPRISE] = { "eyes_wide",  NULL,         false,  -2,  0,    0,   1200 },
    /* 大笑：笑眼 + 张嘴，1.5 秒 */
    [RIG_EXPR_LAUGH]    = { "eyes_smile", "mouth_open", false,   0,  0,    0,   1500 },
    /* 闹脾气：嘟嘴 + 摇头（"哼！"），按住期间续期 */
    [RIG_EXPR_POUT]     = { NULL,         "mouth_pout", false,   0,  3,  700,    300 },
};

/** 引擎全局状态（仅渲染任务上下文读写，无锁） */
typedef struct {
    const rig_model_t *m;

    /* ---- 补丁层索引（-1 = 该层不存在，功能降级） ----
     * "补丁层" = 差分切出来的变体小块（挂 head 下），默认隐藏，
     * 由 眨眼/口型/表情 逻辑按需点亮。 */
    int idx_eyes_closed;    /* 闭眼（眨眼 + 蹭头共用） */
    int idx_eyes_smile;     /* 笑眼 ^_^（害羞/大笑） */
    int idx_eyes_wide;      /* 惊讶圆眼 */
    int idx_mouth_half;     /* 嘴微张（idle 说话串） */
    int idx_mouth_open;     /* 嘴大开（说话/大笑） */
    int idx_mouth_pout;     /* 嘟嘴 */
    int idx_blush;          /* 腮红 */

    /* ---- 眨眼节拍 ---- */
    uint32_t next_blink_ms;     /* 下次眨眼的时刻 */
    uint32_t blink_until_ms;    /* 本次闭眼演到几点（0=没在眨） */

    /* ---- 口型 ---- */
    rig_mouth_t mouth_ext;          /* 外部驱动口型（TTS） */
    bool mouth_ext_active;          /* 外部驱动是否生效 */
    uint32_t burst_start_ms;        /* 当前 idle 说话串起点 */
    uint32_t next_burst_ms;         /* 下一串什么时候开始 */
    uint32_t speak_start_ms;        /* 按需说话（rig_rig_speak）起点 */
    uint32_t speak_until_ms;        /* 按需说话截止时刻（0=没在说） */

    /* ---- 表情状态机 ---- */
    rig_expr_t expr;            /* 当前表情（NONE=待机） */
    uint32_t expr_until_ms;     /* 表情演到几点 */
    uint32_t expr_start_ms;     /* 表情开始时刻（算摇晃相位用） */
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

    /* 绑定全部补丁层：缺层只降级不报错（旧 rigbin 兼容） */
    s.idx_eyes_closed = layer_idx(m, "eyes_closed");
    s.idx_eyes_smile  = layer_idx(m, "eyes_smile");
    s.idx_eyes_wide   = layer_idx(m, "eyes_wide");
    s.idx_mouth_half  = layer_idx(m, "mouth_half");
    s.idx_mouth_open  = layer_idx(m, "mouth_open");
    s.idx_mouth_pout  = layer_idx(m, "mouth_pout");
    s.idx_blush       = layer_idx(m, "blush");

    /* 眨眼/说话串的第一次触发时刻随机化，避免每次开机节奏一模一样 */
    s.next_blink_ms = 2000 + (esp_random() % 2000);
    s.next_burst_ms = 3000 + (esp_random() % 3000);

    ESP_LOGI(TAG, "动画引擎就绪: eyes[闭%d 笑%d 惊%d] mouth[半%d 开%d 嘟%d] 腮红%d",
             s.idx_eyes_closed, s.idx_eyes_smile, s.idx_eyes_wide,
             s.idx_mouth_half, s.idx_mouth_open, s.idx_mouth_pout, s.idx_blush);
    return ESP_OK;
}

void rig_rig_trigger(rig_expr_t expr)
{
    if (expr < RIG_EXPR_NONE || expr >= RIG_EXPR_MAX) {
        return;
    }
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    /* 同款表情重复触发时只续期不重置起点——蹭头的摇晃相位就不会每次
     * 触发都从 0 跳变（视觉上晃动是连续的） */
    if (expr != RIG_EXPR_NONE && expr == s.expr) {
        s.expr_until_ms = now + s_expr_defs[expr].duration_ms;
        return;
    }
    s.expr = expr;
    s.expr_start_ms = now;
    /* NONE 的 duration=0 → expr_until 落在过去 → 下一帧 tick 就回待机 */
    s.expr_until_ms = now + s_expr_defs[expr].duration_ms;
}

void rig_rig_set_mouth(rig_mouth_t level)
{
    s.mouth_ext = level;
    s.mouth_ext_active = true;
}

void rig_rig_speak(uint32_t duration_ms)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    s.speak_start_ms = now;
    s.speak_until_ms = now + duration_ms;
}

/** 口型步进串：150ms 一步 half→open→half→closed，模拟说话节奏 */
static rig_mouth_t mouth_seq_at(uint32_t t_ms)
{
    static const rig_mouth_t seq[4] = {
        RIG_MOUTH_HALF, RIG_MOUTH_OPEN, RIG_MOUTH_HALF, RIG_MOUTH_CLOSED
    };
    return seq[(t_ms / 150) % 4];
}

/** 口型目标：外部 TTS 驱动 > 按需说话（闲聊） > idle 演示串 > 闭嘴 */
static rig_mouth_t mouth_target(uint32_t now)
{
    if (s.mouth_ext_active) {
        return s.mouth_ext;
    }
    if (s.speak_until_ms > now) {
        return mouth_seq_at(now - s.speak_start_ms);
    }
    uint32_t t = now - s.burst_start_ms;
    if (t < MOUTH_BURST_MS) {
        return mouth_seq_at(t);
    }
    return RIG_MOUTH_CLOSED;
}

void rig_rig_tick(uint32_t now, rig_pose_t *out)
{
    const rig_model_t *m = s.m;

    /* ---- 0. 姿态初始化：基础层全可见，偏移全 0 ---- */
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < m->layer_count; i++) {
        out->visible[i] = 1;
    }
    /* 补丁层（挂 head 下的变体小块）默认全部隐藏，后面由各逻辑按需点亮。
     * 用"parent==head"判定而不是硬编码层名，未来加新变体无需改这里。 */
    const int idx_head = m->idx_head;
    if (idx_head >= 0) {
        for (int i = 0; i < m->layer_count; i++) {
            if (m->layers[i].parent == idx_head) {
                out->visible[i] = 0;
            }
        }
    }

    /* ---- 1. 表情是否还在演出中 ----
     * 到期自动切回待机（expr=None），眨眼/口型串随即恢复接管。 */
    if (s.expr != RIG_EXPR_NONE && now >= s.expr_until_ms) {
        s.expr = RIG_EXPR_NONE;
    }
    const bool expr_on = (s.expr != RIG_EXPR_NONE);
    const rig_expr_def_t *ed = expr_on ? &s_expr_defs[s.expr] : NULL;

    /* ---- 2. 呼吸：身体 ±1px，头部 ±1.2px 相位差 0.9s（有层次感） ---- */
    float phase = (now % (uint32_t)BREATH_PERIOD_MS) / BREATH_PERIOD_MS * 6.2832f;
    float body_b = 1.0f * sinf_approx(phase);
    float head_b = 1.2f * sinf_approx(phase - 1.7f);

    /* ---- 3. 头部位移合成 = 呼吸 + 表情俯仰 + 表情摇晃 ----
     * 先算好最终值，下面写姿态时头和子层（补丁）统一继承，
     * 保证整颗头（含眼/嘴补丁）一动全动。 */
    float head_dx = 0.0f;
    float head_dy = head_b;
    if (expr_on) {
        head_dy += (float)ed->head_dy;                  /* 低头/抬头 */
        if (ed->sway_amp > 0 && ed->sway_ms > 0) {
            /* 从表情开始时刻起算相位；PAT 重复触发不重置起点 → 晃动连续 */
            float sw = (float)((now - s.expr_start_ms) % ed->sway_ms)
                       / (float)ed->sway_ms * 6.2832f;
            head_dx += sinf_approx(sw) * (float)ed->sway_amp;
        }
    }

    /* ---- 4. 写姿态：头先算，子层继承 ----
     * 遍历按层表顺序（body=0, head=1, 补丁 2+），head 先写、补丁后拷贝，
     * 顺序天然正确。补丁层偏移继承头部 → 转头/低头时五官跟着走。 */
    for (int i = 0; i < m->layer_count; i++) {
        const rig_layer_t *L = &m->layers[i];
        if (i == idx_head) {
            out->dx[i] = (int16_t)(head_dx + 0.5f);
            out->dy[i] = (int16_t)(head_dy + 0.5f);
        } else if (idx_head >= 0 && L->parent == idx_head) {
            /* 补丁层：眼/嘴/腮红，偏移完全继承头部 */
            out->dx[i] = out->dx[idx_head];
            out->dy[i] = out->dy[idx_head];
        } else if (L->parent < 0) {
            /* 其他根层（body）：只随呼吸上下浮动 */
            out->dy[i] = (int16_t)(body_b + 0.5f);
        }
    }

    /* ---- 5. 眼睛调度 ----
     * 表情期间：表情指定哪副眼睛就亮哪副（闭眼蹭头也走这里），
     * 眨眼逻辑暂停——害羞到一半突然眨个眼就穿帮了。
     * 待机期间：眨眼状态机随机演出。 */
    if (expr_on) {
        int eye_idx = -1;
        if (ed->eyes_layer) {
            /* 表情要求的眼睛变体（引擎启动时没绑定到就静默降级） */
            if (strcmp(ed->eyes_layer, "eyes_smile") == 0) {
                eye_idx = s.idx_eyes_smile;
            } else if (strcmp(ed->eyes_layer, "eyes_closed") == 0) {
                eye_idx = s.idx_eyes_closed;
            } else if (strcmp(ed->eyes_layer, "eyes_wide") == 0) {
                eye_idx = s.idx_eyes_wide;
            }
        }
        if (eye_idx >= 0) {
            out->visible[eye_idx] = 1;
        }
    } else if (s.idx_eyes_closed >= 0) {
        /* 眨眼：闭眼 130ms，间隔 2.6~5.2s 随机 */
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

    /* ---- 6. 嘴调度 ----
     * 表情期间：表情指定哪张嘴就亮哪张（大笑张嘴/嘟嘴）。
     * 待机期间：外部 TTS 驱动优先，否则 idle 说话串每隔几秒演一串。 */
    if (expr_on) {
        int mouth_idx = -1;
        if (ed->mouth_layer) {
            if (strcmp(ed->mouth_layer, "mouth_open") == 0) {
                mouth_idx = s.idx_mouth_open;
            } else if (strcmp(ed->mouth_layer, "mouth_half") == 0) {
                mouth_idx = s.idx_mouth_half;
            } else if (strcmp(ed->mouth_layer, "mouth_pout") == 0) {
                mouth_idx = s.idx_mouth_pout;
            }
        }
        if (mouth_idx >= 0) {
            out->visible[mouth_idx] = 1;
        }
    } else {
        rig_mouth_t mt = mouth_target(now);
        /* 到点开一串新的 idle 说话串（外部驱动时不抢戏） */
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

    /* ---- 7. 腮红 ----
     * 独立于眼嘴调度：害羞系表情（SHY/PAT）叠画，其余场景隐藏 */
    if (s.idx_blush >= 0) {
        out->visible[s.idx_blush] = (expr_on && ed->blush) ? 1 : 0;
    }
}
