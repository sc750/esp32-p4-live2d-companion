/**
 * @file    gw_client.c
 * @brief   语音网关 WS 客户端实现（步骤 1：通道打通）
 *
 * 事件驱动：esp_websocket_client 自带任务回调事件，本模块只做
 * 状态记录 + 握手 + 消息日志；重连由组件内置 reconnect_timeout 兜底。
 *
 * 消息约定（步骤 1）：
 *   设备 → 网关：{"type":"hello","device":"esp32p4","version":"..."}
 *                {"type":"ping"}
 *                {"type":"text","data":"..."}
 *   网关 → 设备：{"type":"welcome"| "pong" | "echo" | "stats", ...}
 *
 * @date    2026-09-12
 * @version 1.0.0
 */

/* 1. 自身公开头 */
#include "gw_client.h"

/* 2. C 标准库 */
#include <string.h>
#include <stdio.h>

/* 3. 项目级 */
#include "app_config.h"         /* 无强依赖，保留项目级头顺序一致 */

/* 4. 平台/厂商头 */
#include "esp_log.h"
#include "esp_check.h"        /* ESP_RETURN_ON_* */
#include "cJSON.h"            /* 网关消息解析 */
#include "esp_heap_caps.h"      /* 重组缓冲 realloc（PSRAM） */
#include "esp_websocket_client.h"

#define TAG "gw"

#define GW_HELLO_FMT \
    "{\"type\":\"hello\",\"device\":\"esp32p4\",\"version\":\"step1\"}"

/* 模块状态 */
static struct {
    bool inited;                                /* 幂等闸门 */
    esp_websocket_client_handle_t client;       /* WS 客户端句柄 */
    volatile bool connected;                    /* 当前连接状态 */
    void (*msg_cb)(const char *type, const char *data);         /* 文本消息处理器 */
    void (*bin_cb)(const uint8_t *pcm, size_t bytes);           /* 二进制帧处理器（TTS PCM） */
} s_gw;

/** WS 事件回调：连接/断开/收数据 */
static void gw_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)arg;
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_gw.connected = true;
        ESP_LOGI(TAG, "已连接网关");             /* 连接日志 */
        esp_websocket_client_send_text(client, GW_HELLO_FMT,
                                       strlen(GW_HELLO_FMT), portMAX_DELAY);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        if (s_gw.connected) {                   /* 首次断开才打日志（重连风暴防刷屏） */
            ESP_LOGW(TAG, "网关断开，自动重连中");
        }
        s_gw.connected = false;
        break;
    case WEBSOCKET_EVENT_DATA: {
        if (ev->data_len <= 0) {
            break;                                      /* 空帧忽略 */
        }
        if (ev->op_code == 0x00 || ev->op_code == 0x02) {
            /* 二进制帧（0x02 首片 + 0x00 续片）：大于 buffer_size 的大帧
             * 会被组件按 buffer_size 切成多个 DATA 事件（2026-09-12 上板
             * 实测：豆包 PCM 块 48KB 级，4096 缓冲下整帧被碎成 12 片）。
             * 必须按 payload_offset/payload_len 重组后再分发，否则 PCM 流
             * 千疮百孔（听感"没声音"）。文本帧（0x01）不切分，走原路。 */
            if (s_gw.bin_cb == NULL) {
                break;
            }
            static uint8_t *acc = NULL;                 /* 重组累积缓冲（PSRAM） */
            static size_t acc_len = 0, acc_cap = 0;
            if (ev->payload_offset == 0) {              /* 首片：重置累积 */
                acc_len = 0;
            }
            if (acc_cap < (size_t)ev->payload_len) {    /* 按整帧需求扩容 */
                uint8_t *nb = heap_caps_realloc(acc, (size_t)ev->payload_len,
                                                MALLOC_CAP_SPIRAM);
                if (!nb) {
                    ESP_LOGW(TAG, "重组缓冲扩容失败(%dB)", ev->payload_len);
                    acc = NULL;
                    acc_cap = 0;
                    break;                              /* 本帧放弃 */
                }
                acc = nb;
                acc_cap = (size_t)ev->payload_len;
            }
            if (acc) {
                memcpy(acc + ev->payload_offset, ev->data_ptr, ev->data_len);
                acc_len = (size_t)(ev->payload_offset + ev->data_len);
                if (acc_len >= (size_t)ev->payload_len) {       /* 整帧到手：分发 */
                    s_gw.bin_cb(acc, acc_len);
                }
            }
            break;
        }
        if (ev->op_code != 0x01) {
            break;                                      /* 其他 opcode 忽略 */
        }
        int len = ev->data_len;
        if (len > 512) {
            len = 512;                          /* 步骤 1 消息都很小，截断防御 */
        }
        char buf[513];                          /* 栈上够 */
        memcpy(buf, ev->data_ptr, len);
        buf[len] = '\0';
        /* 步骤 2 起：解析 {"type","data"} 分发给注册方（ASR 结果等） */
        if (s_gw.msg_cb) {
            char *type = NULL, *data = NULL;
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                cJSON *jt = cJSON_GetObjectItem(root, "type");
                cJSON *jd = cJSON_GetObjectItem(root, "data");
                if (jt && cJSON_IsString(jt)) type = jt->valuestring;
                if (jd && cJSON_IsString(jd)) data = jd->valuestring;
                s_gw.msg_cb(type ? type : "", data ? data : "");
            } else {
                ESP_LOGI(TAG, "网关→: %s", buf);        /* 非 JSON 行仅日志 */
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGI(TAG, "网关→: %s", buf);            /* 未注册处理器：仅日志 */
        }
        break;
    }
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WS 错误");                /* 组件内部会触发重连 */
        break;
    default:
        break;                                  /* 其余事件忽略 */
    }
}

esp_err_t gw_client_init(void)
{
    if (s_gw.inited) {                          /* 幂等闸门 */
        return ESP_OK;
    }
    esp_websocket_client_config_t cfg = {
        .uri = CONFIG_GW_URL,
        .reconnect_timeout_ms = CONFIG_GW_RECONNECT_MS,         /* 断线自动重连 */
        .network_timeout_ms = 10000,                            /* 网络超时 10s */
        .buffer_size = 4096,                                    /* 步骤 2 音频帧预留 */
        .task_stack = 6 * 1024,
    };
    s_gw.client = esp_websocket_client_init(&cfg);
    ESP_RETURN_ON_FALSE(s_gw.client, ESP_ERR_NO_MEM, TAG, "ws init failed");
    esp_err_t err = esp_websocket_register_events(
        s_gw.client, WEBSOCKET_EVENT_ANY, gw_event_handler, s_gw.client);
    ESP_RETURN_ON_ERROR(err, TAG, "reg events failed");
    err = esp_websocket_client_start(s_gw.client);      /* 启动（含首次连接） */
    ESP_RETURN_ON_ERROR(err, TAG, "ws start failed");
    s_gw.inited = true;
    ESP_LOGI(TAG, "网关客户端启动: %s", CONFIG_GW_URL);  /* 日志 */
    return ESP_OK;
}

bool gw_client_is_connected(void)
{
    return s_gw.connected;
}

esp_err_t gw_client_send_text(const char *json_text)
{
    if (!s_gw.connected) {                              /* 未连接 */
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_text(
        s_gw.client, json_text, strlen(json_text), portMAX_DELAY);
    return (sent >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t gw_client_send_binary(const void *data, size_t len)
{
    if (!s_gw.connected) {                              /* 未连接 */
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_bin(
        s_gw.client, data, len, portMAX_DELAY);
    return (sent >= 0) ? ESP_OK : ESP_FAIL;
}

void gw_client_set_msg_handler(void (*cb)(const char *type, const char *data))
{
    s_gw.msg_cb = cb;                                   /* 注册/注销消息处理器 */
}

void gw_client_set_binary_handler(void (*cb)(const uint8_t *pcm, size_t bytes))
{
    s_gw.bin_cb = cb;                                   /* 注册/注销二进制帧处理器 */
}
