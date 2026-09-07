/**
 * @file    tts_client.c
 * @brief   TTS 客户端实现——SSE 音频块解析 + base64 → PCM
 *
 * 协议要点（官方文档）：
 *   - 合成文本必须放 role:"assistant" 的 content；role:"user" 是风格指令
 *   - audio: {"format":"pcm16","voice":"冰糖"}；流式时每个 SSE data 行的
 *     choices[0].delta.audio.data 是一段 base64 的 24kHz PCM16LE mono
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#include "tts_client.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

#include "ai_http.h"

#define TAG "tts"

static struct {
    bool inited;
    char url[192];
    char key[96];
    int chunks;         /* 诊断：收到的音频块数 */
    size_t bytes;       /* 诊断：累计 PCM 字节 */
    bool err_dumped;    /* 错误载荷只 dump 一次 */
    int64_t t0;         /* 合成起点（算首块延迟） */
} s_tts;

/** 单次请求的音频回调上下文；SSE 处理在 tts_synthesize 调用期内同步执行。 */
typedef struct {
    tts_audio_cb_t on_audio;
    void *ctx;
} tts_callback_ctx_t;

esp_err_t tts_client_init(void)
{
    if (s_tts.inited) {
        return ESP_OK;
    }
    const char *base = CONFIG_AI_MIMO_BASE_URL;
    size_t bl = strlen(base);
    if (bl > 0 && base[bl - 1] == '/') {
        bl--;
    }
    snprintf(s_tts.url, sizeof(s_tts.url), "%.*s/chat/completions",
             (int)bl, base);
    strlcpy(s_tts.key, CONFIG_AI_MIMO_KEY, sizeof(s_tts.key));
    s_tts.inited = true;
    ESP_LOGI(TAG, "TTS 就绪: %s (model=mimo-v2.5-tts, voice=冰糖)", s_tts.url);
    return ESP_OK;
}

/** SSE 单行 → delta.audio.data → base64 解码 → PCM 回调 */
static void sse_audio_handler(const char *data_line, void *arg)
{
    tts_callback_ctx_t *callback = (tts_callback_ctx_t *)arg;
    if (strcmp(data_line, "[DONE]") == 0) {
        return;
    }
    cJSON *root = cJSON_Parse(data_line);
    if (!root) {
        return;
    }
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *choice = (choices && cJSON_IsArray(choices))
                    ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *delta = choice ? cJSON_GetObjectItem(choice, "delta") : NULL;
    cJSON *audio = delta ? cJSON_GetObjectItem(delta, "audio") : NULL;
    cJSON *data = audio ? cJSON_GetObjectItem(audio, "data") : NULL;
    if (data && cJSON_IsString(data) && data->valuestring[0]) {
        const char *b64 = data->valuestring;
        size_t blen = strlen(b64);
        size_t cap = blen * 3 / 4 + 3;
        unsigned char *pcm = heap_caps_malloc(cap, MALLOC_CAP_DEFAULT);
        if (pcm) {
            size_t olen = 0;
            if (mbedtls_base64_decode(pcm, cap, &olen,
                                      (const unsigned char *)b64, blen) == ESP_OK
                && olen >= 2) {
                s_tts.chunks++;
                s_tts.bytes += olen;
                if (s_tts.chunks == 1) {
                    ESP_LOGI(TAG, "收到首个音频块 (%uB)，开始播放 "
                             "（⏱ TTS 首块: %lldms）", (unsigned)olen,
                             (esp_timer_get_time() - s_tts.t0) / 1000);
                }
                callback->on_audio((const int16_t *)pcm, olen / 2, callback->ctx);
            }
            free(pcm);
        }
    } else if (!s_tts.err_dumped) {
        /* 非 audio 数据行：可能是流内错误载荷——dump 一次定位问题 */
        s_tts.err_dumped = true;
        ESP_LOGW(TAG, "非音频数据行: %.200s", data_line);
    }
    cJSON_Delete(root);
}

esp_err_t tts_synthesize(const char *text, const char *style,
                         tts_audio_cb_t on_audio, void *ctx)
{
    ESP_RETURN_ON_FALSE(s_tts.inited && text && text[0],
                        ESP_ERR_INVALID_ARG, TAG, "bad arg");

    /* 手工组包（文本含中文/引号 → cJSON 只负责字符串转义再拼进模板） */
    cJSON *jtext = cJSON_CreateString(text);
    ESP_RETURN_ON_FALSE(jtext, ESP_ERR_NO_MEM, TAG, "no mem");
    char *esc = cJSON_PrintUnformatted(jtext);      /* 带引号的转义字符串 */
    cJSON_Delete(jtext);
    ESP_RETURN_ON_FALSE(esc, ESP_ERR_NO_MEM, TAG, "esc failed");

    char *body = heap_caps_malloc(strlen(esc) + 512, MALLOC_CAP_SPIRAM);
    if (!body) {
        free(esc);
        return ESP_ERR_NO_MEM;
    }
    snprintf(body, strlen(esc) + 512,
             "{\"model\":\"mimo-v2.5-tts\",\"stream\":true,"
             "\"messages\":["
             "{\"role\":\"user\",\"content\":\"%s\"},"
             "{\"role\":\"assistant\",\"content\":%s}],"
             "\"audio\":{\"format\":\"pcm16\",\"voice\":\"冰糖\"}}",
             style ? style : "", esc);
    free(esc);

    tts_callback_ctx_t callback = {
        .on_audio = on_audio,
        .ctx = ctx,
    };
    s_tts.chunks = 0;
    s_tts.bytes = 0;
    s_tts.err_dumped = false;
    s_tts.t0 = esp_timer_get_time();
    esp_err_t err = ai_http_post_sse(s_tts.url, s_tts.key, body,
                                     sse_audio_handler, &callback, 60);
    free(body);
    ESP_LOGI(TAG, "TTS 流结束: %d 块 / %uKB PCM%s",
             s_tts.chunks, (unsigned)(s_tts.bytes / 1024),
             s_tts.chunks ? "" : "  ← 零音频！看上面的'非音频数据行'定位");
    return err;
}
