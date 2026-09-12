/**
 * @file    bsp_audio.h
 * @brief   音频 BSP 接口（ES8311 录放 + NS4150 功放）
 *
 * 硬件事实（官方 BSP 冻结基线）：
 *   - Codec: ES8311 (I2C 地址 0x18)，I2S1 全双工，MCLK=13/BCLK=12/WS=10/DOUT=9/DSIN=11
 *   - 麦克风为 ES8311 模拟输入（非 PDM），录放同一 codec
 *   - 功放(NS4150)使能脚 PA=GPIO53，由 codec 驱动经 PA 逻辑托管
 * 初始化模式对齐官方 esp_brookesia_phone 的 bsp_extra 封装：
 *   speaker_init/microphone_init（内部懒初始化 I2C+I2S）→ set_fs(16k/16bit/2ch)
 *
 * @date    2026-09-03
 * @version 1.0.0
 */

#ifndef BSP_AUDIO_H
#define BSP_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 默认音频参数（与官方 bsp_extra 一致） */
#define BSP_AUDIO_DEFAULT_SAMPLE_RATE   (16000)
#define BSP_AUDIO_DEFAULT_BIT_WIDTH     (16)
#define BSP_AUDIO_DEFAULT_CHANNEL       (2)
#define BSP_AUDIO_DEFAULT_VOLUME        (50)   /* 输出音量 0~100 */
#define BSP_AUDIO_DEFAULT_ADC_GAIN      (24.0f) /* 麦克风输入增益 dB */

/**
 * @brief 初始化音频子系统（speaker + microphone，ES8311），打开 codec
 *
 * 幂等；重复调用直接返回 ESP_OK。
 */
esp_err_t bsp_audio_open(void);

/** @brief 音频子系统是否已就绪 */
bool bsp_audio_is_ready(void);

/**
 * @brief 设置扬声器音量
 * @param volume 0~100
 */
esp_err_t bsp_audio_set_volume(int volume);

/**
 * @brief 运行时切换采样格式（重开 codec，阻塞）
 */
esp_err_t bsp_audio_set_fs(uint32_t rate, uint32_t bits, uint32_t channels);

/**
 * @brief 阻塞写扬声器
 * @param data PCM 数据（按当前 fs 的 interleaved 格式）
 * @param len  字节数
 */
esp_err_t bsp_audio_play(const void *data, size_t len);

/**
 * @brief 阻塞读麦克风
 * @param data 输出缓冲
 * @param len  期望字节数
 */
esp_err_t bsp_audio_record(void *data, size_t len);

/**
 * @brief 自检：扬声器播放短提示音 + 麦克风采样电平检查（阻塞约 1s）
 *
 * 用于装机验证；应用层可不调用。
 */
esp_err_t bsp_audio_self_test(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_AUDIO_H */
