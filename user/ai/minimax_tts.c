/**
 * @file    minimax_tts.c
 * @brief   MiniMax T2A v2 客户端实现——hex 解码 + PCM 整段输出
 *
 * 协议要点（官方 OpenAPI 逐条核对）：
 *   - 端点 POST {AI_TTS_MINIMAX_URL}，Authorization: Bearer <key>
 *   - 合成文本放 "text"；音色/语速/情绪放 voice_setting；音频规格放
 *     audio_setting（2026-09-12 起为 24000Hz/64kbps/mono/mp3——pcm 裸流
 *     再经 hex 编码线上要 96KB/s，Wi-Fi 降级模式常断粮；mp3 砍 6 倍，
 *     到本地用 esp_audio_simple_dec 解回 PCM，接口不变）
 *   - 响应 JSON：data.audio 为 hex 编码的 mp3 字节流（每 2 字符 1 字节，
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
#include "esp_audio_simple_dec.h"           /* MP3 → PCM 解码（复用音乐服务同款解码器） */
#include "esp_audio_simple_dec_default.h"   /* 封装解析器注册 */
#include "esp_audio_dec_default.h"          /* 底层 MP3 解码器注册 */

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

/**
 * MP3 裸流 → PCM 整段解码（feed 式，与 music_service 同款解码器）。
 * 输入：MiniMax 返回的 mp3 字节（24kHz mono，64kbps）；输出：int16 mono PCM。
 * @return ESP_OK 成功；*pcm_out 所有权转移给调用方（heap_caps_free 释放）
 */
esp_err_t minimax_tts_mp3_decode(const uint8_t *mp3, size_t mp3_len,
                                 int16_t **pcm_out, size_t *samples_out)
{
    esp_audio_simple_dec_cfg_t cfg = {                      /* feed 式解码器配置 */
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,          /* MP3 */
        .dec_cfg = NULL,                                    /* 内置默认参数 */
        .cfg_size = 0,
        .use_frame_dec = false,                             /* feed 模式（自动解析帧） */
    };
    esp_audio_simple_dec_handle_t dec = NULL;               /* 解码器句柄 */
    if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "MP3 解码器打开失败");                 /* 报错 */
        return ESP_FAIL;
    }

    /* 64kbps @24kHz 压缩比约 6:1 → 预分配 8 倍余量，不够再翻倍扩容 */
    size_t pcm_cap = mp3_len * 8;
    uint8_t *pcm = heap_caps_malloc(pcm_cap, MALLOC_CAP_SPIRAM);    /* PCM 输出（PSRAM） */
    if (!pcm) {
        esp_audio_simple_dec_close(dec);
        return ESP_ERR_NO_MEM;
    }
    size_t pcm_len = 0;                                     /* 已解码 PCM 字节 */
    esp_audio_simple_dec_raw_t raw = {                      /* 输入描述（整段一次给） */
        .buffer = (uint8_t *)mp3,
        .len = mp3_len,
        .eos = true,                                        /* 单段合成：喂入即流结束 */
        .consumed = 0,
        .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
    };
    esp_err_t ret = ESP_OK;                                 /* 返回值 */
    while (1) {                                             /* 解码主循环 */
        esp_audio_simple_dec_out_t frame = {                /* 输出落点（剩余空间） */
            .buffer = pcm + pcm_len,
            .len = pcm_cap - pcm_len,
            .needed_size = 0,
            .decoded_size = 0,
        };
        esp_audio_err_t dr = esp_audio_simple_dec_process(dec, &raw, &frame);
        if (dr == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {          /* PCM 缓冲不够 */
            pcm_cap = pcm_len + frame.needed_size;          /* 按需求扩容（同 music_service 模式） */
            uint8_t *nb = heap_caps_realloc(pcm, pcm_cap, MALLOC_CAP_SPIRAM);
            if (!nb) {
                ESP_LOGE(TAG, "PCM 输出扩容失败");
                ret = ESP_ERR_NO_MEM;
                break;
            }
            pcm = nb;                                       /* 换新缓冲，重试同一输入 */
            continue;
        }
        if (dr != ESP_AUDIO_ERR_OK) {                       /* 单帧坏不弃整段 */
            ESP_LOGW(TAG, "MP3 解码错误 %d，提前收尾", dr);  /* 告警 */
            break;
        }
        pcm_len += frame.decoded_size;                      /* 累计 PCM 产量 */
        if (raw.consumed >= raw.len) {                      /* 输入耗尽 */
            break;
        }
        raw.buffer += raw.consumed;                         /* 游标推进 */
        raw.len -= raw.consumed;
        raw.consumed = 0;
    }
    esp_audio_simple_dec_close(dec);                        /* 关解码器 */
    if (ret != ESP_OK || pcm_len < 2) {                     /* 解码失败或空输出 */
        free(pcm);
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }
    *pcm_out = (int16_t *)pcm;                              /* 所有权交调用方 */
    *samples_out = pcm_len / sizeof(int16_t);               /* 样本数 = 字节/2 */
    return ESP_OK;
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
             "\"voice_setting\":{\"voice_id\":\"%s\",\"speed\":0.9,\"vol\":1.0,\"pitch\":0},"
             "\"audio_setting\":{\"sample_rate\":24000,\"bitrate\":64000,\"format\":\"mp3\",\"channel\":1}}",
             s_mm.model, esc, s_mm.voice);                  /* 模型/文本/音色 */
    free(esc);                                              /* 转义串用完释放 */

    /* ---- 2. POST 非流式：整段 hex 音频一次收齐（响应可达数百 KB） ----
     * 容量 2MB 是实测定的（2026-09-10）：MiniMax 回的是 hex，字符数 = PCM
     * 字节数 × 2，三玖一条 292 字回复就合成出 795KB PCM → hex 1.59MB，
     * 加上 JSON 骨架约 1.6MB。原先给 768KB，收满被静默截断，JSON 不完整
     * → cJSON_Parse 失败 → 每次都白白回退到 MiMo（日志"响应非 JSON
     * （共 767KB）"，767 = 768 - 1 正是截断标志）。PSRAM 余量 14MB，
     * 2MB 给得起；峰值内存 = 2MB 响应 + 1.6MB PCM 解码缓冲 ≈ 3.6MB。 */
    size_t resp_cap = 2 * 1024 * 1024;                      /* 响应缓冲 2MB（hex 2 倍膨胀 + 余量） */
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

    /* ---- 3. 解析响应：data.audio（hex）+ base_resp.status_code ----
     * free(resp) 必须放在所有诊断日志之后（use-after-free 会让日志读到空） */
    cJSON *root = cJSON_Parse(resp);                        /* 解析响应 JSON */
    if (!root) {                                            /* 解析失败：打印长度+头部定位 */
        ESP_LOGE(TAG, "响应非 JSON（共 %uKB）头 64B: %.64s",         /* 长度可判断截断，       */
                 (unsigned)(strlen(resp) / 1024), resp);    /* 头部可判断错误页/gzip */
        free(resp);                                         /* 释放后返回 */
        return ESP_ERR_INVALID_STATE;                       /* 返回数据错误 */
    }
    free(resp);                                             /* 解析成功，缓冲用完释放 */
    resp = NULL;                                            /* 防止后续误用 */

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

    /* ---- 4. hex 解码 → MP3 字节 → 本地解回 PCM（2026-09-12 换 mp3 容器：
     * 原 pcm 裸流再经 hex 编码，线上字节数 = 音频 2 倍 ≈96KB/s，Wi-Fi 降级
     * 模式下经常断粮（卡顿根因）；mp3@64kbps + hex ≈16KB/s，砍 6 倍。
     * 解码后仍是 24kHz mono PCM，下游接口不变。 ---- */
    const char *hex = jaudio->valuestring;                  /* hex 字符串 */
    size_t hex_len = strlen(hex);                           /* hex 长度 */
    size_t mp3_cap = hex_len / 2;                           /* 解码后 mp3 字节数 */
    uint8_t *mp3 = heap_caps_malloc(mp3_cap, MALLOC_CAP_SPIRAM);    /* mp3 缓冲（PSRAM） */
    if (!mp3) {                                             /* 分配失败 */
        cJSON_Delete(root);                                 /* 释放 JSON 树 */
        return ESP_ERR_NO_MEM;                              /* 返回内存错误 */
    }
    size_t mp3_len = hex_decode(hex, hex_len, mp3);         /* hex → mp3 二进制 */
    cJSON_Delete(root);                                     /* JSON 树用完释放 */
    if (mp3_len == (size_t)-1 || mp3_len < 4) {             /* 非法 hex 或数据过短 */
        free(mp3);                                          /* 释放缓冲 */
        ESP_LOGE(TAG, "hex 解码失败或 mp3 数据过短");
        return ESP_ERR_INVALID_STATE;                       /* 返回数据错误 */
    }

    /* ---- 5. MP3 → PCM（所有权转移给调用方） ---- */
    esp_err_t derr = minimax_tts_mp3_decode(mp3, mp3_len, pcm_out, samples_out);    /* 解码整段 */
    free(mp3);                                              /* mp3 中间缓冲用完释放 */
    if (derr != ESP_OK) {                                   /* 解码失败 */
        return derr;                                        /* 返回错误（调用方回退 MiMo） */
    }
    ESP_LOGI(TAG, "MiniMax 合成: mp3 %uB → PCM %u 样本 (24kHz mono)",
             (unsigned)mp3_len, (unsigned)*samples_out);    /* 带宽证据：mp3 字节数 vs 旧 pcm */
    return ESP_OK;                                          /* 成功返回 */
}
