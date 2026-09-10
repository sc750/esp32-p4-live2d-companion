/**
 * @file    music_service.h
 * @brief   音乐播放服务（L4）——SD 卡扫描 + MP3/WAV 软解播放 + 控制命令
 *
 * 架构：单播放任务 + 命令队列。解码用 espressif/esp_audio_codec 的
 * esp_audio_simple_dec（feed 式，任意切块）；输出复用 bsp_audio_play
 * （阻塞写 I2S 天然节流）。歌曲开始按文件采样率 set_fs，停止/打断后
 * 恢复 16k——保证录音/ASR/TTS 链路的采样率不受污染。
 *
 * 使用约定：MP3/WAV 放 SD 卡 /music/ 目录（FATFS，长文件名已启用）。
 *
 * @date    2026-09-10
 * @version 1.0.0
 */

#ifndef MUSIC_SERVICE_H
#define MUSIC_SERVICE_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_NAME_MAX      64      /* 文件名（不含路径）上限 */
#define MUSIC_LIST_MAX      100     /* 曲目上限 */

/**
 * @brief 初始化：挂载 SD 卡（失败仅告警不阻塞启动）+ 建播放任务（幂等）
 */
esp_err_t music_service_init(void);

/**
 * @brief 扫描 /sdcard/music/ 下的 mp3/wav（重复调用重扫）
 * @return 曲目数（SD 未挂载/无目录返回 0）
 */
int music_scan(void);

/** 曲目数（上次扫描结果） */
int music_count(void);

/** 第 idx 首曲名（去路径去扩展名；越界返回 NULL） */
const char *music_name_at(int idx);

/** 播放第 idx 首（0 起）；当前有歌则切换。ESP_ERR_NOT_FOUND=索引无效 */
esp_err_t music_play_index(int idx);

/** 下一首/上一首（列表循环）；无曲目返回 ESP_ERR_INVALID_STATE */
esp_err_t music_next(void);
esp_err_t music_prev(void);

/** 暂停/恢复/停止（幂等；停止后采样率自动恢复 16k） */
esp_err_t music_pause(void);
esp_err_t music_resume(void);
esp_err_t music_stop(void);

/** 音量 0~100（透传 bsp_audio_set_volume） */
esp_err_t music_set_volume(int volume);

/** 是否正在播放（含暂停态返回 false） */
bool music_is_playing(void);

/** 是否处于暂停态（在播但被 pause 暂停；与"已停止"区分） */
bool music_is_paused(void);

/** 当前曲名（文件名去路径；无歌返回 NULL） */
const char *music_current_name(void);

/** 当前曲已播放秒数（暂停不计） */
int music_position_sec(void);

/**
 * @brief 语音会话开始：停掉音乐并等播放任务真正退出（≤3s），
 *        保证 I2S 采样率已恢复 16k。无音乐时立即返回。
 */
void music_notify_voice_start(void);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_SERVICE_H */
