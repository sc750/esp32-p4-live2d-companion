/**
 * @file    ai_http.c
 * @brief   AI 云端 HTTP 客户端实现（L3）——esp_http_client + SSE 行解析
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#include "ai_http.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"

#define TAG "ai_http"

/* SSE 行缓冲：TTS 流式单行可能是整段音频的大 base64，给足余量（PSRAM）。
 * 注意：超长行会被静默截断——解析端需能容忍残行（cJSON 解析失败即忽略） */
#define SSE_LINE_BUF_SIZE   (64 * 1024)

/** 请求公共准备：创建 client + 头 + 发 body。成功后 *out 持有句柄 */
static esp_err_t http_common_setup(esp_http_client_handle_t *out,
                                   const char *url, const char *api_key,
                                   const char *json_body, int timeout_ms)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        /* 流式 TTS 相邻音频块可能间隔十几秒，读取超时必须由调用方传入。
         * 过去这里写死 10 秒，tts_synthesize(..., 60) 实际完全没生效。 */
        .timeout_ms = timeout_ms,
        .buffer_size = 4 * 1024,
        .buffer_size_tx = 4 * 1024,
        .crt_bundle_attach = esp_crt_bundle_attach, /* HTTPS 证书校验（内置 CA 集） */
    };
    *out = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(*out, ESP_FAIL, TAG, "client init failed");
    esp_http_client_set_header(*out, "Content-Type", "application/json");
    char auth[192];
    snprintf(auth, sizeof(auth), "Bearer %s", api_key);
    esp_http_client_set_header(*out, "Authorization", auth);
    esp_http_client_set_header(*out, "Accept", "text/event-stream");
    int len = (int)strlen(json_body);
    esp_err_t err = esp_http_client_open(*out, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(*out);
        *out = NULL;
        return err;
    }
    if (esp_http_client_write(*out, json_body, len) != len) {
        ESP_LOGE(TAG, "write body failed");
        esp_http_client_cleanup(*out);
        *out = NULL;
        return ESP_FAIL;
    }
    if (esp_http_client_fetch_headers(*out) < 0) {
        ESP_LOGE(TAG, "fetch headers failed");
        esp_http_client_cleanup(*out);
        *out = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ai_http_post_sse(const char *url, const char *api_key,
                           const char *json_body, ai_sse_cb_t on_data,
                           void *ctx, int recv_timeout_s)
{
    esp_http_client_handle_t client = NULL;
    int timeout_ms = (recv_timeout_s > 0 ? recv_timeout_s : 10) * 1000;
    esp_err_t err = http_common_setup(&client, url, api_key, json_body, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        /* 错误体读出来打日志（前 512B 足够定位：quota/鉴权/模型名） */
        char errbuf[512] = {0};
        int n = 0, r;
        while ((r = esp_http_client_read(client, errbuf + n, sizeof(errbuf) - 1 - n)) > 0) {
            n += r;
            if (n >= (int)sizeof(errbuf) - 1) break;
        }
        ESP_LOGE(TAG, "HTTP %d: %.*s", status, n, errbuf);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* 行缓冲 + 读块缓冲，SSE 以 \n 分行、行首 "data:" 为载荷 */
    char *line = heap_caps_malloc(SSE_LINE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    char *rbuf = heap_caps_malloc(2048, MALLOC_CAP_DEFAULT);
    esp_err_t ret = ESP_OK;
    if (!line || !rbuf) {
        ret = ESP_ERR_NO_MEM;
        goto out;
    }
    size_t llen = 0;
    size_t total_read = 0;                      /* 诊断：服务端到底发了多少 */
    int r;
    while ((r = esp_http_client_read(client, rbuf, sizeof(rbuf))) > 0) {
        total_read += r;
        for (int i = 0; i < r; i++) {
            char c = rbuf[i];
            if (c == '\r') continue;
            if (c != '\n') {
                if (llen < SSE_LINE_BUF_SIZE - 1) {
                    line[llen++] = c;
                }
                continue;
            }
            /* 整行到手 */
            line[llen] = '\0';
            llen = 0;
            if (strncmp(line, "data:", 5) == 0) {
                const char *payload = line + 5;
                if (*payload == ' ') payload++;         /* 剥一个空格 */
                on_data(payload, ctx);
                if (strcmp(payload, "[DONE]") == 0) {
                    goto out;                           /* 服务端收尾 */
                }
            }
            /* 非 data 行（空行/注释/event:）忽略 */
        }
    }
    if (r < 0) {
        ESP_LOGW(TAG, "读流中断（recv 超时或连接关闭）");
        ret = ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "SSE 流结束: 共读 %lu 字节", (unsigned long)total_read);
out:
    free(line);
    free(rbuf);
    esp_http_client_cleanup(client);
    return ret;
}

esp_err_t ai_http_post_json(const char *url, const char *api_key,
                            const char *json_body,
                            char *resp_buf, size_t resp_size,
                            int recv_timeout_s)
{
    esp_http_client_handle_t client = NULL;
    int timeout_ms = (recv_timeout_s > 0 ? recv_timeout_s : 10) * 1000;
    esp_err_t err = http_common_setup(&client, url, api_key, json_body, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    size_t n = 0;
    int r = 0;
    while (n + 1 < resp_size &&
           (r = esp_http_client_read(client, resp_buf + n, (int)(resp_size - 1 - n))) > 0) {
        n += (size_t)r;
    }
    resp_buf[n] = '\0';
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (r < 0) {
        ESP_LOGW(TAG, "读响应中断");
        return ESP_ERR_TIMEOUT;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d: %.256s", status, resp_buf);
        return ESP_FAIL;
    }
    return ESP_OK;
}
