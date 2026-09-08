/**
 * @file    asr_client.c
 * @brief   ASR 客户端实现——双后端选择（讯飞流式 / MiMo 整段）
 *
 * 后端选择（M4）：配置了讯飞 APPID → 走 xfyun_iat 流式听写（快）；
 * 未配置或讯飞失败 → 回退 MiMo REST 整段识别（稳）。
 *
 * MiMo 后端的内存设计（30s 音频 ≈ 1.3MB 的背景下）：
 *   不用 cJSON 组包——2MB 的 base64 字符串在 cJSON 树里走一遭会
 *   瞬间翻倍占内存。手工拼 JSON：固定前缀 + base64 + 固定后缀，
 *   base64 分块编码直写目标缓冲，峰值内存 = wav + body 两份。
 *
 * @date    2026-09-06
 * @version 2.0.0  M4：新增讯飞流式后端选择与自动回退
 */

#include "asr_client.h"     /* 本模块公开接口 */

#include <string.h>         /* strlen / memcpy / strncmp */
#include <stdio.h>          /* snprintf */
#include <stdlib.h>         /* strdup / free */

#include "esp_log.h"        /* ESP_LOGx 日志 */
#include "esp_check.h"      /* ESP_RETURN_ON_* 检查宏 */
#include "esp_heap_caps.h"  /* heap_caps_malloc（PSRAM 分配） */
#include "mbedtls/base64.h" /* base64 分块编码 */
#include "cJSON.h"          /* 响应 JSON 解析 */

#include "ai_http.h"        /* HTTP POST 封装（ai_http_post_json） */
#include "xfyun_iat.h"      /* 讯飞流式听写后端 */

#define TAG "asr"           /* 本模块日志标签 */

/* MiMo 后端状态（URL/Key 在 init 时从 Kconfig 载入） */
static struct {
    bool inited;            /* 初始化完成标志（幂等闸门） */
    char url[192];          /* 完整接口地址：MiMo base + /chat/completions */
    char key[96];           /* MiMo API Key（Bearer 令牌） */
} s_asr;

/**
 * 初始化 ASR 客户端：拼 MiMo URL + 载 Key + 顺带初始化讯飞后端
 * @return ESP_OK 就绪（幂等，重复调用直接返回）
 */
esp_err_t asr_client_init(void)
{
    if (s_asr.inited) {                                     /* 幂等闸门：已初始化直接返回 */
        return ESP_OK;                                      /* 重复调用无害 */
    }
    const char *base = CONFIG_AI_MIMO_BASE_URL;             /* Kconfig 里的 MiMo 基地址 */
    size_t bl = strlen(base);                               /* 基地址长度 */
    if (bl > 0 && base[bl - 1] == '/') {                    /* 若基地址以 / 结尾 */
        bl--;                                               /* 去掉尾斜杠（避免双斜杠） */
    }
    snprintf(s_asr.url, sizeof(s_asr.url), "%.*s/chat/completions", /* 拼 OpenAI 兼容端点 */
             (int)bl, base);                              /* 用精度截断去掉尾斜杠 */
    strlcpy(s_asr.key, CONFIG_AI_MIMO_KEY, sizeof(s_asr.key));  /* 载入 MiMo API Key */
    s_asr.inited = true;                                    /* 置就绪标志 */
    ESP_LOGI(TAG, "ASR 就绪: %s (model=mimo-v2.5-asr)", s_asr.url); /* 打印后端信息 */
    /* 讯飞流式后端（配置了 APPID 才启用；失败自动回退 MiMo） */
    xfyun_iat_init();                                       /* 顺带初始化讯飞适配层 */
    return ESP_OK;                                          /* 初始化成功 */
}

/**
 * 标准 44 字节 PCM WAV 头拼装（小端逐字段直写）
 * @param h        输出头缓冲（≥44B）
 * @param pcm_len  PCM 数据字节数
 * @param rate     采样率（Hz）
 * @param ch       声道数
 * @param bits     位深
 * @return 恒为 44（WAV 标准头长度）
 */
static size_t wav_make_header(uint8_t *h, size_t pcm_len,      /* 头缓冲与数据长度 */
                              uint32_t rate, uint16_t ch, uint16_t bits) /* 音频规格 */
{
    uint32_t byte_rate = rate * ch * bits / 8;              /* 字节率 = 采样率×声道×位深/8 */
    uint16_t block_align = ch * bits / 8;                   /* 块对齐 = 声道×位深/8 */
    memcpy(h, "RIFF", 4);                                   /* RIFF 魔数 */
    uint32_t v = (uint32_t)(36 + pcm_len);          memcpy(h + 4, &v, 4);   /* 文件总长 -8 */
    memcpy(h + 8, "WAVEfmt ", 8);                           /* WAVE 标识 + fmt 块标识 */
    v = 16;                                         memcpy(h + 16, &v, 4);  /* fmt 块长度 = 16 */
    uint16_t u16 = 1;                               memcpy(h + 20, &u16, 2); /* 音频格式 = PCM */
    u16 = ch;                                       memcpy(h + 22, &u16, 2); /* 声道数 */
    v = rate;                                       memcpy(h + 24, &v, 4);  /* 采样率 */
    v = byte_rate;                                  memcpy(h + 28, &v, 4);  /* 字节率 */
    u16 = block_align;                              memcpy(h + 32, &u16, 2); /* 块对齐 */
    u16 = bits;                                     memcpy(h + 34, &u16, 2); /* 位深 */
    memcpy(h + 36, "data", 4);                              /* data 块标识 */
    v = (uint32_t)pcm_len;                          memcpy(h + 40, &v, 4);  /* PCM 数据长度 */
    return 44;                                              /* 标准头恒为 44 字节 */
}

/**
 * 识别入口：自动选择后端（讯飞流式优先，MiMo 兜底）
 *
 * 讯飞后端要裸 PCM（剥掉 44B WAV 头）；MiMo 后端要完整 WAV（含头）。
 * @param wav       完整 WAV 数据（44B 头 + PCM）
 * @param wav_len   数据总长
 * @param text_out  成功时输出识别文本（堆上，调用方 free）
 * @return ESP_OK 成功；其他=两个后端都失败
 */
esp_err_t asr_recognize(const char *wav, size_t wav_len, char **text_out) /* 入参：完整 WAV */
{
    /* 参数校验：已初始化、数据非空、长度至少超过 WAV 头 */
    ESP_RETURN_ON_FALSE(s_asr.inited && wav && wav_len > 44 && text_out,  /* 三重检查 */
                        ESP_ERR_INVALID_ARG, TAG, "bad arg");             /* 无效参数直接拒 */
    *text_out = NULL;                                   /* 输出指针先置空（失败语义） */

    /* ---- 后端选择：配置了讯飞 APPID → 流式听写（wss）；否则 MiMo REST ----
     * 讯飞要裸 PCM（无 WAV 头）；我们的录音是单声道 16k/16bit，恰好满足。
     * 讯飞分帧 40ms/1280B：上传耗时≈音频时长，但识别并行、末帧后 ~300ms
     * 出全文——总耗时 ≈ max(音频时长, 2s)，远优于 MiMo 整段的 6s。 */
    if (xfyun_iat_configured()) {                       /* 讯飞凭据已配置 → 优先流式后端 */
        esp_err_t xerr = xfyun_iat_recognize(wav + 44, wav_len - 44, text_out); /* 剥头后送讯飞 */
        if (xerr == ESP_OK) {                           /* 讯飞识别成功 */
            return ESP_OK;                              /* 直接返回，不碰 MiMo */
        }
        ESP_LOGW(TAG, "讯飞识别失败(%s)，回退 MiMo 整段", esp_err_to_name(xerr)); /* 记录回退原因 */
    }

    /* ---- 1. 分块 base64 编码到 body 中段 ----
     * 输入 3 字节 → 输出 4 字节；按 3000B 分块（除 pad 干净），末块单独编 */
    const char *BODY_HEAD =                             /* JSON 固定前缀：模型+消息结构+Data URL 头 */
        "{\"model\":\"mimo-v2.5-asr\",\"messages\":[{\"role\":\"user\","
        "\"content\":[{\"type\":\"input_audio\",\"input_audio\":{"
        "\"data\":\"data:audio/wav;base64,";
    const char *BODY_TAIL =                             /* JSON 固定后缀：闭合 + 中文语种 */
        "\"}}]}],\"asr_options\":{\"language\":\"zh\"}}";
    size_t head_len = strlen(BODY_HEAD);                /* 前缀长度 */
    size_t tail_len = strlen(BODY_TAIL);                /* 后缀长度 */
    size_t b64_len = ((wav_len + 2) / 3) * 4;           /* base64 理论最大长度（含 pad） */

    char *body = heap_caps_malloc(head_len + b64_len + tail_len + 1,    /* 前缀+b64+后缀+结尾 */
                                  MALLOC_CAP_SPIRAM);   /* 大块走 PSRAM */
    ESP_RETURN_ON_FALSE(body, ESP_ERR_NO_MEM, TAG, "body alloc %uKB 失败",      /* 分配失败报错 */
                        (unsigned)((head_len + b64_len + tail_len) / 1024));    /* 打印所需 KB 数 */
    memcpy(body, BODY_HEAD, head_len);                  /* 先写 JSON 前缀 */
    char *w = body + head_len;                          /* 写游标定位到前缀末尾 */

    const unsigned char *src = (const unsigned char *)wav;      /* base64 输入源（整个 WAV） */
    size_t left = wav_len;                              /* 剩余未编码字节数 */
    while (left > 0) {                                  /* 分块循环直到全部编码 */
        size_t chunk = (left > 3000) ? 3000 : left;     /* 每块 3000B（3 的倍数，无中间 pad） */
        size_t olen = 0;                                /* 本块编码输出长度 */
        /* +1：mbedtls 编码完会补一个 '\0'，余量必须给它留一位
         * （教训：卡在最后一组 BUFFER_TOO_SMALL） */
        size_t dcap = b64_len + 1 - (size_t)(w - body - head_len);      /* 剩余可写空间（+1 给结尾符） */
        esp_err_t e = mbedtls_base64_encode((unsigned char *)w, dcap,   /* 编码本块直写 body */
                                            &olen, src, chunk);         /* 输出长度回填 */
        if (e != ESP_OK) {                              /* 编码失败（缓冲不够/参数错） */
            free(body);                                 /* 释放 body 防泄漏 */
            ESP_LOGE(TAG, "base64 编码失败: %d (0x%04x)", e, -e);        /* 打印 mbedtls 错误码 */
            return e;                                   /* 直接返回编码错误 */
        }
        w += olen;                                      /* 写游标前进 */
        src += chunk;                                   /* 源游标前进 */
        left -= chunk;                                  /* 剩余递减 */
    }
    memcpy(w, BODY_TAIL, tail_len);                     /* 写 JSON 后缀 */
    w[tail_len] = '\0';                                 /* 补字符串结尾 */

    /* ---- 2. POST 非流式：整包发出，同步收完整响应（8KB 上限） ---- */
    char *resp = heap_caps_malloc(8 * 1024, MALLOC_CAP_SPIRAM); /* 响应缓冲（PSRAM） */
    if (!resp) {                                        /* 分配失败 */
        free(body);                                     /* 释放 body 防泄漏 */
        return ESP_ERR_NO_MEM;                          /* 返回内存错误 */
    }
    esp_err_t err = ai_http_post_json(s_asr.url, s_asr.key, body,       /* POST JSON 收完整响应 */
                                      resp, 8 * 1024, 60);      /* 60s 超时（ASR 推理较慢） */
    free(body);                                         /* body 用完即释放 */
    if (err != ESP_OK) {                                /* 网络/HTTP 错误 */
        free(resp);                                     /* 释放响应缓冲 */
        return err;                                     /* 返回网络错误 */
    }

    /* ---- 3. 解析 choices[0].message.content = 识别文本 ---- */
    cJSON *root = cJSON_Parse(resp);                    /* 解析响应 JSON */
    free(resp);                                         /* 响应缓冲用完释放 */
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_STATE, TAG, "响应非 JSON: %.200s", ""); /* 解析失败报错 */
    cJSON *choices = cJSON_GetObjectItem(root, "choices");      /* 取 choices 数组 */
    cJSON *choice = (choices && cJSON_IsArray(choices))         /* 确认是数组 */
                    ? cJSON_GetArrayItem(choices, 0) : NULL;    /* 取第一个候选 */
    cJSON *msg = choice ? cJSON_GetObjectItem(choice, "message") : NULL;    /* 取 message 对象 */
    cJSON *content = msg ? cJSON_GetObjectItem(msg, "content") : NULL;      /* 取识别文本字段 */
    esp_err_t ret = ESP_FAIL;                           /* 默认失败 */
    if (content && cJSON_IsString(content) && content->valuestring[0]) {    /* 有非空文本才算成功 */
        *text_out = strdup(content->valuestring);       /* 复制文本交调用方 */
        ret = *text_out ? ESP_OK : ESP_ERR_NO_MEM;      /* 复制成功与否决定返回值 */
    } else {                                            /* 无文本 = 识别为空/出错 */
        char *dump = cJSON_PrintUnformatted(root);      /* 打印原始响应帮助定位 */
        ESP_LOGW(TAG, "ASR 无文本结果: %.200s", dump ? dump : "?");      /* 输出诊断信息 */
        cJSON_free(dump);                               /* 释放 dump 字符串 */
    }
    cJSON_Delete(root);                                 /* 释放 JSON 树 */
    if (ret == ESP_OK) {                                /* 识别成功 */
        ESP_LOGI(TAG, "识别: %s", *text_out);           /* 打印识别全文 */
    }
    return ret;                                         /* 返回最终结果 */
}
