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

#ifdef __cplusplus
}
#endif

#endif /* BSP_INIT_H */
