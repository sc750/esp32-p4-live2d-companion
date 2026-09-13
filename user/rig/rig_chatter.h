/**
 * @file    rig_chatter.h
 * @brief   三玖闲聊轮播（L4）——主页字幕区的"灵魂内容"引擎
 *
 * 职责：按随机间隔（3~8 分钟）从本地语料库挑一句应景的话，
 * 通过回调交给上层显示（回调里做什么是上层的事——通常走
 * ui_bridge 更新主页字幕 + rig_rig_speak 让口型动起来）。
 *
 * 设计约束：
 *   - L4 纯逻辑：不碰 LVGL、不碰 FreeRTOS，主循环 tick 驱动
 *   - 语料为 static const，改文案直接编辑 rig_chatter.c 的语料表；
     若引入字库外的新汉字，须先跑 tools/gen_nino_font.py 重新生成字体
 *   - 分时段 bucket（早晨/白天/晚间/深夜），只挑应景的话
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#ifndef RIG_CHATTER_H
#define RIG_CHATTER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 闲聊触发回调（chatter 挑好一句话时调用）
 *
 * @param text     文案（static const，调用方无需拷贝/释放）
 * @param speak_ms 建议口型时长 ms（按文案长度估算）
 * @param ctx      注册时传入的上下文
 */
typedef void (*rig_chatter_cb_t)(const char *text, uint32_t speak_ms, void *ctx);

/** 初始化（重置随机节奏；必须在主循环开始 tick 前调用） */
esp_err_t rig_chatter_init(void);

/** 注册触发回调（不注册 = 只计时不开腔） */
void rig_chatter_set_callback(rig_chatter_cb_t cb, void *ctx);

/**
 * @brief 周期驱动（主循环 ~1s 一次即可，内部用 esp_timer 时基）
 *
 * 到点时挑一句调用回调，并掷出下一个随机间隔。
 * 内部有 60s 静音窗口：触发的头一分钟内不再触发（防开机连珠炮）。
 */
void rig_chatter_tick(void);

/**
 * @brief 触摸反应（R11）：被摸/被戳时立刻接一句台词
 *
 * 由 rig_lvgl 手势识别在单击角色时调用（渲染任务上下文）。
 * 带 2.5s 冷却防连摸刷屏；触发后顺延下一次 idle 闲聊档期（话不赶话）。
 *
 * @param on_head true=摸头台词；false=戳身体台词
 */
void rig_chatter_touch(bool on_head);

/**
 * @brief 对话忙静默（步骤 6）：忙=true 期间闲聊轮播与触摸台词都不刷字幕
 *
 * 语音对话/播报进行中调用，防止闲聊语料覆盖对话字幕。闲聊计时照常走，
 * 忙解除后到点自然恢复。
 */
void rig_chatter_set_busy(bool busy);

/** 忙结束并请求快速恢复：5 秒后投放一条语录（而非等原 3~8 分钟间隔） */
void rig_chatter_set_busy_soon(bool busy);

#ifdef __cplusplus
}
#endif

#endif /* RIG_CHATTER_H */
