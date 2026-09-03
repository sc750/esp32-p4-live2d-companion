/**
 * @file    bsp_wifi.h
 * @brief   Wi-Fi BSP 接口（STA 模式，esp_wifi_remote 经 esp_hosted SDIO 驱动板载 C6）
 *
 * 硬件事实（官方 BSP 冻结基线）：
 *   - Wi-Fi 由板载 ESP32-C6 提供，P4 经 SDIO 通信（CLK=18/CMD=19/D0-3=14~17/RST=54）
 *   - 软件走标准 esp_wifi API，esp_wifi_remote 透明接管，esp_hosted 3.x 传输
 * 事件流：esp_netif_init → event loop → create_default_wifi_sta → esp_wifi_init
 *         → set_mode(STA) → set_config → start → connect
 *
 * @date    2026-09-03
 * @version 1.0.0
 */

#ifndef BSP_WIFI_H
#define BSP_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 Wi-Fi 子系统（netif + event loop + STA 模式启动）
 *
 * 幂等；只启动不连接。重复调用直接返回 ESP_OK。
 */
esp_err_t bsp_wifi_init(void);

/**
 * @brief 连接指定 AP（异步，结果经日志与 is_connected 体现）
 */
esp_err_t bsp_wifi_connect(const char *ssid, const char *password);

/**
 * @brief 用 Kconfig 配置（CONFIG_BSP_WIFI_SSID/PASSWORD）连接
 *
 * SSID 未配置时返回 ESP_ERR_INVALID_STATE 且不报错。
 */
esp_err_t bsp_wifi_connect_from_config(void);

/** @brief 是否已获取 IP */
bool bsp_wifi_is_connected(void);

/**
 * @brief 获取 IP 字符串（点分十进制）
 * @param buf 输出缓冲（≥16 字节）
 * @return ESP_OK 已连接；ESP_ERR_NOT_FINISHED 未连接
 */
esp_err_t bsp_wifi_get_ip(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* BSP_WIFI_H */
