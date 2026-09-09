/**
 * @file    memory_store.h
 * @brief   长期记忆存储（L4）——条目 CRUD + 关键词搜索 + SPIFFS 持久化
 *
 * 三层记忆模型的"长期记忆"层：RAM 常驻（PSRAM 数组，微秒级检索）+
 * 整文件 JSON 落盘 /spiffs/data/memories.json（add/delete 后即存）。
 * 摘要提取（memory_extract）与 system prompt 注入（dialog_manager）是它的两个客户。
 *
 * @date    2026-09-09
 * @version 1.0.0
 */

#ifndef MEMORY_STORE_H
#define MEMORY_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 容量参数（PSRAM 富余，条目上限保守取 200；SPIFFS 4MB 整文件写入无压力） ---- */
#define MEM_CONTENT_MAX     256     /* 单条记忆内容上限（bytes，UTF-8，约 85 个汉字） */
#define MEM_MAX_ENTRIES     200     /* 长期记忆条目上限（挤满后拒绝新增，防 SPIFFS 膨胀） */
#define MEM_PATH            "/spiffs/data/memories.json"    /* 持久化文件路径 */

/** 记忆分类（与 PRD M08 对齐；提取端 LLM 输出 type 字符串映射到此） */
typedef enum {
    MEM_TYPE_PROFILE = 0,           /* 用户画像：职业/年龄/偏好等 */
    MEM_TYPE_FACT,                  /* 事实：用户告知的具体信息 */
    MEM_TYPE_EVENT,                 /* 事件：发生过的重要事情 */
    MEM_TYPE_EMOTION,               /* 情感：情绪状态记录 */
    MEM_TYPE_HABIT,                 /* 习惯：日常作息模式 */
} memory_type_t;

/** 单条长期记忆 */
typedef struct {
    uint32_t    id;                                     /* 自增唯一 ID（从 1 起） */
    memory_type_t type;                                 /* 分类 */
    char        content[MEM_CONTENT_MAX];               /* 内容（UTF-8，NUL 结尾） */
    int         importance;                             /* 重要性 1~10（搜索排序用） */
    uint32_t    created_at;                             /* 创建时刻（Unix 秒） */
} memory_entry_t;

/** 记忆类型 → 中文名（日志/串口显示用） */
const char *memory_type_name(memory_type_t type);

/**
 * @brief 初始化：加载 SPIFFS 已有记忆到 RAM（文件不存在则空库起步；幂等）
 */
esp_err_t memory_store_init(void);

/** 当前条目数 */
int memory_store_count(void);

/**
 * @brief 新增一条记忆（自动去重：与现有条目 content 相同则跳过返回其 id）
 *
 * @param type        分类
 * @param content     内容（超长截断到 MEM_CONTENT_MAX-1）
 * @param importance  重要性 1~10（越界收敛到边界）
 * @param out_id      输出新条目 ID（可为 NULL）
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 条目已满；ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t memory_store_add(memory_type_t type, const char *content,
                           int importance, uint32_t *out_id);

/** 按 ID 删除一条（ESP_ERR_NOT_FOUND = 无此 ID） */
esp_err_t memory_store_delete(uint32_t id);

/**
 * @brief 关键词搜索（子串匹配；空/NULL query 返回按重要性降序的前 max 条）
 *
 * @param query   关键词（UTF-8 子串）
 * @param out     结果数组（调用方提供）
 * @param max     数组容量
 * @return 实际命中条数
 */
int memory_store_search(const char *query, memory_entry_t *out, int max);

/** 立即落盘（add/delete 内部已自动调用；供定时兜底/串口调试用） */
esp_err_t memory_store_save(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_STORE_H */
