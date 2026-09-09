/**
 * @file    memory_extract.c
 * @brief   对话摘要提取实现——素材缓冲 + LLM 提取 + JSON 解析入库
 *
 * 时序契约（见 .h 头注）：note_round 只在"一轮结束、下一轮未开始"的空隙
 * 被调（dialog_manager 串行上下文），故本模块无需与对话链互斥。
 *
 * @date    2026-09-09
 * @version 1.0.0
 */

#include "memory_extract.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "llm_client.h"
#include "memory_store.h"

#define TAG "mem_ext"

/* ---- 参数（MVP 保守值，后续可调） ---- */
#define EXTRACT_ROUNDS      4               /* 攒满几轮触发一次提取 */
#define ROUND_SLICE_MAX     512             /* 单轮素材上限（user/reply 各截到此） */
#define EXTRACT_BUF_MAX     (8 * 1024)      /* 素材缓冲总容量（PSRAM） */
#define EXTRACT_TIMEOUT_S   20              /* 提取 LLM 超时（秒）；超时丢素材不阻塞 */
#define EXTRACT_REPLY_MAX   (4 * 1024)      /* LLM 响应缓冲（提取结果是小 JSON） */

/** 素材缓冲区里的一轮 */
typedef struct {
    char user[ROUND_SLICE_MAX];             /* 用户输入（截断版） */
    char reply[ROUND_SLICE_MAX];            /* 回复（截断版） */
} extract_round_t;

static struct {
    bool inited;                            /* 幂等闸门 */
    extract_round_t *rounds;                /* 素材环（PSRAM） */
    int count;                              /* 已攒轮数 */
    char *buf;                              /* 拼接工作区（PSRAM，组 prompt 用） */
} s_ext;

/** 流式 token 攒完整响应的最小 accumulator */
typedef struct {
    char *buf;                              /* 响应缓冲 */
    size_t len, cap;                        /* 长度/容量 */
} acc_t;

/** token 回调：片段追加到 acc（超容截断） */
static void acc_token_cb(const char *text, void *arg)
{
    acc_t *a = arg;                         /* 取上下文 */
    size_t tl = strlen(text);               /* 本片段长 */
    if (a->len + tl >= a->cap) {            /* 容量防御 */
        tl = a->cap - a->len - 1;           /* 截到正好填满 */
        if (tl == 0) return;                /* 满了丢弃后续 */
    }
    memcpy(a->buf + a->len, text, tl);      /* 追加 */
    a->len += tl;                           /* 前进 */
    a->buf[a->len] = '\0';                  /* 保持 NUL 结尾 */
}

/** 剥 markdown 代码围栏（LLM 有时包 ```json ...```） */
static char *strip_fence(char *s)
{
    char *p = s;                            /* 头部游标 */
    if (strncmp(p, "```", 3) == 0) {        /* 头围栏 */
        p += 3;                             /* 跳过 ``` */
        while (*p != '\n' && *p != '\0') p++;       /* 跳过语言标记行 */
        if (*p == '\n') p++;                /* 跳过换行 */
    }
    size_t n = strlen(p);                   /* 尾部检查长度 */
    if (n >= 3 && strncmp(p + n - 3, "```", 3) == 0) {      /* 尾围栏 */
        p[n - 3] = '\0';                    /* 掐掉 */
    }
    return p;                               /* 返回纯 JSON 起点 */
}

/** type 字符串 → 枚举（未知值按"事实"兜底） */
static memory_type_t type_from_str(const char *s)
{
    if (!s) return MEM_TYPE_FACT;                       /* 缺省 */
    if (strcmp(s, "profile") == 0) return MEM_TYPE_PROFILE;     /* 画像 */
    if (strcmp(s, "event") == 0) return MEM_TYPE_EVENT;         /* 事件 */
    if (strcmp(s, "emotion") == 0) return MEM_TYPE_EMOTION;     /* 情感 */
    if (strcmp(s, "habit") == 0) return MEM_TYPE_HABIT;         /* 习惯 */
    return MEM_TYPE_FACT;                               /* fact 及未知值 */
}

/** 用当前素材做一次提取；成功入库并返回 true（素材由调用方清） */
static bool do_extract(void)
{
    if (s_ext.count == 0) {                             /* 没素材不白跑 */
        return false;                                   /* 直接失败语义 */
    }
    /* ---- 1. 拼对话记录（[用户]/[三玖] 逐轮堆进工作区） ---- */
    s_ext.buf[0] = '\0';                                /* 清工作区 */
    for (int i = 0; i < s_ext.count; i++) {             /* 逐轮拼接 */
        strlcat(s_ext.buf, "[用户] ", EXTRACT_BUF_MAX);         /* 前缀 */
        strlcat(s_ext.buf, s_ext.rounds[i].user, EXTRACT_BUF_MAX);      /* 用户侧 */
        strlcat(s_ext.buf, "\n[三玖] ", EXTRACT_BUF_MAX);       /* 前缀 */
        strlcat(s_ext.buf, s_ext.rounds[i].reply, EXTRACT_BUF_MAX);     /* 角色侧 */
        strlcat(s_ext.buf, "\n", EXTRACT_BUF_MAX);      /* 轮间空行 */
    }

    /* ---- 2. 组 messages（system 提取指令 + user 对话记录） ---- */
    cJSON *arr = cJSON_CreateArray();                   /* messages 数组 */
    if (!arr) return false;                             /* 分配失败 */
    cJSON *sys = cJSON_CreateObject();                  /* system 条 */
    cJSON *usr = cJSON_CreateObject();                  /* user 条 */
    if (!sys || !usr) {                                 /* 分配失败 */
        cJSON_Delete(arr);                              /* 清理 */
        return false;                                   /* 放弃 */
    }
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content",             /* 提取指令（严格 JSON 约束） */
        "你是记忆提取器。从对话中提取值得长期记住的、关于用户本人的信息"
        "（事实/偏好/事件/情感/习惯）。输出 JSON 数组，每项形如"
        "{\"content\":\"不超过40字\",\"type\":\"profile|fact|event|emotion|habit\","
        "\"importance\":1到10}。只提取用户侧信息；寒暄闲聊和助手的回复不要提取；"
        "没有值得记的就输出 []。只输出 JSON 数组本身，不要任何解释或代码围栏。");
    cJSON_AddStringToObject(usr, "role", "user");
    cJSON_AddStringToObject(usr, "content", s_ext.buf); /* 对话记录 */
    cJSON_AddItemToArray(arr, sys);
    cJSON_AddItemToArray(arr, usr);
    char *messages_json = cJSON_PrintUnformatted(arr);  /* 序列化 */
    cJSON_Delete(arr);                                  /* 树释放 */
    if (!messages_json) return false;                   /* 分配失败 */

    /* ---- 3. 调 LLM（流式但只攒不全播；同步阻塞，契约上处于对话空隙） ---- */
    char *resp = heap_caps_malloc(EXTRACT_REPLY_MAX, MALLOC_CAP_SPIRAM);        /* 响应缓冲 */
    if (!resp) {                                        /* 分配失败 */
        cJSON_free(messages_json);                      /* 清理 */
        return false;                                   /* 放弃 */
    }
    acc_t acc = { .buf = resp, .cap = EXTRACT_REPLY_MAX, .len = 0 };    /* accumulator */
    resp[0] = '\0';                                     /* 预置 NUL */
    int64_t t0 = esp_timer_get_time();                  /* 计时起点 */
    /* llm_chat_stream 无超时参数——ai_http 底层 recv 靠服务端关流结束；
     * 提取请求短平快，实测 2~5s，极端网络挂起由下一轮对话自然抢占兜底 */
    esp_err_t err = llm_chat_stream(messages_json, acc_token_cb, &acc);
    cJSON_free(messages_json);                          /* 请求串释放 */
    if (err != ESP_OK || acc.len == 0) {                /* 网络败/空响应 */
        ESP_LOGW(TAG, "提取 LLM 调用失败: %s", esp_err_to_name(err));
        free(resp);                                     /* 释放响应缓冲 */
        return false;                                   /* 放弃本轮素材 */
    }

    /* ---- 4. 解析 JSON 数组 → 逐条入库 ---- */
    char *json = strip_fence(resp);                     /* 剥围栏 */
    cJSON *list = cJSON_Parse(json);                    /* 解析 */
    int added = 0;                                      /* 入库计数 */
    if (cJSON_IsArray(list)) {                          /* 合法数组才处理 */
        cJSON *it = NULL;                               /* 遍历游标 */
        cJSON_ArrayForEach(it, list) {                  /* 逐项 */
            cJSON *jc = cJSON_GetObjectItem(it, "content");     /* 内容 */
            cJSON *jt = cJSON_GetObjectItem(it, "type");        /* 类型 */
            cJSON *ji = cJSON_GetObjectItem(it, "importance");  /* 重要性 */
            if (cJSON_IsString(jc) && jc->valuestring[0]) {     /* 内容有效 */
                uint32_t id = 0;                        /* 入库返回 ID */
                memory_store_add(type_from_str(cJSON_IsString(jt) ? jt->valuestring : NULL),
                                 jc->valuestring,
                                 cJSON_IsNumber(ji) ? ji->valueint : 5,
                                 &id);                  /* 去重在 store 内置 */
                if (id > 0) added++;                    /* 统计真实新增 */
            }
        }
    } else {
        ESP_LOGW(TAG, "提取结果非 JSON 数组: %.96s", json);     /* 模型跑飞兜底 */
    }
    cJSON_Delete(list);                                 /* 树释放 */
    free(resp);                                         /* 响应缓冲释放 */
    ESP_LOGI(TAG, "提取完成: %d 轮素材 → 新增 %d 条记忆（%lld ms）",
             s_ext.count, added, (long long)((esp_timer_get_time() - t0) / 1000));
    return true;                                        /* 成功（素材可清） */
}

esp_err_t memory_extract_init(void)
{
    if (s_ext.inited) {                                 /* 幂等 */
        return ESP_OK;                                  /* 无害返回 */
    }
    memset(&s_ext, 0, sizeof(s_ext));                   /* 清零 */
    s_ext.rounds = calloc(EXTRACT_ROUNDS, sizeof(extract_round_t));     /* 素材环 */
    ESP_RETURN_ON_FALSE(s_ext.rounds, ESP_ERR_NO_MEM, TAG, "no mem rounds");
    s_ext.buf = heap_caps_malloc(EXTRACT_BUF_MAX, MALLOC_CAP_SPIRAM);   /* 工作区 */
    ESP_RETURN_ON_FALSE(s_ext.buf, ESP_ERR_NO_MEM, TAG, "no mem buf");
    s_ext.inited = true;                                /* 就绪 */
    ESP_LOGI(TAG, "摘要提取就绪（攒 %d 轮触发）", EXTRACT_ROUNDS);
    return ESP_OK;                                      /* 成功 */
}

void memory_extract_note_round(const char *user, const char *reply)
{
    if (!s_ext.inited || !user || !reply) {             /* 参数/状态防御 */
        return;                                         /* 静默忽略 */
    }
    /* 攒素材（老素材循环覆盖：第 5 轮写回槽 0——提取频率高于覆盖频率，无损） */
    extract_round_t *r = &s_ext.rounds[s_ext.count % EXTRACT_ROUNDS];   /* 目标槽 */
    strlcpy(r->user, user, ROUND_SLICE_MAX);            /* 用户侧截断入槽 */
    strlcpy(r->reply, reply, ROUND_SLICE_MAX);          /* 回复侧截断入槽 */
    s_ext.count++;                                      /* 轮数推进 */
    if (s_ext.count % EXTRACT_ROUNDS != 0) {            /* 未到阈值 */
        return;                                         /* 继续攒 */
    }
    /* 到阈值：就地同步提取（对话空隙，网络空闲；失败只清素材不影响对话） */
    if (do_extract()) {                                 /* 提取成功 */
        s_ext.count = 0;                                /* 清素材重新攒 */
    } else {
        s_ext.count = 0;                                /* 失败也清（旧素材过时无价值） */
    }
}
