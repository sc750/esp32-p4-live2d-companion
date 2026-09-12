/**
 * @file    doubao_tts.c
 * @brief   豆包（火山引擎）语音合成大模型 TTS 客户端——HTTP chunked 流式实现
 *
 * 数据通路：POST JSON（文本+音色+mp3/24k 参数）→ 服务端边合成边推
 * chunked JSON 行（data = base64 mp3 块）→ base64 解码 → mp3 累积缓冲
 * → 跨块 feed 式 MP3 解码（解码器会话跨 chunk 保持，帧边界由解码器
 * 内部缓冲兜住）→ 逐块回调 24k/mono PCM。
 *
 * @date    2026-09-12
 * @version 1.0.0
 */

#include "doubao_tts.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "mbedtls/base64.h"

#include "ai_http.h"
#include "minimax_tts.h"        /* 复用 minimax_tts_mp3_decode 跨块解码器封装 */
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"   /* 封装解析器注册 */
#include "esp_audio_dec_default.h"          /* 底层 MP3 解码器注册 */

#define TAG "db_tts"

/* 模块状态（URL/凭据/音色 从 Kconfig 注入） */
static struct {
    bool inited;
    char url[160];      /* V3 单向流式端点 */
    char appid[32];     /* App ID */
    char key[160];      /* Access Token / API Key */
    char voice[64];     /* 音色 ID */
    char resource[32];  /* 资源 ID（seed-tts-2.0） */
} s_db;

esp_err_t doubao_tts_init(void)
{
    if (s_db.inited) {                                          /* 幂等闸门 */
        return ESP_OK;
    }
    strlcpy(s_db.url, CONFIG_AI_TTS_DOUBAO_URL, sizeof(s_db.url));
    strlcpy(s_db.appid, CONFIG_AI_TTS_DOUBAO_APPID, sizeof(s_db.appid));
    strlcpy(s_db.key, CONFIG_AI_TTS_DOUBAO_KEY, sizeof(s_db.key));
    strlcpy(s_db.voice, CONFIG_AI_TTS_DOUBAO_VOICE, sizeof(s_db.voice));
    strlcpy(s_db.resource, CONFIG_AI_TTS_DOUBAO_RESOURCE, sizeof(s_db.resource));
    s_db.inited = true;
    if (s_db.key[0] != '\0') {
        ESP_LOGI(TAG, "豆包 TTS 就绪: %s (voice=%s res=%s)",
                 s_db.url, s_db.voice, s_db.resource);
    }
    return ESP_OK;
}

bool doubao_tts_configured(void)
{
    return s_db.inited && s_db.appid[0] != '\0' && s_db.key[0] != '\0';
}

/** 单次流式会话的上下文（收流侧状态全部挂这里，函数内生命周期） */
typedef struct {
    doubao_audio_cb_t on_audio;     /* 调用方音频回调 */
    void *ctx;                      /* 回调上下文 */
    uint8_t *mp3;                   /* mp3 累积缓冲（PSRAM，翻倍扩容） */
    size_t mp3_len;                 /* 当前 mp3 字节数 */
    size_t mp3_cap;                 /* 缓冲容量 */
    int16_t *out;                   /* 单次解码输出缓冲 48KB（PSRAM，会话复用） */
    bool failed;                    /* 业务/协议错误标志 */
    bool first_audio;               /* 是否已回调过首块（延迟日志用） */
    int64_t t0;                     /* 会话起点（首块延迟测量） */
} db_stream_t;

/** base64 音频块 → mp3 累积缓冲（扩容安全，失败置 failed） */
static void db_append_mp3(db_stream_t *st, const uint8_t *mp3_chunk, size_t len)
{
    if (st->failed || len == 0) {
        return;
    }
    if (st->mp3_len + len > st->mp3_cap) {              /* 需要扩容 */
        size_t ncap = st->mp3_cap ? st->mp3_cap : 64 * 1024;    /* 首次 64KB */
        while (ncap < st->mp3_len + len) {
            ncap *= 2;                                  /* 翻倍直到够 */
        }
        uint8_t *nb = heap_caps_realloc(st->mp3, ncap, MALLOC_CAP_SPIRAM);
        if (!nb) {
            ESP_LOGW(TAG, "mp3 缓冲扩容失败(%uB)", (unsigned)ncap);
            st->failed = true;
            return;
        }
        st->mp3 = nb;
        st->mp3_cap = ncap;
    }
    memcpy(st->mp3 + st->mp3_len, mp3_chunk, len);      /* 追加本块 */
    st->mp3_len += len;
}

/** 解码会话句柄（跨 chunk 保持；open/close 由 synthesize_stream 管理） */
static esp_audio_simple_dec_handle_t s_dec;
static bool s_dec_open;

/** 把 mp3 累积缓冲里"尚未解码的尾部"喂给解码器，PCM 逐块回调 */
static void db_decode_pending(db_stream_t *st)
{
    if (st->failed || st->mp3_len == 0) {
        return;
    }
    if (!s_dec_open) {                                  /* 首块：开解码器会话 */
        esp_audio_simple_dec_cfg_t cfg = {
            .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
            .dec_cfg = NULL,
            .cfg_size = 0,
            .use_frame_dec = false,                     /* feed 模式 */
        };
        if (esp_audio_simple_dec_open(&cfg, &s_dec) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "MP3 解码器打开失败");
            st->failed = true;
            return;
        }
        s_dec_open = true;
    }

    int16_t *out = st->out;                             /* 会话级输出缓冲（PSRAM） */
    esp_audio_simple_dec_raw_t raw = {                  /* 输入=全部未解码字节 */
        .buffer = (uint8_t *)st->mp3,
        .len = st->mp3_len,
        .eos = false,                                   /* 流未结束，末尾再补 eos 喂 */
        .consumed = 0,
        .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
    };
    while (raw.consumed < raw.len) {                    /* 喂到输入耗尽 */
        esp_audio_simple_dec_out_t frame = {            /* 输出落点 */
            .buffer = (uint8_t *)out,
            .len = 24 * 1024,                           /* 48KB 容量（字节数） */
            .needed_size = 0,
            .decoded_size = 0,
        };
        esp_audio_err_t dr = esp_audio_simple_dec_process(s_dec, &raw, &frame);
        if (dr == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {      /* 48KB 不够（不会发生，防御） */
            ESP_LOGW(TAG, "解码输出缓冲不足");
            break;
        }
        if (dr != ESP_AUDIO_ERR_OK) {                   /* 坏帧跳过（网络块边界常见） */
            ESP_LOGW(TAG, "解码错误 %d，跳过", dr);
            break;
        }
        if (frame.decoded_size > 0 && st->on_audio) {   /* 有 PCM 产出才回调 */
            if (!st->first_audio) {                     /* 首块延迟日志 */
                st->first_audio = true;
                ESP_LOGI(TAG, "首块 PCM %uB（⏱ %lldms）",
                         (unsigned)frame.decoded_size,
                         (esp_timer_get_time() - st->t0) / 1000);
            }
            st->on_audio(out, frame.decoded_size / sizeof(int16_t), st->ctx);
        }
        if (frame.decoded_size == 0 && raw.consumed == 0) {
            break;                                      /* 无进展（帧不完整，等下一块） */
        }
        /* raw.consumed 由解码器回填：consumed==len 时循环退出；
         * 不足一个完整帧的字节留在缓冲头部，下一轮与后续块拼接
         * ——这里用"整体重喂"的简化法：解码器内部已消费的部分会
         * 通过 consumed 返回，把已消费部分移除后重试。 */
        if (raw.consumed > 0) {
            memmove(st->mp3, st->mp3 + raw.consumed, raw.len - raw.consumed);
            st->mp3_len -= raw.consumed;
            raw.buffer = st->mp3;
            raw.len = st->mp3_len;
            raw.consumed = 0;
        }
    }
}

/** 行式 JSON 回调：音频块取 data(base64) 追加并解码；code!=0 报错 */
static void db_line_handler(const char *line, void *arg)
{
    db_stream_t *st = (db_stream_t *)arg;               /* 会话上下文 */
    cJSON *root = cJSON_Parse(line);                    /* 解析本行 JSON */
    if (!root) {
        return;                                         /* 空行/残行静默忽略 */
    }
    cJSON *jcode = cJSON_GetObjectItem(root, "code");
    if (jcode && cJSON_IsNumber(jcode) && jcode->valueint != 0) {
        cJSON *jmsg = cJSON_GetObjectItem(root, "message");
        ESP_LOGE(TAG, "豆包错误 %d: %s", jcode->valueint,
                 jmsg && cJSON_IsString(jmsg) ? jmsg->valuestring : "?");
        st->failed = true;
        cJSON_Delete(root);
        return;
    }
    cJSON *jdata = cJSON_GetObjectItem(root, "data");   /* 音频块（base64 mp3） */
    if (jdata && cJSON_IsString(jdata) && jdata->valuestring[0]) {
        const char *b64 = jdata->valuestring;           /* base64 串 */
        size_t blen = strlen(b64);
        size_t cap = blen * 3 / 4 + 3;                  /* 解码上限 */
        uint8_t *tmp = heap_caps_malloc(cap, MALLOC_CAP_DEFAULT);       /* 小块，内部堆即可 */
        if (tmp) {
            size_t olen = 0;
            if (mbedtls_base64_decode(tmp, cap, &olen,
                                      (const unsigned char *)b64, blen) == ESP_OK
                && olen > 0) {
                db_append_mp3(st, tmp, olen);           /* 追加到 mp3 缓冲 */
                db_decode_pending(st);                  /* 立即解码回调（真流式） */
            }
            free(tmp);
        }
    }
    /* data=null + sentence 行：文本时间戳事件，本链路用不到，忽略 */
    cJSON_Delete(root);
}

esp_err_t doubao_tts_synthesize_stream(const char *text,
                                       doubao_audio_cb_t on_audio, void *ctx)
{
    ESP_RETURN_ON_FALSE(s_db.inited && text && text[0] && on_audio,
                        ESP_ERR_INVALID_ARG, TAG, "bad arg");

    /* ---- 1. 组请求体（text 经 cJSON 转义内联，防中文/引号破坏 JSON） ---- */
    cJSON *jtext = cJSON_CreateString(text);            /* 文本转 JSON 字符串 */
    ESP_RETURN_ON_FALSE(jtext, ESP_ERR_NO_MEM, TAG, "no mem");
    char *esc = cJSON_PrintUnformatted(jtext);          /* 带引号转义串 */
    cJSON_Delete(jtext);
    ESP_RETURN_ON_FALSE(esc, ESP_ERR_NO_MEM, TAG, "esc failed");

    size_t body_cap = strlen(esc) + 320;                /* 骨架 + 文本 + 音频参数 */
    char *body = heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM);
    if (!body) {
        free(esc);
        return ESP_ERR_NO_MEM;
    }
    snprintf(body, body_cap,
             "{\"user\":{\"uid\":\"sanjiu_esp32p4\"},"
             "\"req_params\":{\"text\":%s,"
             "\"speaker\":\"%s\","
             "\"audio_params\":{\"format\":\"mp3\",\"sample_rate\":24000,\"bit_rate\":64000}}}",
             esc, s_db.voice);                          /* 模型/文本/音色/音频规格 */
    free(esc);

    /* ---- 2. 鉴权头（X-Api-App-Id + X-Api-Access-Key + 资源 ID） ---- */
    char h_appid[64], h_key[192], h_res[64];
    snprintf(h_appid, sizeof(h_appid), "X-Api-App-Id: %s", s_db.appid);
    snprintf(h_key, sizeof(h_key), "X-Api-Access-Key: %s", s_db.key);
    snprintf(h_res, sizeof(h_res), "X-Api-Resource-Id: %s", s_db.resource);
    const char *extra[] = { h_appid, h_key, h_res, NULL };

    /* ---- 3. 流式收块 + 逐块解码回调 ---- */
    db_stream_t st = {                                  /* 会话状态 */
        .on_audio = on_audio,
        .ctx = ctx,
        .mp3 = NULL,
        .mp3_len = 0,
        .mp3_cap = 0,
        .out = heap_caps_malloc(24 * 1024 * sizeof(int16_t), MALLOC_CAP_SPIRAM),        /* 48KB 解码输出（PSRAM） */
        .failed = false,
        .first_audio = false,
        .t0 = esp_timer_get_time(),
    };
    if (!st.out) {                                      /* 分配失败直接放弃 */
        free(body);
        return ESP_ERR_NO_MEM;
    }
    s_dec_open = false;                                 /* 解码器会话复位 */
    esp_err_t err = ai_http_post_lines(s_db.url, "", extra, body,
                                       db_line_handler, &st, 60);
    free(body);

    /* ---- 4. 收尾：补 eos 喂净解码器残样，释放会话资源 ---- */
    if (!st.failed && s_dec_open && st.mp3_len > 0) {   /* 还有残留字节 */
        esp_audio_simple_dec_raw_t raw = {              /* 空 eos 喂，冲出末帧 */
            .buffer = st.mp3,
            .len = st.mp3_len,
            .eos = true,
            .consumed = 0,
            .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
        };
        esp_audio_simple_dec_out_t frame = {
            .buffer = (uint8_t *)st.out,
            .len = 24 * 1024 * sizeof(int16_t),
            .needed_size = 0,
            .decoded_size = 0,
        };
        esp_audio_err_t dr = esp_audio_simple_dec_process(s_dec, &raw, &frame);
        if (dr == ESP_AUDIO_ERR_OK && frame.decoded_size > 0 && st.on_audio) {
            st.on_audio(st.out, frame.decoded_size / sizeof(int16_t), st.ctx);  /* 尾块也回调 */
        }
    }
    if (s_dec_open) {
        esp_audio_simple_dec_close(s_dec);              /* 关解码器会话 */
        s_dec_open = false;
    }
    free(st.mp3);                                       /* mp3 缓冲用完释放 */
    free(st.out);                                       /* 解码输出缓冲释放 */

    if (st.failed) {
        return ESP_FAIL;                                /* 业务/协议错误 */
    }
    if (err != ESP_OK) {
        return err;                                     /* 网络/HTTP 错误 */
    }
    ESP_LOGI(TAG, "TTS 流结束: %uB mp3 / 首块 %lldms",
             (unsigned)st.mp3_len,
             st.first_audio ? (esp_timer_get_time() - st.t0) / 1000 : -1);
    return st.first_audio ? ESP_OK : ESP_ERR_NOT_FOUND; /* 零音频=异常 */
}
