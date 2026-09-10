/**
 * @file    bsp_init.cpp
 * @brief   板级支持包初始化实现
 *
 * 对齐官方 esp_brookesia_phone 示例的初始化流程：
 *   NVS → SPIFFS → 音频 → 显示(含 LVGL/触摸) → 背光
 * I2C 由官方 BSP 内部懒初始化（GPIO7/8, 400kHz，触摸/codec/相机 SCCB 共总线）。
 *
 * @date    2026-09-02
 * @version 1.1.0  R2: 显式配置 DSI lane 速率；初始化顺序对齐官方；转 UTF-8
 */

#include "bsp_init.h"

#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"

/* 官方 BSP（noglib 变体） */
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"

/* 本地 LVGL 适配器（MIPI-DSI + LVGL + 触摸） */
#include "lvgl_adapter_init.h"

/* 音频 BSP（R3） */
#include "bsp_audio.h"

/* 摄像头 BSP（R4） */
#include "bsp_camera.h"

/* Wi-Fi BSP（R5） */
#include "bsp_wifi.h"

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
 * @brief 初始化显示屏（与官方 esp_brookesia_phone 一致）
 *
 * 显式设置 DSI lane 速率（官方 main.cpp 同款）：
 * lane_bit_rate_mbps=0 会导致 DSI 总线创建异常，必须取 BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS。
 */
static esp_err_t init_display(void)
{
    ESP_LOGI(TAG, "正在初始化显示屏...");

    bsp_display_config_t cfg = {
        .hdmi_resolution = BSP_HDMI_RES_NONE,
        .dsi_bus = {
            /* phy_clk_src 保持 0（与官方一致），仅显式配置 lane 速率 */
            .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
        },
    };

    /* 创建 LVGL 显示设备（内部完成 MIPI-DSI + LVGL + 触摸） */
    lv_display_t *disp = lvgl_adapter_init(&cfg);
    if (disp == nullptr) {
        ESP_LOGE(TAG, "LVGL 适配器初始化失败");
        return ESP_FAIL;
    }

    /* 打开背光 */
    ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "背光打开失败");

    s_is_display_ready = true;
    ESP_LOGI(TAG, "显示屏初始化成功 (%dx%d)", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
}

extern "C" esp_err_t bsp_init_all(void)
{
    ESP_LOGI(TAG, "========== BSP 初始化开始 ==========");

    /* 1. NVS */
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "NVS init failed");

    /* 2. SPIFFS（空白分区自动格式化，见 CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL） */
    ESP_RETURN_ON_ERROR(bsp_spiffs_mount(), TAG, "SPIFFS mount failed");
    ESP_LOGI(TAG, "SPIFFS 挂载成功");

    /* 3. 音频（ES8311 录放 + PA）。R11：开机不再自检播放提示音（吵），
     * 需要 时可手动调 bsp_audio_self_test()（诊断用 API 保留） */
    ESP_RETURN_ON_ERROR(bsp_audio_open(), TAG, "audio init failed");

    /* 4. 显示屏 + LVGL */
    ESP_RETURN_ON_ERROR(init_display(), TAG, "display init failed");

    /* 5. 摄像头（SC2336，无模组时不阻塞启动）。
     * R11：开机不再自动取流（帧计数日志是测试残留，且 Phase 6 前无人消费画面）；
     * 需要时调 bsp_camera_start_stream() 手动验证 */
    if (bsp_camera_init() != ESP_OK) {
        ESP_LOGW(TAG, "摄像头不可用（未安装模组或探测失败），跳过");
    }

    /* 6. Wi-Fi（STA，esp_hosted SDIO→C6）；配置了 SSID 则自动连接 */
    if (bsp_wifi_init() == ESP_OK) {
        bsp_wifi_connect_from_config();
    } else {
        ESP_LOGW(TAG, "Wi-Fi 初始化失败，网络功能不可用");
    }

    ESP_LOGI(TAG, "========== BSP 初始化完成 ==========");
    return ESP_OK;
}

extern "C" bool bsp_display_is_ready(void)
{
    return s_is_display_ready;
}

/* ---------- SD 卡（Phase4 M10：音乐播放的数据源） ---------- */

static bool s_sd_mounted = false;           /* 挂载状态（幂等 + 状态查询） */

extern "C" esp_err_t bsp_sd_mount(void)
{
    if (s_sd_mounted) {                     /* 已挂载（重复调用无害） */
        return ESP_OK;                      /* 直接成功 */
    }
    /* 厂商 BSP 一键挂载：内部按板级走线取 SDMMC 槽 + FATFS 挂到 /sdcard。
     * 注意不能复用同名符号——厂商已导出 bsp_sdcard_mount，故本封装另起名。 */
    esp_err_t err = ::bsp_sdcard_mount();
    if (err == ESP_OK) {                    /* 成功 */
        s_sd_mounted = true;                /* 记状态 */
    } else if (err == ESP_ERR_INVALID_STATE) {  /* 已挂过（VFS 层返回） */
        s_sd_mounted = true;                /* 视为可用 */
        err = ESP_OK;                       /* 归一为成功 */
    }
    return err;                             /* 返回结果 */
}

extern "C" bool bsp_sd_is_mounted(void)
{
    return s_sd_mounted;                    /* 状态直读 */
}
