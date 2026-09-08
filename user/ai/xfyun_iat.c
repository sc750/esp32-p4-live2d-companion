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

#include "xfyun_iat.h"                  /* 本模块公开接口（含 stdbool） */

#include <string.h>                     /* memcpy / strlen / strncmp */
#include <stdio.h>                      /* snprintf / strftime */
#include <stdlib.h>                     /* strdup / free */
#include <time.h>                       /* time / gmtime_r / strftime（鉴权 date） */
#include <stdbool.h>                    /* bool（C23 前的显式保险） */

#include "esp_log.h"                    /* ESP_LOGx 日志 */
#include "esp_check.h"                  /* ESP_RETURN_ON_* 检查宏 */
#include "esp_heap_caps.h"              /* heap_caps_malloc（PSRAM/内部堆） */
#include "esp_websocket_client.h"       /* WebSocket 客户端（托管组件） */
#include "esp_crt_bundle.h"             /* 证书 bundle（wss 校验） */
#include "esp_event.h"                  /* 事件注册/派发 */
#include "mbedtls/base64.h"             /* base64 编解码（音频/鉴权） */
#include "mbedtls/md.h"                 /* hmac-sha256（鉴权签名） */
#include "cJSON.h"                      /* 结果 JSON 解析 */
#include "freertos/FreeRTOS.h"          /* FreeRTOS 基础（pdMS_TO_TICKS 等） */
#include "freertos/event_groups.h"      /* 事件组（握手/结果同步） */

#define TAG "xfyun"                     /* 本模块日志标签 */

#define IAT_HOST            "iat-api.xfyun.cn"  /* 听写服务主机名（鉴权+连接共用） */
#define IAT_PATH            "/v2/iat"           /* WebSocket 握手路径 */
#define FRAME_AUDIO_BYTES   1280        /* 每帧原始音频字节数（协议建议值） */
#define CONNECT_TIMEOUT_MS  8000        /* 等握手的超时（含 DNS/TCP/TLS） */
#define RESULT_TIMEOUT_MS   15000       /* 等识别结果收完的超时 */

/* 事件组位定义：跨任务同步握手与结果状态 */
#define EVT_CONNECTED   BIT0        /* WebSocket 握手成功（101） */
#define EVT_DONE        BIT1        /* 结果收完（status==2）或连接断开/出错 */
#define EVT_FAILED      BIT2        /* 连接级失败（与业务错误区分） */

/* 模块全局状态：仅 ASR 调用上下文（voice_pipe/console 任务）串行访问 */
static struct {
    bool inited;                    /* 初始化完成标志（幂等闸门） */
    char app_id[64];                /* 讯飞 APPID（Kconfig 注入） */
    char api_key[64];               /* 讯飞 APIKey（鉴权用户名部分） */
    char api_secret[64];            /* 讯飞 APISecret（HMAC 签名密钥） */

    EventGroupHandle_t evt;         /* 事件组：CONNECTED/DONE/FAILED 同步 */
    volatile bool frame_done;       /* 服务端结果收完（status==2） */
    volatile int  srv_code;         /* 服务端业务错误码（0=无错误） */
    char text[2048];                /* 识别文本累积缓冲（追加式拼接） */
    size_t text_len;                /* 当前文本长度（追加游标） */
} s_iat;

/**
 * 初始化讯飞听写模块：从 Kconfig 载入三件套凭据 + 建事件组
 * @return ESP_OK 就绪；ESP_ERR_NO_MEM 事件组分配失败
 */
esp_err_t xfyun_iat_init(void)
{
    if (s_iat.inited) {                                     /* 幂等闸门：已初始化直接返回 */
        return ESP_OK;                                      /* 重复调用无害 */
    }
    strlcpy(s_iat.app_id, CONFIG_ASR_XFYUN_APPID, sizeof(s_iat.app_id));        /* 拷贝 APPID */
    strlcpy(s_iat.api_key, CONFIG_ASR_XFYUN_APIKEY, sizeof(s_iat.api_key));     /* 拷贝 APIKey */
    strlcpy(s_iat.api_secret, CONFIG_ASR_XFYUN_APISECRET, sizeof(s_iat.api_secret)); /* 拷贝 APISecret */
    s_iat.evt = xEventGroupCreate();                        /* 创建事件组（握手/结果同步用） */
    ESP_RETURN_ON_FALSE(s_iat.evt, ESP_ERR_NO_MEM, TAG, "evt alloc failed");    /* 分配失败即报错 */
    s_iat.inited = true;                                    /* 置就绪标志 */
    ESP_LOGI(TAG, "讯飞听写就绪 (appid=%s)", s_iat.app_id[0] ? s_iat.app_id : "<未配置>"); /* 打印凭据状态 */
    return ESP_OK;                                          /* 初始化成功 */
}

/**
 * 查询讯飞凭据是否已配置（APPID 非空视为已配置）
 * @return true=已配置可走讯飞；false=回退 MiMo
 */
bool xfyun_iat_configured(void)
{
    return s_iat.inited && s_iat.app_id[0] != '\0';         /* 就绪且 APPID 非空才走讯飞 */
}

/* ---------- 鉴权 URL 构造 ---------- */

/**
 * URL 百分号编码（保守实现：仅保留 RFC3986 非保留字符，其余全转义）
 * @return 实际写入 dst 的字节数（不含 '\0'）
 */
static size_t urlencode(const char *src, size_t slen, char *dst, size_t dcap)
{
    static const char hex[] = "0123456789ABCDEF";           /* 十六进制字符表 */
    size_t w = 0;                                           /* 写入游标 */
    for (size_t i = 0; i < slen && w + 4 <= dcap; i++) {    /* 逐字符扫描（预留 %XX 三字节） */
        unsigned char c = (unsigned char)src[i];            /* 取当前字符（无符号避免符号扩展） */
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||                       /* 英文字母 */
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') { /* 数字与非保留符号 */
            dst[w++] = (char)c;                             /* 无需转义，原样拷贝 */
        } else {                                            /* 其余字符需要百分号编码 */
            dst[w++] = '%';                                 /* 前缀 % */
            dst[w++] = hex[c >> 4];                         /* 高 4 位十六进制 */
            dst[w++] = hex[c & 0xF];                        /* 低 4 位十六进制 */
        }
    }
    dst[w] = '\0';                                          /* 补字符串结尾 */
    return w;                                               /* 返回编码后长度 */
}

/**
 * 构造带鉴权参数的 wss URL（五步签名流程，对照官方文档）
 * @param out  输出缓冲（调用方保证 ≥512B）
 * @param cap  输出缓冲容量
 * @return ESP_OK 成功；ESP_FAIL HMAC 计算失败
 */
static esp_err_t build_auth_url(char *out, size_t cap)
{
    /* 步骤 1：date = 当前 UTC 时间，RFC1123 格式（依赖 SNTP 已校时；偏差 >300s 被 403） */
    time_t now = time(NULL);                                /* 取系统时钟（SNTP 校准后即墙钟） */
    struct tm tm_gmt;                                       /* GMT 时间结构体 */
    gmtime_r(&now, &tm_gmt);                                /* 转 UTC（绝不能用 localtime，会偏 8h） */
    char date[64];                                          /* date 字符串缓冲 */
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm_gmt); /* 格式化为 RFC1123 */

    /* 步骤 2：拼签名原文 "host: x\ndate: y\nGET /v2/iat HTTP/1.1"（\n 与冒号后空格均按协议） */
    char origin[256];                                       /* 签名原文缓冲 */
    int n = snprintf(origin, sizeof(origin),                /* 按协议顺序拼接三要素 */
                     "host: %s\ndate: %s\nGET %s HTTP/1.1", /* host 行 + date 行 + 请求行 */
                     IAT_HOST, date, IAT_PATH);             /* 值来自常量与刚生成的 date */

    /* 步骤 3：以 APISecret 为密钥做 HMAC-SHA256 → base64 = signature */
    unsigned char sha[32];                                  /* SHA256 摘要（32B） */
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256); /* 取 SHA256 算法描述 */
    int r = mbedtls_md_hmac(md,                             /* 执行 HMAC 计算 */
                            (const unsigned char *)s_iat.api_secret,    /* 密钥 = APISecret */
                            strlen(s_iat.api_secret),       /* 密钥长度 */
                            (const unsigned char *)origin, n, /* 签名原文 */
                            sha);                           /* 输出摘要 */
    if (r != 0) {                                           /* HMAC 失败（理论上不会） */
        ESP_LOGE(TAG, "hmac 失败: %d", r);                  /* 打印错误码 */
        return ESP_FAIL;                                    /* 直接失败 */
    }
    unsigned char sig_b64[64];                              /* signature 的 base64 缓冲 */
    size_t sig_len = 0;                                     /* 实际 base64 长度 */
    mbedtls_base64_encode(sig_b64, sizeof(sig_b64), &sig_len, sha, 32); /* 摘要编码为 base64 */

    /* 步骤 4：拼 authorization_origin（api_key/算法/头列表/签名）→ 再 base64 = authorization */
    char auth_origin[256];                                  /* authorization 明文缓冲 */
    n = snprintf(auth_origin, sizeof(auth_origin),          /* 按协议格式拼接四要素 */
                 "api_key=\"%s\", algorithm=\"hmac-sha256\", " /* api_key 与算法固定写法 */
                 "headers=\"host date request-line\", signature=\"%s\"", /* 参与签名的参数名固定 */
                 s_iat.api_key, (char *)sig_b64);           /* APIKey 与步骤 3 的签名 */
    unsigned char auth_b64[384];                            /* authorization 的 base64 缓冲 */
    size_t auth_len = 0;                                    /* 实际长度 */
    mbedtls_base64_encode(auth_b64, sizeof(auth_b64), &auth_len,          /* 对明文做 base64 */
                          (const unsigned char *)auth_origin, n);          /* 得到最终鉴权参数 */

    /* 步骤 5：拼最终 URL（date 里的空格/逗号/冒号必须百分号编码） */
    char date_enc[192];                                     /* 编码后的 date 缓冲 */
    urlencode(date, strlen(date), date_enc, sizeof(date_enc)); /* 对 date 做百分号编码 */
    snprintf(out, cap,                                      /* 组装完整 wss URL */
             "wss://%s%s?authorization=%.*s&date=%s&host=%s", /* 鉴权三参数挂查询串 */
             IAT_HOST, IAT_PATH,                            /* 主机与路径 */
             (int)auth_len, (const char *)auth_b64,         /* authorization（带长度截断打印） */
             date_enc, IAT_HOST);                           /* 编码后的 date 与主机名 */
    return ESP_OK;                                          /* URL 构造成功 */
}

/* ---------- WebSocket 事件与结果拼接 ---------- */

/**
 * 解析一片识别结果 JSON：把 ws[].cw[].w 顺序追加进累积缓冲；
 * code!=0 记为业务错误；data.status==2 置"结果收完"标志
 */
static void parse_result_json(const char *json, size_t json_len)
{
    /* 解析副本走堆：本函数跑在 websocket 组件任务里（默认栈仅 4KB），
     * 2KB 栈副本+cJSON 帧曾把组件栈打爆（M4 栈保护崩机实测） */
    char *tmp = heap_caps_malloc(json_len + 1, MALLOC_CAP_DEFAULT); /* 堆上解析副本 */
    if (!tmp) {                                             /* 分配失败（罕见） */
        ESP_LOGW(TAG, "结果解析副本分配失败");               /* 告警放弃本片 */
        return;                                             /* 静默丢弃 */
    }
    memcpy(tmp, json, json_len);                            /* 拷贝 JSON 文本 */
    tmp[json_len] = '\0';                                   /* 确保字符串结尾 */

    cJSON *root = cJSON_Parse(tmp);                         /* 解析整段 JSON */
    if (!root) {                                            /* 解析失败（截断/非 JSON） */
        ESP_LOGW(TAG, "结果 JSON 解析失败（分帧/截断?）: %.120s", tmp); /* 打印前 120 字节定位 */
        return;                                             /* 静默丢弃该片 */
    }
    cJSON *jcode = cJSON_GetObjectItem(root, "code");       /* 取业务错误码字段 */
    if (jcode && cJSON_IsNumber(jcode) && jcode->valueint != 0) { /* 非 0 = 服务端报错 */
        s_iat.srv_code = jcode->valueint;                   /* 记录错误码供调用方判断 */
        cJSON *jmsg = cJSON_GetObjectItem(root, "message"); /* 取错误描述 */
        ESP_LOGE(TAG, "服务端错误 %d: %s", jcode->valueint, /* 打印错误码与描述 */
                 jmsg && cJSON_IsString(jmsg) ? jmsg->valuestring : "?"); /* 描述可能缺失 */
        xEventGroupSetBits(s_iat.evt, EVT_DONE | EVT_FAILED); /* 标记完成+失败 */
        cJSON_Delete(root);                                 /* 释放 JSON 树 */
        return;                                             /* 错误路径结束 */
    }
    cJSON *jdata = cJSON_GetObjectItem(root, "data");       /* 取 data 对象 */
    cJSON *jstatus = jdata ? cJSON_GetObjectItem(jdata, "status") : NULL;   /* 结果状态（0/1/2） */
    cJSON *jresult = jdata ? cJSON_GetObjectItem(jdata, "result") : NULL;   /* 结果对象（可能缺） */
    if (jresult) {                                          /* 有结果才解析词组 */
        /* 未开动态修正（dwa）：各片结果为纯追加，顺序拼 w 即可 */
        cJSON *jws = cJSON_GetObjectItem(jresult, "ws");    /* ws 数组 = 分词列表 */
        if (jws && cJSON_IsArray(jws)) {                    /* 确认是数组才遍历 */
            int n = cJSON_GetArraySize(jws);                /* 本片分词数量 */
            for (int i = 0; i < n && s_iat.text_len < sizeof(s_iat.text) - 1; i++) { /* 逐词（带溢出保护） */
                cJSON *ws = cJSON_GetArrayItem(jws, i);     /* 第 i 个分词节点 */
                cJSON *cw = cJSON_GetObjectItem(ws, "cw");  /* 该节点的 cw 数组 */
                cJSON *c0 = (cw && cJSON_IsArray(cw)) ? cJSON_GetArrayItem(cw, 0) : NULL; /* 首个候选 */
                cJSON *w = c0 ? cJSON_GetObjectItem(c0, "w") : NULL; /* 取字词字符串 */
                if (w && cJSON_IsString(w)) {               /* 有效字词才追加 */
                    size_t wl = strlen(w->valuestring);     /* 字词字节长度（UTF-8 多字节） */
                    if (s_iat.text_len + wl >= sizeof(s_iat.text) - 1) { /* 累积缓冲将满 */
                        break;                              /* 停止追加（2KB 上限保护） */
                    }
                    memcpy(s_iat.text + s_iat.text_len, w->valuestring, wl); /* 追加到累积缓冲 */
                    s_iat.text_len += wl;                   /* 游标前进 */
                }
            }
            s_iat.text[s_iat.text_len] = '\0';              /* 维护字符串结尾 */
        }
    }
    if (jstatus && cJSON_IsNumber(jstatus) && jstatus->valueint == 2) { /* status==2 = 最后一片 */
        s_iat.frame_done = true;                            /* 置"结果收完"标志 */
        xEventGroupSetBits(s_iat.evt, EVT_DONE);            /* 唤醒等待识别结果的调用方 */
    }
    cJSON_Delete(root);                                     /* 释放 JSON 树 */
    free(tmp);                                              /* 释放解析副本 */
}

/**
 * WebSocket 事件回调：连接/断开/数据/错误 四类事件的分发处理。
 * 注意：本回调运行在组件私有事件循环上下文，必须快速返回。
 */
static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *event_data)
{
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)event_data; /* 事件数据（DATA 事件用） */
    switch (id) {                                           /* 按事件 ID 分发 */
    case WEBSOCKET_EVENT_CONNECTED:                         /* 握手成功（HTTP 101） */
        ESP_LOGI(TAG, "WS 已连接（鉴权通过）");             /* 鉴权通过的标志性日志 */
        xEventGroupSetBits(s_iat.evt, EVT_CONNECTED);       /* 唤醒等待握手的调用方 */
        break;                                              /* CONNECTED 处理完 */
    case WEBSOCKET_EVENT_DISCONNECTED:                      /* 连接断开（含服务端主动断） */
        ESP_LOGW(TAG, "WS 断开");                           /* 记录断开事件 */
        xEventGroupSetBits(s_iat.evt, EVT_DONE);            /* 断开也算"结果阶段结束" */
        break;                                              /* DISCONNECTED 处理完 */
    case WEBSOCKET_EVENT_DATA:                              /* 收到数据帧 */
        /* opcode 0x1 = TextMessage（官方要求结果帧全部为文本类型） */
        if (e->op_code == 0x1 && e->data_ptr && e->data_len > 0) { /* 只处理有内容的文本帧 */
            parse_result_json((const char *)e->data_ptr, (size_t)e->data_len); /* 交给结果解析器 */
        }
        break;                                              /* DATA 处理完 */
    case WEBSOCKET_EVENT_ERROR:                             /* 传输/协议级错误 */
        ESP_LOGE(TAG, "WS 错误");                           /* 记录错误 */
        xEventGroupSetBits(s_iat.evt, EVT_DONE | EVT_FAILED); /* 标记完成+失败 */
        break;                                              /* ERROR 处理完 */
    default:                                                /* 其余事件（BEGIN/CLOSED 等） */
        break;                                              /* 不关心，直接忽略 */
    }
}

/**
 * 发一帧音频 JSON：把音频块 base64 后内联进协议 JSON 再以文本帧发送
 * @param status 0=首帧（带 common/business）/ 1=中间帧 / 2=末帧
 * @param first  是否首帧（决定 JSON 模板形态）
 */
static esp_err_t send_audio_frame(esp_websocket_client_handle_t ws,   /* WS 客户端句柄 */
                                  int status, const char *audio, size_t alen, /* 帧状态与音频块 */
                                  bool first)                         /* 是否首帧 */
{
    unsigned char b64[2200];                            /* 音频块 base64 缓冲（1280B→1708B 有余量） */
    size_t b64_len = 0;                                 /* base64 实际长度 */
    if (alen > 0) {                                     /* 末帧允许空音频 */
        ESP_RETURN_ON_ERROR(mbedtls_base64_encode(b64, sizeof(b64), &b64_len,   /* 编码音频块 */
                                                  (const unsigned char *)audio, alen),  /* 输入原始 PCM */
                            TAG, "b64 failed");         /* 编码失败（理论不会） */
    }
    char *json = heap_caps_malloc(b64_len + 512, MALLOC_CAP_DEFAULT);           /* JSON 帧缓冲（内部堆够用） */
    ESP_RETURN_ON_FALSE(json, ESP_ERR_NO_MEM, TAG, "no mem");   /* 分配失败即报错 */
    int n;                                              /* JSON 实际长度 */
    if (first) {                                        /* 首帧：带 common + business 参数 */
        n = snprintf(json, b64_len + 512,               /* 按协议组首帧 JSON */
                     "{\"common\":{\"app_id\":\"%s\"}," /* common.app_id = 讯飞 APPID */
                     "\"business\":{\"language\":\"zh_cn\",\"domain\":\"iat\","  /* 中文+日常用语 */
                     "\"accent\":\"mandarin\",\"vad_eos\":10000,\"ptt\":1},"      /* 普通话+10s 静音检测+标点 */
                     "\"data\":{\"status\":%d,\"format\":\"audio/L16;rate=16000\"," /* 音频状态与格式 */
                     "\"encoding\":\"raw\",\"audio\":\"%.*s\"}}",             /* raw PCM 的 base64 */
                     s_iat.app_id, status, (int)b64_len, (const char *)b64);  /* 各格式化参数 */
    } else {                                            /* 中间/末帧：只带 data 对象 */
        n = snprintf(json, b64_len + 512,               /* 按协议组中间帧 JSON */
                     "{\"data\":{\"status\":%d,\"format\":\"audio/L16;rate=16000\"," /* 状态与格式 */
                     "\"encoding\":\"raw\",\"audio\":\"%.*s\"}}",  /* 音频 base64 */
                     status, (int)b64_len, (const char *)b64);     /* 各格式化参数 */
    }
    esp_err_t err = esp_websocket_client_send_text(ws, json, n,   /* 以文本帧发送（官方要求 opcode=1） */
                                                   pdMS_TO_TICKS(5000));  /* 发送超时 5s */
    free(json);                                         /* 释放 JSON 缓冲 */
    return (n > 0 && err >= 0) ? ESP_OK : ESP_FAIL;     /* 发送长度>0 且无错误才算成功 */
}

/**
 * 整段识别主流程：鉴权建连 → 分帧上传（1280B/40ms）→ 等结果 → 返回文本
 *
 * 上传耗时 ≈ 音频时长（40ms 节拍模拟实时）；识别在服务端与上传并行，
 * 末帧后 ~300ms 出全文。真正的"边录边传"（省掉上传段延迟）是下一步优化。
 */
esp_err_t xfyun_iat_recognize(const char *pcm_mono, size_t len, char **text_out) /* 入参：裸单声道 PCM */
{
    /* 参数校验：模块已初始化且讯飞已配置；音频非空且 ≤60s（1920000B=30s@16k mono×2 余量） */
    ESP_RETURN_ON_FALSE(s_iat.inited && xfyun_iat_configured(),   /* 初始化+凭据双检查 */
                        ESP_ERR_INVALID_STATE, TAG, "讯飞未配置"); /* 未配置时报状态错误 */
    ESP_RETURN_ON_FALSE(pcm_mono && len > 320 && len <= 1920000,  /* 音频有效性（>320B≈20ms） */
                        ESP_ERR_INVALID_ARG, TAG, "音频为空或超过 60s"); /* 长度越界报参数错误 */
    *text_out = NULL;                                   /* 输出指针先置空（失败语义） */

    /* 诊断：临时提升 WS/传输层日志级别，定位握手卡点（DNS/TCP/TLS/101） */
    esp_log_level_set("websocket_client", ESP_LOG_DEBUG);  /* WS 客户端组件日志 → DEBUG */
    esp_log_level_set("tcp_transport", ESP_LOG_DEBUG);     /* 传输层（TCP/TLS）日志 → DEBUG */
    esp_log_level_set("esp-tls", ESP_LOG_DEBUG);           /* TLS 层日志 → DEBUG */

    /* 步骤 1：时钟健全性检查——讯飞对 date 偏差 >300s 直接 403 拒绝。
     * SNTP 未同步时 time() 停在 1970 附近，早于 2026-01-01 即视为未校准 */
    if (time(NULL) < 1767225600 /* 2026-01-01 的 Unix 秒 */) { /* 时钟明显不对 */
        ESP_LOGW(TAG, "系统时钟未校准，跳过讯飞（下一轮再试）");     /* 提示并走 MiMo 回退 */
        return ESP_ERR_INVALID_STATE;                   /* 快速失败，不浪费 8 秒握手超时 */
    }
    char url[512];                                      /* 鉴权 URL 缓冲 */
    ESP_RETURN_ON_ERROR(build_auth_url(url, sizeof(url)), TAG, "鉴权 URL 失败"); /* 五步签名生成 URL */
    ESP_LOGD(TAG, "鉴权 URL: %s", url);                 /* DEBUG 级打印（联调期可人眼检查 date） */

    esp_websocket_client_config_t cfg = {               /* WS 客户端配置 */
        .uri = url,                                     /* 带鉴权参数的 wss 地址 */
        .buffer_size = 4096,                            /* 接收缓冲（结果 JSON 远小于此） */
        .network_timeout_ms = 10000,                    /* 网络超时 10s */
        .crt_bundle_attach = esp_crt_bundle_attach,     /* wss 证书校验（同 HTTPS 教训：不挂必被拒） */
        .task_stack = 8 * 1024,                         /* 组件默认栈仅 4KB——结果解析回调在其
                                                         * 任务里跑，曾触发栈保护崩机（M4 实测） */
    };
    esp_websocket_client_handle_t ws = esp_websocket_client_init(&cfg); /* 创建 WS 客户端实例 */
    ESP_RETURN_ON_FALSE(ws, ESP_FAIL, TAG, "ws init failed");   /* 创建失败即返回 */

    /* M4 谜案真相：组件创建**私有事件循环**且 dispatch 时只 run 自己的循环——
     * 注册到默认循环的事件永远收不到。必须用组件公开 API 注册。 */
    ESP_RETURN_ON_ERROR(esp_websocket_register_events(ws, ESP_EVENT_ANY_ID,     /* 注册任意事件 */
                                                      ws_event_handler, ws),    /* 回调+上下文 */
                        TAG, "ws handler 注册失败");    /* 注册失败即返回 */

    xEventGroupClearBits(s_iat.evt, EVT_CONNECTED | EVT_DONE | EVT_FAILED);     /* 清三个事件位 */
    s_iat.frame_done = false;                           /* 结果完成标志复位 */
    s_iat.srv_code = 0;                                 /* 业务错误码复位 */
    s_iat.text_len = 0;                                 /* 文本游标复位 */
    s_iat.text[0] = '\0';                               /* 文本缓冲清空 */
    esp_err_t err = esp_websocket_client_start(ws);     /* 启动客户端（内部任务开始连接） */
    if (err != ESP_OK) {                                /* 启动失败（罕见） */
        esp_websocket_client_destroy(ws);               /* 释放客户端资源 */
        return err;                                     /* 返回启动错误 */
    }

    /* 诊断（R15）：轮询连接状态——排查 esp_websocket_client 静默不连的迷案 */
    for (int i = 0; i < 8; i++) {                       /* 最多轮询 8 次（8 秒） */
        vTaskDelay(pdMS_TO_TICKS(1000));                /* 每秒看一次 */
        bool conn = esp_websocket_client_is_connected(ws);  /* 查询组件内部连接状态 */
        ESP_LOGI(TAG, "ws poll[%d]: connected=%d", i, conn);    /* 打印连接状态 */
        if (conn) {                                     /* 组件侧已连上 */
            break;                                      /* 提前结束轮询 */
        }
    }

    /* 步骤 2：等握手成功（EVT_CONNECTED）；若直接 DONE（403/网络错误）也算失败 */
    EventBits_t bits = xEventGroupWaitBits(s_iat.evt, EVT_CONNECTED | EVT_DONE, /* 等两个位任一 */
                                           pdFALSE, pdFALSE,    /* 不清除位、不要求全齐 */
                                           pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));  /* 8 秒握手超时 */
    if (!(bits & EVT_CONNECTED)) {                      /* 没等到 CONNECTED = 握手失败 */
        ESP_LOGE(TAG, "握手失败/超时（检查 key、时钟偏差 ≤300s）"); /* 打印嫌疑提示 */
        esp_websocket_client_destroy(ws);               /* 释放客户端（未连接无需 close） */
        return ESP_ERR_TIMEOUT;                         /* 超时错误 */
    }

    /* 步骤 3：分帧上传——1280B/帧、40ms 间隔（协议建议值）。
     * 发送期间服务端并行识别，结果经事件回调拼进 s_iat.text */
    err = ESP_OK;                                       /* 错误状态复位 */
    size_t off = 0;                                     /* 已发送字节偏移 */
    bool first = true;                                  /* 首帧标志（决定 JSON 模板） */
    while (off < len) {                                 /* 循环直到全部音频发完 */
        size_t chunk = (len - off > FRAME_AUDIO_BYTES) ? FRAME_AUDIO_BYTES   /* 满帧 1280B */
                                                       : (len - off);       /* 尾帧取剩余 */
        int status = (off == 0) ? 0 : 1;                /* 首帧 status=0，中间帧 status=1 */
        if (off + chunk >= len) {                       /* 已到最后一块 */
            status = 2;                                 /* 标记为末帧（协议必发） */
        }
        err = send_audio_frame(ws, status, pcm_mono + off, chunk, first);   /* 组 JSON 并发文本帧 */
        first = false;                                  /* 首帧只发一次 */
        if (err != ESP_OK) {                            /* 发送失败（断连/超时） */
            ESP_LOGE(TAG, "帧发送失败（offset=%u）", (unsigned)off); /* 打印失败位置 */
            break;                                      /* 中断上传循环 */
        }
        off += chunk;                                   /* 偏移推进 */
        vTaskDelay(pdMS_TO_TICKS(10));                  /* 帧间隔 10ms（协议建议 40ms 模拟实时；
                                                         * 整段模式下 40ms 会让 ASR 耗时≈音频时长。
                                                         * M5 实测 10ms=4 倍速，识别无劣化；
                                                         * 若出现识别错误可回退 40ms） */
    }
    /* 注：最后一帧已在循环里以 status=2 发出（off+chunk>=len 判定），无需补空末帧 */

    /* 步骤 4：等识别结果收完（status==2 的最后一片，或连接断开） */
    bits = xEventGroupWaitBits(s_iat.evt, EVT_DONE,     /* 等 DONE 位 */
                               pdFALSE, pdFALSE,        /* 不清除位 */
                               pdMS_TO_TICKS(RESULT_TIMEOUT_MS)); /* 15 秒结果超时 */
    esp_websocket_unregister_events(ws, ESP_EVENT_ANY_ID, ws_event_handler); /* 注销事件回调 */
    esp_websocket_client_close(ws, pdMS_TO_TICKS(2000)); /* 优雅关闭连接（2s 超时） */
    esp_websocket_client_destroy(ws);                   /* 销毁客户端实例 */

    if (!(bits & EVT_DONE)) {                           /* 15 秒没等到最后一片 */
        ESP_LOGE(TAG, "等待识别结果超时");               /* 打印超时 */
        return ESP_ERR_TIMEOUT;                         /* 返回超时错误 */
    }
    if (s_iat.srv_code != 0) {                          /* 服务端报了业务错误 */
        return ESP_FAIL;                                /* 直接失败（错误详情已打印） */
    }
    if (s_iat.text_len == 0) {                          /* 识别文本为空 = 没听到话 */
        ESP_LOGW(TAG, "识别结果为空（静音？）");         /* 提示可能静音 */
        return ESP_ERR_NOT_FOUND;                       /* 返回"无结果" */
    }
    *text_out = strdup(s_iat.text);                     /* 复制识别文本交调用方（free 归调用方） */
    ESP_LOGI(TAG, "⏱ 讯飞识别: %s", *text_out ? *text_out : "(null)");  /* 打印识别全文 */
    return *text_out ? ESP_OK : ESP_ERR_NO_MEM;         /* 复制成功与否决定返回值 */
}
