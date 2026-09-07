/**
 * @file    bsp_wifi.c
 * @brief   Wi-Fi BSP implementation (STA + esp_wifi_remote over ESP-Hosted SDIO)
 *
 * ESP32-P4 has no native Wi-Fi. The board's ESP32-C6 is reset and brought up
 * by ESP-Hosted before app_main(), but its Wi-Fi RPC feature becomes usable a
 * little later. Treating esp_wifi_init() as a one-shot call made the result
 * depend on a narrow boot-time race. This module owns that handshake and
 * retains a requested connection until the remote stack is ready.
 */

#include "bsp_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "bsp_wifi"

#define IP_BUF_LEN                (16)
#define WIFI_INIT_RETRY_COUNT     (8)
#define WIFI_INIT_RETRY_DELAY_MS  (500)
#define RETRY_BASE_US             (2 * 1000000ULL)
#define RETRY_MAX_US              (10 * 1000000ULL)

typedef enum {
    WIFI_CORE_OFF = 0,
    WIFI_CORE_STARTING,
    WIFI_CORE_READY,
    WIFI_CORE_FAILED,
} wifi_core_state_t;

static bool s_platform_ready;
static bool s_event_handlers_ready;
static bool s_wifi_driver_ready;
static bool s_auto_connect;
static bool s_got_ip;
static int s_retry;
static wifi_core_state_t s_core_state = WIFI_CORE_OFF;
static bsp_wifi_state_t s_state = BSP_WIFI_DISCONNECTED;
static esp_timer_handle_t s_retry_timer;
static TaskHandle_t s_init_task;
static char s_ip_str[IP_BUF_LEN];
static char s_ssid[sizeof(((wifi_config_t *)0)->sta.ssid)];
static char s_password[sizeof(((wifi_config_t *)0)->sta.password)];

static void start_pending_connection(void);

static void retry_timer_cb(void *arg)
{
    (void)arg;
    start_pending_connection();
}

static void schedule_connection_retry(void)
{
    if (!s_auto_connect || !s_wifi_driver_ready) {
        return;
    }

    if (s_retry_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = retry_timer_cb,
            .name = "wifi_retry",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_retry_timer));
    }

    s_retry++;
    uint64_t delay_us = RETRY_BASE_US * s_retry;
    if (delay_us > RETRY_MAX_US) {
        delay_us = RETRY_MAX_US;
    }
    esp_timer_stop(s_retry_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_retry_timer, delay_us));
    ESP_LOGI(TAG, "%llu ms 后发起第 %d 次重连",
             (unsigned long long)(delay_us / 1000), s_retry);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA 已启动");
            break;
        case WIFI_EVENT_STA_CONNECTED:
            s_state = BSP_WIFI_CONNECTING;
            ESP_LOGI(TAG, "已关联 AP");
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_got_ip = false;
            if (s_auto_connect) {
                s_state = BSP_WIFI_CONNECTING;
                ESP_LOGW(TAG, "连接断开，自动重连中");
                schedule_connection_retry();
            } else {
                s_state = BSP_WIFI_DISCONNECTED;
                ESP_LOGI(TAG, "连接已断开（用户关闭）");
            }
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        s_got_ip = true;
        s_retry = 0;
        s_state = BSP_WIFI_CONNECTED;
        ESP_LOGI(TAG, "已获取 IP: %s", s_ip_str);
    }
}

static esp_err_t prepare_network_platform(void)
{
    if (s_platform_ready) {
        return ESP_OK;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }
    s_platform_ready = true;
    return ESP_OK;
}

static esp_err_t install_wifi_driver(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (!s_event_handlers_ready) {
        ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL),
                            TAG, "register Wi-Fi event handler failed");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL),
                            TAG, "register IP event handler failed");
        s_event_handlers_ready = true;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi failed");
    s_wifi_driver_ready = true;
    return ESP_OK;
}

static void wifi_init_task(void *arg)
{
    (void)arg;
    esp_err_t err = prepare_network_platform();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "网络基础设施初始化失败: %s (0x%x)", esp_err_to_name(err), err);
        s_core_state = WIFI_CORE_FAILED;
        s_init_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* Wait for ESP-Hosted to register the C6 Wi-Fi RPC endpoint. */
    vTaskDelay(pdMS_TO_TICKS(WIFI_INIT_RETRY_DELAY_MS));
    for (int attempt = 1; attempt <= WIFI_INIT_RETRY_COUNT; ++attempt) {
        err = install_wifi_driver();
        if (err == ESP_OK) {
            s_core_state = WIFI_CORE_READY;
            ESP_LOGI(TAG, "Wi-Fi 子系统初始化完成 (STA 模式, esp_hosted SDIO)");
            start_pending_connection();
            s_init_task = NULL;
            vTaskDelete(NULL);
            return;
        }

        ESP_LOGW(TAG, "远端 Wi-Fi 初始化第 %d/%d 次失败: %s (0x%x)",
                 attempt, WIFI_INIT_RETRY_COUNT, esp_err_to_name(err), err);
        /* Remote glue allocates channels before the C6 answers; unwind first. */
        esp_wifi_deinit();
        vTaskDelay(pdMS_TO_TICKS(WIFI_INIT_RETRY_DELAY_MS));
    }

    s_core_state = WIFI_CORE_FAILED;
    s_state = BSP_WIFI_DISCONNECTED;
    s_init_task = NULL;
    ESP_LOGE(TAG, "远端 Wi-Fi 初始化失败，已停止重试");
    vTaskDelete(NULL);
}

static void start_pending_connection(void)
{
    if (!s_auto_connect || !s_wifi_driver_ready || s_ssid[0] == '\0') {
        return;
    }

    wifi_config_t config = { 0 };
    strlcpy((char *)config.sta.ssid, s_ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, s_password, sizeof(config.sta.password));

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "发起连接失败: %s (0x%x)", esp_err_to_name(err), err);
        schedule_connection_retry();
        return;
    }

    s_got_ip = false;
    s_state = BSP_WIFI_CONNECTING;
    ESP_LOGI(TAG, "正在连接 \"%s\" ...", s_ssid);
}

esp_err_t bsp_wifi_init(void)
{
    if (s_core_state == WIFI_CORE_READY || s_core_state == WIFI_CORE_STARTING) {
        return ESP_OK;
    }

    s_core_state = WIFI_CORE_STARTING;
    if (xTaskCreate(wifi_init_task, "wifi_init", 4096, NULL, 8, &s_init_task) != pdPASS) {
        s_core_state = WIFI_CORE_FAILED;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "等待 ESP-Hosted Wi-Fi RPC 就绪...");
    return ESP_OK;
}

esp_err_t bsp_wifi_connect(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(ssid != NULL && ssid[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "ssid empty");
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_password, password ? password : "", sizeof(s_password));
    s_auto_connect = true;
    s_retry = 0;
    s_state = BSP_WIFI_CONNECTING;

    if (s_core_state == WIFI_CORE_OFF || s_core_state == WIFI_CORE_FAILED) {
        ESP_RETURN_ON_ERROR(bsp_wifi_init(), TAG, "Wi-Fi init request failed");
    }
    start_pending_connection();
    return ESP_OK;
}

esp_err_t bsp_wifi_connect_from_config(void)
{
    if (CONFIG_BSP_WIFI_SSID[0] == '\0') {
        ESP_LOGI(TAG, "未配置 SSID（menuconfig: BSP Wi-Fi 配置），跳过自动连接");
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_wifi_connect(CONFIG_BSP_WIFI_SSID, CONFIG_BSP_WIFI_PASSWORD);
}

esp_err_t bsp_wifi_disconnect(void)
{
    s_auto_connect = false;
    s_got_ip = false;
    s_state = BSP_WIFI_DISCONNECTED;
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    if (!s_wifi_driver_ready) {
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "esp_wifi_disconnect: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "已断开并停用自动重连（用户开关 OFF）");
    return ESP_OK;
}

bsp_wifi_state_t bsp_wifi_get_state(void)
{
    return s_state;
}

bool bsp_wifi_is_connected(void)
{
    return s_got_ip;
}

esp_err_t bsp_wifi_get_ip(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len < IP_BUF_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_got_ip) {
        buf[0] = '\0';
        return ESP_ERR_NOT_FINISHED;
    }
    strlcpy(buf, s_ip_str, buf_len);
    return ESP_OK;
}
