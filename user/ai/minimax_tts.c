/**
 * @file    minimax_tts.c
 * @brief   MiniMax T2A v2 客户端实现——hex 解码 + PCM 整段输出
 *
 * 协议要点（官方 OpenAPI 逐条核对）：
 *   - 端点 POST {AI_TTS_MINIMAX_URL}，Authorization: Bearer <key>
 *   - 合成文本放 "text"；音色/语速/情绪放 voice_setting；音频规格放
 *     audio_setting（本模块固定 24000Hz/16bit/mono/pcm 裸流，直推 codec）
 *   - 响应 JSON：data.audio 为 **hex 编码** PCM（每 2 字符 1 字节，
 *     注意与讯飞/MiMo 的 base64 不同！）；base_resp.status_code!=0 为错误
 *
 * @date    2026-09-08
 * @version 1.0.0
 */

#include "minimax_tts.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "ai_http.h"

#define TAG "mm_tts"

/* 模块状态（URL/Key/模型/音色 从 Kconfig 注入） */
static struct {
    bool inited;
    char url[192];      /* T2A v2 端点 */
    char key[160];      /* Bearer key（sk-api- 前缀较长，留足余量） */
    char model[48];     /* 模型名（speech-2.6-turbo 等） */
    char voice[64];     /* 音色 ID */
} s_mm;

esp_err_t minimax_tts_init(void)
{
    if (s_mm.inited) {                                          /* 幂等闸门 */
        return ESP_OK;                                          /* 重复调用无害 */
    }
    strlcpy(s_mm.url, CONFIG_AI_TTS_MINIMAX_URL, sizeof(s_mm.url));     /* 端点 */
    strlcpy(s_mm.key, CONFIG_AI_TTS_MINIMAX_KEY, sizeof(s_mm.key));     /* Key */
    strlcpy(s_mm.model, CONFIG_AI_TTS_MINIMAX_MODEL, sizeof(s_mm.model));       /* 模型 */
    strlcpy(s_mm.voice, CONFIG_AI_TTS_MINIMAX_VOICE, sizeof(s_mm.voice));       /* 音色 */
    s_mm.inited = true;                                     /* 置就绪标志 */
    ESP_LOGI(TAG, "MiniMax TTS 就绪: %s (model=%s voice=%s)",
             s_mm.url, s_mm.model, s_mm.voice);
    return ESP_OK;                                          /* 初始化成功 */
}

bool minimax_tts_configured(void)
{
    return s_mm.inited && s_mm.key[0] != '\0';              /* Key 非空即视为启用 */
}

/**
 * hex 解码：每 2 个 hex 字符 → 1 字节（大小写均兼容）
 * @return 解码后字节数；(size_t)-1 表示遇到非法字符
 */
static size_t hex_decode(const char *hex, size_t hex_len, uint8_t *out)
{
    static const int8_t val[256] = {                        /* hex 字符 → 数值查找表 */
        ['0'] = 1,  ['1'] = 2,  ['2'] = 3,  ['3'] = 4,  ['4'] = 5,  /* 0-9 → 值+1 */
        ['5'] = 6,  ['6'] = 7,  ['7'] = 8,  ['8'] = 9,  ['9'] = 10,
        ['a'] = 11, ['b'] = 12, ['c'] = 13, ['d'] = 14, ['e'] = 15, ['f'] = 16, /* 小写 a-f */
        ['A'] = 11, ['B'] = 12, ['C'] = 13, ['D'] = 14, ['E'] = 15, ['F'] = 16, /* 大写 A-F */
    };
    size_t out_len = 0;                                     /* 输出字节游标 */
    for (size_t i = 0; i + 1 < hex_len; i += 2) {           /* 每 2 字符一组 */
        int8_t hi = val[(unsigned char)hex[i]];             /* 高半字节 */
        int8_t lo = val[(unsigned char)hex[i + 1]];         /* 低半字节 */
        if (hi == 0 || lo == 0) {                           /* 非法 hex 字符（表外为 0） */
            return (size_t)-1;                              /* 报告错误 */
        }
        out[out_len++] = (uint8_t)((hi - 1) << 4 | (lo - 1));       /* 拼字节（表值-1 还原） */
    }
    return out_len;                                         /* 返回解码字节数 */
}

esp_err_t minimax_tts_synthesize(const char *text,
                                 int16_t **pcm_out, size_t *samples_out)
{
    ESP_RETURN_ON_FALSE(s_mm.inited && text && text[0],     /* 参数校验 */
                        ESP_ERR_INVALID_ARG, TAG, "bad arg");
    *pcm_out = NULL;                                        /* 输出指针先置空 */
    *samples_out = 0;

    /* ---- 1. 组请求体（text 用 cJSON 转义后内联，防中文/引号破坏 JSON） ---- */
    cJSON *jtext = cJSON_CreateString(text);                /* 文本转 JSON 字符串节点 */
    ESP_RETURN_ON_FALSE(jtext, ESP_ERR_NO_MEM, TAG, "no mem");
    char *esc = cJSON_PrintUnformatted(jtext);              /* 带引号的转义文本 */
    cJSON_Delete(jtext);                                    /* 临时节点释放 */
    ESP_RETURN_ON_FALSE(esc, ESP_ERR_NO_MEM, TAG, "esc failed");

    size_t body_cap = strlen(esc) + 640;                    /* 请求体容量（固定骨架+文本） */
    char *body = heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM);     /* 请求体（PSRAM） */
    if (!body) {                                            /* 分配失败 */
        free(esc);                                          /* 释放转义串 */
        return ESP_ERR_NO_MEM;                              /* 返回内存错误 */
    }
    snprintf(body, body_cap,                                /* 按官方 OpenAPI 组包 */
             "{\"model\":\"%s\",\"text\":%s,\"stream\":false,"      /* 模型/文本/非流式 */
             "\"voice_setting\":{\"voice_id\":\"%s\",\"speed\":1.0,\"vol\":1.0,\"pitch\":0},"
             "\"audio_setting\":{\"sample_rate\":24000,\"format\":\"pcm\",\"channel\":1}}",
             s_mm.model, esc, s_mm.voice);                  /* 模型/文本/音色 */
    free(esc);                                              /* 转义串用完释放 */

    /* ---- 2. POST 非流式：整段 hex 音频一次收齐（响应可达数百 KB） ---- */
    size_t resp_cap = 768 * 1024;                           /* 响应缓冲 768KB（hex 2 倍膨胀） */
    char *resp = heap_caps_malloc(resp_cap, MALLOC_CAP_SPIRAM);     /* 响应缓冲（PSRAM） */
    if (!resp) {                                            /* 分配失败 */
        free(body);                                         /* 释放 body */
        return ESP_ERR_NO_MEM;                              /* 返回内存错误 */
    }
    esp_err_t err = ai_http_post_json(s_mm.url, s_mm.key, body,     /* POST 收完整响应 */
                                      resp, resp_cap, 90);          /* 90s 超时（整段合成较慢） */
    free(body);                                             /* 请求体用完释放 */
    if (err != ESP_OK) {                                    /* 网络/HTTP 错误 */
        free(resp);                                         /* 释放响应缓冲 */
        return err;                                         /* 返回网络错误 */
    }

    /* ---- 3. 解析响应：data.audio（hex）+ base_resp.status_code ---- */
    cJSON *root = cJSON_Parse(resp);                        /* 解析响应 JSON */
    free(resp);                                             /* 响应缓冲用完释放 */
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_STATE, TAG, "响应非 JSON");

    cJSON *base_resp = cJSON_GetObjectItem(root, "base_resp");      /* 业务状态对象 */
    cJSON *jcode = base_resp ? cJSON_GetObjectItem(base_resp, "status_code") : NULL;
    if (jcode && cJSON_IsNumber(jcode) && jcode->valueint != 0) {   /* 非 0 = 业务错误 */
        cJSON *jmsg = cJSON_GetObjectItem(base_resp, "status_msg"); /* 取错误描述 */
        ESP_LOGE(TAG, "MiniMax 错误 %d: %s", jcode->valueint,       /* 打印错误 */
                 jmsg && cJSON_IsString(jmsg) ? jmsg->valuestring : "?");
        cJSON_Delete(root);                             /* 释放 JSON 树 */
        return ESP_FAIL;                                /* 返回失败 */
    }

    cJSON *jdata = cJSON_GetObjectItem(root, "data");       /* data 对象（可能为 null） */
    cJSON *jaudio = jdata ? cJSON_GetObjectItem(jdata, "audio") : NULL;     /* hex 音频字段 */
    if (!jaudio || !cJSON_IsString(jaudio) || jaudio->valuestring[0] == '\0') {     /* 无音频 */
        ESP_LOGW(TAG, "MiniMax 无音频数据");                 /* 告警 */
        cJSON_Delete(root);                                 /* 释放 JSON 树 */
        return ESP_ERR_NOT_FOUND;                           /* 返回无结果 */
    }

    /* ---- 4. hex 解码 → PCM（int16 mono 24kHz） ---- */
    const char *hex = jaudio->valuestring;                  /* hex 字符串 */
    size_t hex_len = strlen(hex);                           /* hex 长度 */
    size_t pcm_cap = hex_len / 2;                           /* 解码后 PCM 字节数 */
    uint8_t *pcm_bytes = heap_caps_malloc(pcm_cap, MALLOC_CAP_SPIRAM);      /* PCM 缓冲（PSRAM） */
    if (!pcm_bytes) {                                       /* 分配失败 */
        cJSON_Delete(root);                                 /* 释放 JSON 树 */
        return ESP_ERR_NO_MEM;                              /* 返回内存错误 */
    }
    size_t pcm_bytes_len = hex_decode(hex, hex_len, pcm_bytes);     /* hex → 二进制 */
    cJSON_Delete(root);                                     /* JSON 树用完释放 */
    if (pcm_bytes_len == (size_t)-1 || pcm_bytes_len < 2) { /* 非法 hex 或样本不足 */
        free(pcm_bytes);                                    /* 释放缓冲 */
        ESP_LOGE(TAG, "hex 解码失败或样本过短");
        return ESP_ERR_INVALID_STATE;                       /* 返回数据错误 */
    }

    /* ---- 5. 交出 PCM（所有权转移给调用方） ---- */
    *pcm_out = (int16_t *)pcm_bytes;                        /* 输出 PCM 指针 */
    *samples_out = pcm_bytes_len / sizeof(int16_t);         /* 样本数 = 字节/2 */
    ESP_LOGI(TAG, "MiniMax 合成: %u 样本 (24kHz mono)", (unsigned)*samples_out);
    return ESP_OK;                                          /* 成功返回 */
}
