/**
 * @file    scr_music.h
 * @brief   音乐页（全屏控制面板）—— 电台名 + 播控 + 音量 + 播放列表
 *
 * 与 scr_home 同一套分层约定：UI 不碰业务层，所有动作经
 * `scr_music_set_control_cb()` 由编排层（main_app.c）注入回调后执行；
 * 状态由编排层经 `scr_music_set_state()` 喂进来，本模块只负责画。
 *
 * 线程模型：本模块**不自带锁**（与 scr_home 一致）——建页发生在
 * ui_manager_init 的持锁区内，其余入口一律经 ui_bridge 封装加锁后调用。
 * 切勿在非 LVGL 任务里直接调本模块的函数。
 *
 * 布局（1024×600 三段式）：
 *   顶栏：[返回] ♪ 音乐            [播放中/已暂停/已停止]
 *   中部：电台名 + 已播时长 + 上一首/播放暂停/下一首 + 音量滑块
 *   底部：播放列表（当前曲高亮，可滚动）
 *
 * @date    2026-09-10
 * @version 1.0.0
 */

#ifndef SCR_MUSIC_H
#define SCR_MUSIC_H

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 播放列表最多建项数（编排层喂列表时不得超过；超出截断并告警） */
#define SCR_MUSIC_LIST_MAX  32

/**
 * @brief 音乐页发起的控制命令（arg 含义随命令不同）
 *
 * 刻意用本模块自有的枚举而非直接 include music_service.h——
 * UI 层不依赖业务层的类型，中间由编排层做一次翻译。
 */
typedef enum {
    SCR_MUSIC_CMD_PLAY = 0,     /* 播放，arg = 曲目索引（0 起） */
    SCR_MUSIC_CMD_PAUSE,        /* 暂停（arg 未用） */
    SCR_MUSIC_CMD_RESUME,       /* 从暂停继续（arg 未用） */
    SCR_MUSIC_CMD_NEXT,         /* 下一首/下一台（arg 未用） */
    SCR_MUSIC_CMD_PREV,         /* 上一首/上一台（arg 未用） */
    SCR_MUSIC_CMD_VOL,          /* 设置音量，arg = 0~100 */
} scr_music_cmd_t;

/**
 * @brief 创建音乐页（默认隐藏，由 ui_manager_navigate 控制显隐）
 *
 * @param[in] parent  LVGL 父对象（ui_manager 的主屏幕）
 * @return 页面容器指针（失败返回 NULL）
 */
lv_obj_t *scr_music_create(lv_obj_t *parent);

/**
 * @brief 注入返回按钮回调（编排层 → ui_manager_navigate(UI_PAGE_HOME)）
 */
void scr_music_set_back_cb(void (*cb)(void *ctx), void *ctx);

/**
 * @brief 注入播控回调（编排层 → music_service 各 API）
 *
 * @param[in] cb   回调，cmd 取 scr_music_cmd_t，arg 随命令含义不同
 * @param[in] ctx  透传上下文
 */
void scr_music_set_control_cb(void (*cb)(int cmd, int arg, void *ctx), void *ctx);

/**
 * @brief 用播放列表建/重建列表项（names 为曲名数组，可为 NULL 表示空列表）
 *
 * 幂等：重复调用会先清空旧项。列表中第 cur_idx 项高亮为主题色。
 */
void scr_music_set_playlist(const char *const *names, int count);

/**
 * @brief 刷新播放状态（编排层按秒节拍调用）
 *
 * 内部先与上次值比对，无变化不做任何 LVGL 操作；页面隐藏时直接返回
 * —— 避免每秒触发无谓重绘。
 *
 * @param[in] playing   正在播放
 * @param[in] paused    暂停中（与 playing 互斥，由编排层从 music_service 取）
 * @param[in] cur_idx   当前曲索引（-1 = 无）
 * @param[in] pos_sec   当前曲已播秒数
 */
void scr_music_set_state(bool playing, bool paused, int cur_idx, int pos_sec);

/**
 * @brief 设置音量滑块位置（不触发控制回调）
 *
 * 进页面时同步一次真实音量用；拖动滑块产生的变更是走控制回调的。
 */
void scr_music_set_volume(int vol);

#ifdef __cplusplus
}
#endif

#endif /* SCR_MUSIC_H */
