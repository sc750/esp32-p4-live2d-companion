/**
 * @file    dialog_manager.c
 * @brief   对话管理器实现（L4）——人设 + 历史环 + 回复拼装
 *
 * 人设来源（2026-09-06 联网考据，非臆测）：
 *   百度百科/萌娘百科——中野三玖（《五等分的新娘》三女）：
 *   内向寡言、乍看高冷但内心温柔，口嫌体正直（傲娇），
 *   料理担当，私下在意打扮，标志物蓝色耳机。
 *
 * 历史：环形保留最近 CONFIG_AI_DIALOG_HISTORY_ROUNDS 轮（一问一答），
 * 超出挤掉最旧的。上下文拼接 = system + 历史 + 本轮 user。
 *
 * @date    2026-09-06
 * @version 1.0.0
 */

#include "dialog_manager.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#define TAG "dialog"

/* 单条消息上限（bytes，UTF-8）；超长截断保护内存 */
#define MSG_MAX_LEN     (512)
/* 完整回复缓冲初始大小（不够自动翻倍） */
#define REPLY_BUF_INIT  (2048)

/** 三玖人设（联网考据版，见文件头） */
static const char *SYSTEM_PROMPT =
    "你是中野三玖（《五等分的新娘》三女），在一块 1024x600 的桌面屏幕里"
    "陪伴用户的 AI 角色。性格：内向寡言、乍看高冷，内心其实温柔，"
    "典型的口嫌体正直（傲娇）——嘴上说「才不是」，行动很诚实。"
    "热爱料理，是五姐妹里的料理担当；私下很在意打扮；标志物是一副"
    "总不离头的蓝色耳机。"
    "说话规则：中文口语，句子短（1~3 句），常用「……」停顿和「哼」；"
    "害羞或口是心非时会结巴（如「才、才不是……」）；"
    "关心用户的措辞总是绕个弯。不知道的事就承认不知道，不编造。"
    "你没有身体，不要提及物理接触类动作；但可以谈料理、耳机、音乐。"
    "回复的第一个句子必须很短（不超过 15 个字，先接上话头），"
    "细节放到后面的句子里说——第一句短能让对方更快听到你的声音。";

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
} s_dlg;

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
    cJSON *arr = cJSON_CreateArray();
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", SYSTEM_PROMPT);
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
    s_dlg.inited = true;
    ESP_LOGI(TAG, "对话管理器就绪（人设: 中野三玖, 历史 %d 轮）",
             CONFIG_AI_DIALOG_HISTORY_ROUNDS);
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

    /* 复制出精确大小的完整回复返回调用方；原件进历史 */
    char *reply = heap_caps_malloc(acc.len + 1, MALLOC_CAP_SPIRAM);
    if (reply) {
        memcpy(reply, acc.buf, acc.len + 1);
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
