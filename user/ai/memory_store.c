/**
 * @file    memory_store.c
 * @brief   长期记忆存储实现——PSRAM 常驻数组 + SPIFFS 整文件 JSON 持久化
 *
 * 设计取舍：
 *   - RAM 常驻全量条目（200×~280B≈56KB PSRAM，九牛一毛），检索零 IO；
 *   - 每次增删后整文件重写（几十 KB 级，SPIFFS 毫秒级完成），
 *     换取实现简单 + 断电一致性（绝不出现半条记忆）；
 *   - 去重用"content 完全相同即跳过"，语义级去重留给打磨期的 LLM 整理。
 *
 * @date    2026-09-09
 * @version 1.0.0
 */

#include "memory_store.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "esp_timer.h"

#define TAG "mem_store"

/* 模块状态（全量条目常驻 PSRAM；save 自旋防重入由 lock 保证） */
static struct {
    bool inited;                                        /* 幂等闸门 */
    SemaphoreHandle_t lock;                             /* CRUD 与落盘的并发保护 */
    memory_entry_t *entries;                            /* 条目数组（PSRAM 堆） */
    int count;                                          /* 有效条目数 */
    uint32_t next_id;                                   /* 下一个分配的 ID */
} s_mem;

/** 当前 Unix 秒（SNTP 未同步时也给出单调近似值，created_at 仅作排序展示用） */
static uint32_t now_sec(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);        /* 开机微秒 → 秒 */
}

const char *memory_type_name(memory_type_t type)
{
    static const char *names[] = {                      /* 枚举序号对齐下标 */
        "画像", "事实", "事件", "情感", "习惯",
    };
    if ((int)type < 0 || (int)type > MEM_TYPE_HABIT) {  /* 越界兜底 */
        return "?";                                     /* 未知类型 */
    }
    return names[type];                                 /* 返回中文名 */
}

/** 按 ID 查条目下标（内部用，调用方须持锁）；找不到 -1 */
static int find_index_by_id(uint32_t id)
{
    for (int i = 0; i < s_mem.count; i++) {             /* 线性扫（≤200 条，微秒级） */
        if (s_mem.entries[i].id == id) {                /* 命中 */
            return i;                                   /* 返回下标 */
        }
    }
    return -1;                                          /* 未找到 */
}

/** 与现有条目完全相同的内容 → 返回其下标（去重用）；找不到 -1 */
static int find_duplicate(const char *content)
{
    for (int i = 0; i < s_mem.count; i++) {             /* 逐条比对 */
        if (strcmp(s_mem.entries[i].content, content) == 0) {   /* 完全一致 */
            return i;                                   /* 重复命中 */
        }
    }
    return -1;                                          /* 无重复 */
}

esp_err_t memory_store_init(void)
{
    if (s_mem.inited) {                                 /* 幂等 */
        return ESP_OK;                                  /* 重复调用无害 */
    }
    memset(&s_mem, 0, sizeof(s_mem));                   /* 清零状态 */
    s_mem.lock = xSemaphoreCreateMutex();               /* 并发保护 */
    ESP_RETURN_ON_FALSE(s_mem.lock, ESP_ERR_NO_MEM, TAG, "no mem for lock");

    /* 条目数组一次到位（PSRAM；SPIRAM_USE_MALLOC 下 >16KB 自动路由） */
    s_mem.entries = calloc(MEM_MAX_ENTRIES, sizeof(memory_entry_t));
    ESP_RETURN_ON_FALSE(s_mem.entries, ESP_ERR_NO_MEM, TAG, "no mem for entries");

    /* ---- 读回持久化文件（不存在 = 首次开机，空库起步） ---- */
    FILE *f = fopen(MEM_PATH, "r");                     /* 只读打开 */
    if (f) {                                            /* 文件存在才解析 */
        fseek(f, 0, SEEK_END);                          /* 跳到尾部取长度 */
        long size = ftell(f);                           /* 文件字节数 */
        fseek(f, 0, SEEK_SET);                          /* 回到开头 */
        if (size > 0 && size < 512 * 1024) {            /* 合理范围（防异常巨型文件） */
            char *buf = malloc(size + 1);               /* 读缓冲（自动走 PSRAM） */
            if (buf) {                                  /* 分配成功才解析 */
                size_t n = fread(buf, 1, size, f);      /* 整读 */
                buf[n] = '\0';                          /* 补 NUL */
                cJSON *root = cJSON_Parse(buf);         /* 解析 JSON */
                if (root) {                             /* 解析成功 */
                    cJSON *jid = cJSON_GetObjectItem(root, "next_id");      /* ID 游标 */
                    cJSON *jarr = cJSON_GetObjectItem(root, "entries");     /* 条目数组 */
                    if (cJSON_IsNumber(jid)) {          /* 恢复 ID 游标 */
                        s_mem.next_id = (uint32_t)jid->valuedouble;
                    }
                    cJSON *it = NULL;                   /* 遍历游标 */
                    cJSON_ArrayForEach(it, jarr) {      /* 逐条恢复 */
                        if (s_mem.count >= MEM_MAX_ENTRIES) {   /* 容量保护 */
                            break;                      /* 满了就停 */
                        }
                        memory_entry_t *e = &s_mem.entries[s_mem.count];        /* 落位槽 */
                        cJSON *v;                       /* 字段游标 */
                        v = cJSON_GetObjectItem(it, "id");                      /* ID */
                        e->id = cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 0;
                        v = cJSON_GetObjectItem(it, "type");                    /* 类型 */
                        e->type = cJSON_IsNumber(v) ? (memory_type_t)v->valueint
                                                    : MEM_TYPE_FACT;    /* 缺省按事实 */
                        v = cJSON_GetObjectItem(it, "content");                 /* 内容 */
                        strlcpy(e->content, cJSON_IsString(v) ? v->valuestring : "",
                                MEM_CONTENT_MAX);
                        v = cJSON_GetObjectItem(it, "importance");              /* 重要性 */
                        e->importance = cJSON_IsNumber(v) ? v->valueint : 5;
                        v = cJSON_GetObjectItem(it, "created_at");              /* 时间戳 */
                        e->created_at = cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 0;
                        s_mem.count++;                  /* 有效条数 +1 */
                    }
                    cJSON_Delete(root);                 /* 释放 JSON 树 */
                    ESP_LOGI(TAG, "记忆读回: %d 条 (next_id=%u)",
                             s_mem.count, (unsigned)s_mem.next_id);
                } else {
                    ESP_LOGW(TAG, "记忆文件损坏，按空库起步");   /* 坏文件不致命 */
                }
                free(buf);                              /* 读缓冲释放 */
            }
        }
        fclose(f);                                      /* 关文件 */
    } else {
        ESP_LOGI(TAG, "首次开机，空记忆库起步");         /* 无历史文件 */
    }
    if (s_mem.next_id == 0) {                           /* 空库/坏文件时 */
        s_mem.next_id = 1;                              /* ID 从 1 起 */
    }
    s_mem.inited = true;                                /* 置就绪 */
    return ESP_OK;                                      /* 初始化完成 */
}

int memory_store_count(void)
{
    return s_mem.inited ? s_mem.count : 0;              /* 未初始化按 0 */
}

/** 把 RAM 全量条目序列化写盘（调用方须持锁） */
static esp_err_t save_locked(void)
{
    cJSON *root = cJSON_CreateObject();                 /* 根对象 */
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "no mem json");
    cJSON_AddNumberToObject(root, "version", 1);        /* 格式版本 */
    cJSON_AddNumberToObject(root, "next_id", (double)s_mem.next_id);    /* ID 游标 */
    cJSON *arr = cJSON_CreateArray();                   /* 条目数组 */
    cJSON_AddItemToObject(root, "entries", arr);        /* 挂到根 */
    for (int i = 0; i < s_mem.count; i++) {             /* 逐条序列化 */
        memory_entry_t *e = &s_mem.entries[i];          /* 当前条目 */
        cJSON *o = cJSON_CreateObject();                /* 条目对象 */
        cJSON_AddNumberToObject(o, "id", (double)e->id);        /* ID */
        cJSON_AddNumberToObject(o, "type", (double)e->type);    /* 类型 */
        cJSON_AddStringToObject(o, "content", e->content);      /* 内容 */
        cJSON_AddNumberToObject(o, "importance", e->importance);        /* 重要性 */
        cJSON_AddNumberToObject(o, "created_at", (double)e->created_at);        /* 时间戳 */
        cJSON_AddItemToArray(arr, o);                   /* 入数组 */
    }
    char *txt = cJSON_PrintUnformatted(root);           /* 紧凑序列化 */
    cJSON_Delete(root);                                 /* JSON 树用完即释 */
    ESP_RETURN_ON_FALSE(txt, ESP_ERR_NO_MEM, TAG, "print failed");

    /* 原子写：先写临时文件再 rename（SPIFFS 支持 rename；掉电最多丢本次新增） */
    const char *tmp = MEM_PATH ".tmp";                  /* 临时文件路径 */
    FILE *f = fopen(tmp, "w");                          /* 覆盖写临时文件 */
    if (!f) {                                           /* 打不开（SPiffs 满等） */
        cJSON_free(txt);                                /* 释放序列化串 */
        ESP_LOGE(TAG, "写盘失败: %s", tmp);             /* 报错 */
        return ESP_FAIL;                                /* 返回失败 */
    }
    size_t len = strlen(txt);                           /* 待写长度 */
    size_t wn = fwrite(txt, 1, len, f);                 /* 整写 */
    fclose(f);                                          /* 关闭（刷缓冲） */
    cJSON_free(txt);                                    /* 序列化串释放 */
    if (wn != len) {                                    /* 写半截 */
        remove(tmp);                                    /* 清掉残片 */
        ESP_LOGE(TAG, "写盘不完整 %u/%u", (unsigned)wn, (unsigned)len);
        return ESP_FAIL;                                /* 返回失败 */
    }
    remove(MEM_PATH);                                   /* 删旧文件（rename 前清位） */
    if (rename(tmp, MEM_PATH) != 0) {                   /* 原子换名 */
        ESP_LOGE(TAG, "rename 失败");                   /* 极端情况：旧已删新未名 */
        return ESP_FAIL;                                /* 返回失败 */
    }
    return ESP_OK;                                      /* 落盘成功 */
}

esp_err_t memory_store_save(void)
{
    if (!s_mem.inited) {                                /* 未初始化 */
        return ESP_ERR_INVALID_STATE;                   /* 拒绝 */
    }
    if (xSemaphoreTake(s_mem.lock, pdMS_TO_TICKS(3000)) != pdTRUE) {    /* 抢锁 */
        return ESP_ERR_TIMEOUT;                         /* 超时放弃 */
    }
    esp_err_t err = save_locked();                      /* 持锁落盘 */
    xSemaphoreGive(s_mem.lock);                         /* 还锁 */
    return err;                                         /* 返回结果 */
}

esp_err_t memory_store_add(memory_type_t type, const char *content,
                           int importance, uint32_t *out_id)
{
    ESP_RETURN_ON_FALSE(s_mem.inited && content && content[0],
                        ESP_ERR_INVALID_STATE, TAG, "bad arg");
    if (importance < 1) importance = 1;                 /* 重要性下界收敛 */
    if (importance > 10) importance = 10;               /* 重要性上界收敛 */

    esp_err_t err = ESP_OK;                             /* 返回值预备 */
    if (xSemaphoreTake(s_mem.lock, pdMS_TO_TICKS(3000)) != pdTRUE) {    /* 抢锁 */
        return ESP_ERR_TIMEOUT;                         /* 超时放弃 */
    }
    int dup = find_duplicate(content);                  /* 查重 */
    if (dup >= 0) {                                     /* 已有同样内容 */
        if (out_id) *out_id = s_mem.entries[dup].id;    /* 返回旧条目 ID */
        if (importance > s_mem.entries[dup].importance) {       /* 新的重要级更高 */
            s_mem.entries[dup].importance = importance; /* 就地提升 */
            err = save_locked();                        /* 变更落盘 */
        }
        xSemaphoreGive(s_mem.lock);                     /* 还锁 */
        return err;                                     /* 幂等成功 */
    }
    if (s_mem.count >= MEM_MAX_ENTRIES) {               /* 库满 */
        xSemaphoreGive(s_mem.lock);                     /* 还锁 */
        ESP_LOGW(TAG, "记忆库已满（%d 条），拒绝新增", s_mem.count);
        return ESP_ERR_NO_MEM;                          /* 拒绝新增 */
    }
    memory_entry_t *e = &s_mem.entries[s_mem.count];    /* 取空槽 */
    memset(e, 0, sizeof(*e));                           /* 清槽 */
    e->id = s_mem.next_id++;                            /* 分配 ID 并前进游标 */
    e->type = type;                                     /* 分类 */
    strlcpy(e->content, content, MEM_CONTENT_MAX);      /* 内容（超长截断） */
    e->importance = importance;                         /* 重要性 */
    e->created_at = now_sec();                          /* 时间戳 */
    s_mem.count++;                                      /* 条数 +1 */
    if (out_id) {                                       /* 调用方要 ID 才回填 */
        *out_id = e->id;                                /* 回填新条目 ID（曾漏此步 → 串口回执恒 #0） */
    }
    err = save_locked();                                /* 即刻落盘 */
    xSemaphoreGive(s_mem.lock);                         /* 还锁 */
    ESP_LOGI(TAG, "记忆入库 #%u [%s] %s",
             (unsigned)e->id, memory_type_name(type), e->content);
    return err;                                         /* 返回结果 */
}

esp_err_t memory_store_delete(uint32_t id)
{
    if (!s_mem.inited) {                                /* 未初始化 */
        return ESP_ERR_INVALID_STATE;                   /* 拒绝 */
    }
    if (xSemaphoreTake(s_mem.lock, pdMS_TO_TICKS(3000)) != pdTRUE) {    /* 抢锁 */
        return ESP_ERR_TIMEOUT;                         /* 超时放弃 */
    }
    int idx = find_index_by_id(id);                     /* 定位条目 */
    if (idx < 0) {                                      /* 无此 ID */
        xSemaphoreGive(s_mem.lock);                     /* 还锁 */
        return ESP_ERR_NOT_FOUND;                       /* 报不存在 */
    }
    /* 删除策略：末条补位（顺序无关紧要，搜索时才排序） */
    s_mem.entries[idx] = s_mem.entries[s_mem.count - 1];        /* 末条搬过来 */
    s_mem.count--;                                      /* 条数 -1 */
    esp_err_t err = save_locked();                      /* 落盘 */
    xSemaphoreGive(s_mem.lock);                         /* 还锁 */
    ESP_LOGI(TAG, "记忆删除 #%u", (unsigned)id);        /* 日志 */
    return err;                                         /* 返回结果 */
}

esp_err_t memory_store_clear(void)
{
    if (!s_mem.inited) {                                /* 未初始化 */
        return ESP_ERR_INVALID_STATE;                   /* 拒绝 */
    }
    if (xSemaphoreTake(s_mem.lock, pdMS_TO_TICKS(3000)) != pdTRUE) {    /* 抢锁 */
        return ESP_ERR_TIMEOUT;                         /* 超时放弃 */
    }
    int n = s_mem.count;                                /* 记下清掉多少（日志用） */
    s_mem.count = 0;                                    /* 条目数清零（数组不必擦） */
    s_mem.next_id = 1;                                  /* ID 游标归位 */
    esp_err_t err = save_locked();                      /* 空库落盘 */
    xSemaphoreGive(s_mem.lock);                         /* 还锁 */
    ESP_LOGI(TAG, "记忆清空（%d 条）", n);              /* 日志 */
    return err;                                         /* 返回结果 */
}

/** 搜索结果排序比较器：重要性降序 */
static int cmp_by_importance(const void *a, const void *b)
{
    const memory_entry_t *ea = a, *eb = b;              /* 强转条目指针 */
    return eb->importance - ea->importance;             /* 降序 */
}

int memory_store_search(const char *query, memory_entry_t *out, int max)
{
    if (!s_mem.inited || !out || max <= 0) {            /* 参数防御 */
        return 0;                                       /* 无结果 */
    }
    bool has_query = query && query[0];                 /* 是否给了关键词 */
    int n = 0;                                          /* 命中数 */
    if (xSemaphoreTake(s_mem.lock, pdMS_TO_TICKS(3000)) != pdTRUE) {    /* 抢锁 */
        return 0;                                       /* 超时按无结果 */
    }
    for (int i = 0; i < s_mem.count && n < max; i++) {  /* 线性扫描 */
        if (!has_query || strstr(s_mem.entries[i].content, query)) {    /* 空词全收 */
            out[n++] = s_mem.entries[i];                /* 拷贝出条目（持锁内快照） */
        }
    }
    xSemaphoreGive(s_mem.lock);                         /* 还锁 */
    qsort(out, n, sizeof(memory_entry_t), cmp_by_importance);   /* 命中集重要性降序 */
    return n;                                           /* 返回条数 */
}
