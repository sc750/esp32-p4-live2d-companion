/**
 * @file    rig_mem.c
 * @brief   rig 内存实现——全组件唯一接触 heap_caps 的文件（L1 平台点）
 *
 * @date    2026-09-04
 * @version 1.0.1  块头记 size，free 精确减账
 */

#include "rig_mem.h"

#include "esp_heap_caps.h"

typedef struct {
    size_t size;
} rig_mem_hdr_t;

static size_t s_used = 0;

void *rig_mem_alloc(size_t size)
{
    /* 大块纹理走 PSRAM；不够再退内部 RAM */
    rig_mem_hdr_t *h = heap_caps_malloc(size + sizeof(rig_mem_hdr_t), MALLOC_CAP_SPIRAM);
    if (!h) {
        h = heap_caps_malloc(size + sizeof(rig_mem_hdr_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    }
    if (!h) {
        return NULL;
    }
    h->size = size;
    s_used += size;
    return h + 1;
}

void rig_mem_free(void *p)
{
    if (!p) {
        return;
    }
    rig_mem_hdr_t *h = (rig_mem_hdr_t *)p - 1;
    s_used -= h->size;
    heap_caps_free(h);
}

size_t rig_mem_total_used(void)
{
    return s_used;
}
