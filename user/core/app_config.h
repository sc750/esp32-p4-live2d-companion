/**
 * @file    app_config.h
 * @brief   NVS 配置管理接口
 *
 * NVS（Non-Volatile Storage）是 ESP-IDF 提供的持久化存储，
 * 断电后数据不丢失。用于保存用户的配置，比如：
 *   - Wi-Fi 的 SSID 和密码
 *   - 音量大小
 *   - 主题选择（日间/夜间）
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 应用配置结构体
 *
 * 存放所有用户可配置的参数。
 * 启动时从 NVS 加载，修改后写回 NVS。
 */
typedef struct {
    char wifi_ssid[33];         /* Wi-Fi 名称（最长 32 个字符 + 结束符） */
    char wifi_password[65];     /* Wi-Fi 密码（最长 64 个字符 + 结束符） */
    int volume;                 /* 音量大小，范围 0~100，默认 70 */
    int theme;                  /* 主题：0=日间（浅色），1=夜间（深色） */
    bool is_japanese_subtitle;  /* 是否显示日文字幕：true=显示，false=隐藏 */
} app_config_t;

/**
 * @brief 初始化配置管理
 *
 * 从 NVS 中读取之前保存的配置。如果 NVS 中没有配置，
 * 就使用默认值（音量 70，日间主题等）。
 *
 * @return ESP_OK 成功
 */
esp_err_t app_config_init(void);

/**
 * @brief 获取当前配置
 *
 * @return 配置结构体指针（只读，不要直接修改它）
 */
const app_config_t *app_config_get(void);

/**
 * @brief 保存 Wi-Fi 凭据
 *
 * 把 SSID 和密码保存到 NVS，下次开机还能用。
 *
 * @param[in] ssid      Wi-Fi 名称
 * @param[in] password  Wi-Fi 密码
 * @return ESP_OK 成功
 */
esp_err_t app_config_set_wifi(const char *ssid, const char *password);

/**
 * @brief 保存音量设置
 *
 * @param[in] volume  音量大小，范围 0~100
 * @return ESP_OK 成功，ESP_ERR_INVALID_ARG 参数无效
 */
esp_err_t app_config_set_volume(int volume);

/**
 * @brief 保存主题设置
 *
 * @param[in] theme  主题：0=日间，1=夜间
 * @return ESP_OK 成功
 */
esp_err_t app_config_set_theme(int theme);

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */
