/**
 * @file    llm_client.c
 * @brief   LLM 客户端实现（L4）——SSE token 解析 + UTF-8 边界处理
 *
 * 关键细节（不处理必出豆腐块）：
 *   LLM 的 token 按字节切，一个汉字（3 字节 UTF-8）可能被拆在两个 SSE
 *   chunk 里。本模块内部维护字节累积缓冲，只把"完整的 UTF-8 序列"回调
 *   给上层，残缺尾字节留到下一个 chunk 拼上。
 *
 * 职责边界：完整回复的拼装归调用方（dialog_manager 要存历史，
 * 它在 token 回调里顺手攒即可），本模块只保证 token 是 UTF-8 完整片段。
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#include "llm_client.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "ai_http.h"

#define TAG "llm"

/* SSE data 行的 JSON 里含 delta.content，行缓冲 16KB 足够 */
#define TOKEN_ACC_SIZE  (8 * 1024)

static struct {
    bool inited;
    char url[160];      /* base + "/chat/completions" */
    char key[96];
    char model[64];
} s_llm;

esp_err_t llm_client_init(void)
{
    if (s_llm.inited) {
        return ESP_OK;
    }
    /* 拼 URL：base 去尾斜杠 + /chat/completions */
    const char *base = CONFIG_AI_LLM_BASE_URL;
    size_t bl = strlen(base);
    if (bl > 0 && base[bl - 1] == '/') {
        bl--;
    }
    snprintf(s_llm.url, sizeof(s_llm.url), "%.*s/chat/completions",
             (int)bl, base);
    strlcpy(s_llm.key, CONFIG_AI_LLM_KEY, sizeof(s_llm.key));
    strlcpy(s_llm.model, CONFIG_AI_LLM_MODEL, sizeof(s_llm.model));
    s_llm.inited = true;
    ESP_LOGI(TAG, "LLM 就绪: %s (model=%s)", s_llm.url, s_llm.model);
    return ESP_OK;
}

/**
 * UTF-8 累积器：返回可安全 flush 的长度，残缺尾字节留住
 */
static size_t utf8_complete_len(const uint8_t *buf, size_t len)
{
    /* 从尾往前找序列首字节（非 10xxxxxx 续字节） */
    size_t i = len;
    while (i > 0 && (buf[i - 1] & 0xC0) == 0x80) {
        i--;
    }
    if (i == 0) {
        return len;                     /* 全是续字节？异常流，全交出去 */
    }
    uint8_t lead = buf[i - 1];
    size_t need;                        /* 该序列应有总长 */
    if ((lead & 0x80) == 0)         need = 1;
    else if ((lead & 0xE0) == 0xC0) need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xF8) == 0xF0) need = 4;
    else return len;                    /* 非法首字节，交出去由 UI 兜底 */
    size_t have = len - (i - 1);
    return (have >= need) ? len : (i - 1);   /* 不完整 → 只交前半 */
}

/** SSE 单行处理上下文 */
typedef struct {
    char *acc;                  /* UTF-8 字节累积缓冲 */
    size_t len;
    llm_token_cb_t on_token;    /* 上层回调（可为 NULL=只烧流量测试） */
    void *ctx;
} sse_ctx_t;

static void sse_line_handler(const char *data_line, void *arg)
{
    sse_ctx_t *sc = (sse_ctx_t *)arg;
    if (strcmp(data_line, "[DONE]") == 0) {
        return;                             /* 流结束标志，外层 read 循环会收尾 */
    }
    cJSON *root = cJSON_Parse(data_line);
    if (!root) {
        return;                             /* keep-alive/残行，静默忽略 */
    }
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *choice = (choices && cJSON_IsArray(choices))
                    ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *delta = choice ? cJSON_GetObjectItem(choice, "delta") : NULL;
    cJSON *content = delta ? cJSON_GetObjectItem(delta, "content") : NULL;
    if (content && cJSON_IsString(content) && content->valuestring[0]) {
        const char *piece = content->valuestring;
        size_t plen = strlen(piece);
        /* 拼进累积缓冲（超长硬截：8KB 没被消费说明上游卡死，属异常） */
        size_t room = TOKEN_ACC_SIZE - 1 - sc->len;
        if (plen > room) {
            plen = room;
        }
        memcpy(sc->acc + sc->len, piece, plen);
        sc->len += plen;

        size_t ok = utf8_complete_len((const uint8_t *)sc->acc, sc->len);
        if (ok > 0 && sc->on_token) {
            sc->acc[ok] = '\0';
            sc->on_token(sc->acc, sc->ctx);         /* 整段完整字符回调 */
        }
        if (ok > 0) {
            memmove(sc->acc, sc->acc + ok, sc->len - ok);   /* 残字节前移 */
            sc->len -= ok;
        }
    }
    cJSON_Delete(root);
}

esp_err_t llm_chat_stream(const char *messages_json,
                          llm_token_cb_t on_token, void *ctx)
{
    ESP_RETURN_ON_FALSE(s_llm.inited, ESP_ERR_INVALID_STATE, TAG, "not init");

    /* 组请求体：cJSON 负责文本转义（引号/换行/非 ASCII 一手包办） */
    cJSON *body = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(body, ESP_ERR_NO_MEM, TAG, "no mem");
    cJSON_AddStringToObject(body, "model", s_llm.model);
    cJSON_AddBoolToObject(body, "stream", true);
    cJSON *msgs = cJSON_Parse(messages_json);
    if (!msgs) {
        cJSON_Delete(body);
        ESP_LOGE(TAG, "messages_json 非法");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_AddItemToObject(body, "messages", msgs);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    ESP_RETURN_ON_FALSE(body_str, ESP_ERR_NO_MEM, TAG, "print body failed");

    sse_ctx_t sc = {
        .acc = heap_caps_malloc(TOKEN_ACC_SIZE + 1, MALLOC_CAP_DEFAULT),
        .len = 0,
        .on_token = on_token,
        .ctx = ctx,
    };
    esp_err_t err;
    if (!sc.acc) {
        err = ESP_ERR_NO_MEM;
    } else {
        err = ai_http_post_sse(s_llm.url, s_llm.key, body_str,
                               sse_line_handler, &sc, 60);
        /* 尾巴：把残字节也吐出去（正常应为 0 长度） */
        if (err == ESP_OK && sc.len > 0 && on_token) {
            sc.acc[sc.len] = '\0';
            on_token(sc.acc, ctx);
        }
    }
    free(sc.acc);
    free(body_str);
    return err;
}
