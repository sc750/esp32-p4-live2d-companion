/**
 * @file    xfyun_iat.c
 * @brief   讯飞流式听写 WebSocket 客户端实现（L4）
 *
 * 协议实现要点（对照官方文档 2025-09 版）：
 *   1. 鉴权：date(RFC1123 GMT，偏差≤300s) + host + request-line 拼接后
 *      以 APISecret 做 hmac-sha256 → base64 = signature；
 *      api_key/algorithm/headers/signature 拼接 → base64 = authorization；
 *      三参数挂 URL 查询串完成握手。
 *   2. 首帧必须带 common.app_id + business 参数；中间帧只有 data(status=1)；
 *      末帧 data(status=2) 必发（audio 可为空串）。
 *   3. 每帧音频 base64 后 ≤13000B（建议原始 1280B），帧间隔 40ms；
 *      >10s 不发数据服务端断连；会话 ≤60s。
 *   4. 结果为 TextMessage JSON：data.result.ws[].cw[].w 顺序拼接；
 *      data.status==2 表示最后一片；code!=0 为业务错误。
 *
 * 已知简化（MVP）：
 *   - 未开动态修正（dwa=wpgs），结果为纯追加拼接
 *   - DATA 事件假设一事件一个完整 JSON（esp_websocket_client 默认
 *     recv 缓冲 4096 > 结果 JSON 尺寸，官方分帧警告场景未实测出现）
 *
 * @date    2026-09-07
 * @version 1.0.0
 */

#include "xfyun_iat.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define TAG "xfyun"

#define IAT_HOST            "iat-api.xfyun.cn"
#define IAT_PATH            "/v2/iat"
#define FRAME_AUDIO_BYTES   1280        /* 每帧原始音频字节数（协议建议值） */
#define CONNECT_TIMEOUT_MS  8000
#define RESULT_TIMEOUT_MS   15000

/* 事件位 */
#define EVT_CONNECTED   BIT0
#define EVT_DONE        BIT1        /* 收到 data.status==2 或服务端错误 */
#define EVT_FAILED      BIT2

static struct {
    bool inited;
    char app_id[64];
    char api_key[64];
    char api_secret[64];

    EventGroupHandle_t evt;
    volatile bool frame_done;           /* 服务端结果收完（status==2） */
    volatile int  srv_code;             /* 服务端业务错误码（0=无） */
    char text[2048];                    /* 识别文本累积（追加式） */
    size_t text_len;
} s_iat;

esp_err_t xfyun_iat_init(void)
{
    if (s_iat.inited) {
        return ESP_OK;
    }
    strlcpy(s_iat.app_id, CONFIG_ASR_XFYUN_APPID, sizeof(s_iat.app_id));
    strlcpy(s_iat.api_key, CONFIG_ASR_XFYUN_APIKEY, sizeof(s_iat.api_key));
    strlcpy(s_iat.api_secret, CONFIG_ASR_XFYUN_APISECRET, sizeof(s_iat.api_secret));
    s_iat.evt = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_iat.evt, ESP_ERR_NO_MEM, TAG, "evt alloc failed");
    s_iat.inited = true;
    ESP_LOGI(TAG, "讯飞听写就绪 (appid=%s)", s_iat.app_id[0] ? s_iat.app_id : "<未配置>");
    return ESP_OK;
}

bool xfyun_iat_configured(void)
{
    return s_iat.inited && s_iat.app_id[0] != '\0';
}

/* ---------- 鉴权 URL 构造 ---------- */

static size_t urlencode(const char *src, size_t slen, char *dst, size_t dcap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t w = 0;
    for (size_t i = 0; i < slen && w + 4 <= dcap; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[w++] = (char)c;
        } else {
            dst[w++] = '%';
            dst[w++] = hex[c >> 4];
            dst[w++] = hex[c & 0xF];
        }
    }
    dst[w] = '\0';
    return w;
}

/** 构造带鉴权参数的 wss URL。成功写入 out（调用方保证容量 512B） */
static esp_err_t build_auth_url(char *out, size_t cap)
{
    /* 1. date：RFC1123 GMT（依赖 SNTP 已校时；偏差 >300s 会被 403） */
    time_t now = time(NULL);
    struct tm tm_gmt;
    gmtime_r(&now, &tm_gmt);
    char date[64];
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm_gmt);

    /* 2. signature_origin = "host: x\ndate: y\nGET /v2/iat HTTP/1.1" */
    char origin[256];
    int n = snprintf(origin, sizeof(origin),
                     "host: %s\ndate: %s\nGET %s HTTP/1.1",
                     IAT_HOST, date, IAT_PATH);

    /* 3. HMAC-SHA256(APISecret, origin) → base64 = signature */
    unsigned char sha[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int r = mbedtls_md_hmac(md,
                            (const unsigned char *)s_iat.api_secret,
                            strlen(s_iat.api_secret),
                            (const unsigned char *)origin, n, sha);
    if (r != 0) {
        ESP_LOGE(TAG, "hmac 失败: %d", r);
        return ESP_FAIL;
    }
    unsigned char sig_b64[64];
    size_t sig_len = 0;
    mbedtls_base64_encode(sig_b64, sizeof(sig_b64), &sig_len, sha, 32);

    /* 4. authorization_origin → base64 = authorization */
    char auth_origin[256];
    n = snprintf(auth_origin, sizeof(auth_origin),
                 "api_key=\"%s\", algorithm=\"hmac-sha256\", "
                 "headers=\"host date request-line\", signature=\"%s\"",
                 s_iat.api_key, (char *)sig_b64);
    unsigned char auth_b64[384];
    size_t auth_len = 0;
    mbedtls_base64_encode(auth_b64, sizeof(auth_b64), &auth_len,
                          (const unsigned char *)auth_origin, n);

    /* 5. 拼 URL（date 需 urlencode：空格/逗号/冒号） */
    char date_enc[192];
    urlencode(date, strlen(date), date_enc, sizeof(date_enc));
    snprintf(out, cap,
             "wss://%s%s?authorization=%.*s&date=%s&host=%s",
             IAT_HOST, IAT_PATH,
             (int)auth_len, (const char *)auth_b64, date_enc, IAT_HOST);
    return ESP_OK;
}

/* ---------- WebSocket 事件与结果拼接 ---------- */

/** 解析一片结果 JSON：追加 ws[].cw[].w；status==2 置完成 */
static void parse_result_json(const char *json, size_t json_len)
{
    char tmp[2048];
    size_t cp = json_len < sizeof(tmp) - 1 ? json_len : sizeof(tmp) - 1;
    memcpy(tmp, json, cp);
    tmp[cp] = '\0';

    cJSON *root = cJSON_Parse(tmp);
    if (!root) {
        ESP_LOGW(TAG, "结果 JSON 解析失败（分帧/截断?）: %.120s", tmp);
        return;
    }
    cJSON *jcode = cJSON_GetObjectItem(root, "code");
    if (jcode && cJSON_IsNumber(jcode) && jcode->valueint != 0) {
        s_iat.srv_code = jcode->valueint;
        cJSON *jmsg = cJSON_GetObjectItem(root, "message");
        ESP_LOGE(TAG, "服务端错误 %d: %s", jcode->valueint,
                 jmsg && cJSON_IsString(jmsg) ? jmsg->valuestring : "?");
        xEventGroupSetBits(s_iat.evt, EVT_DONE | EVT_FAILED);
        cJSON_Delete(root);
        return;
    }
    cJSON *jdata = cJSON_GetObjectItem(root, "data");
    cJSON *jstatus = jdata ? cJSON_GetObjectItem(jdata, "status") : NULL;
    cJSON *jresult = jdata ? cJSON_GetObjectItem(jdata, "result") : NULL;
    if (jresult) {
        /* 未开动态修正：各片顺序追加 ws[].cw[].w */
        cJSON *jws = cJSON_GetObjectItem(jresult, "ws");
        if (jws && cJSON_IsArray(jws)) {
            int n = cJSON_GetArraySize(jws);
            for (int i = 0; i < n && s_iat.text_len < sizeof(s_iat.text) - 1; i++) {
                cJSON *ws = cJSON_GetArrayItem(jws, i);
                cJSON *cw = cJSON_GetObjectItem(ws, "cw");
                cJSON *c0 = (cw && cJSON_IsArray(cw)) ? cJSON_GetArrayItem(cw, 0) : NULL;
                cJSON *w = c0 ? cJSON_GetObjectItem(c0, "w") : NULL;
                if (w && cJSON_IsString(w)) {
                    size_t wl = strlen(w->valuestring);
                    if (s_iat.text_len + wl >= sizeof(s_iat.text) - 1) {
                        break;
                    }
                    memcpy(s_iat.text + s_iat.text_len, w->valuestring, wl);
                    s_iat.text_len += wl;
                }
            }
            s_iat.text[s_iat.text_len] = '\0';
        }
    }
    if (jstatus && cJSON_IsNumber(jstatus) && jstatus->valueint == 2) {
        s_iat.frame_done = true;
        xEventGroupSetBits(s_iat.evt, EVT_DONE);
    }
    cJSON_Delete(root);
}

static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *event_data)
{
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)event_data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WS 已连接（鉴权通过）");
        xEventGroupSetBits(s_iat.evt, EVT_CONNECTED);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WS 断开");
        xEventGroupSetBits(s_iat.evt, EVT_DONE);
        break;
    case WEBSOCKET_EVENT_DATA:
        /* opcode 0x1 = TextMessage（官方要求帧类型为文本） */
        if (e->op_code == 0x1 && e->data_ptr && e->data_len > 0) {
            parse_result_json((const char *)e->data_ptr, (size_t)e->data_len);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WS 错误");
        xEventGroupSetBits(s_iat.evt, EVT_DONE | EVT_FAILED);
        break;
    default:
        break;
    }
}

/** 发一帧音频 JSON（b64 编码内联构造）。status: 0 首帧 / 1 中间 / 2 末帧 */
static esp_err_t send_audio_frame(esp_websocket_client_handle_t ws,
                                  int status, const char *audio, size_t alen,
                                  bool first)
{
    unsigned char b64[2200];
    size_t b64_len = 0;
    if (alen > 0) {
        ESP_RETURN_ON_ERROR(mbedtls_base64_encode(b64, sizeof(b64), &b64_len,
                                                  (const unsigned char *)audio, alen),
                            TAG, "b64 failed");
    }
    char *json = heap_caps_malloc(b64_len + 512, MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(json, ESP_ERR_NO_MEM, TAG, "no mem");
    int n;
    if (first) {
        n = snprintf(json, b64_len + 512,
                     "{\"common\":{\"app_id\":\"%s\"},"
                     "\"business\":{\"language\":\"zh_cn\",\"domain\":\"iat\","
                     "\"accent\":\"mandarin\",\"vad_eos\":10000,\"ptt\":1},"
                     "\"data\":{\"status\":%d,\"format\":\"audio/L16;rate=16000\","
                     "\"encoding\":\"raw\",\"audio\":\"%.*s\"}}",
                     s_iat.app_id, status, (int)b64_len, (const char *)b64);
    } else {
        n = snprintf(json, b64_len + 512,
                     "{\"data\":{\"status\":%d,\"format\":\"audio/L16;rate=16000\","
                     "\"encoding\":\"raw\",\"audio\":\"%.*s\"}}",
                     status, (int)b64_len, (const char *)b64);
    }
    esp_err_t err = esp_websocket_client_send_text(ws, json, n,
                                                   pdMS_TO_TICKS(5000));
    free(json);
    return (n > 0 && err >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t xfyun_iat_recognize(const char *pcm_mono, size_t len, char **text_out)
{
    ESP_RETURN_ON_FALSE(s_iat.inited && xfyun_iat_configured(),
                        ESP_ERR_INVALID_STATE, TAG, "讯飞未配置");
    ESP_RETURN_ON_FALSE(pcm_mono && len > 320 && len <= 1920000,
                        ESP_ERR_INVALID_ARG, TAG, "音频为空或超过 60s");
    *text_out = NULL;

    /* 1. 鉴权 URL + 客户端 */
    char url[512];
    ESP_RETURN_ON_ERROR(build_auth_url(url, sizeof(url)), TAG, "鉴权 URL 失败");

    esp_websocket_client_config_t cfg = {
        .uri = url,
        .buffer_size = 4096,
        .network_timeout_ms = 10000,
    };
    esp_websocket_client_handle_t ws = esp_websocket_client_init(&cfg);
    ESP_RETURN_ON_FALSE(ws, ESP_FAIL, TAG, "ws init failed");

    xEventGroupClearBits(s_iat.evt, EVT_CONNECTED | EVT_DONE | EVT_FAILED);
    s_iat.frame_done = false;
    s_iat.srv_code = 0;
    s_iat.text_len = 0;
    s_iat.text[0] = '\0';
    esp_err_t err = esp_websocket_client_start(ws);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(ws);
        return err;
    }

    /* 2. 等握手 */
    EventBits_t bits = xEventGroupWaitBits(s_iat.evt, EVT_CONNECTED | EVT_DONE,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if (!(bits & EVT_CONNECTED)) {
        ESP_LOGE(TAG, "握手失败/超时（检查 key、时钟偏差 ≤300s）");
        esp_websocket_client_destroy(ws);
        return ESP_ERR_TIMEOUT;
    }

    /* 3. 分帧上传：1280B/帧、40ms 间隔（协议建议值）
     * 发送期间服务端并行识别，结果经事件回调拼进 s_iat.text */
    err = ESP_OK;
    size_t off = 0;
    bool first = true;
    while (off < len) {
        size_t chunk = (len - off > FRAME_AUDIO_BYTES) ? FRAME_AUDIO_BYTES
                                                       : (len - off);
        int status = (off == 0) ? 0 : 1;
        if (off + chunk >= len) {
            status = 2;                         /* 最后一帧 */
        }
        err = send_audio_frame(ws, status, pcm_mono + off, chunk, first);
        first = false;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "帧发送失败（offset=%u）", (unsigned)off);
            break;
        }
        off += chunk;
        vTaskDelay(pdMS_TO_TICKS(40));          /* 协议建议的 40ms 帧间隔 */
    }
    /* 若音频恰好整除没走到 status=2，补一个空末帧 */
    if (err == ESP_OK && off >= len && s_iat.text_len >= 0) {
        /* 上面的循环里最后一帧已带 status=2（off+chunk>=len 判定），
         * 仅当 len==0 时才会走到这里——已在入参校验挡掉，无需处理 */
    }

    /* 4. 等结果收完（status==2 或连接断开） */
    bits = xEventGroupWaitBits(s_iat.evt, EVT_DONE,
                               pdFALSE, pdFALSE,
                               pdMS_TO_TICKS(RESULT_TIMEOUT_MS));
    esp_websocket_client_close(ws, pdMS_TO_TICKS(2000));
    esp_websocket_client_destroy(ws);

    if (!(bits & EVT_DONE)) {
        ESP_LOGE(TAG, "等待识别结果超时");
        return ESP_ERR_TIMEOUT;
    }
    if (s_iat.srv_code != 0) {
        return ESP_FAIL;
    }
    if (s_iat.text_len == 0) {
        ESP_LOGW(TAG, "识别结果为空（静音？）");
        return ESP_ERR_NOT_FOUND;
    }
    *text_out = strdup(s_iat.text);
    ESP_LOGI(TAG, "⏱ 讯飞识别: %s", *text_out ? *text_out : "(null)");
    return *text_out ? ESP_OK : ESP_ERR_NO_MEM;
}
