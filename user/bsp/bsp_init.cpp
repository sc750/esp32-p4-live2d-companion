/**
 * @file    bsp_init.cpp
 * @brief   鏉跨骇鏀寔鍖呭垵濮嬪寲瀹炵幇
 *
 * 鍩轰簬 esp_brookesia_phone 瀹樻柟 demo 鐨勫垵濮嬪寲娴佺▼銆? * 浣跨敤瀹樻柟 BSP + esp_lv_adapter v0.5 + esp_lvgl_port v2.4銆? *
 * @date    2026-09-02
 * @version 1.0.0
 */

#include "bsp_init.h"

#include <cstring>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

/* 瀹樻柟 BSP */
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"

/* LVGL Adapter v0.5 */
#include "esp_lv_adapter.h"

/* LVGL */
#include "lvgl.h"

/* 鏈湴 LVGL 閫傞厤鍣ㄥ垵濮嬪寲 */
#include "lvgl_adapter_init.h"

static const char *TAG = "bsp_init";
static bool s_is_display_ready = false;

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

/**
 * @brief 鍒濆鍖栨樉绀哄睆锛堜娇鐢ㄨ兘鐢ㄧ殑瀹樻柟 demo 鐨勫垵濮嬪寲娴佺▼锛? */
static esp_err_t init_display(void)
{
    ESP_LOGI(TAG, "姝ｅ湪鍒濆鍖栨樉绀哄睆...");
    /* 閰嶇疆鏄剧ず锛堜笌鑳界敤鐨?esp_brookesia_phone 瀹屽叏涓€鑷达級 */
    bsp_display_config_t cfg = {};

    /* 鍒涘缓 LVGL 鏄剧ず璁惧锛堝唴閮ㄥ畬鎴?MIPI-DSI + LVGL + 瑙︽懜锛?*/
    lv_display_t *disp = lvgl_adapter_init(&cfg);
    if (disp == nullptr) {
        ESP_LOGE(TAG, "LVGL 閫傞厤鍣ㄥ垵濮嬪寲澶辫触");
        return ESP_FAIL;
    }

    /* 鎵撳紑鑳屽厜 */
    ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "鑳屽厜鎵撳紑澶辫触");

    s_is_display_ready = true;
    ESP_LOGI(TAG, "鏄剧ず灞忓垵濮嬪寲鎴愬姛 (%dx%d)", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
}

extern "C" esp_err_t bsp_init_all(void)
{
    ESP_LOGI(TAG, "========== BSP 鍒濆鍖栧紑濮?==========");

    /* NVS */
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "NVS init failed");

    /* SPIFFS */
    ESP_ERROR_CHECK(bsp_spiffs_mount());
    ESP_LOGI(TAG, "SPIFFS 鎸傝浇鎴愬姛");

    /* 闊抽锛圥hase 3 瀹炵幇锛?*/
    ESP_LOGI(TAG, "闊抽鍒濆鍖栧皢鍦?Phase 3 瀹炵幇");

    /* 鏄剧ず灞?+ LVGL */
    ESP_RETURN_ON_ERROR(init_display(), TAG, "display init failed");

    ESP_LOGI(TAG, "========== BSP 鍒濆鍖栧畬鎴?==========");
    return ESP_OK;
}

extern "C" bool bsp_display_is_ready(void)
{
    return s_is_display_ready;
}

extern "C" bool bsp_wifi_is_connected(void)
{
    return false;
}

extern "C" esp_err_t bsp_wifi_get_ip(char *buf, size_t buf_len)
{
    if (!buf || !buf_len) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';
    return ESP_ERR_NOT_FINISHED;
}

