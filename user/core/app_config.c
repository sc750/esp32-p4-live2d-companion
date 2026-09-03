/**
 * @file    app_config.c
 * @brief   NVS 配置管理实现
 *
 * 使用 ESP-IDF 的 NVS API 读写用户配置。
 * NVS 数据保存在 Flash 中，断电不丢失。
 *
 * @date    2026-09-01
 * @version 1.0.0
 */

/* 1. 自身公开头 */
#include "app_config.h"

/* 2. C 标准库 */
#include <string.h>

/* 3. 项目级 */

/* 4. 平台/厂商头 */
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "app_config";

/* NVS 命名空间：所有配置项都存在 "app_cfg" 这个命名空间下 */
#define NVS_NAMESPACE  "app_cfg"

/* 默认配置值（首次开机时使用） */
static app_config_t s_config = {
    .wifi_ssid = "",                /* Wi-Fi 名称：空（未配置） */
    .wifi_password = "",            /* Wi-Fi 密码：空（未配置） */
    .volume = 70,                   /* 音量：70% */
    .theme = 0,                     /* 主题：日间（浅色） */
    .is_japanese_subtitle = false,  /* 日文字幕：关闭 */
};

/**
 * @brief 从 NVS 读取字符串类型配置项（内部辅助函数）
 *
 * @param[in]  hdl      NVS 句柄
 * @param[in]  key      配置项名称（如 "wifi_ssid"）
 * @param[out] buf      读取缓冲区
 * @param[in]  buf_len  缓冲区大小
 * @return ESP_OK 读取成功，其他 表示该配置项不存在
 */
static esp_err_t nvs_read_str(nvs_handle_t hdl, const char *key,
                               char *buf, size_t buf_len)
{
    size_t len = buf_len;
    return nvs_get_str(hdl, key, buf, &len);
}

/**
 * @brief 从 NVS 加载所有配置
 *
 * 读取流程：
 *   1. 打开 NVS 命名空间
 *   2. 逐个读取配置项（如果某个项不存在，就用默认值）
 *   3. 关闭 NVS
 */
esp_err_t app_config_init(void)
{
    nvs_handle_t hdl;

    /* 打开 NVS（只读模式） */
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &hdl);
    if (ret != ESP_OK) {
        /* NVS 中没有保存过配置，使用默认值即可 */
        ESP_LOGW(TAG, "没有找到已保存的配置，使用默认值");
        return ESP_OK;
    }

    /* 逐个读取配置项（失败不影响，保持默认值） */
    nvs_read_str(hdl, "wifi_ssid", s_config.wifi_ssid,
                 sizeof(s_config.wifi_ssid));          /* 读取 Wi-Fi 名称 */
    nvs_read_str(hdl, "wifi_pass", s_config.wifi_password,
                 sizeof(s_config.wifi_password));      /* 读取 Wi-Fi 密码 */

    int32_t val = 0;  /* NVS 只支持 i32 类型的整数 */
    if (nvs_get_i32(hdl, "volume", &val) == ESP_OK) {
        s_config.volume = (int)val;  /* 读取音量 */
    }
    if (nvs_get_i32(hdl, "theme", &val) == ESP_OK) {
        s_config.theme = (int)val;   /* 读取主题 */
    }
    if (nvs_get_i32(hdl, "jp_sub", &val) == ESP_OK) {
        s_config.is_japanese_subtitle = (val != 0);  /* 读取日文字幕开关 */
    }

    nvs_close(hdl);  /* 关闭 NVS */
    ESP_LOGI(TAG, "配置加载成功: 音量=%d, 主题=%d", s_config.volume, s_config.theme);
    return ESP_OK;
}

const app_config_t *app_config_get(void)
{
    return &s_config;  /* 返回配置指针（只读） */
}

/**
 * @brief 写入一个 i32 类型的配置项到 NVS（内部辅助函数）
 *
 * 流程：打开 NVS → 写入 → 提交 → 关闭
 */
static esp_err_t nvs_write_i32(const char *key, int32_t val)
{
    nvs_handle_t hdl;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &hdl),
                        TAG, "NVS 打开失败");
    ESP_RETURN_ON_ERROR(nvs_set_i32(hdl, key, val), TAG, "NVS 写入失败");
    ESP_RETURN_ON_ERROR(nvs_commit(hdl), TAG, "NVS 提交失败");
    nvs_close(hdl);
    return ESP_OK;
}

esp_err_t app_config_set_wifi(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 打开 NVS，写入 SSID 和密码 */
    nvs_handle_t hdl;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &hdl),
                        TAG, "NVS 打开失败");
    ESP_RETURN_ON_ERROR(nvs_set_str(hdl, "wifi_ssid", ssid), TAG, "NVS 写入失败");
    ESP_RETURN_ON_ERROR(nvs_set_str(hdl, "wifi_pass", password), TAG, "NVS 写入失败");
    ESP_RETURN_ON_ERROR(nvs_commit(hdl), TAG, "NVS 提交失败");
    nvs_close(hdl);

    /* 同步更新内存中的配置 */
    strncpy(s_config.wifi_ssid, ssid, sizeof(s_config.wifi_ssid) - 1);
    strncpy(s_config.wifi_password, password, sizeof(s_config.wifi_password) - 1);
    ESP_LOGI(TAG, "Wi-Fi 配置已保存: SSID=%s", ssid);
    return ESP_OK;
}

esp_err_t app_config_set_volume(int volume)
{
    /* 参数范围检查 */
    if (volume < 0 || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(nvs_write_i32("volume", volume), TAG, "保存失败");
    s_config.volume = volume;  /* 同步更新内存 */
    return ESP_OK;
}

esp_err_t app_config_set_theme(int theme)
{
    ESP_RETURN_ON_ERROR(nvs_write_i32("theme", theme), TAG, "保存失败");
    s_config.theme = theme;  /* 同步更新内存 */
    return ESP_OK;
}
