/**
 * @file    music_source.c
 * @brief   音乐音源抽象实现——FILE（SD 卡/SPIFFS）与 HTTP（Icecast 流/直链）双后端
 *
 * 读语义统一：>0 实读字节数 / 0 流结束 / <0 读错误。
 * 播放侧按此语义决定"这块能否喂解码器""是否该收尾"。
 *
 * @date    2026-09-10
 * @version 1.0.0
 */

#include "music_source.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#define TAG "mus_src"

#define HTTP_BUF_SIZE       (4096)      /* esp_http_client 内部收发缓冲 */
#define HTTP_DEF_TIMEOUT_MS (10000)     /* 默认单次读超时（流式块间隔余量） */

/** 音源句柄（两后端互斥使用） */
struct music_source {
    bool is_http;                               /* true = 网络源 */
    FILE *fp;                                   /* 本地文件句柄 */
    esp_http_client_handle_t http;              /* HTTP 客户端句柄 */
    bool eof;                                   /* 流已结束（后续 read 直接回 0） */
};

/** uri 是否网络地址（http:// 或 https:// 前缀） */
static bool uri_is_http(const char *uri)
{
    return strncmp(uri, "http://", 7) == 0 ||     /* 明文 HTTP */
           strncmp(uri, "https://", 8) == 0;      /* TLS HTTPS（占用 mbedTLS） */
}

struct music_source *music_source_open(const char *uri)
{
    if (!uri || !uri[0]) {                          /* 参数防御 */
        return NULL;                                /* 无源可开 */
    }
    struct music_source *s = calloc(1, sizeof(*s)); /* 句柄（小块，内部 SRAM 足够） */
    if (!s) {                                       /* 分配失败 */
        return NULL;                                /* 放弃 */
    }

    /* ---- 本地文件后端 ---- */
    if (!uri_is_http(uri)) {                        /* 非网络前缀 = 本地路径 */
        s->fp = fopen(uri, "r");                    /* FATFS/SPIFFS 只读打开 */
        if (!s->fp) {                               /* 打不开（没卡/路径错） */
            ESP_LOGW(TAG, "打开失败: %s", uri);     /* 告警 */
            free(s);                                /* 释放句柄 */
            return NULL;                            /* 失败 */
        }
        return s;                                   /* 成功 */
    }

    /* ---- 网络流后端（独立 client，不共用 ai_http 的全局锁） ---- */
    s->is_http = true;                              /* 标记网络源 */
    esp_http_client_config_t cfg = {
        .url = uri,                                 /* 完整地址 */
        .method = HTTP_METHOD_GET,                  /* 流式拉取 */
        .timeout_ms = HTTP_DEF_TIMEOUT_MS,          /* 单次读超时 */
        .buffer_size = HTTP_BUF_SIZE,               /* 接收缓冲 */
        .buffer_size_tx = 1024,                     /* 发送缓冲（无 body，小即可） */
        .crt_bundle_attach = esp_crt_bundle_attach, /* https 才用得上（http 忽略） */
        .disable_auto_redirect = false,             /* 跟随 302（电台常重定向到边缘节点） */
    };
    s->http = esp_http_client_init(&cfg);           /* 建客户端 */
    if (!s->http) {                                 /* 分配失败 */
        ESP_LOGE(TAG, "HTTP client init 失败");     /* 报错 */
        free(s);                                    /* 释放句柄 */
        return NULL;                                /* 失败 */
    }
    /* 电台不认识 Range/Expect，去掉这些隐式头更稳 */
    esp_http_client_set_header(s->http, "Icy-MetaData", "0");   /* 不要穿插曲目元数据 */
    esp_err_t err = esp_http_client_open(s->http, 0);           /* 建立连接（GET 无 body） */
    if (err != ESP_OK) {                            /* 连接失败 */
        ESP_LOGW(TAG, "连接失败 %s: %s", uri, esp_err_to_name(err));
        esp_http_client_cleanup(s->http);           /* 清理 */
        free(s);                                    /* 释放句柄 */
        return NULL;                                /* 失败 */
    }
    esp_http_client_fetch_headers(s->http);         /* 收完响应头（无限流立即返回） */
    int status = esp_http_client_get_status_code(s->http);      /* HTTP 状态码 */
    if (status != 200) {                            /* 非 200（404/403/重定向未跟等） */
        ESP_LOGW(TAG, "HTTP %d: %s", status, uri);  /* 告警 */
        esp_http_client_cleanup(s->http);           /* 清理 */
        free(s);                                    /* 释放句柄 */
        return NULL;                                /* 失败 */
    }
    ESP_LOGI(TAG, "网络音源已连接: %s", uri);        /* 日志 */
    return s;                                       /* 成功 */
}

int music_source_read(struct music_source *src, void *buf, size_t len,
                      int timeout_ms)
{
    if (!src || !buf || len == 0 || src->eof) {     /* 参数/终止态防御 */
        return 0;                                   /* 视为流结束 */
    }
    /* ---- 本地文件：fread 短读即 EOF（FATFS/SPIFFS 语义） ---- */
    if (!src->is_http) {
        size_t n = fread(buf, 1, len, src->fp);     /* 读一块 */
        if (n == 0) {                               /* 真到尾 */
            src->eof = true;                        /* 置终止态 */
            return 0;                               /* 流结束 */
        }
        if (n < len) {                              /* 短读：本次数据有效但已到尾 */
            src->eof = true;                        /* 下次读回 0 */
        }
        return (int)n;                              /* 回本次实读字节数 */
    }
    /* ---- 网络流：esp_http_client_read 阻塞到有数据/超时/对端关流 ---- */
    if (timeout_ms > 0) {                           /* 调用方指定了超时 */
        esp_http_client_set_timeout_ms(src->http, timeout_ms);  /* 运行时调整 */
    }
    int n = esp_http_client_read(src->http, buf, len);          /* 流式读一块 */
    if (n < 0) {                                    /* 读错误（超时/连接中断） */
        ESP_LOGW(TAG, "网络读失败，连接中断");      /* 告警 */
        src->eof = true;                            /* 置终止态（由播放侧决定是否重连） */
        return -1;                                  /* 报错 */
    }
    if (n == 0) {                                   /* 对端正常关流 */
        src->eof = true;                            /* 置终止态 */
        return 0;                                   /* 流结束 */
    }
    return n;                                       /* 回实读字节数 */
}

void music_source_close(struct music_source *src)
{
    if (!src) {                                     /* NULL 安全 */
        return;                                     /* 直接返 */
    }
    if (src->fp) {                                  /* 文件源 */
        fclose(src->fp);                            /* 关文件 */
        src->fp = NULL;                             /* 清句柄 */
    }
    if (src->http) {                                /* 网络源 */
        esp_http_client_cleanup(src->http);         /* 关连接 + 释放（会中断在途读） */
        src->http = NULL;                           /* 清句柄 */
    }
    free(src);                                      /* 释放音源句柄 */
}

bool music_source_is_network(const struct music_source *src)
{
    return src ? src->is_http : false;              /* 无源按本地处理 */
}
