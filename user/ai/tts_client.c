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

#include "tts_client.h"     /* 本模块公开接口 */

#include <string.h>         /* strlen / strcmp */
#include <stdlib.h>         /* free */
#include <stdio.h>          /* snprintf */

#include "esp_log.h"        /* ESP_LOGx 日志 */
#include "esp_check.h"      /* ESP_RETURN_ON_* 检查宏 */
#include "esp_heap_caps.h"  /* heap_caps_malloc（音频块缓冲） */
#include "esp_timer.h"      /* esp_timer_get_time（首块延迟测量） */
#include "mbedtls/base64.h" /* base64 解码（音频块） */
#include "cJSON.h"          /* SSE 行 JSON 解析 */

#include "ai_http.h"        /* HTTP POST + SSE 流式封装 */

#define TAG "tts"           /* 本模块日志标签 */

/* 模块状态 + 诊断计数器（每轮合成开始时复位） */
static struct {
    bool inited;            /* 初始化完成标志（幂等闸门） */
    char url[192];          /* 完整接口地址：MiMo base + /chat/completions */
    char key[96];           /* MiMo API Key（Bearer 令牌） */
    int chunks;             /* 诊断：收到的音频块数 */
    size_t bytes;           /* 诊断：累计 PCM 字节 */
    bool err_dumped;        /* 错误载荷只 dump 一次 */
    int64_t t0;             /* 合成起点（算首块延迟） */
} s_tts;

/** 单次请求的音频回调上下文；SSE 处理在 tts_synthesize 调用期内同步执行。 */
typedef struct {
    tts_audio_cb_t on_audio;    /* 调用方的音频块回调 */
    void *ctx;                  /* 调用方上下文 */
} tts_callback_ctx_t;

/**
 * 初始化 TTS 客户端：拼 MiMo URL + 载 Key（幂等）
 * @return ESP_OK 就绪
 */
esp_err_t tts_client_init(void)
{
    if (s_tts.inited) {                                     /* 幂等闸门：已初始化直接返回 */
        return ESP_OK;                                      /* 重复调用无害 */
    }
    const char *base = CONFIG_AI_MIMO_BASE_URL;             /* Kconfig 里的 MiMo 基地址 */
    size_t bl = strlen(base);                               /* 基地址长度 */
    if (bl > 0 && base[bl - 1] == '/') {                    /* 若基地址以 / 结尾 */
        bl--;                                               /* 去掉尾斜杠（避免双斜杠） */
    }
    snprintf(s_tts.url, sizeof(s_tts.url), "%.*s/chat/completions", /* 拼 OpenAI 兼容端点 */
             (int)bl, base);                              /* 用精度截断去掉尾斜杠 */
    strlcpy(s_tts.key, CONFIG_AI_MIMO_KEY, sizeof(s_tts.key));  /* 载入 MiMo API Key */
    s_tts.inited = true;                                    /* 置就绪标志 */
    ESP_LOGI(TAG, "TTS 就绪: %s (model=mimo-v2.5-tts, voice=冰糖)", s_tts.url); /* 打印后端信息 */
    return ESP_OK;                                          /* 初始化成功 */
}

/** SSE 单行 → delta.audio.data → base64 解码 → PCM 回调 */
static void sse_audio_handler(const char *data_line, void *arg)
{
    tts_callback_ctx_t *callback = (tts_callback_ctx_t *)arg;   /* 取回调用上下文 */
    if (strcmp(data_line, "[DONE]") == 0) {                 /* [DONE] = 流结束标记 */
        return;                                             /* 无需处理，外层收尾 */
    }
    cJSON *root = cJSON_Parse(data_line);                   /* 解析本行 JSON */
    if (!root) {                                            /* 解析失败（keep-alive/残行） */
        return;                                             /* 静默忽略 */
    }
    cJSON *choices = cJSON_GetObjectItem(root, "choices");  /* 取 choices 数组 */
    cJSON *choice = (choices && cJSON_IsArray(choices))     /* 确认是数组 */
                    ? cJSON_GetArrayItem(choices, 0) : NULL;/* 取第一个候选 */
    cJSON *delta = choice ? cJSON_GetObjectItem(choice, "delta") : NULL;    /* 取 delta 对象 */
    cJSON *audio = delta ? cJSON_GetObjectItem(delta, "audio") : NULL;      /* 取 audio 对象 */
    cJSON *data = audio ? cJSON_GetObjectItem(audio, "data") : NULL;        /* 取 base64 数据字段 */
    if (data && cJSON_IsString(data) && data->valuestring[0]) {     /* 有音频数据才处理 */
        const char *b64 = data->valuestring;                /* base64 字符串 */
        size_t blen = strlen(b64);                          /* base64 长度 */
        size_t cap = blen * 3 / 4 + 3;                      /* 解码后最大 PCM 字节数 */
        unsigned char *pcm = heap_caps_malloc(cap, MALLOC_CAP_DEFAULT);     /* 解码缓冲（内部堆即可） */
        if (pcm) {                                          /* 分配成功才解码 */
            size_t olen = 0;                                /* 解码输出长度 */
            if (mbedtls_base64_decode(pcm, cap, &olen,      /* base64 → PCM 二进制 */
                                      (const unsigned char *)b64, blen) == ESP_OK
                && olen >= 2) {                             /* 至少 1 个样本（2 字节） */
                s_tts.chunks++;                             /* 音频块计数 +1 */
                s_tts.bytes += olen;                        /* 累计 PCM 字节 */
                if (s_tts.chunks == 1) {                    /* 首个音频块打延迟日志 */
                    ESP_LOGI(TAG, "收到首个音频块 (%uB)，开始播放 "         /* 提示开始播放 */
                             "（⏱ TTS 首块: %lldms）", (unsigned)olen,     /* 首块大小 */
                             (esp_timer_get_time() - s_tts.t0) / 1000);     /* 首块延迟 ms */
                }
                callback->on_audio((const int16_t *)pcm, olen / 2, callback->ctx);  /* 交调用方播放 */
            }
            free(pcm);                                      /* 释放解码缓冲 */
        }
    } else if (!s_tts.err_dumped) {                         /* 非音频行：可能是流内错误 */
        s_tts.err_dumped = true;                            /* 只 dump 一次防刷屏 */
        ESP_LOGW(TAG, "非音频数据行: %.200s", data_line);   /* 输出载荷帮助定位 */
    }
    cJSON_Delete(root);                                     /* 释放 JSON 树 */
}

/**
 * 流式合成一段文本：SSE 逐块回调 24kHz PCM16LE mono
 *
 * 组包规则（官方文档）：合成文本放 role:assistant；风格指令放
 * role:user（可为空串）；audio.format=pcm16 才是流式分块返回。
 *
 * @return ESP_OK 合成完成；其他=网络/HTTP/无音频
 */
esp_err_t tts_synthesize(const char *text, const char *style,   /* 文本与风格指令 */
                         tts_audio_cb_t on_audio, void *ctx)    /* 音频块回调 */
{
    /* 参数校验：已初始化、文本非空 */
    ESP_RETURN_ON_FALSE(s_tts.inited && text && text[0],
                        ESP_ERR_INVALID_ARG, TAG, "bad arg");

    /* 手工组包（文本含中文/引号 → cJSON 只负责字符串转义再拼进模板） */
    cJSON *jtext = cJSON_CreateString(text);                /* 把文本包成 JSON 字符串节点 */
    ESP_RETURN_ON_FALSE(jtext, ESP_ERR_NO_MEM, TAG, "no mem");  /* 分配失败即报错 */
    char *esc = cJSON_PrintUnformatted(jtext);              /* 序列化为带引号的转义字符串 */
    cJSON_Delete(jtext);                                    /* 临时节点用完即删 */
    ESP_RETURN_ON_FALSE(esc, ESP_ERR_NO_MEM, TAG, "esc failed");    /* 序列化失败即报错 */

    char *body = heap_caps_malloc(strlen(esc) + 512, MALLOC_CAP_SPIRAM);    /* 请求体缓冲（PSRAM） */
    if (!body) {                                            /* 分配失败 */
        free(esc);                                          /* 释放转义串防泄漏 */
        return ESP_ERR_NO_MEM;                              /* 返回内存错误 */
    }
    snprintf(body, strlen(esc) + 512,                       /* 按官方组包规则拼请求体 */
             "{\"model\":\"mimo-v2.5-tts\",\"stream\":true,"        /* 模型 + 流式开关 */
             "\"messages\":["                               /* 消息数组开始 */
             "{\"role\":\"user\",\"content\":\"%s\"},"      /* 风格指令（合成时不说出来） */
             "{\"role\":\"assistant\",\"content\":%s}],"    /* 合成文本（转义后内联） */
             "\"audio\":{\"format\":\"pcm16\",\"voice\":\"冰糖\"}}", /* 24k PCM16 + 冰糖音色 */
             style ? style : "", esc);                      /* 空风格传空串 */
    free(esc);                                              /* 转义串用完释放 */

    tts_callback_ctx_t callback = {                         /* 打包回调上下文给 SSE 处理器 */
        .on_audio = on_audio,                               /* 调用方音频回调 */
        .ctx = ctx,                                         /* 调用方上下文 */
    };
    s_tts.chunks = 0;                                       /* 音频块计数复位 */
    s_tts.bytes = 0;                                        /* 字节统计复位 */
    s_tts.err_dumped = false;                               /* 错误 dump 标志复位 */
    s_tts.t0 = esp_timer_get_time();                        /* 记录合成起点（算首块延迟） */
    esp_err_t err = ai_http_post_sse(s_tts.url, s_tts.key, body,    /* 发起 SSE 流式请求 */
                                     sse_audio_handler, &callback, 60);     /* 60s 读超时 */
    free(body);                                             /* 释放请求体 */
    ESP_LOGI(TAG, "TTS 流结束: %d 块 / %uKB PCM%s",         /* 打印本轮合成统计 */
             s_tts.chunks, (unsigned)(s_tts.bytes / 1024),
             s_tts.chunks ? "" : "  ← 零音频！看上面的'非音频数据行'定位");   /* 零音频提示排查方向 */
    return err;                                             /* 返回流式请求结果 */
}
