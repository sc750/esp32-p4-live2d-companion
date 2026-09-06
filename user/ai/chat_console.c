/**
 * @file    chat_console.c
 * @brief   串口对话输入实现——USB-SJ 驱动直读（UTF-8 全通）
 *
 * 教训三连（M0 实测，每个都烧了一轮）：
 *   1) IDF 5.x UART 控制台默认不装 RX 驱动，fgets(stdin) 读不到输入
 *   2) 用户线插的是内置 USB-Serial-JTAG（COM41, VID 303A:1001），
 *      UART0(GPIO38/37) 物理不接 PC——REPL 必须绑 USB-SJ
 *   3) esp_console 的 linenoise 在收行后用 sanitize()+isprint() 过滤，
 *      UTF-8 高位字节（signed char 负数）被判不可打印【全部剥掉】——
 *      "chat 你好呀" 到达命令层只剩 "chat "。esp_console 是 ASCII-only。
 *
 * 终极方案：自己装 usb_serial_jtag 驱动逐字节读，不经过任何过滤层。
 * 行结束 = \r 或 \n；收到的字节回显；UTF-8 原样透传。
 *
 * @date    2026-09-06
 * @version 3.0.0
 */

#include "chat_console.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "chat_con"

#define LINE_MAX    (512)
#define TASK_STACK  (8 * 1024)      /* 行缓冲 + 下游对话调用栈余量 */

static chat_line_cb_t s_cb;
static void *s_ctx;

/** 一行到手（已剥行尾、非空） */
static void dispatch_line(char *line, size_t len)
{
    /* 剥 "chat " 前缀（可剥可不剥，少打字） */
    if (len >= 5 && strncmp(line, "chat ", 5) == 0) {
        memmove(line, line + 5, len - 5);
        len -= 5;
    }
    if (len == 0 || s_cb == NULL) {
        return;
    }
    s_cb(line, s_ctx);
}

static void console_task(void *arg)
{
    char line[LINE_MAX];
    size_t len = 0;
    ESP_LOGI(TAG, "串口对话通道就绪：直接输入一句话（或 'chat <话>'），回车发送");

    while (1) {
        uint8_t c;
        int r = usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY);
        if (r <= 0) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (len == 0) {
                continue;               /* 空行忽略（\r\n 会来两个） */
            }
            line[len] = '\0';
            usb_serial_jtag_write_bytes("\r\n", 2, portMAX_DELAY);
            dispatch_line(line, len);
            len = 0;
            continue;
        }
        /* 回显 + 入缓冲（退格 0x7F/0x08 简单处理） */
        if (c == 0x7F || c == 0x08) {
            if (len > 0) {
                len--;
                usb_serial_jtag_write_bytes("\b \b", 3, portMAX_DELAY);
            }
            continue;
        }
        usb_serial_jtag_write_bytes(&c, 1, portMAX_DELAY);
        if (len < LINE_MAX - 1) {
            line[len++] = (char)c;
        }
    }
}

void chat_console_start(chat_line_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_ctx = ctx;

    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = 1024,
        .tx_buffer_size = 1024,
    };
    /* 控制台子系统可能已装过驱动（ESP_ERR_INVALID_STATE）——复用即可 */
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "USB-SJ 驱动已安装");
    } else if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "USB-SJ 驱动已由控制台安装，直接复用");
    } else {
        ESP_LOGE(TAG, "USB-SJ 驱动安装失败: %s", esp_err_to_name(err));
        return;
    }
    if (xTaskCreate(console_task, "chat_con", TASK_STACK, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建串口任务失败");
    }
}
