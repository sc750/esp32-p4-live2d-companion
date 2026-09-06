/**
 * @file    chat_console.h
 * @brief   串口对话输入通道（M0 的"嘴替"）——esp_console REPL
 *
 * M0 没有耳朵（ASR 在 M1），串口命令是 LLM 链路的标准验证通道；
 * M1 语音接入后此通道保留为调试口。
 *
 * 用法（串口终端，115200）：
 *   chat 你好呀              ← 触发一轮对话（多词自动拼回一句话）
 *   help                     ← 列出命令
 *
 * @date    2026-09-06
 * @version 2.0.0  改用 esp_console REPL（fgets 读不到 UART 输入的教训：
 *          IDF 5.x UART 控制台默认不装 RX 驱动，必须走 esp_console）
 */

#ifndef CHAT_CONSOLE_H
#define CHAT_CONSOLE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 收到一条对话文本（"chat" 后的完整内容，词间以单空格拼接） */
typedef void (*chat_line_cb_t)(const char *text, void *ctx);

/** 启动 REPL（安装 UART 驱动 + 注册 chat 命令 + 起任务） */
void chat_console_start(chat_line_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CHAT_CONSOLE_H */
