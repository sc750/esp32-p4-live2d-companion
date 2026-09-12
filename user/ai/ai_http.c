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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "ai_http"

/* SSE 行缓冲：TTS 流式单行可能是整段音频的大 base64，给足余量（PSRAM）。
 * 注意：超长行会被静默截断——解析端需能容忍残行（cJSON 解析失败即忽略） */
#define SSE_LINE_BUF_SIZE   (64 * 1024)

/* 全局网络互斥：LLM/TTS/提取/日记共用一条 HTTP 通道，同时只放一个请求。
 * 根因是 PRD 2.2 硬约束——mbedTLS 共享 SHA/AES 硬件加速器，并发 TLS 会崩。
 * Phase3 链路天然串行掩盖了它；Phase4 日记定时器打破串行，故在此咽喉设闸。 */
static SemaphoreHandle_t s_net_lock;

esp_err_t ai_http_init(void)
{
    if (s_net_lock) {                                   /* 幂等 */
        return ESP_OK;                                  /* 无害返回 */
    }
    s_net_lock = xSemaphoreCreateMutex();               /* 建锁 */
    ESP_RETURN_ON_FALSE(s_net_lock, ESP_ERR_NO_MEM, TAG, "no mem for net lock");
    return ESP_OK;                                      /* 成功 */
}

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

/** SSE/行式 内部实现（调用方持锁）；sse_mode=true 按 "data:" 行（OpenAI 系），false 整行 JSON（豆包 chunked） */
static esp_err_t post_sse_inner(const char *url, const char *api_key,
                                const char *const *extra_headers,
                                const char *json_body, ai_sse_cb_t on_data,
                                void *ctx, int recv_timeout_s, bool sse_mode)
{
    esp_http_client_handle_t client = NULL;
    int timeout_ms = (recv_timeout_s > 0 ? recv_timeout_s : 10) * 1000;
    esp_err_t err = http_common_setup(&client, url, api_key, json_body, timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    if (extra_headers) {                                /* 追加自定义鉴权头（豆包 X-Api-* 等） */
        for (int i = 0; extra_headers[i]; i++) {
            const char *sep = strchr(extra_headers[i], ':');
            if (sep) {                                  /* 按 "Key: Value" 拆 */
                char k[64];
                size_t klen = (size_t)(sep - extra_headers[i]);
                if (klen < sizeof(k)) {
                    memcpy(k, extra_headers[i], klen);
                    k[klen] = '\0';
                    const char *v = sep + 1;
                    if (*v == ' ') v++;                 /* 跳过冒号后的空格 */
                    esp_http_client_set_header(client, k, v);
                }
            }
        }
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
            } else if (!sse_mode && line[0] != '\0') {
                on_data(line, ctx);                     /* 行式模式：非空行整行回调 */
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

/** SSE 外壳：持全局网络锁再进内部实现（锁等待上限 150s 覆盖长流） */
esp_err_t ai_http_post_sse(const char *url, const char *api_key,
                           const char *json_body, ai_sse_cb_t on_data,
                           void *ctx, int recv_timeout_s)
{
    if (s_net_lock && xSemaphoreTake(s_net_lock, pdMS_TO_TICKS(150000)) != pdTRUE) {
        ESP_LOGW(TAG, "网络锁等待超时（有请求卡 150s+）");      /* 极端拥堵告警 */
        return ESP_ERR_TIMEOUT;                         /* 放弃本次 */
    }
    esp_err_t r = post_sse_inner(url, api_key, NULL, json_body, on_data, ctx,
                                 recv_timeout_s, true);
    if (s_net_lock) {                                   /* 归还锁（若存在） */
        xSemaphoreGive(s_net_lock);                     /* 释放通道 */
    }
    return r;                                           /* 返回内部结果 */
}

/** 行式流外壳：持全局网络锁再进内部实现（豆包 chunked JSON 行） */
esp_err_t ai_http_post_lines(const char *url, const char *api_key,
                             const char *const *extra_headers,
                             const char *json_body, ai_sse_cb_t on_line,
                             void *ctx, int recv_timeout_s)
{
    if (s_net_lock && xSemaphoreTake(s_net_lock, pdMS_TO_TICKS(150000)) != pdTRUE) {
        ESP_LOGW(TAG, "网络锁等待超时（有请求卡 150s+）");      /* 极端拥堵告警 */
        return ESP_ERR_TIMEOUT;                         /* 放弃本次 */
    }
    esp_err_t r = post_sse_inner(url, api_key, extra_headers, json_body, on_line,
                                 ctx, recv_timeout_s, false);
    if (s_net_lock) {                                   /* 归还锁（若存在） */
        xSemaphoreGive(s_net_lock);                     /* 释放通道 */
    }
    return r;                                           /* 返回内部结果 */
}

/** JSON 内部实现（调用方持锁） */
static esp_err_t post_json_inner(const char *url, const char *api_key,
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
    /* 截断告警（2026-09-10 追加）：循环是"缓冲满就停"，调用方拿到的是被
     * 割掉尾巴的 JSON —— cJSON_Parse 只会报"解析失败"，看不出是容量问题。
     * MiniMax TTS 的 768KB 缓冲就这么坑过一次（响应恒 767KB 非 JSON），
     * 故这里主动说明，把"猜"变成"一眼看到"。 */
    if (n + 1 >= resp_size) {                       /* 顶到容量上限 = 极可能被截断 */
        ESP_LOGE(TAG, "响应缓冲已满(%uB)，JSON 很可能被截断——请调大调用方 resp_cap",   /* 明确指路 */
                 (unsigned)resp_size);               /* 报当前容量 */
    }
    ESP_LOGI(TAG, "POST 响应: HTTP %d, 收 %uB",      /* 诊断：定位 200+空 body 场景 */
             status, (unsigned)n);
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

/** JSON 外壳：持全局网络锁再进内部实现（锁等待上限 150s） */
esp_err_t ai_http_post_json(const char *url, const char *api_key,
                            const char *json_body,
                            char *resp_buf, size_t resp_size,
                            int recv_timeout_s)
{
    if (s_net_lock && xSemaphoreTake(s_net_lock, pdMS_TO_TICKS(150000)) != pdTRUE) {
        ESP_LOGW(TAG, "网络锁等待超时（有请求卡 150s+）");      /* 极端拥堵告警 */
        return ESP_ERR_TIMEOUT;                         /* 放弃本次 */
    }
    esp_err_t r = post_json_inner(url, api_key, json_body, resp_buf, resp_size,
                                  recv_timeout_s);
    if (s_net_lock) {                                   /* 归还锁（若存在） */
        xSemaphoreGive(s_net_lock);                     /* 释放通道 */
    }
    return r;                                           /* 返回内部结果 */
}
