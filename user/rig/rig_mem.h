/**
 * @file    rig_mem.h
 * @brief   rig 内存接口——L4 唯一允许的分配入口（隔离平台分配器）
 *
 * @date    2026-09-04
 * @version 1.0.0
 */

#ifndef RIG_MEM_H
#define RIG_MEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void  *rig_mem_alloc(size_t size);          /* PSRAM 优先，失败返回 NULL */
void   rig_mem_free(void *p);
size_t rig_mem_total_used(void);            /* 当前 rig 占用字节数 */

#ifdef __cplusplus
}
#endif

#endif /* RIG_MEM_H */
