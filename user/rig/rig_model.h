/**
 * @file    rig_model.h
 * @brief   rigbin 模型加载接口（L4）
 *
 * 资源分发：rigbin 以 EMBED_FILES 嵌入固件（assets/character_01.rigbin），
 * 加载时把 atlas 拷入 PSRAM（渲染期 250MHz 高速读取），flash 仅作载体。
 *
 * @date    2026-09-04
 * @version 1.0.0
 */

#ifndef RIG_MODEL_H
#define RIG_MODEL_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "rig_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 从内存 blob 加载（校验 magic/长度/字段，atlas 拷入 PSRAM）
 * @param data rigbin 数据（flash 映射或 RAM 均可，只读）
 */
esp_err_t rig_model_load_mem(rig_model_t *m, const uint8_t *data, size_t len);

/**
 * @brief 加载默认嵌入角色（_binary_character_01_rigbin_start）
 */
esp_err_t rig_model_load_default(rig_model_t *m);

/** @brief 卸载（释放 PSRAM，清零结构） */
void rig_model_unload(rig_model_t *m);

/** @brief 按名查找图层，未找到返回 NULL */
const rig_layer_t *rig_model_find_layer(const rig_model_t *m, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* RIG_MODEL_H */
