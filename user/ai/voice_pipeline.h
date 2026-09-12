/**
 * @file    voice_pipeline.h
 * @brief   语音管线编排（L5）——录音 → ASR → LLM → 字幕 全流程
 *
 * 两个入口：
 *   按住说话   hold_start/hold_stop（LVGL 按钮事件调用，非阻塞，内部任务跑）
 *   定时录音   record_ms（阻塞，串口调试/自动化测试用）
 *
 * UI 反馈经注入回调（main_app 接 ui_bridge），ai 层不直接依赖 ui。
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#ifndef VOICE_PIPELINE_H
#define VOICE_PIPELINE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** UI 反馈回调（state 取 scr_dialog_state_t 值） */
typedef struct {
    void (*on_state)(int state, void *ctx);
    void (*on_subtitle)(const char *text, void *ctx);
    void *ctx;
} voice_ui_cb_t;

esp_err_t voice_pipeline_init(void);

/** 注入 UI 回调（init 后、首次使用前调用） */
void voice_pipeline_set_ui(const voice_ui_cb_t *cb);

/** 按住说话：按下沿（非阻塞，录制在内部任务中进行） */
void voice_pipeline_hold_start(void);

/** 打断当前播报（barge-in）：三玖说话时调用——立即停播+通知网关取消+可立即录音 */
void voice_pipeline_barge_in(void);

/** VAD 连续对话模式开关（步骤 5）：播完自动续听、说完静音自动断句 */
void voice_pipeline_set_vad(bool on);

/** VAD 人声 RMS 阈值调节（环境噪声大时上调；默认 250） */
void voice_pipeline_set_vad_thresh(int thresh);

/** 按住说话：松开沿（停止录音并走完 ASR+LLM 管线） */
void voice_pipeline_hold_stop(void);

/**
 * @brief 定时录音：录 ms 毫秒后走完 ASR+LLM（阻塞，调用方任务内执行）
 * @note  串口调试/自动化测试入口；与按住说话互斥（忙时直接返回忙）
 */
esp_err_t voice_pipeline_record_ms(uint32_t ms);

/**
 * @brief 取消全部未播完的 TTS（音乐点播抢占，非阻塞、任意任务可调）
 *
 * 清空句子队列与就绪队列（就地释放堆内存），并在途/未入队的后续短语
 * 一律丢弃；正在播的一句会在下一个 4KB 块边界立即停口。
 * 下一轮对话（hold/record/speak）开始时自动复位。
 */
void voice_pipeline_tts_cancel(void);

/**
 * @brief 播报一句话（M2：TTS 流式 + 口型同步；阻塞至播完）
 *
 * 绿点（SPEAKING）期间拉取 MiMo TTS（24k/mono pcm16）连播扬声器，
 * 口型随音频能量开合；播完回 IDLE 并释放口型控制。
 * 串口文本对话与语音对话共用本出口。
 *
 * @param text 要念的文本（LLM 回复）
 */
void voice_pipeline_speak(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_PIPELINE_H */
