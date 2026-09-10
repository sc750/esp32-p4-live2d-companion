/**
 * @file    diary_service.h
 * @brief   日记服务（L4）——当日素材收集 + LLM 第一人称生成 + SPIFFS 存储
 *
 * 触发：串口 diary now（手动）/ 每天 22:00 esp_timer 自动（SNTP 就绪才启动）。
 * 素材（MVP）：当日新增长期记忆条目 + 当日对话轮数；生成结果落
 * /spiffs/data/diary/YYYY-MM-DD.json，重启不丢。
 *
 * @date    2026-09-10
 * @version 1.0.0
 */

#ifndef DIARY_SERVICE_H
#define DIARY_SERVICE_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DIARY_DIR           "/spiffs/data/diary"    /* 日记目录 */
#define DIARY_CONTENT_MAX   2048                    /* 单篇日记正文上限（bytes） */
#define DIARY_DATE_MAX      11                      /* "YYYY-MM-DD" + NUL */

/** 单篇日记（读回用） */
typedef struct {
    char date[DIARY_DATE_MAX];              /* 日期 YYYY-MM-DD */
    char content[DIARY_CONTENT_MAX];        /* 日记正文 */
    int  rounds;                            /* 当日对话轮数（元数据） */
    uint32_t created_at;                    /* 生成时刻 Unix 秒 */
} diary_entry_t;

/**
 * @brief 初始化：建目录 + 启动 22:00 定时器（SNTP 未同步则定时器延后启动；幂等）
 */
esp_err_t diary_service_init(void);

/**
 * @brief 立即生成今日日记（同步阻塞数秒；已有今日日记则覆盖）
 * @return ESP_OK 成功；ESP_FAIL LLM/网络失败；ESP_ERR_INVALID_STATE 素材/状态异常
 */
esp_err_t diary_generate_today(void);

/**
 * @brief 读指定日期日记（date 形如 "2026-09-10"）
 * @return ESP_OK 找到并填充 out；ESP_ERR_NOT_FOUND 无此日
 */
esp_err_t diary_read(const char *date, diary_entry_t *out);

/**
 * @brief 列出已有日记日期（时间倒序）
 * @param dates  输出字符串数组（调用方提供，每项 DIARY_DATE_MAX）
 * @param max    数组容量
 * @return 实际条数
 */
int diary_list(char dates[][DIARY_DATE_MAX], int max);

/** 今日对话轮数记录（dialog 每轮 +1，日记素材用） */
void diary_note_round(void);

#ifdef __cplusplus
}
#endif

#endif /* DIARY_SERVICE_H */
