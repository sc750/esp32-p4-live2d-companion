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

#include "memory_store.h"

#define TAG "chat_con"

#define LINE_MAX    (512)
#define TASK_STACK  (8 * 1024)      /* 行缓冲 + 下游对话调用栈余量 */

static chat_line_cb_t s_cb;
static void *s_ctx;

/** 串口输出一行（统一出口，省得每处写 usb_serial_jtag_write_bytes） */
static void say(const char *s)
{
    usb_serial_jtag_write_bytes(s, strlen(s), portMAX_DELAY);       /* 阻塞写全 */
}

/** "mem" 命令族：list / add <内容> / del <id> / clear */
static void exec_mem_cmd(char *rest)
{
    char what[16] = {0};                                /* 子命令缓冲 */
    rest += strspn(rest, " ");                          /* 剥前导空格 */
    int n = sscanf(rest, "%15s", what);                 /* 取子命令词 */
    if (n != 1) {                                       /* 裸 "mem"：帮助 */
        say("用法: mem list | mem add <内容> | mem del <id> | mem clear\r\n");
        return;                                         /* 结束 */
    }
    if (strcmp(what, "list") == 0) {                    /* 列出全部 */
        memory_entry_t out[32];                         /* 单页最多 32 条 */
        int cnt = memory_store_search(NULL, out, 32);   /* 空 query = 重要性降序 */
        char line[400];                                 /* 行拼装缓冲 */
        snprintf(line, sizeof(line), "共 %d 条记忆:\r\n", memory_store_count());
        say(line);                                      /* 总数行 */
        for (int i = 0; i < cnt; i++) {                 /* 逐条打印 */
            snprintf(line, sizeof(line), "#%u [%s] %s\r\n",
                     (unsigned)out[i].id, memory_type_name(out[i].type),
                     out[i].content);                   /* id [类型] 内容 */
            say(line);                                  /* 输出 */
        }
        return;                                         /* 结束 */
    }
    if (strcmp(what, "add") == 0) {                     /* 手动添加 */
        char *body = rest + strlen(what);               /* 跳过子命令词 */
        body += strspn(body, " ");                      /* 剥空格 */
        if (!*body) {                                   /* 没内容 */
            say("用法: mem add <内容>\r\n");            /* 提示 */
            return;                                     /* 结束 */
        }
        uint32_t id = 0;                                /* 新条目 ID */
        memory_store_add(MEM_TYPE_FACT, body, 5, &id);  /* 事实类、重要性 5 */
        char line[64];                                  /* 回执行 */
        snprintf(line, sizeof(line), "已记住 (#%u)\r\n", (unsigned)id);
        say(line);                                      /* 回执 */
        return;                                         /* 结束 */
    }
    if (strcmp(what, "del") == 0) {                     /* 按 ID 删 */
        unsigned id = 0;                                /* 目标 ID */
        if (sscanf(rest + strlen(what), " %u", &id) != 1) {     /* 取 ID */
            say("用法: mem del <id>\r\n");              /* 没给 ID */
            return;                                     /* 结束 */
        }
        esp_err_t e = memory_store_delete(id);          /* 执行删除 */
        say(e == ESP_OK ? "已删除\r\n" : "无此 ID\r\n");        /* 回执 */
        return;                                         /* 结束 */
    }
    if (strcmp(what, "clear") == 0) {                   /* 清空 */
        memory_store_clear();                           /* 清库落盘 */
        say("记忆已清空\r\n");                          /* 回执 */
        return;                                         /* 结束 */
    }
    say("未知子命令。用法: mem list | mem add <内容> | mem del <id> | mem clear\r\n");
}

/** 一行到手（已剥行尾、非空） */
static void dispatch_line(char *line, size_t len)
{
    /* 命令路由："mem " 进记忆命令族（不进对话） */
    if (len >= 4 && strncmp(line, "mem ", 4) == 0) {
        exec_mem_cmd(line + 4);                         /* 交给 mem 处理器 */
        return;                                         /* 命令不说话 */
    }
    if (len == 3 && strncmp(line, "mem", 3) == 0) {     /* 裸 "mem" 也给帮助 */
        exec_mem_cmd("");                               /* 空参数触发用法 */
        return;                                         /* 结束 */
    }
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
