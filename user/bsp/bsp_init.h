/**
 * @file    bsp_init.h
 * @brief   板级支持包初始化接口（C 兼容）
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

#ifndef BSP_INIT_H
#define BSP_INIT_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_init_all(void);
bool bsp_display_is_ready(void);
bool bsp_wifi_is_connected(void);
esp_err_t bsp_wifi_get_ip(char *buf, size_t buf_len);

/**
 * @brief 挂载 SD 卡到 BSP_SD_MOUNT_POINT（/sdcard）
 *
 * 厂商 BSP 调用的薄封装：AI/应用层不直接 include 厂商头（分层约束）。
 * 注意命名——厂商已导出 bsp_sdcard_mount()，本封装用 bsp_sd_mount() 避让。
 */
esp_err_t bsp_sd_mount(void);

/** SD 卡是否已挂载（音乐等依赖卡的功能可据此降级） */
bool bsp_sd_is_mounted(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_INIT_H */
