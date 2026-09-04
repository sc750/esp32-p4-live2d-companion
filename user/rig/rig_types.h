/**
 * @file    rig_types.h
 * @brief   类 Live2D rig 数据类型（与 tools/rigpack.py 的 rigbin v1 格式对应）
 *
 * rigbin v1（小端）: 32B 头 + 36B×图层表 + atlas 尺寸 + RGBA8888 像素。
 * 本文件只放纯数据类型，不含任何平台头（L4 纯净）。
 *
 * @date    2026-09-04
 * @version 1.0.0
 */

#ifndef RIG_TYPES_H
#define RIG_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RIG_MAGIC           "RIG1"
#define RIG_VERSION         1
#define RIG_LAYER_NAME_LEN  12
#define RIG_MAX_LAYERS      16

/* 图层 flags */
#define RIG_LAYER_PHYSICS   0x0001  /* Verlet 物理链（呆毛等） */

/** 图层描述（对应 rigbin 36B 条目，字段已转为宿主序） */
typedef struct {
    char     name[RIG_LAYER_NAME_LEN + 1];  /* \0 结尾 */
    int16_t  parent;                        /* 父层索引，-1=根 */
    uint16_t atlas_x, atlas_y;              /* 图集内位置 */
    uint16_t atlas_w, atlas_h;              /* 图集内尺寸 */
    int16_t  base_x, base_y;                /* 画布基准位置（层原点） */
    int16_t  z;                             /* 绘制序，大者在前 */
    uint16_t flags;
} rig_layer_t;

/** 每帧姿态：逐层偏移 + 补丁层可见性（渲染/动画共享） */
typedef struct {
    int16_t dx[RIG_MAX_LAYERS];
    int16_t dy[RIG_MAX_LAYERS];
    uint8_t visible[RIG_MAX_LAYERS];    /* 0=隐藏，1=显示 */
} rig_pose_t;

/** 加载完成的模型（atlas 在 PSRAM） */
typedef struct {
    uint16_t    canvas_w, canvas_h;
    uint16_t    layer_count;
    uint16_t    atlas_w, atlas_h;
    uint8_t    *atlas;                      /* RGBA8888，PSRAM */
    rig_layer_t layers[RIG_MAX_LAYERS];
    int         idx_head;                   /* "head" 层索引，-1=无 */
    bool        loaded;
    size_t      total_bytes;                /* 占用内存（atlas+结构） */
} rig_model_t;

#ifdef __cplusplus
}
#endif

#endif /* RIG_TYPES_H */
