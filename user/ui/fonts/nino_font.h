/**
 * @file    nino_font.h
 * @brief   三玖桌面助手自定义 CJK 字体（16px 思源风黑体，tools/gen_nino_font.py 生成）
 *
 * 为什么不用 LVGL 内置 source_han_sans_sc_16_cjk：
 *   内置字库实测缺 监/听/说/开/屏/玖 等 20+ 常用字（覆盖率表过时），
 *   AI 字幕又是动态文本 → 用 tools/gen_nino_font.py 从 Windows 黑体
 *   生成精确覆盖的自定义字库（132 汉字 + ASCII，约 100KB flash）。
 * 加字：往 gen_nino_font.py 的 CHARSET 加字符重跑即可。
 *
 * @date    2026-09-05
 * @version 1.0.0
 */

#ifndef NINO_FONT_H
#define NINO_FONT_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 自定义中文字体（16px，含 ASCII + 132 常用汉字 + 全角标点） */
LV_FONT_DECLARE(nino_cjk_16);

/** 取中文字体指针（语义化封装，方便以后换字号只改这里） */
static inline const lv_font_t *nino_font_cjk16(void)
{
    return &nino_cjk_16;
}

#ifdef __cplusplus
}
#endif

#endif /* NINO_FONT_H */
