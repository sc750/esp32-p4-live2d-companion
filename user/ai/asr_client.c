/**
 * @file    asr_client.c
 * @brief   ASR 客户端实现——WAV 头拼装 + 分块 base64 + 手工组包
 *
 * 内存设计（30s 16k 立体声 ≈ 1.92MB PCM 的背景下）：
 *   不用 cJSON 组包——2.5MB 的 base64 字符串在 cJSON 树里走一遭会
 *   瞬间翻倍占内存。手工拼 JSON：固定前缀 + base64 + 固定后缀，
 *   base64 分块编码直写目标缓冲，峰值内存 = wav + body 两份。
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#include "asr_client.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

#include "ai_http.h"
#include "xfyun_iat.h"

#define TAG "asr"

static struct {
    bool inited;
    char url[192];      /* MiMo base + /chat/completions */
    char key[96];
} s_asr;

esp_err_t asr_client_init(void)
{
    if (s_asr.inited) {
        return ESP_OK;
    }
    const char *base = CONFIG_AI_MIMO_BASE_URL;
    size_t bl = strlen(base);
    if (bl > 0 && base[bl - 1] == '/') {
        bl--;
    }
    snprintf(s_asr.url, sizeof(s_asr.url), "%.*s/chat/completions",
             (int)bl, base);
    strlcpy(s_asr.key, CONFIG_AI_MIMO_KEY, sizeof(s_asr.key));
    s_asr.inited = true;
    ESP_LOGI(TAG, "ASR 就绪: %s (model=mimo-v2.5-asr)", s_asr.url);
    /* 讯飞流式后端（配置了 APPID 才启用；失败自动回退 MiMo） */
    xfyun_iat_init();
    return ESP_OK;
}

/** 标准 44 字节 PCM WAV 头（16k/16bit，声道数可变） */
static size_t wav_make_header(uint8_t *h, size_t pcm_len,
                              uint32_t rate, uint16_t ch, uint16_t bits)
{
    uint32_t byte_rate = rate * ch * bits / 8;
    uint16_t block_align = ch * bits / 8;
    memcpy(h, "RIFF", 4);
    uint32_t v = (uint32_t)(36 + pcm_len);          memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;                                         memcpy(h + 16, &v, 4);
    uint16_t u16 = 1;                               memcpy(h + 20, &u16, 2); /* PCM */
    u16 = ch;                                       memcpy(h + 22, &u16, 2);
    v = rate;                                       memcpy(h + 24, &v, 4);
    v = byte_rate;                                  memcpy(h + 28, &v, 4);
    u16 = block_align;                              memcpy(h + 32, &u16, 2);
    u16 = bits;                                     memcpy(h + 34, &u16, 2);
    memcpy(h + 36, "data", 4);
    v = (uint32_t)pcm_len;                          memcpy(h + 40, &v, 4);
    return 44;
}

esp_err_t asr_recognize(const char *wav, size_t wav_len, char **text_out)
{
    ESP_RETURN_ON_FALSE(s_asr.inited && wav && wav_len > 44 && text_out,
                        ESP_ERR_INVALID_ARG, TAG, "bad arg");
    *text_out = NULL;

    /* ---- 后端选择：配置了讯飞 APPID → 流式听写（wss）；否则 MiMo REST ----
     * 讯飞要裸 PCM（无 WAV 头）；我们的录音是单声道 16k/16bit，恰好满足。
     * 讯飞分帧 40ms/1280B：上传耗时≈音频时长，但识别并行、末帧后 ~300ms
     * 出全文——总耗时 ≈ max(音频时长, 2s)，远优于 MiMo 整段的 6s。 */
    if (xfyun_iat_configured()) {
        esp_err_t xerr = xfyun_iat_recognize(wav + 44, wav_len - 44, text_out);
        if (xerr == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "讯飞识别失败(%s)，回退 MiMo 整段", esp_err_to_name(xerr));
    }

    /* ---- 1. 分块 base64 编码到 body 中段 ----
     * 输入 3 字节 → 输出 4 字节；按 3000B 分块（除 pad 干净），末块单独编 */
    const char *BODY_HEAD =
        "{\"model\":\"mimo-v2.5-asr\",\"messages\":[{\"role\":\"user\","
        "\"content\":[{\"type\":\"input_audio\",\"input_audio\":{"
        "\"data\":\"data:audio/wav;base64,";
    const char *BODY_TAIL =
        "\"}}]}],\"asr_options\":{\"language\":\"zh\"}}";
    size_t head_len = strlen(BODY_HEAD);
    size_t tail_len = strlen(BODY_TAIL);
    size_t b64_len = ((wav_len + 2) / 3) * 4;

    char *body = heap_caps_malloc(head_len + b64_len + tail_len + 1,
                                  MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(body, ESP_ERR_NO_MEM, TAG, "body alloc %uKB 失败",
                        (unsigned)((head_len + b64_len + tail_len) / 1024));
    memcpy(body, BODY_HEAD, head_len);
    char *w = body + head_len;

    const unsigned char *src = (const unsigned char *)wav;
    size_t left = wav_len;
    while (left > 0) {
        size_t chunk = (left > 3000) ? 3000 : left;
        size_t olen = 0;
        /* +1：mbedtls 编码完会补一个 '\0'，余量必须给它留一位
         * （教训：卡在最后一组 BUFFER_TOO_SMALL） */
        size_t dcap = b64_len + 1 - (size_t)(w - body - head_len);
        esp_err_t e = mbedtls_base64_encode((unsigned char *)w, dcap,
                                            &olen, src, chunk);
        if (e != ESP_OK) {
            free(body);
            ESP_LOGE(TAG, "base64 编码失败: %d (0x%04x)", e, -e);
            return e;
        }
        w += olen;
        src += chunk;
        left -= chunk;
    }
    memcpy(w, BODY_TAIL, tail_len);
    w[tail_len] = '\0';

    /* ---- 2. POST 非流式 ---- */
    char *resp = heap_caps_malloc(8 * 1024, MALLOC_CAP_SPIRAM);
    if (!resp) {
        free(body);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ai_http_post_json(s_asr.url, s_asr.key, body,
                                      resp, 8 * 1024, 60);
    free(body);
    if (err != ESP_OK) {
        free(resp);
        return err;
    }

    /* ---- 3. 解析 choices[0].message.content ---- */
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_STATE, TAG, "响应非 JSON: %.200s", "");
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *choice = (choices && cJSON_IsArray(choices))
                    ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *msg = choice ? cJSON_GetObjectItem(choice, "message") : NULL;
    cJSON *content = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
    esp_err_t ret = ESP_FAIL;
    if (content && cJSON_IsString(content) && content->valuestring[0]) {
        *text_out = strdup(content->valuestring);
        ret = *text_out ? ESP_OK : ESP_ERR_NO_MEM;
    } else {
        char *dump = cJSON_PrintUnformatted(root);
        ESP_LOGW(TAG, "ASR 无文本结果: %.200s", dump ? dump : "?");
        cJSON_free(dump);
    }
    cJSON_Delete(root);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "识别: %s", *text_out);
    }
    return ret;
}
