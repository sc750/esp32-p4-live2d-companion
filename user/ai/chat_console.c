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
#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "memory_store.h"
#include "diary_service.h"
#include "music_service.h"

#define TAG "chat_con"

#define LINE_MAX    (512)
/* 任务栈：行缓冲 + 下游对话调用 + diary 命令的 diary_entry_t(≈2KB) 余量。
 * MVP 教训：此栈上曾放 8.7KB 快照数组 → 栈溢出 panic，故一律堆分配 + 留足栈。 */
#define TASK_STACK  (12 * 1024)

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
        /* 快照数组走 PSRAM 堆（200×272B≈54KB）——绝不能放栈上：
         * 曾用 memory_entry_t out[32] 放栈（≈8.7KB）直接压爆 8KB 任务栈，
         * 表现为执行 mem list 即 Guru Meditation Stack protection fault。 */
        static memory_entry_t *snap = NULL;             /* 惰性分配的快照区 */
        if (!snap) {                                    /* 首次使用才分配 */
            snap = heap_caps_malloc(MEM_MAX_ENTRIES * sizeof(memory_entry_t),
                                    MALLOC_CAP_SPIRAM);
            if (!snap) {                                /* 分配失败 */
                say("内存不足，无法列出\r\n");          /* 提示 */
                return;                                 /* 结束 */
            }
        }
        int cnt = memory_store_search(NULL, snap, MEM_MAX_ENTRIES);     /* 空 query = 重要性降序 */
        char line[400];                                 /* 行拼装缓冲 */
        snprintf(line, sizeof(line), "共 %d 条记忆:\r\n", memory_store_count());
        say(line);                                      /* 总数行 */
        for (int i = 0; i < cnt; i++) {                 /* 逐条打印 */
            snprintf(line, sizeof(line), "#%u [%s] %s\r\n",
                     (unsigned)snap[i].id, memory_type_name(snap[i].type),
                     snap[i].content);                  /* id [类型] 内容 */
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

/** "diary" 命令族：now / list / read [date]（生成阻塞数秒，串口上下文可接受） */
static void exec_diary_cmd(char *rest)
{
    char what[16] = {0};                                /* 子命令缓冲 */
    rest += strspn(rest, " ");                          /* 剥前导空格 */
    if (sscanf(rest, "%15s", what) != 1) {              /* 裸 "diary"：帮助 */
        say("用法: diary now | diary list | diary read [今天|YYYY-MM-DD]\r\n");
        return;                                         /* 结束 */
    }
    if (strcmp(what, "now") == 0) {                     /* 立即生成 */
        say("正在生成今日日记（数秒）……\r\n");          /* 提示耗时 */
        esp_err_t e = diary_generate_today();           /* 同步生成 */
        if (e == ESP_OK) {                              /* 成功 */
            static diary_entry_t d;                     /* 读回正文展示（static：2KB 不进栈） */
            /* 生成完读今日：直接再查一次（日期在服务内部） */
            char dates[1][DIARY_DATE_MAX];              /* 列表容器 */
            if (diary_list(dates, 1) > 0 &&             /* 索引头即最新 */
                diary_read(dates[0], &d) == ESP_OK) {   /* 读最新一篇 */
                say("──── 今日日记 ────\r\n");          /* 分隔头 */
                say(d.content);                         /* 正文 */
                say("\r\n────────────────\r\n");        /* 分隔尾 */
            } else {
                say("生成成功但读回失败\r\n");          /* 罕见 */
            }
        } else {                                        /* 失败 */
            say("生成失败（网络/时钟未同步/无素材）\r\n");      /* 提示 */
        }
        return;                                         /* 结束 */
    }
    if (strcmp(what, "list") == 0) {                    /* 列日期 */
        char dates[16][DIARY_DATE_MAX];                 /* 最多列 16 条 */
        int n = diary_list(dates, 16);                  /* 读索引 */
        char line[128];                                 /* 行缓冲（GCC 对未定长参数保守，给大防 truncation 告警） */
        snprintf(line, sizeof(line), "共 %d 篇日记:\r\n", n);
        say(line);                                      /* 总数行 */
        for (int i = 0; i < n; i++) {                   /* 逐条 */
            say("  ");                                  /* 缩进 */
            say(dates[i]);                              /* 日期（直写，绕开 %s 截断告警） */
            say("\r\n");                                /* 换行 */
        }
        return;                                         /* 结束 */
    }
    if (strcmp(what, "read") == 0) {                    /* 读某天 */
        char date[DIARY_DATE_MAX] = {0};                /* 目标日期 */
        char word[20] = {0};                            /* 参数词 */
        if (sscanf(rest + strlen(what), " %19s", word) == 1 &&  /* 有参数 */
            sscanf(word, "%10s", date) == 1 && strlen(date) == 10) {    /* 形如日期 */
            /* 传入 YYYY-MM-DD */
        } else {                                        /* 无参/别的不认：读最新 */
            char dates[1][DIARY_DATE_MAX];              /* 容器 */
            if (diary_list(dates, 1) <= 0) {            /* 没有任何日记 */
                say("还没有日记，先 diary now 生成一篇\r\n");
                return;                                 /* 结束 */
            }
            strlcpy(date, dates[0], DIARY_DATE_MAX);    /* 取最新 */
        }
        static diary_entry_t d;                         /* 条目容器（static：2KB 不进栈，控制台单线程安全） */
        if (diary_read(date, &d) == ESP_OK) {           /* 读到 */
            char line[128];                             /* 头行（给大防 truncation 告警） */
            snprintf(line, sizeof(line), "──── %s 日记 ────\r\n", d.date);
            say(line);                                  /* 头 */
            say(d.content);                             /* 正文 */
            say("\r\n────────────────\r\n");            /* 尾 */
        } else {
            say("该日期无日记\r\n");                    /* 无此日 */
        }
        return;                                         /* 结束 */
    }
    say("未知子命令。用法: diary now | diary list | diary read [YYYY-MM-DD]\r\n");
}

/** "music" 命令族：scan/list/play/pause/resume/stop/next/prev/vol/status */
static void exec_music_cmd(char *rest)
{
    char what[16] = {0};                                /* 子命令缓冲 */
    rest += strspn(rest, " ");                          /* 剥前导空格 */
    if (sscanf(rest, "%15s", what) != 1) {              /* 裸 "music"：帮助 */
        say("用法: music scan|list|play <n>|pause|resume|stop|next|prev|vol <0-100>|status\r\n");
        return;                                         /* 结束 */
    }
    if (strcmp(what, "scan") == 0) {                    /* 重扫 SD */
        char line[64];                                  /* 回执行 */
        snprintf(line, sizeof(line), "扫描到 %d 首\r\n", music_scan());
        say(line);                                      /* 输出数目 */
        return;                                         /* 结束 */
    }
    if (strcmp(what, "list") == 0) {                    /* 列曲目 */
        int n = music_count();                          /* 曲目数 */
        char line[64];                                  /* 行缓冲 */
        snprintf(line, sizeof(line), "共 %d 首:\r\n", n);
        say(line);                                      /* 总数行 */
        for (int i = 0; i < n; i++) {                   /* 逐条打印 */
            say("  ");                                  /* 缩进 */
            say(music_name_at(i));                      /* 曲名（见下） */
            say("\r\n");                                /* 换行 */
        }
        return;                                         /* 结束 */
    }
    if (strcmp(what, "play") == 0) {                    /* 播指定/第 0 首 */
        int idx = 0;                                    /* 默认第 0 首 */
        sscanf(rest + strlen(what), " %d", &idx);       /* 有参数就取 */
        esp_err_t e = music_play_index(idx);            /* 发播放命令 */
        say(e == ESP_OK ? "播放中\r\n" : "无此曲目（先 music scan）\r\n");
        return;                                         /* 结束 */
    }
    if (strcmp(what, "pause") == 0) {                   /* 暂停 */
        say(music_pause() == ESP_OK ? "已暂停\r\n" : "没在播放\r\n");
        return;                                         /* 结束 */
    }
    if (strcmp(what, "resume") == 0) {                  /* 恢复 */
        say(music_resume() == ESP_OK ? "继续播放\r\n" : "没有暂停中的歌\r\n");
        return;                                         /* 结束 */
    }
    if (strcmp(what, "stop") == 0) {                    /* 停止 */
        music_stop();                                   /* 停（内部等退出） */
        say("已停止\r\n");                              /* 回执 */
        return;                                         /* 结束 */
    }
    if (strcmp(what, "next") == 0) {                    /* 下一首 */
        say(music_next() == ESP_OK ? "切下一首\r\n" : "列表为空\r\n");
        return;                                         /* 结束 */
    }
    if (strcmp(what, "prev") == 0) {                    /* 上一首 */
        say(music_prev() == ESP_OK ? "切上一首\r\n" : "列表为空\r\n");
        return;                                         /* 结束 */
    }
    if (strcmp(what, "vol") == 0) {                     /* 音量 */
        int v = -1;                                     /* 目标音量 */
        sscanf(rest + strlen(what), " %d", &v);         /* 取参数 */
        if (v < 0) {                                    /* 没给参数 */
            say("用法: music vol <0-100>\r\n");         /* 提示 */
            return;                                     /* 结束 */
        }
        say(music_set_volume(v) == ESP_OK ? "音量已设\r\n" : "音量设失败\r\n");
        return;                                         /* 结束 */
    }
    if (strcmp(what, "status") == 0) {                  /* 状态 */
        const char *cur = music_current_name();         /* 当前曲名（无歌为 NULL） */
        char line[160];                                 /* 状态行 */
        if (cur && music_is_playing()) {                /* 正在播 */
            snprintf(line, sizeof(line), "播放中: %s | %ds\r\n",
                     cur, music_position_sec());        /* 曲名 + 已播秒数 */
        } else if (cur && music_is_paused()) {          /* 暂停态（2026-09-10 修） */
            /* 原先没有这一支：music_is_playing() 把暂停并入了 false，
             * 于是 pause 之后 status 会打"已停止"，与实际状态不符 */
            snprintf(line, sizeof(line), "已暂停: %s | %ds\r\n",
                     cur, music_position_sec());        /* 曲名 + 已播秒数 */
        } else if (cur) {                               /* 有目标但没在播 */
            snprintf(line, sizeof(line), "已停止（上次: %s）| 共 %d 首\r\n",
                     cur, music_count());               /* 提示上次曲目 */
        } else {                                        /* 从未播过 */
            snprintf(line, sizeof(line), "空闲（%d 首待播）\r\n", music_count());
        }
        say(line);                                      /* 输出 */
        return;                                         /* 结束 */
    }
    say("未知子命令\r\n");                              /* 兜底 */
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
    /* 命令路由："diary" 族（now/list/read） */
    if (len >= 6 && strncmp(line, "diary ", 6) == 0) {
        exec_diary_cmd(line + 6);                       /* 交给 diary 处理器 */
        return;                                         /* 命令不说话 */
    }
    if (len == 5 && strncmp(line, "diary", 5) == 0) {   /* 裸 "diary" 给帮助 */
        exec_diary_cmd("");                             /* 空参数触发用法 */
        return;                                         /* 结束 */
    }
    /* 命令路由："music" 族（scan/list/play/...） */
    if (len >= 6 && strncmp(line, "music ", 6) == 0) {
        exec_music_cmd(line + 6);                       /* 交给 music 处理器 */
        return;                                         /* 命令不说话 */
    }
    if (len == 5 && strncmp(line, "music", 5) == 0) {   /* 裸 "music" 给帮助 */
        exec_music_cmd("");                             /* 空参数触发用法 */
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
