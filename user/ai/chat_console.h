/**
 * @file    chat_console.h
 * @brief   串口对话输入通道（M0 的"嘴替"）——USB-SJ 驱动直读
 *
 * M0 没有耳朵（ASR 在 M1），串口命令是 LLM 链路的标准验证通道；
 * M1 语音接入后此通道保留为调试口。
 *
 * 用法（串口终端，115200）：
 *   直接输入一句话回车       ← 触发一轮对话
 *   rec 5                    ← 录 5 秒并走完整语音管线（调试）
 *
 * @date    2026-09-06
 * @version 3.0.0  改为 usb_serial_jtag 驱动直读（fgets/esp_console 的
 *          UTF-8 过滤与私有事件循环两坑详见 chat_console.c 文件头）
 */

#ifndef CHAT_CONSOLE_H
#define CHAT_CONSOLE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 收到一行对话文本（已剥行尾与 "chat " 前缀） */
typedef void (*chat_line_cb_t)(const char *text, void *ctx);

/** 安装 USB-SJ 驱动并启动行读取任务 */
void chat_console_start(chat_line_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CHAT_CONSOLE_H */
