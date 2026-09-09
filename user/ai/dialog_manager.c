/**
 * @file    dialog_manager.c
 * @brief   对话管理器实现（L4）——人设 + 记忆注入 + 历史环 + 回复拼装
 *
 * Phase4 起 system prompt 每轮动态组装：
 *   persona（可编辑，/spiffs/data/persona.json）
 *   + 长期记忆 top-N（memory_store，重要性降序）
 *   + 当前时间（SNTP 已同步才有）
 *
 * 历史：环形保留最近 CONFIG_AI_DIALOG_HISTORY_ROUNDS 轮（一问一答），
 * 超出挤掉最旧的。上下文拼接 = system + 历史 + 本轮 user。
 *
 * @date    2026-09-06
 * @version 2.0.0
 */

#include "dialog_manager.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "persona.h"
#include "memory_store.h"
#include "time_sync.h"

#define TAG "dialog"

/* 单条消息上限（bytes，UTF-8）；超长截断保护内存 */
#define MSG_MAX_LEN     (512)
/* 完整回复缓冲初始大小（不够自动翻倍） */
#define REPLY_BUF_INIT  (2048)
/* 动态 system prompt 缓冲（人设 2KB + 记忆 8×256B + 时间行，4KB 稳妥） */
#define SYS_PROMPT_MAX  (4096)
/* 每轮注入记忆条数上限（重要性降序 top-N；太多稀释注意力也费 token） */
#define MEM_INJECT_MAX  (8)

/** 一轮问答 */
typedef struct {
    char *user;
    char *reply;
} dialog_round_t;

static struct {
    bool inited;
    SemaphoreHandle_t lock;     /* 串口对话与语音对话并发保护（M1） */
    dialog_round_t rounds[CONFIG_AI_DIALOG_HISTORY_ROUNDS];
    int head;                   /* 最旧一轮的下标（环形） */
    int count;                  /* 环内有效轮数 */
    char *sys_prompt;           /* 动态 system prompt 组装区（PSRAM，持锁内使用） */
} s_dlg;

/**
 * 组装本轮 system prompt：人设 + 长期记忆 top-N + 当前时间。
 * 每轮调用（记忆/时间都可能刚变化）；结果写入 s_dlg.sys_prompt。
 */
static void build_system_prompt(void)
{
    char *p = s_dlg.sys_prompt;                         /* 写游标（strlcat 追加式） */
    p[0] = '\0';                                        /* 清空重来 */
    strlcpy(p, persona_base(), SYS_PROMPT_MAX);         /* 1. 基础人设打底 */

    /* 2. 长期记忆 top-N（空 query = 按重要性降序取前几条） */
    memory_entry_t mems[MEM_INJECT_MAX];                /* 结果快照数组（栈，2KB 级） */
    int nmem = memory_store_search(NULL, mems, MEM_INJECT_MAX); /* 取 top-N */
    if (nmem > 0) {                                     /* 有记忆才注入 */
        strlcat(p, "\n\n关于用户的记忆（可在对话中自然运用，别生硬复述）：",
                SYS_PROMPT_MAX);                        /* 引导语 */
        for (int i = 0; i < nmem; i++) {                /* 逐条追加 */
            strlcat(p, "\n- ", SYS_PROMPT_MAX);         /* 条目前缀 */
            strlcat(p, mems[i].content, SYS_PROMPT_MAX);        /* 记忆内容 */
        }
    }

    /* 3. 当前时间（SNTP 已同步才可信；让三玖知道"现在几点"才能聊作息） */
    if (time_sync_is_synced()) {                        /* 时钟可信 */
        time_t now = time(NULL);                        /* Unix 秒 */
        struct tm tm_now;                               /* 本地时间 */
        localtime_r(&now, &tm_now);                     /* 转本地 */
        char tbuf[64];                                  /* 时间行缓冲（防 format-truncation 告警） */
        static const char *wday[] = {"日", "一", "二", "三", "四", "五", "六"};
        snprintf(tbuf, sizeof(tbuf),                    /* 形如：当前时间：2026-09-09 21:30 周三 */
                 "\n\n当前时间：%04d-%02d-%02d %02d:%02d 周%s",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, wday[tm_now.tm_wday]);
        strlcat(p, tbuf, SYS_PROMPT_MAX);               /* 追加到 prompt */
    }
}

/** 压入一轮（环满挤最旧） */
static void history_push(char *user, char *reply)
{
    if (s_dlg.count < CONFIG_AI_DIALOG_HISTORY_ROUNDS) {
        int idx = (s_dlg.head + s_dlg.count) % CONFIG_AI_DIALOG_HISTORY_ROUNDS;
        s_dlg.rounds[idx].user = user;
        s_dlg.rounds[idx].reply = reply;
        s_dlg.count++;
    } else {
        /* 环满：释放最旧，占位 */
        int idx = s_dlg.head;
        free(s_dlg.rounds[idx].user);
        free(s_dlg.rounds[idx].reply);
        s_dlg.rounds[idx].user = user;
        s_dlg.rounds[idx].reply = reply;
        s_dlg.head = (s_dlg.head + 1) % CONFIG_AI_DIALOG_HISTORY_ROUNDS;
    }
}

/** messages 数组 JSON：system + 历史按时间序 + 本轮 user。调用方 cJSON_Delete */
static cJSON *build_messages(const char *user_text)
{
    build_system_prompt();                              /* 每轮刷新（人设/记忆/时间） */
    cJSON *arr = cJSON_CreateArray();
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", s_dlg.sys_prompt);  /* 动态组装版 */
    cJSON_AddItemToArray(arr, sys);

    /* 历史：head 起按时间序（环形遍历） */
    for (int i = 0; i < s_dlg.count; i++) {
        int idx = (s_dlg.head + i) % CONFIG_AI_DIALOG_HISTORY_ROUNDS;
        cJSON *mu = cJSON_CreateObject();
        cJSON_AddStringToObject(mu, "role", "user");
        cJSON_AddStringToObject(mu, "content", s_dlg.rounds[idx].user);
        cJSON_AddItemToArray(arr, mu);
        cJSON *ma = cJSON_CreateObject();
        cJSON_AddStringToObject(ma, "role", "assistant");
        cJSON_AddStringToObject(ma, "content", s_dlg.rounds[idx].reply);
        cJSON_AddItemToArray(arr, ma);
    }

    cJSON *mu = cJSON_CreateObject();
    cJSON_AddStringToObject(mu, "role", "user");
    cJSON_AddStringToObject(mu, "content", user_text);
    cJSON_AddItemToArray(arr, mu);
    return arr;
}

/** token 回调 trampoline：转发 UI 回调 + 攒完整回复（自动扩容） */
typedef struct {
    llm_token_cb_t user_cb;
    dialog_sentence_cb_t sentence_cb;
    void *user_ctx;
    char *buf;
    size_t len, cap;
    char sentence[512];
    size_t sentence_len;
} reply_acc_t;

static char *dialog_ask_locked(const char *user_text, llm_token_cb_t on_token,
                               dialog_sentence_cb_t on_sentence, void *ctx);

/**
 * 原地 UTF-8 消毒：只保留合法序列（含 ASCII），非法字节剔除。
 * 教训（M6）：Xunfei WS 结果 JSON 若被分帧截断，拼出的文本可能含
 * 破碎 UTF-8——存进历史后每次 LLM 请求都被 DeepSeek 以
 * "invalid unicode code point" 400 拒绝，且历史是环形的会持续投毒。
 */
static void utf8_sanitize(char *s)
{
    size_t r = 0, w = 0;                                /* 读/写游标 */
    while (s[r] != '\0') {                              /* 逐字节扫描 */
        unsigned char c = (unsigned char)s[r];          /* 取首字节判断序列长度 */
        size_t need;                                    /* 该序列应有总长 */
        if (c < 0x80) {                                 /* ASCII */
            need = 1;                                   /* 单字节 */
        } else if ((c & 0xE0) == 0xC0 && c >= 0xC2) {   /* 2 字节序列首字节 */
            need = 2;                                   /* 双字节 */
        } else if ((c & 0xF0) == 0xE0) {                /* 3 字节序列首字节 */
            need = 3;                                   /* 三字节 */
        } else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {   /* 4 字节序列首字节 */
            need = 4;                                   /* 四字节 */
        } else {                                        /* 非法首字节 */
            r++;                                        /* 跳过这一个坏字节 */
            continue;                                   /* 继续扫描 */
        }
        /* 校验续字节（必须都是 10xxxxxx）且不越界 */
        bool ok = (r + need <= strlen(s));              /* 先看长度是否越界 */
        for (size_t k = 1; ok && k < need; k++) {       /* 逐个检查续字节 */
            if (((unsigned char)s[r + k] & 0xC0) != 0x80) {     /* 非法续字节 */
                ok = false;                             /* 标记无效 */
            }
        }
        if (!ok) {                                      /* 序列残缺 */
            r++;                                        /* 跳过坏首字节 */
            continue;                                   /* 继续扫描 */
        }
        memmove(s + w, s + r, need);                    /* 合法序列搬到写入位 */
        w += need;                                      /* 写游标前进 */
        r += need;                                      /* 读游标前进 */
    }
    s[w] = '\0';                                        /* 补字符串结尾 */
}

static char *dialog_ask_locked(const char *user_text, llm_token_cb_t on_token,
                               dialog_sentence_cb_t on_sentence, void *ctx);

static bool is_sentence_end(const char *text, size_t remain, size_t *width)
{
    if (text[0] == '.' || text[0] == '!' || text[0] == '?' || text[0] == '\n') {
        *width = 1;
        return true;
    }
    if (remain >= 3) {
        bool chinese_period = (uint8_t)text[0] == 0xE3 &&
                              (uint8_t)text[1] == 0x80 &&
                              (uint8_t)text[2] == 0x82;
        bool chinese_exclaim_or_question = (uint8_t)text[0] == 0xEF &&
                                            (uint8_t)text[1] == 0xBC &&
                                            ((uint8_t)text[2] == 0x81 ||
                                             (uint8_t)text[2] == 0x9F);
        if (chinese_period || chinese_exclaim_or_question) {
            *width = 3;
            return true;
        }
    }
    return false;
}

/** 软断句只在短语已有一定长度时生效，避免把自然语言切成单字播报。 */
static bool is_phrase_break(const char *text, size_t remain, size_t *width)
{
    if (text[0] == ',' || text[0] == ';' || text[0] == ':') {
        *width = 1;
        return true;
    }
    if (remain >= 3) {
        bool chinese_comma = (uint8_t)text[0] == 0xEF &&
                             (uint8_t)text[1] == 0xBC &&
                             (uint8_t)text[2] == 0x8C;
        bool chinese_enumeration_comma = (uint8_t)text[0] == 0xE3 &&
                                         (uint8_t)text[1] == 0x80 &&
                                         (uint8_t)text[2] == 0x81;
        if (chinese_comma || chinese_enumeration_comma) {
            *width = 3;
            return true;
        }
    }
    return false;
}

static void sentence_append(reply_acc_t *ra, const char *text)
{
    if (!ra->sentence_cb) {
        return;
    }
    size_t len = strlen(text);
    size_t pos = 0;
    while (pos < len) {
        size_t width = 1;
        bool terminal = is_sentence_end(text + pos, len - pos, &width);
        bool soft_break = !terminal && is_phrase_break(text + pos, len - pos, &width);
        if (ra->sentence_len + width >= sizeof(ra->sentence)) {
            /* 回答异常长且没有标点时，仍然保证 TTS 不会无限等待。 */
            ra->sentence[ra->sentence_len] = '\0';
            ra->sentence_cb(ra->sentence, ra->user_ctx);
            ra->sentence_len = 0;
        }
        memcpy(ra->sentence + ra->sentence_len, text + pos, width);
        ra->sentence_len += width;
        pos += width;
        if ((terminal || (soft_break && ra->sentence_len >= 18)) && ra->sentence_len > 0) {
            ra->sentence[ra->sentence_len] = '\0';
            ra->sentence_cb(ra->sentence, ra->user_ctx);
            ra->sentence_len = 0;
        }
    }
}

static void reply_token_cb(const char *text, void *arg)
{
    reply_acc_t *ra = (reply_acc_t *)arg;
    size_t tl = strlen(text);
    if (ra->len + tl + 1 > ra->cap) {
        size_t ncap = ra->cap * 2;
        while (ra->len + tl + 1 > ncap) {
            ncap *= 2;
        }
        char *nb = heap_caps_realloc(ra->buf, ncap, MALLOC_CAP_SPIRAM);
        if (!nb) {
            return;                 /* 扩容失败丢弃该段（内存紧张属异常） */
        }
        ra->buf = nb;
        ra->cap = ncap;
    }
    memcpy(ra->buf + ra->len, text, tl);
    ra->len += tl;
    ra->buf[ra->len] = '\0';
    if (ra->user_cb) {
        ra->user_cb(text, ra->user_ctx);
    }
    sentence_append(ra, text);
}

esp_err_t dialog_manager_init(void)
{
    if (s_dlg.inited) {
        return ESP_OK;
    }
    memset(&s_dlg, 0, sizeof(s_dlg));
    s_dlg.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_dlg.lock, ESP_ERR_NO_MEM, TAG, "no mem for lock");
    ESP_RETURN_ON_ERROR(persona_init(), TAG, "persona init");   /* Phase4：人设先行（幂等） */
    s_dlg.sys_prompt = heap_caps_malloc(SYS_PROMPT_MAX, MALLOC_CAP_SPIRAM);     /* 组装区 */
    ESP_RETURN_ON_FALSE(s_dlg.sys_prompt, ESP_ERR_NO_MEM, TAG, "no mem for prompt");
    s_dlg.inited = true;
    ESP_LOGI(TAG, "对话管理器就绪（人设: %s, 历史 %d 轮, 记忆注入 ≤%d 条）",
             persona_name(), CONFIG_AI_DIALOG_HISTORY_ROUNDS, MEM_INJECT_MAX);
    return ESP_OK;
}

char *dialog_ask(const char *user_text, llm_token_cb_t on_token, void *ctx)
{
    return dialog_ask_stream(user_text, on_token, NULL, ctx);
}

char *dialog_ask_stream(const char *user_text, llm_token_cb_t on_token,
                        dialog_sentence_cb_t on_sentence, void *ctx)
{
    ESP_RETURN_ON_FALSE(s_dlg.inited && user_text && user_text[0],
                        NULL, TAG, "bad arg");
    /* 串口对话（console 任务）与语音对话（voice 任务）可能并发，
     * 一轮对话全程持锁串行化 */
    if (xSemaphoreTake(s_dlg.lock, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGW(TAG, "上一轮对话还没结束，本轮丢弃");
        return NULL;
    }
    char *result = dialog_ask_locked(user_text, on_token, on_sentence, ctx);
    xSemaphoreGive(s_dlg.lock);
    return result;
}

/** 持锁版：真实流程（历史/组包/LLM/拼装） */
static char *dialog_ask_locked(const char *user_text, llm_token_cb_t on_token,
                               dialog_sentence_cb_t on_sentence, void *ctx)
{

    /* 入参副本进历史（截断保护） */
    char *user_copy = heap_caps_malloc(MSG_MAX_LEN, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(user_copy, NULL, TAG, "no mem");
    strlcpy(user_copy, user_text, MSG_MAX_LEN);
    utf8_sanitize(user_copy);                       /* M6：剥离非法 UTF-8（防 DeepSeek 400） */

    /* 组 messages → 交给 LLM */
    cJSON *msgs = build_messages(user_copy);
    if (!msgs) {
        free(user_copy);
        return NULL;
    }
    char *messages_json = cJSON_PrintUnformatted(msgs);
    cJSON_Delete(msgs);
    if (!messages_json) {
        free(user_copy);
        return NULL;
    }

    reply_acc_t acc = {
        .user_cb = on_token,
        .sentence_cb = on_sentence,
        .user_ctx = ctx,
        .buf = heap_caps_malloc(REPLY_BUF_INIT, MALLOC_CAP_SPIRAM),
        .len = 0,
        .cap = REPLY_BUF_INIT,
    };
    if (!acc.buf) {
        free(user_copy);
        free(messages_json);
        return NULL;
    }
    acc.buf[0] = '\0';

    esp_err_t err = llm_chat_stream(messages_json, reply_token_cb, &acc);
    free(messages_json);

    if (err == ESP_OK && acc.sentence_cb && acc.sentence_len > 0) {
        acc.sentence[acc.sentence_len] = '\0';
        acc.sentence_cb(acc.sentence, acc.user_ctx);
    }

    if (err != ESP_OK || acc.len == 0) {
        ESP_LOGW(TAG, "LLM 一轮失败: %s", esp_err_to_name(err));
        free(user_copy);
        free(acc.buf);
        return NULL;
    }

    /* 复制出精确大小的完整回复返回调用方；原件消毒后进历史 */
    char *reply = heap_caps_malloc(acc.len + 1, MALLOC_CAP_SPIRAM);
    if (reply) {
        memcpy(reply, acc.buf, acc.len + 1);
        utf8_sanitize(reply);                   /* M6：回复也消毒（LLM 理论上输出合法
                                                 * UTF-8，但分帧拼装的边角不可信） */
    }
    free(acc.buf);

    if (reply) {
        char *reply_copy = heap_caps_malloc(acc.len + 1, MALLOC_CAP_SPIRAM);
        if (reply_copy) {
            memcpy(reply_copy, reply, acc.len + 1);
            history_push(user_copy, reply_copy);
        } else {
            free(user_copy);        /* 历史满了放不下就算了，回复照常给 */
        }
        ESP_LOGI(TAG, "一轮完成（问 %u 字节 / 答 %u 字节）",
                 (unsigned)strlen(user_text), (unsigned)acc.len);
    }
    return reply;
}
