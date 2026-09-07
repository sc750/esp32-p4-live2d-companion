/**
 * @file    bsp_wifi.c
 * @brief   Wi-Fi BSP 实现（STA + esp_wifi_remote + esp_hosted SDIO → C6）
 *
 * 使用标准 esp_wifi/esp_netif API；底层由 esp_wifi_remote 透明转发至板载 C6。
 *
 * R12 状态机化（状态栏开关按钮 + 断线自动重连）：
 *   DISCONNECTED ──connect()──► CONNECTING ──got_ip──► CONNECTED
 *        ▲                        │                    │
 *        └── disconnect()(用户关) ◄─ DISCONNECTED 事件 ◄┘
 *   断线时若 auto-connect 闸门开着 → esp_timer 延迟重连（2s×min(次数,5)
 *   指数退避，上限 10s），避免事件回调里硬重试造成连接风暴。
 *
 * 事件：WIFI_EVENT_STA_START→记录；DISCONNECTED→清标志+调度重连；
 *       IP_EVENT_STA_GOT_IP→存 IP、清重试计数、进 CONNECTED。
 *
 * @date    2026-09-03
 * @version 2.0.0
 */

#include "bsp_wifi.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"

#define TAG "bsp_wifi"

/* IP 字符串缓冲：xxx.xxx.xxx.xxx\0 */
#define IP_BUF_LEN (16)

/* 断线重连节奏：基础 2s，每连败翻倍，封顶 10s（2,4,6..10 封顶由 min 保证） */
#define RETRY_BASE_US   (2 * 1000000ULL)
#define RETRY_MAX_US    (10 * 1000000ULL)

static bool s_inited = false;
static bool s_got_ip = false;
static char s_ip_str[IP_BUF_LEN] = {0};

static bsp_wifi_state_t s_state = BSP_WIFI_DISCONNECTED;
static bool s_auto_connect = false;     /* 自动连接/重连闸门（用户开关） */
static int s_retry = 0;                 /* 连续重试次数（拿到 IP 清零） */
static esp_timer_handle_t s_retry_timer = NULL;

static void retry_timer_cb(void *arg)
{
    /* 延迟重连：闸门仍开着且还没拿到 IP 才真的发起 */
    if (s_auto_connect && !s_got_ip && s_inited) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "重连发起失败: %s", esp_err_to_name(err));
        }
    }
}

static void schedule_retry(void)
{
    if (s_retry_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = retry_timer_cb,
            .name = "wifi_retry",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_retry_timer));
    }
    s_retry++;
    /* 指数退避：2s, 4s, 6s... 封顶 10s */
    uint64_t delay = RETRY_BASE_US * s_retry;
    if (delay > RETRY_MAX_US) {
        delay = RETRY_MAX_US;
    }
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, delay);
    ESP_LOGI(TAG, "%llu ms 后发起第 %d 次重连", (unsigned long long)(delay / 1000), s_retry);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA 已启动");
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "已关联 AP");
            s_state = BSP_WIFI_CONNECTING;
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_got_ip) {
                ESP_LOGW(TAG, "连接断开（原已获取 IP），%s",
                         s_auto_connect ? "自动重连中" : "自动重连已关闭");
            } else {
                ESP_LOGW(TAG, "连接失败/未完成");
            }
            s_got_ip = false;
            if (s_auto_connect) {
                s_state = BSP_WIFI_CONNECTING;
                schedule_retry();       /* 延迟重连，不在事件回调里硬试 */
            } else {
                s_state = BSP_WIFI_DISCONNECTED;
            }
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, IP_BUF_LEN, IPSTR, IP2STR(&event->ip_info.ip));
        s_got_ip = true;
        s_retry = 0;                    /* 成功一次，退避计数归零 */
        s_state = BSP_WIFI_CONNECTED;
        ESP_LOGI(TAG, "已获取 IP: %s", s_ip_str);
    }
}

esp_err_t bsp_wifi_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    /* 网络接口 + 默认事件循环 */
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(sta_netif, ESP_FAIL, TAG, "create sta netif failed");

    /* Wi-Fi 驱动（remote→hosted→C6） */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL),
                        TAG, "register wifi handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL),
                        TAG, "register ip handler failed");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    s_inited = true;
    ESP_LOGI(TAG, "Wi-Fi 子系统初始化完成 (STA 模式, esp_hosted SDIO)");
    return ESP_OK;
}

esp_err_t bsp_wifi_connect(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "wifi not init");
    ESP_RETURN_ON_FALSE(ssid && ssid[0], ESP_ERR_INVALID_ARG, TAG, "ssid empty");

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (password) {
        strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set config failed");
    s_got_ip = false;
    s_retry = 0;
    s_auto_connect = true;              /* 打开闸门：断线后自动重连 */
    s_state = BSP_WIFI_CONNECTING;
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "connect failed");
    ESP_LOGI(TAG, "正在连接 \"%s\" ...", ssid);
    return ESP_OK;
}

esp_err_t bsp_wifi_connect_from_config(void)
{
    const char *ssid = CONFIG_BSP_WIFI_SSID;
    const char *pass = CONFIG_BSP_WIFI_PASSWORD;
    if (ssid[0] == '\0') {
        ESP_LOGI(TAG, "未配置 SSID（menuconfig: BSP Wi-Fi 配置），跳过自动连接");
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_wifi_connect(ssid, pass);
}

esp_err_t bsp_wifi_disconnect(void)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "wifi not init");
    /* 先关闸门再断开：DISCONNECTED 事件看到闸门关闭就不会调度重连 */
    s_auto_connect = false;
    s_state = BSP_WIFI_DISCONNECTED;
    s_got_ip = false;
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);  /* 取消已排队的重连 */
    }
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_disconnect: %s", esp_err_to_name(err));
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
    if (!buf || buf_len < IP_BUF_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_got_ip) {
        buf[0] = '\0';
        return ESP_ERR_NOT_FINISHED;
    }
    strlcpy(buf, s_ip_str, buf_len);
    return ESP_OK;
}
