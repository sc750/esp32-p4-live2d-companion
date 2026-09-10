/**
 * @file    diary_service.c
 * @brief   日记服务实现——素材收集 + LLM 第一人称生成 + SPIFFS 存储 + 22:00 定时
 *
 * 关键取舍：
 *   - 不用 SPIFFS 目录枚举（仿真行为不可靠），自维护 index.json 索引日期列表；
 *   - 定时用"每小时巡检"而非对齐式单次定时——天然容忍 SNTP 晚同步；
 *   - 生成在独立任务执行（esp_timer 回调栈小，扛不住网络+LLM 调用）。
 *
 * @date    2026-09-10
 * @version 1.0.0
 */

#include "diary_service.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "cJSON.h"

#include "llm_client.h"
#include "memory_store.h"
#include "persona.h"
#include "time_sync.h"

#define TAG "diary"

/* ---- 参数 ---- */
#define DIARY_TASK_STACK    (10 * 1024)     /* 生成任务栈（LLM 调用+cJSON 组包余量） */
#define DIARY_PROMPT_MAX    (3 * 1024)      /* 素材 prompt 组装区 */
#define DIARY_REPLY_MAX     (6 * 1024)      /* LLM 响应缓冲（日记 200~500 汉字） */
#define MEM_FOR_DIARY       6               /* 素材里带几条长期记忆 */
#define DIARY_INDEX_MAX     60              /* 索引最多记 60 天（约两个月） */
#define DIARY_HOUR          22              /* 自动生成时刻（小时） */

#define IDX_PATH            DIARY_DIR "/index.json"         /* 索引文件 */

#define EVT_GEN             (BIT0)          /* 事件位：请生成日记 */

static struct {
    bool inited;                            /* 幂等闸门 */
    EventGroupHandle_t evt;                 /* 生成请求事件组 */
    char *prompt;                           /* 素材组装区（PSRAM） */
    int today_rounds;                       /* 今日对话轮数（RAM，重启清零可接受） */
} s_dia;

/** 今天日期串（SNTP 未同步返回 false 不写字） */
static bool today_str(char out[DIARY_DATE_MAX])
{
    if (!time_sync_is_synced()) {           /* 时钟不可信 */
        return false;                       /* 拒绝给日期 */
    }
    time_t now = time(NULL);                /* Unix 秒 */
    struct tm tm_now;                       /* 本地时间 */
    localtime_r(&now, &tm_now);             /* 转本地 */
    char tmp[40];                           /* 中转缓冲（3 个 %d 最坏 11B 各 + 分隔符，给大防 truncation 告警） */
    snprintf(tmp, sizeof(tmp), "%04d-%02d-%02d",    /* YYYY-MM-DD */
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
    strlcpy(out, tmp, DIARY_DATE_MAX);      /* 拷到定长输出 */
    return true;                            /* 成功 */
}

/** 指定日期的日记文件路径（调用方给足缓冲） */
static void date_path(const char *date, char *out, size_t cap)
{
    snprintf(out, cap, DIARY_DIR "/%s.json", date);         /* 目录前缀+日期 */
}

/* ---------- 索引（日期列表，时间倒序维护） ---------- */

/** 追加一个日期到 index.json（已存在则不动；索引满挤掉最旧） */
static void index_append(const char *date)
{
    char dates[DIARY_INDEX_MAX][DIARY_DATE_MAX];            /* 读出的旧索引 */
    int n = 0;                                              /* 旧条数 */
    FILE *f = fopen(IDX_PATH, "r");                         /* 读旧索引 */
    if (f) {                                                /* 有旧索引 */
        char buf[DIARY_INDEX_MAX * 16];                     /* 读缓冲（60×15B 级） */
        size_t rd = fread(buf, 1, sizeof(buf) - 1, f);      /* 整读 */
        fclose(f);                                          /* 关文件 */
        buf[rd] = '\0';                                     /* 补 NUL */
        cJSON *root = cJSON_Parse(buf);                     /* 解析 */
        cJSON *arr = root ? cJSON_GetObjectItem(root, "dates") : NULL;      /* 日期数组 */
        cJSON *it = NULL;                                   /* 遍历游标 */
        cJSON_ArrayForEach(it, arr) {                       /* 逐条拷出 */
            if (cJSON_IsString(it) && n < DIARY_INDEX_MAX) {
                strlcpy(dates[n++], it->valuestring, DIARY_DATE_MAX);       /* 拷日期 */
            }
        }
        cJSON_Delete(root);                                 /* 树释放 */
    }
    for (int i = 0; i < n; i++) {                           /* 查重（今日已生成再生成） */
        if (strcmp(dates[i], date) == 0) {                  /* 已在索引 */
            return;                                         /* 不动 */
        }
    }
    if (n < DIARY_INDEX_MAX) {                              /* 未满 */
        n++;                                                /* 腾出新槽位 */
    }
    for (int i = n - 1; i > 0; i--) {                       /* 整体后移一格 */
        strlcpy(dates[i], dates[i - 1], DIARY_DATE_MAX);    /* 新日期插最前 */
    }
    strlcpy(dates[0], date, DIARY_DATE_MAX);                /* 头部放今日 */

    cJSON *root = cJSON_CreateObject();                     /* 重写索引 */
    cJSON *arr = cJSON_AddArrayToObject(root, "dates");     /* dates 数组 */
    for (int i = 0; i < n; i++) {                           /* 逐条写回 */
        cJSON_AddItemToArray(arr, cJSON_CreateString(dates[i]));
    }
    char *txt = cJSON_PrintUnformatted(root);               /* 序列化 */
    cJSON_Delete(root);                                     /* 树释放 */
    if (!txt) return;                                       /* 分配失败放弃 */
    f = fopen(IDX_PATH, "w");                               /* 覆盖写索引 */
    if (f) {                                                /* 打开成功 */
        fputs(txt, f);                                      /* 整写 */
        fclose(f);                                          /* 关闭刷盘 */
    }
    cJSON_free(txt);                                        /* 串释放 */
}

/* ---------- LLM 流式收全（最小 accumulator） ---------- */
typedef struct {
    char *buf;                                              /* 响应缓冲 */
    size_t len, cap;                                        /* 长度/容量 */
} d_acc_t;

/** token 回调：追加（超容截断） */
static void d_token_cb(const char *text, void *arg)
{
    d_acc_t *a = arg;                                       /* 上下文 */
    size_t tl = strlen(text);                               /* 片段长 */
    if (a->len + tl >= a->cap) {                            /* 容量防御 */
        tl = a->cap - a->len - 1;                           /* 截满 */
        if (tl == 0) return;                                /* 满则丢 */
    }
    memcpy(a->buf + a->len, text, tl);                      /* 追加 */
    a->len += tl;                                           /* 前进 */
    a->buf[a->len] = '\0';                                  /* NUL 结尾 */
}

/* ---------- 生成 ---------- */

/** 剥 markdown 围栏（与 memory_extract 同款处理） */
static char *d_strip_fence(char *s)
{
    char *p = s;                                            /* 头游标 */
    if (strncmp(p, "```", 3) == 0) {                        /* 头围栏 */
        p += 3;                                             /* 跳 ``` */
        while (*p != '\n' && *p != '\0') p++;               /* 跳语言行 */
        if (*p == '\n') p++;                                /* 跳换行 */
    }
    size_t n = strlen(p);                                   /* 尾检查 */
    if (n >= 3 && strncmp(p + n - 3, "```", 3) == 0) {      /* 尾围栏 */
        p[n - 3] = '\0';                                    /* 掐掉 */
    }
    return p;                                               /* 纯文本起点 */
}

/** 收集素材并组装 system/user prompt（结果在 s_dia.prompt 与堆串） */
static esp_err_t build_prompt(char **user_out)
{
    /* system：角色 + 日记文体要求 */
    snprintf(s_dia.prompt, DIARY_PROMPT_MAX,                /* 组 system 文本 */
             "你是%s。请以你的第一人称写一篇今天的日记。"
             "要求：温馨可爱有感情，可以提今天聊过的话题和心情；"
             "200~400 字，可以少量颜文字；不要标题，直接写正文。",
             persona_name());                               /* 名字动态注入 */

    /* user：日期 + 素材 */
    char date[DIARY_DATE_MAX];                              /* 今日日期 */
    today_str(date);                                        /* 取日期 */
    char weekday[8] = "周?";                                /* 星期占位 */
    {
        time_t now = time(NULL);                            /* 当前秒 */
        struct tm tm_now;                                   /* 本地时间 */
        localtime_r(&now, &tm_now);                         /* 转本地 */
        static const char *wd[] = {"日", "一", "二", "三", "四", "五", "六"};
        snprintf(weekday, sizeof(weekday), "周%s", wd[tm_now.tm_wday]);     /* 算星期 */
    }
    char *u = heap_caps_malloc(DIARY_PROMPT_MAX, MALLOC_CAP_SPIRAM);        /* user 缓冲 */
    ESP_RETURN_ON_FALSE(u, ESP_ERR_NO_MEM, TAG, "no mem");
    snprintf(u, DIARY_PROMPT_MAX,                           /* 头部：日期+轮数 */
             "今天是%s %s。今天我们聊了 %d 轮。\n",
             date, weekday, s_dia.today_rounds);
    memory_entry_t mems[MEM_FOR_DIARY];                     /* 记忆素材 */
    int nmem = memory_store_search(NULL, mems, MEM_FOR_DIARY);      /* top-N */
    if (nmem > 0) {                                         /* 有记忆才追加 */
        strlcat(u, "关于用户你还记得这些：\n", DIARY_PROMPT_MAX);   /* 引导语 */
        for (int i = 0; i < nmem; i++) {                    /* 逐条 */
            strlcat(u, "- ", DIARY_PROMPT_MAX);             /* 前缀 */
            strlcat(u, mems[i].content, DIARY_PROMPT_MAX);  /* 内容 */
            strlcat(u, "\n", DIARY_PROMPT_MAX);             /* 换行 */
        }
    } else {
        strlcat(u, "今天似乎没有特别的对话记录。\n", DIARY_PROMPT_MAX);     /* 空日记兜底 */
    }
    strlcat(u, "\n请写今天的日记。", DIARY_PROMPT_MAX);     /* 收尾指令 */
    *user_out = u;                                          /* 交出 */
    return ESP_OK;                                          /* 成功 */
}

/** 内部生成（在 diary 任务或串口任务上下文执行；阻塞数秒） */
static esp_err_t generate_locked(void)
{
    char date[DIARY_DATE_MAX];                              /* 今日日期 */
    if (!today_str(date)) {                                 /* 时钟未同步 */
        ESP_LOGW(TAG, "时钟未同步，无法生成日记");          /* 告警 */
        return ESP_ERR_INVALID_STATE;                       /* 拒绝 */
    }
    /* 1. 组 prompt */
    char *user_msg = NULL;                                  /* user 消息串 */
    esp_err_t err = build_prompt(&user_msg);                /* 组装 */
    if (err != ESP_OK) return err;                          /* 失败即返 */

    cJSON *arr = cJSON_CreateArray();                       /* messages 数组 */
    cJSON *sys = cJSON_CreateObject();                      /* system 条 */
    cJSON *usr = cJSON_CreateObject();                      /* user 条 */
    if (!arr || !sys || !usr) {                             /* 分配失败 */
        free(user_msg);                                     /* 清理 */
        cJSON_Delete(arr);                                  /* （NULL 安全） */
        return ESP_ERR_NO_MEM;                              /* 返回 */
    }
    cJSON_AddStringToObject(sys, "role", "system");         /* 角色 */
    cJSON_AddStringToObject(sys, "content", s_dia.prompt);  /* 文体指令 */
    cJSON_AddStringToObject(usr, "role", "user");           /* 角色 */
    cJSON_AddStringToObject(usr, "content", user_msg);      /* 素材 */
    cJSON_AddItemToArray(arr, sys);                         /* 入列 */
    cJSON_AddItemToArray(arr, usr);                         /* 入列 */
    char *messages_json = cJSON_PrintUnformatted(arr);      /* 序列化 */
    cJSON_Delete(arr);                                      /* 树释放 */
    free(user_msg);                                         /* 素材串释放 */
    if (!messages_json) return ESP_ERR_NO_MEM;              /* 分配失败 */

    /* 2. 调 LLM 流式攒全 */
    char *resp = heap_caps_malloc(DIARY_REPLY_MAX, MALLOC_CAP_SPIRAM);      /* 响应缓冲 */
    if (!resp) {                                            /* 分配失败 */
        cJSON_free(messages_json);                          /* 清理 */
        return ESP_ERR_NO_MEM;                              /* 返回 */
    }
    d_acc_t acc = { .buf = resp, .cap = DIARY_REPLY_MAX, .len = 0 };        /* accumulator */
    resp[0] = '\0';                                         /* 预置 NUL */
    err = llm_chat_stream(messages_json, d_token_cb, &acc); /* 同步调用 */
    cJSON_free(messages_json);                              /* 请求串释放 */
    if (err != ESP_OK || acc.len == 0) {                    /* 网络/空响应 */
        ESP_LOGW(TAG, "日记 LLM 调用失败: %s", esp_err_to_name(err));
        free(resp);                                         /* 清理 */
        return ESP_FAIL;                                    /* 返回 */
    }

    /* 3. 剥围栏 + 保存 JSON（date/content/rounds/created_at） */
    char *content = d_strip_fence(resp);                    /* 纯正文 */
    cJSON *root = cJSON_CreateObject();                     /* 条目对象 */
    if (!root) {                                            /* 分配失败 */
        free(resp);                                         /* 清理 */
        return ESP_ERR_NO_MEM;                              /* 返回 */
    }
    cJSON_AddStringToObject(root, "date", date);            /* 日期 */
    cJSON_AddStringToObject(root, "content", content);      /* 正文 */
    cJSON_AddNumberToObject(root, "rounds", s_dia.today_rounds);    /* 当日轮数 */
    cJSON_AddNumberToObject(root, "created_at",
                            (double)(esp_timer_get_time() / 1000000LL));    /* 生成时刻 */
    char *txt = cJSON_PrintUnformatted(root);               /* 序列化 */
    cJSON_Delete(root);                                     /* 树释放 */
    free(resp);                                             /* 响应缓冲释放 */
    if (!txt) return ESP_ERR_NO_MEM;                        /* 分配失败 */

    char path[64];                                          /* 文件路径缓冲 */
    date_path(date, path, sizeof(path));                    /* 拼路径 */
    FILE *f = fopen(path, "w");                             /* 覆盖写 */
    if (!f) {                                               /* 打不开（罕见） */
        cJSON_free(txt);                                    /* 清理 */
        ESP_LOGE(TAG, "日记写盘失败: %s", path);            /* 报错 */
        return ESP_FAIL;                                    /* 返回 */
    }
    fputs(txt, f);                                          /* 整写 */
    fclose(f);                                              /* 刷盘 */
    cJSON_free(txt);                                        /* 串释放 */
    index_append(date);                                     /* 索引登记 */
    s_dia.today_rounds = 0;                                 /* 轮数清零（进入新一天语义） */
    ESP_LOGI(TAG, "日记已生成: %s (%u 字节)", path, (unsigned)strlen(content));
    return ESP_OK;                                          /* 成功 */
}

/** diary 任务：等生成请求 → 执行（网络+LLM 都在这里，栈给足） */
static void diary_task(void *arg)
{
    while (1) {                                             /* 常驻循环 */
        EventBits_t bits = xEventGroupWaitBits(s_dia.evt, EVT_GEN,
                                               pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & EVT_GEN) {                               /* 收到生成请求 */
            generate_locked();                              /* 同步生成（失败仅告警） */
        }
    }
}

/** 每小时巡检定时器回调：22 点 + 今日未生成 + 今天聊过天 → 请求生成 */
static void hourly_check_cb(void *arg)
{
    if (!time_sync_is_synced()) {                           /* 时钟未同步 */
        return;                                             /* 不误触发 */
    }
    time_t now = time(NULL);                                /* 当前秒 */
    struct tm tm_now;                                       /* 本地时间 */
    localtime_r(&now, &tm_now);                             /* 转本地 */
    if (tm_now.tm_hour != DIARY_HOUR) {                     /* 不是 22 点档 */
        return;                                             /* 跳过 */
    }
    if (s_dia.today_rounds == 0) {                          /* 今天没聊过天 */
        return;                                             /* 没素材不写 */
    }
    char date[DIARY_DATE_MAX];                              /* 今日日期 */
    today_str(date);                                        /* 取日期 */
    char path[64];                                          /* 路径缓冲 */
    date_path(date, path, sizeof(path));                    /* 拼路径 */
    FILE *f = fopen(path, "r");                             /* 探测今日日记 */
    if (f) {                                                /* 已生成过 */
        fclose(f);                                          /* 关文件 */
        return;                                             /* 幂等跳过 */
    }
    xEventGroupSetBits(s_dia.evt, EVT_GEN);                 /* 请求生成（任务执行） */
}

/* ---------- 公共 API ---------- */

esp_err_t diary_service_init(void)
{
    if (s_dia.inited) {                                     /* 幂等 */
        return ESP_OK;                                      /* 无害返回 */
    }
    memset(&s_dia, 0, sizeof(s_dia));                       /* 清零 */
    s_dia.evt = xEventGroupCreate();                        /* 事件组 */
    ESP_RETURN_ON_FALSE(s_dia.evt, ESP_ERR_NO_MEM, TAG, "no mem evt");
    s_dia.prompt = heap_caps_malloc(DIARY_PROMPT_MAX, MALLOC_CAP_SPIRAM);   /* 组装区 */
    ESP_RETURN_ON_FALSE(s_dia.prompt, ESP_ERR_NO_MEM, TAG, "no mem prompt");

    mkdir(DIARY_DIR, 0);                    /* 建目录（SPIFFS 是前缀仿真，失败无害） */
    if (xTaskCreate(diary_task, "diary", DIARY_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建 diary 任务失败");               /* 报错 */
        return ESP_FAIL;                                    /* 返回 */
    }
    /* 每小时巡检（首延迟 1 分钟起，避免开机高峰） */
    const esp_timer_create_args_t targs = {
        .callback = hourly_check_cb,                        /* 回调 */
        .name = "diary_chk",                                /* 定时器名 */
    };
    esp_timer_handle_t th = NULL;                           /* 句柄 */
    ESP_RETURN_ON_FALSE(esp_timer_create(&targs, &th) == ESP_OK,
                        ESP_FAIL, TAG, "timer create");
    esp_timer_start_periodic(th, 3600ULL * 1000000ULL);     /* 每小时一次 */
    s_dia.inited = true;                                    /* 就绪 */
    ESP_LOGI(TAG, "日记服务就绪（每日 %02d:00 自动，串口 diary now 手动）", DIARY_HOUR);
    return ESP_OK;                                          /* 成功 */
}

esp_err_t diary_generate_today(void)
{
    if (!s_dia.inited) {                                    /* 未初始化 */
        return ESP_ERR_INVALID_STATE;                       /* 拒绝 */
    }
    return generate_locked();                               /* 同步生成（调用方任务上下文） */
}

esp_err_t diary_read(const char *date, diary_entry_t *out)
{
    ESP_RETURN_ON_FALSE(s_dia.inited && date && out, ESP_ERR_INVALID_STATE, TAG, "bad arg");
    char path[64];                                          /* 路径缓冲 */
    date_path(date, path, sizeof(path));                    /* 拼路径 */
    FILE *f = fopen(path, "r");                             /* 只读 */
    if (!f) return ESP_ERR_NOT_FOUND;                       /* 无此日 */
    char buf[DIARY_CONTENT_MAX + 256];                      /* 读缓冲 */
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);           /* 整读 */
    fclose(f);                                              /* 关文件 */
    buf[n] = '\0';                                          /* 补 NUL */
    cJSON *root = cJSON_Parse(buf);                         /* 解析 */
    ESP_RETURN_ON_FALSE(root, ESP_ERR_INVALID_STATE, TAG, "parse failed");
    cJSON *jc = cJSON_GetObjectItem(root, "content");       /* 正文 */
    cJSON *jr = cJSON_GetObjectItem(root, "rounds");        /* 轮数 */
    strlcpy(out->date, date, DIARY_DATE_MAX);               /* 日期 */
    strlcpy(out->content, cJSON_IsString(jc) ? jc->valuestring : "",
            DIARY_CONTENT_MAX);                             /* 正文 */
    out->rounds = cJSON_IsNumber(jr) ? jr->valueint : 0;    /* 轮数 */
    out->created_at = 0;                                    /* 读回不还原时间戳（展示不用） */
    cJSON_Delete(root);                                     /* 树释放 */
    return ESP_OK;                                          /* 成功 */
}

int diary_list(char dates[][DIARY_DATE_MAX], int max)
{
    if (!s_dia.inited || !dates || max <= 0) return 0;      /* 参数防御 */
    FILE *f = fopen(IDX_PATH, "r");                         /* 读索引 */
    if (!f) return 0;                                       /* 无索引 = 空 */
    char buf[DIARY_INDEX_MAX * 16];                         /* 读缓冲 */
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);           /* 整读 */
    fclose(f);                                              /* 关文件 */
    buf[n] = '\0';                                          /* 补 NUL */
    cJSON *root = cJSON_Parse(buf);                         /* 解析 */
    if (!root) return 0;                                    /* 坏索引按空 */
    cJSON *arr = cJSON_GetObjectItem(root, "dates");        /* 日期数组 */
    int cnt = 0;                                            /* 输出计数 */
    cJSON *it = NULL;                                       /* 游标 */
    cJSON_ArrayForEach(it, arr) {                           /* 逐条（已是倒序） */
        if (cnt >= max) break;                              /* 容量保护 */
        if (cJSON_IsString(it)) {                           /* 合法 */
            strlcpy(dates[cnt++], it->valuestring, DIARY_DATE_MAX);         /* 拷出 */
        }
    }
    cJSON_Delete(root);                                     /* 树释放 */
    return cnt;                                             /* 返回条数 */
}

void diary_note_round(void)
{
    s_dia.today_rounds++;                                   /* 简单累加（溢出不可能） */
}
