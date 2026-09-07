/**
 * @file    rig_rig.h
 * @brief   rig 参数动画引擎（L4）——呼吸/眨眼/口型/触摸表情
 *
 * 参数→图层姿态（rig_pose）每帧求值；渲染任务以固定节拍调用
 * rig_rig_tick 取姿态后交给 rig_render_pose。
 *
 * 表情系统（R6 新增）：
 *   UI/触摸层调用 rig_rig_trigger() 点播一种"被摸反应"，
 *   引擎在表情持续期内接管 眼/嘴/腮红 图层可见性 + 头部偏移/摇晃，
 *   期间眨眼与 idle 口型演示串暂停，表情到期后自动回到待机。
 *   "按住类"手势（摸头/闹脾气）由调用方每帧重复触发来续期。
 *
 * @date    2026-09-04
 * @version 2.0.0  R6: 删头部弹簧跟随，加触摸表情状态机
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

/** 口型级别（TTS 外部驱动用；对应 rigbin 里的 mouth_half / mouth_open 补丁） */
typedef enum {
    RIG_MOUTH_CLOSED = 0,
    RIG_MOUTH_HALF   = 1,
    RIG_MOUTH_OPEN   = 2,
    RIG_MOUTH_AUTO   = 3,   /* 释放外部控制，交还 idle 演示串（TTS 播完用） */
} rig_mouth_t;

/**
 * 触摸表情种类（与 rig_rig.c 里 EXPR_DEFS 表一一对应）
 *
 * 手势 → 表情映射（由 rig_lvgl.c 的手势识别负责判定，引擎只管"演"）：
 *   SHY      单击头部 → 害羞低头（腮红+笑眼+头下移，2s）
 *   PAT      按住头部 ≥600ms（摸头）→ 蹭头（腮红+闭眼+轻晃，按住期间持续）
 *   SURPRISE 单击身体 → 惊讶（圆眼+头上抬，1.2s）
 *   LAUGH    双击角色任意区 → 大笑（笑眼+张嘴，1.5s）
 *   POUT     按住身体 ≥1.2s → 闹脾气嘟嘴（+摇头，按住期间持续）
 */
typedef enum {
    RIG_EXPR_NONE = 0,      /* 无表情（待机态） */
    RIG_EXPR_SHY,           /* 害羞低头 */
    RIG_EXPR_PAT,           /* 被摸头蹭蹭 */
    RIG_EXPR_SURPRISE,      /* 惊讶 */
    RIG_EXPR_LAUGH,         /* 大笑 */
    RIG_EXPR_POUT,          /* 嘟嘴闹脾气 */
    RIG_EXPR_MAX
} rig_expr_t;

/**
 * 初始化动画引擎
 *
 * 按图层名绑定补丁层（eyes_closed/eyes_smile/eyes_wide/mouth_half/
 * mouth_open/mouth_pout/blush），缺哪层就降级对应功能，不报错——
 * 这样旧 rigbin（只有 5 层）也能跑，只是表情少几个花样。
 */
esp_err_t rig_rig_init(const rig_model_t *m);

/**
 * @brief 每帧求值姿态（渲染任务以固定节拍调用）
 * @param now_ms   毫秒时基（任意单调起点，与 rig_rig_trigger 内部时基一致）
 * @param out_pose 输出姿态：逐层偏移 + 补丁层可见性
 */
void rig_rig_tick(uint32_t now_ms, rig_pose_t *out_pose);

/**
 * @brief 触发表情（点播一个"被摸反应"）
 *
 * 一次性表情（SHY/SURPRISE/LAUGH）：触发一次演 EXPR_DEFS 里的时长后自动回待机；
 * 按住类表情（PAT/POUT）：时长很短（~300ms），调用方在按住期间每帧重复调用
 * 本函数即可无缝续期，松手停止调用后自然过期。
 * 重复触发任意表情都会直接覆盖当前表情（如害羞中来一下双击 → 立刻转大笑）。
 *
 * @param expr 表情种类；RIG_EXPR_NONE = 立即取消当前表情
 */
void rig_rig_trigger(rig_expr_t expr);

/** 外部口型驱动（TTS 接入后由音频层调用；未驱动时内部演示循环接管） */
void rig_rig_set_mouth(rig_mouth_t level);

/**
 * @brief 按需说一句话（R10 闲聊轮播用）
 *
 * 让口型以 150ms 步进串播 duration_ms 长（模拟说话），期间 idle 演示串
 * 让位；外部 TTS 驱动（set_mouth）优先级更高。表情激活期间口型被表情
 * 接管，本调用只排队不生效（说完了表情结束自动恢复）。
 *
 * @param duration_ms 说话持续时长（如按文案字数 × 220ms 估算）
 */
void rig_rig_speak(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif /* RIG_RIG_H */
