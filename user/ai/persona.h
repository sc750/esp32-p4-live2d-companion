/**
 * @file    persona.h
 * @brief   人设系统（L4）——/spiffs/data/persona.json 加载 + 缺省自动生成
 *
 * 人设文本外置到 SPIFFS：改文件即可调性格，不用重编固件。
 * 文件缺失（首次开机）时自动写入内置默认（三玖，联网考据版）。
 * dialog_manager（对话）与 diary_service（M09 日记）共用此模块。
 *
 * @date    2026-09-09
 * @version 1.0.0
 */

#ifndef PERSONA_H
#define PERSONA_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PERSONA_NAME_MAX    48      /* 角色名上限（bytes） */
#define PERSONA_BASE_MAX    2048    /* 基础人设文本上限（bytes，UTF-8） */
#define PERSONA_PATH        "/spiffs/data/persona.json"     /* 人设文件路径 */

/**
 * @brief 初始化：加载人设；文件缺失/损坏时写入默认并加载（幂等）
 */
esp_err_t persona_init(void);

/** 角色名（如"中野三玖"；init 后恒可用） */
const char *persona_name(void);

/** 基础人设文本（性格/说话规则；NUL 结尾，只读） */
const char *persona_base(void);

/**
 * @brief 覆写人设（串口调试/将来 UI 编辑用；立即落盘）
 * @param name  新角色名（NULL = 保持不变）
 * @param base  新基础文本（NULL = 保持不变；超长截断）
 */
esp_err_t persona_set(const char *name, const char *base);

#ifdef __cplusplus
}
#endif

#endif /* PERSONA_H */
