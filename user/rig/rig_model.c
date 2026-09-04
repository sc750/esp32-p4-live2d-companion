/**
 * @file    rig_model.c
 * @brief   rigbin 加载实现（L4）——字段级校验 + atlas 拷贝到 PSRAM
 *
 * 解析对应 tools/rigpack.py 的 rigbin v1：
 *   32B 头（magic "RIG1"/ver/画布/层数） + 36B×N 图层表 + atlas 尺寸 + RGBA
 *
 * @date    2026-09-04
 * @version 1.0.0
 */

#include "rig_model.h"

#include <string.h>

#include "esp_log.h"
#include "esp_check.h"

#include "rig_mem.h"

#define TAG "rig"

/* 头部字段偏移（rigbin v1，小端）：4s + 7×u16 = 18B，两个 u32 在 18/22 */
#define HDR_OFF_MAGIC      0    /* 4B */
#define HDR_OFF_VER        4    /* u16 */
#define HDR_OFF_CANVAS_W   8    /* u16 */
#define HDR_OFF_CANVAS_H   10
#define HDR_OFF_LAYERS     12   /* u16 */
#define HDR_OFF_TOTAL      22   /* u32（reserved u32 在 18） */
#define HDR_SIZE           32
#define LAYER_ENTRY_SIZE   36

/* EMBED_FILES 注入的符号（user/CMakeLists.txt） */
extern const uint8_t rigbin_start[] asm("_binary_character_01_rigbin_start");
extern const uint8_t rigbin_end[]   asm("_binary_character_01_rigbin_end");

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int16_t rd_i16(const uint8_t *p)
{
    return (int16_t)rd_u16(p);
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

esp_err_t rig_model_load_mem(rig_model_t *m, const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(m && data && len > HDR_SIZE, ESP_ERR_INVALID_ARG, TAG, "bad arg");
    ESP_RETURN_ON_FALSE(!m->loaded, ESP_ERR_INVALID_STATE, TAG, "already loaded");

    memset(m, 0, sizeof(*m));
    m->idx_head = -1;

    /* ---- 头部校验 ---- */
    if (memcmp(data + HDR_OFF_MAGIC, RIG_MAGIC, 4) != 0) {
        ESP_LOGE(TAG, "magic 错误（非 RIG1）");
        return ESP_ERR_INVALID_CRC;
    }
    uint16_t ver = rd_u16(data + HDR_OFF_VER);
    ESP_RETURN_ON_FALSE(ver == RIG_VERSION, ESP_ERR_NOT_SUPPORTED, TAG, "版本不支持 v%u", ver);
    uint32_t total = rd_u32(data + HDR_OFF_TOTAL);
    ESP_RETURN_ON_FALSE(total == len, ESP_ERR_INVALID_SIZE, TAG,
                        "长度不符: 头记 %u vs 实际 %u", (unsigned)total, (unsigned)len);

    m->canvas_w = rd_u16(data + HDR_OFF_CANVAS_W);
    m->canvas_h = rd_u16(data + HDR_OFF_CANVAS_H);
    m->layer_count = rd_u16(data + HDR_OFF_LAYERS);
    ESP_RETURN_ON_FALSE(m->layer_count >= 1 && m->layer_count <= RIG_MAX_LAYERS,
                        ESP_ERR_INVALID_SIZE, TAG, "层数越界 %u", m->layer_count);

    /* ---- 图层表 ---- */
    const uint8_t *ly = data + HDR_SIZE;
    for (int i = 0; i < m->layer_count; i++, ly += LAYER_ENTRY_SIZE) {
        rig_layer_t *L = &m->layers[i];
        memcpy(L->name, ly, RIG_LAYER_NAME_LEN);
        L->name[RIG_LAYER_NAME_LEN] = '\0';
        L->parent   = rd_i16(ly + 12);
        L->atlas_x  = rd_u16(ly + 14);
        L->atlas_y  = rd_u16(ly + 16);
        L->atlas_w  = rd_u16(ly + 18);
        L->atlas_h  = rd_u16(ly + 20);
        L->base_x   = rd_i16(ly + 22);
        L->base_y   = rd_i16(ly + 24);
        L->z        = rd_i16(ly + 26);
        L->flags    = rd_u16(ly + 28);

        if (L->parent >= m->layer_count) {
            ESP_LOGE(TAG, "layer[%d] parent 越界", i);
            return ESP_ERR_INVALID_CRC;
        }
        if (strcmp(L->name, "head") == 0) {
            m->idx_head = i;
        }
    }

    /* ---- atlas ---- */
    const uint8_t *ap = data + HDR_SIZE + LAYER_ENTRY_SIZE * m->layer_count;
    m->atlas_w = rd_u16(ap);
    m->atlas_h = rd_u16(ap + 2);
    size_t atlas_bytes = (size_t)m->atlas_w * m->atlas_h * 4;
    ESP_RETURN_ON_FALSE(HDR_SIZE + LAYER_ENTRY_SIZE * m->layer_count + 4 + atlas_bytes == len,
                        ESP_ERR_INVALID_SIZE, TAG, "atlas 尺寸与文件不符");

    m->atlas = rig_mem_alloc(atlas_bytes);
    if (!m->atlas) {
        ESP_LOGE(TAG, "atlas 分配失败 (%u 字节)", (unsigned)atlas_bytes);
        return ESP_ERR_NO_MEM;
    }
    memcpy(m->atlas, ap + 4, atlas_bytes);

    m->loaded = true;
    m->total_bytes = atlas_bytes;
    ESP_LOGI(TAG, "模型加载 OK: canvas=%ux%u layers=%u atlas=%ux%u (%uKB PSRAM)",
             m->canvas_w, m->canvas_h, m->layer_count,
             m->atlas_w, m->atlas_h, (unsigned)(atlas_bytes / 1024));
    for (int i = 0; i < m->layer_count; i++) {
        const rig_layer_t *L = &m->layers[i];
        ESP_LOGI(TAG, "  [%d] %-12s parent=%d atlas=(%u,%u %ux%u) base=(%d,%d) z=%d%s",
                 i, L->name, L->parent, L->atlas_x, L->atlas_y,
                 L->atlas_w, L->atlas_h, L->base_x, L->base_y, L->z,
                 (L->flags & RIG_LAYER_PHYSICS) ? " [physics]" : "");
    }
    return ESP_OK;
}

esp_err_t rig_model_load_default(rig_model_t *m)
{
    return rig_model_load_mem(m, rigbin_start, (size_t)(rigbin_end - rigbin_start));
}

void rig_model_unload(rig_model_t *m)
{
    if (!m || !m->loaded) {
        return;
    }
    rig_mem_free(m->atlas);
    memset(m, 0, sizeof(*m));
    m->idx_head = -1;
}

const rig_layer_t *rig_model_find_layer(const rig_model_t *m, const char *name)
{
    if (!m || !m->loaded) {
        return NULL;
    }
    for (int i = 0; i < m->layer_count; i++) {
        if (strcmp(m->layers[i].name, name) == 0) {
            return &m->layers[i];
        }
    }
    return NULL;
}
