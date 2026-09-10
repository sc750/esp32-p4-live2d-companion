/**
 * @file    music_service.c
 * @brief   音乐播放服务实现——曲目表（网络电台 + SD 文件）+ simple_dec feed 解码 + 播放任务
 *
 * 曲目来源两路（同列一张表，索引统一）：
 *   1) 网络电台——Kconfig 配的 Icecast 流（无需 SD 卡；明文 http 不占 mbedTLS）
 *   2) SD 卡文件——/sdcard/music/ 下 mp3/wav（插卡后 music scan 补齐）
 * 读数据统一经 music_source（本地文件 / HTTP 流），本模块只管解码与播放节奏。
 *
 * 线程模型：
 *   - music 任务（唯一）：收命令队列 → 播放循环（读源→解码→阻塞写 I2S）
 *   - 控制函数（串口/语音任务）：发命令 + 改暂停标志（互斥保护）
 *   - voice 打断：置停标志 + 等 STOP_DONE 事件位（≤3s），退出时恢复 16k
 *
 * @date    2026-09-10
 * @version 2.0.0
 */

#include "music_service.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "bsp_audio.h"
#include "bsp_init.h"           /* bsp_sd_mount */
#include "music_source.h"       /* 音源抽象（文件/网络） */
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"   /* 封装解析器注册（WAV/OGG…） */
#include "esp_audio_dec_default.h"          /* 底层解码器注册（MP3/AAC…） */

#define TAG "music"

/* ---- 参数 ---- */
#define MUSIC_DIR           "/sdcard/music"         /* 本地音乐目录 */
#define MUSIC_TASK_STACK    (12 * 1024)             /* 播放任务栈（解码器工作量大） */
#define MUSIC_TASK_PRIO     4                       /* 高于空闲、低于 UI */
#define DECODE_IN_MAX       (4 * 1024)              /* 喂解码器的压缩块大小（PSRAM） */
#define DECODE_OUT_INIT     (32 * 1024)             /* PCM 输出缓冲初始值（自动扩） */
#define READ_CHUNK_MS       20                      /* 命令轮询周期（暂停响应延迟上限） */
#define NET_READ_TIMEOUT_MS 10000                   /* 网络流单次读超时（块间隔余量） */
#define NET_RETRY_MAX       5                       /* 网络流连续读失败上限（超过即放弃本曲） */

/* 播放任务命令 */
typedef enum {
    MUS_CMD_PLAY = 0,       /* 播指定索引（param=idx） */
    MUS_CMD_NEXT,           /* 下一首 */
    MUS_CMD_PREV,           /* 上一首 */
    MUS_CMD_STOP,           /* 停止 */
} music_cmd_t;

typedef struct {
    music_cmd_t cmd;        /* 命令 */
    int param;              /* PLAY 的目标索引 */
} music_msg_t;

/** 一条曲目（本地文件或网络流，播放侧统一处理） */
typedef struct {
    char name[MUSIC_NAME_MAX];      /* 显示名（曲名/电台名） */
    char uri[MUSIC_URI_MAX];        /* 音源地址（路径或 URL） */
    bool network;                   /* true = 网络流（决定读超时与重试策略） */
} music_track_t;

/* 模块状态 */
static struct {
    bool inited;                            /* 幂等闸门 */
    QueueHandle_t cmd_q;                    /* 命令队列 */
    EventGroupHandle_t evt;                 /* STOP_DONE 位 */
    /* 曲目表（网络电台在前，SD 文件在后）。
     * **必须走 PSRAM 动态分配**：100 条 × ~260B ≈ 25KB，早期版本写成静态
     * 数组常驻内部 SRAM（.bss.s_mus 实测 0x6480=25728B），挤压 esp_hosted
     * 的 SDIO 收发内存池 → 联网时 "sdio mempool OOM (RX)"、LLM 请求卡死。 */
    music_track_t *tracks;                  /* 曲目表（PSRAM） */
    int count;                              /* 曲目数 */
    int cur;                                /* 当前曲索引（-1 无） */
    volatile bool playing;                  /* 正在播（任务维护） */
    volatile bool paused;                   /* 暂停标志 */
    volatile bool abort_cur;                /* 中断当前曲（stop/切歌） */
    int pos_sec;                            /* 已播秒数（曲内累计） */
} s_mus;

/** 内置网络电台表（Kconfig 配置；URL 留空则跳过该条） */
static const struct {
    const char *name;                       /* 电台显示名 */
    const char *url;                        /* Icecast 流地址（明文 http 优先） */
} s_radios[] = {
    { "网络电台1 环境电子", CONFIG_MUSIC_RADIO_URL_1 },
    { "网络电台2 舒缓人声", CONFIG_MUSIC_RADIO_URL_2 },
    { "网络电台3 空间氛围", CONFIG_MUSIC_RADIO_URL_3 },
};
#define RADIO_COUNT ((int)(sizeof(s_radios) / sizeof(s_radios[0])))

/** 判断扩展名是否支持（.mp3/.wav，不分大小写） */
static bool ext_supported(const char *name)
{
    size_t n = strlen(name);                            /* 文件名长 */
    if (n < 5) return false;                            /* 至少 x.mp3 */
    const char *ext = name + n - 4;                     /* 末 4 字符 */
    return strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0;
}

int music_scan(void)
{
    s_mus.count = 0;                                    /* 重扫清空 */
    /* ---- 1. 先装网络电台（编译期配置，不依赖任何外设） ---- */
    for (int i = 0; i < RADIO_COUNT && s_mus.count < MUSIC_LIST_MAX; i++) {
        if (!s_radios[i].url || !s_radios[i].url[0]) {  /* URL 留空 = 该台未启用 */
            continue;                                   /* 跳过 */
        }
        music_track_t *t = &s_mus.tracks[s_mus.count];  /* 取槽位 */
        strlcpy(t->name, s_radios[i].name, MUSIC_NAME_MAX);     /* 电台名 */
        strlcpy(t->uri, s_radios[i].url, MUSIC_URI_MAX);        /* 流地址 */
        t->network = true;                              /* 标记网络源 */
        s_mus.count++;                                  /* 计数推进 */
    }
    int radio_n = s_mus.count;                          /* 记住电台条数（日志用） */

    /* ---- 2. 再扫 SD 卡本地文件（没卡就只剩电台） ---- */
    DIR *d = opendir(MUSIC_DIR);                        /* 打开音乐目录 */
    if (d) {                                            /* 卡在且有目录 */
        struct dirent *e;                               /* 目录项游标 */
        while ((e = readdir(d)) != NULL && s_mus.count < MUSIC_LIST_MAX) {
            if (e->d_type != DT_REG) continue;          /* 只收普通文件 */
            if (!ext_supported(e->d_name)) continue;    /* 只收 mp3/wav */
            music_track_t *t = &s_mus.tracks[s_mus.count];      /* 取槽位 */
            strlcpy(t->name, e->d_name, MUSIC_NAME_MAX);        /* 曲名初值 = 文件名 */
            char *dot = strrchr(t->name, '.');          /* 找扩展名点 */
            if (dot) *dot = '\0';                       /* 掐掉扩展名 */
            snprintf(t->uri, MUSIC_URI_MAX, MUSIC_DIR "/%s", e->d_name);    /* 全路径 */
            t->network = false;                         /* 标记本地源 */
            s_mus.count++;                              /* 计数推进 */
        }
        closedir(d);                                    /* 关目录 */
    } else {                                            /* 没卡/没目录 */
        ESP_LOGW(TAG, "未找到 %s（SD 卡未插或未建目录），仅网络电台可用", MUSIC_DIR);
    }
    s_mus.cur = -1;                                     /* 重扫后无当前曲 */
    ESP_LOGI(TAG, "曲目表: %d 首（网络电台 %d + 本地 %d）",
             s_mus.count, radio_n, s_mus.count - radio_n);
    return s_mus.count;                                 /* 返回数目 */
}

int music_count(void)
{
    return s_mus.count;                                 /* 直接读 */
}

const char *music_name_at(int idx)
{
    if (idx < 0 || idx >= s_mus.count) {                /* 越界防御 */
        return NULL;                                    /* 无此曲 */
    }
    return s_mus.tracks[idx].name;                      /* 曲名（静态表，调用方勿释放） */
}

/** 按 0/1/…/N-1 顺序取下一索引（列表循环） */
static int idx_next(void)
{
    if (s_mus.count == 0) return -1;                    /* 没歌 */
    return (s_mus.cur + 1) % s_mus.count;               /* 循环推进 */
}

/** 按 0/1/…/N-1 顺序取上一索引（列表循环） */
static int idx_prev(void)
{
    if (s_mus.count == 0) return -1;                    /* 没歌 */
    return (s_mus.cur - 1 + s_mus.count) % s_mus.count; /* 循环回退 */
}

/** 播一首（本地文件或网络流）直到结束/被打断；返回 true = 正常播完 */
static bool play_one_track(const music_track_t *t)
{
    /* ---- 1. 打开音源 + 建解码器（按 URI 后缀选解码类型） ---- */
    struct music_source *src = music_source_open(t->uri);       /* 统一打开 */
    if (!src) {                                         /* 打不开（没卡/网络不可达） */
        ESP_LOGW(TAG, "音源打开失败: %s", t->uri);      /* 告警 */
        return false;                                   /* 放弃这首 */
    }
    size_t ulen = strlen(t->uri);                       /* URI 长度 */
    bool is_wav = (ulen >= 4 && strcasecmp(t->uri + ulen - 4, ".wav") == 0);
    /* 网络电台 99% 是 MP3；本地按扩展名判别（WAV 头 simple_dec 也能自动识别） */
    esp_audio_simple_dec_type_t type = (is_wav && !t->network)
        ? ESP_AUDIO_SIMPLE_DEC_TYPE_WAV                 /* 本地 wav */
        : ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;                /* 其余按 mp3 */
    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = type,                               /* 类型 */
        .dec_cfg = NULL,                                /* 内置解码器默认参数 */
        .cfg_size = 0,                                  /* 无附加配置 */
        .use_frame_dec = false,                         /* feed 式：内部自动解析帧 */
    };
    esp_audio_simple_dec_handle_t dec = NULL;           /* 解码器句柄 */
    if (esp_audio_simple_dec_open(&dec_cfg, &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "解码器打开失败");                /* 报错 */
        music_source_close(src);                        /* 关音源 */
        return false;                                   /* 放弃 */
    }

    /* ---- 2. 缓冲分配（一次一曲，PSRAM） ---- */
    /* 所有 cleanup 用到的变量必须先声明，避免 goto 跳过初始化（-Werror=maybe-uninitialized） */
    uint8_t *in_buf = heap_caps_malloc(DECODE_IN_MAX, MALLOC_CAP_SPIRAM);       /* 压缩输入 */
    uint8_t *out_buf = heap_caps_malloc(DECODE_OUT_INIT, MALLOC_CAP_SPIRAM);    /* PCM 输出 */
    bool ok = false;                                    /* 播完标志 */
    bool fs_ready = false;                              /* 本曲是否已设采样率（cleanup 需读） */
    if (!in_buf || !out_buf) {                          /* 分配失败 */
        ESP_LOGE(TAG, "解码缓冲分配失败");              /* 报错 */
        goto cleanup;                                   /* 统一清理 */
    }
    s_mus.pos_sec = 0;                                  /* 进度归零 */
    s_mus.playing = true;                               /* 置播放态 */
    s_mus.paused = false;                               /* 起始不暂停 */
    s_mus.abort_cur = false;                            /* 清中断标志 */
    ESP_LOGI(TAG, "开始播放: %s (%s)", t->name, t->uri);        /* 日志 */

    /* ---- 3. feed 解码主循环（bsp_audio_play 阻塞写 = 天然按播放速度节流） ---- */
    size_t out_cap = DECODE_OUT_INIT;                   /* 输出缓冲当前容量 */
    bool eos = false;                                   /* 流读尽标志 */
    bool net_broken = false;                            /* 网络源异常收尾（区分"正常结束"） */
    int net_fail = 0;                                   /* 网络连续读失败计数 */
    int rate = 0, ch = 2;                               /* 采样参数（进度换算用） */
    uint64_t bytes_played = 0;                          /* 已产 PCM 字节（进度累计） */
    while (!eos && !s_mus.abort_cur) {                  /* 主循环 */
        /* 命令轮询：STOP 就地生效；切歌类命令"回插队列 + 中断本曲"，
         * 让外层任务循环去处理（否则命令会被播放循环吞掉，下一首无反应） */
        music_msg_t m;                                  /* 命令槽 */
        while (xQueueReceive(s_mus.cmd_q, &m, 0) == pdTRUE) {   /* 清空队列 */
            if (m.cmd == MUS_CMD_STOP) {                /* 停止命令 */
                s_mus.abort_cur = true;                 /* 置中断 */
            } else {                                    /* 切歌/指定播放 */
                xQueueSendToFront(s_mus.cmd_q, &m, 0);  /* 回插队首（外层立刻取到） */
                s_mus.abort_cur = true;                 /* 中断本曲，尽快让位 */
            }
        }
        if (s_mus.paused) {                             /* 暂停态 */
            vTaskDelay(pdMS_TO_TICKS(READ_CHUNK_MS));   /* 空转等待恢复 */
            continue;                                   /* 不读不解码 */
        }
        /* 喂一块压缩数据给解码器（本地/网络统一走 music_source_read） */
        esp_audio_simple_dec_raw_t raw = {              /* 输入描述 */
            .buffer = in_buf,                           /* 输入块 */
            .len = 0,                                   /* 下面填充 */
            .eos = false,                               /* 结束另行置位 */
            .consumed = 0,                              /* 解码器回填 */
            .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,        /* 正常帧 */
        };
        int rd = music_source_read(src, in_buf, DECODE_IN_MAX, NET_READ_TIMEOUT_MS);
        if (rd < 0) {                                   /* 网络读错（超时/连接断） */
            net_fail++;                                 /* 连续失败计数 +1 */
            if (net_fail >= NET_RETRY_MAX) {            /* 超上限 */
                ESP_LOGW(TAG, "网络流连续 %d 次读失败，结束本曲", net_fail);
                net_broken = true;                      /* 标记异常收尾（不自动重连） */
                eos = true;                             /* 结束本曲 */
            } else {                                    /* 还可再试 */
                vTaskDelay(pdMS_TO_TICKS(200));         /* 稍等再读（给网络恢复机会） */
            }
            continue;                                   /* 重试/收尾 */
        }
        net_fail = 0;                                   /* 读到数据即清失败计数 */
        if (rd == 0) {                                  /* 流正常结束 */
            eos = true;                                 /* 收尾 */
            break;                                      /* 跳出读取，走收尾（不喂空块） */
        }
        raw.len = (uint32_t)rd;                         /* 本块字节数 */
        esp_audio_simple_dec_out_t frame = {            /* 输出描述 */
            .buffer = out_buf,                          /* PCM 落点 */
            .len = out_cap,                             /* 缓冲容量 */
            .needed_size = 0,                           /* 解码器回填需求 */
            .decoded_size = 0,                          /* 解码器回填产量 */
        };
        esp_audio_err_t dr = esp_audio_simple_dec_process(dec, &raw, &frame);
        if (dr == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {      /* 输出缓冲不够 */
            out_cap = frame.needed_size;                /* 按需求扩 */
            uint8_t *nb = heap_caps_realloc(out_buf, out_cap, MALLOC_CAP_SPIRAM);
            if (!nb) {                                  /* 扩容失败 */
                ESP_LOGE(TAG, "输出扩容失败");          /* 报错 */
                break;                                  /* 放弃本曲 */
            }
            out_buf = nb;                               /* 换新缓冲 */
            continue;                                   /* 重试同一块 */
        }
        if (dr != ESP_AUDIO_ERR_OK) {                   /* 解码错误 */
            ESP_LOGW(TAG, "解码错误 %d，跳过该块", dr);         /* 单块坏不弃曲 */
            continue;                                   /* 继续喂 */
        }
        if (frame.decoded_size > 0) {                   /* 有 PCM 产出 */
            if (!fs_ready) {                            /* 首块 PCM：信息就绪 */
                esp_audio_simple_dec_info_t info;       /* 采样率等 */
                if (esp_audio_simple_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK
                    && info.sample_rate > 0) {          /* 拿到合法参数 */
                    rate = info.sample_rate;            /* 存进度换算用 */
                    ch = info.channel ? info.channel : 2;       /* 声道兜底 */
                    /* 按音源原生参数切 I2S（BSP 内部已含音量重设） */
                    bsp_audio_set_fs(info.sample_rate,
                                     info.bits_per_sample ? info.bits_per_sample : 16,
                                     ch);
                    ESP_LOGI(TAG, "采样参数: %luHz/%ubit/%uch",
                             (unsigned long)info.sample_rate,
                             info.bits_per_sample, ch);
                    fs_ready = true;                    /* 只设一次 */
                }
            }
            if (fs_ready) {                             /* 参数就绪才写 */
                bsp_audio_play(out_buf, frame.decoded_size);            /* 阻塞写 I2S */
                bytes_played += frame.decoded_size;     /* 累计产量 */
                /* 字节 → 秒：每秒 = rate × ch × 2bytes */
                s_mus.pos_sec = (int)(bytes_played / ((uint64_t)rate * ch * 2));
            }
        }
    }
    /* 是否接下一曲：被打断（stop/切歌）不接；网络流异常中断也不接
     * （否则会在断网时无限重连、把 SDIO 带宽吃光，拖垮同时进行的 LLM/TTS）；
     * 本地文件读完 / 网络流被对端正常关流 → 接下一曲 */
    ok = !s_mus.abort_cur && !net_broken;               /* 决定外层是否接下一曲 */

cleanup:
    /* ---- 4. 清理 + 采样率恢复 16k（保护录音/TTS 链路） ---- */
    s_mus.playing = false;                              /* 清播放态 */
    if (dec) esp_audio_simple_dec_close(dec);           /* 关解码器 */
    if (in_buf) free(in_buf);                           /* 释放输入缓冲 */
    if (out_buf) free(out_buf);                         /* 释放输出缓冲 */
    music_source_close(src);                            /* 关音源（网络源断连接） */
    if (fs_ready) {                                     /* 动过采样率才恢复 */
        bsp_audio_set_fs(16000, 16, 2);                 /* 回系统默认 */
    }
    return ok;                                          /* 返回结果 */
}

/** 播放任务：命令分发 + 顺序连播 */
static void music_task(void *arg)
{
    music_msg_t m;                                      /* 命令消息 */
    while (1) {                                         /* 常驻 */
        if (xQueueReceive(s_mus.cmd_q, &m, portMAX_DELAY) != pdTRUE) {
            continue;                                   /* 理论不达 */
        }
        int target = -1;                                /* 本命令目标曲 */
        switch (m.cmd) {                                /* 解析命令 */
        case MUS_CMD_PLAY:                              /* 指定播放 */
            target = m.param;                           /* 用参数 */
            break;                                      /* */
        case MUS_CMD_NEXT:                              /* 下一首 */
            target = idx_next();                        /* 循环推进 */
            break;                                      /* */
        case MUS_CMD_PREV:                              /* 上一首 */
            target = idx_prev();                        /* 循环回退 */
            break;                                      /* */
        case MUS_CMD_STOP:                              /* 停止 */
            s_mus.abort_cur = true;                     /* 中断当前 */
            xEventGroupSetBits(s_mus.evt, BIT0);        /* 回执 STOP_DONE */
            continue;                                   /* 不播新歌 */
        default:                                        /* 未知命令 */
            continue;                                   /* 忽略 */
        }
        if (target < 0 || target >= s_mus.count) {      /* 索引无效 */
            ESP_LOGW(TAG, "无效曲目索引 %d", target);   /* 告警 */
            continue;                                   /* 忽略 */
        }
        s_mus.cur = target;                             /* 记当前曲 */
        /* 若正在播上一首：先打断（abort），等其退出 */
        if (s_mus.playing) {                            /* 占用中 */
            s_mus.abort_cur = true;                     /* 打断 */
            while (s_mus.playing) {                     /* 等退出 */
                vTaskDelay(pdMS_TO_TICKS(10));          /* 短睡 */
            }
        }
        while (play_one_track(&s_mus.tracks[s_mus.cur])) {      /* 播一曲；本地文件播完自动连播 */
            if (s_mus.count == 0) break;                /* 中途列表被清 */
            s_mus.cur = idx_next();                     /* 下一曲索引 */
            if (s_mus.cur == m.param) break;            /* 绕回一圈就停（单曲循环留打磨期） */
        }
        xEventGroupSetBits(s_mus.evt, BIT0);            /* 本轮播完/停回执 */
    }
}

/* ---------- 公共 API ---------- */

esp_err_t music_service_init(void)
{
    if (s_mus.inited) {                                 /* 幂等 */
        return ESP_OK;                                  /* 无害返回 */
    }
    memset(&s_mus, 0, sizeof(s_mus));                   /* 清零 */
    s_mus.cur = -1;                                     /* 无当前曲 */

    /* 曲目表走 PSRAM（25KB 级，绝不常驻内部 SRAM——会挤压 SDIO 内存池） */
    s_mus.tracks = heap_caps_calloc(MUSIC_LIST_MAX, sizeof(music_track_t),
                                    MALLOC_CAP_SPIRAM); /* 一次到位 */
    ESP_RETURN_ON_FALSE(s_mus.tracks, ESP_ERR_NO_MEM, TAG, "no mem for tracks");

    /* 解码器注册（必须！否则 open 报 "Decoder MP3 not registered" ret=-7）：
     *   esp_audio_dec_register_default      → 底层音频解码器（MP3/AAC/G711…）
     *   esp_audio_simple_dec_register_default → 封装解析器（WAV/OGG/M4A…）
     * MP3 是裸流，靠前者；WAV 头部解析靠后者。两者都幂等。 */
    esp_audio_err_t aer = esp_audio_dec_register_default();     /* 底层解码器 */
    aer |= esp_audio_simple_dec_register_default();             /* 封装解析器 */
    if (aer != ESP_AUDIO_ERR_OK) {                      /* 注册失败（Kconfig 关掉了？） */
        ESP_LOGE(TAG, "解码器注册失败: %d", aer);        /* 报错 */
        return ESP_FAIL;                                /* 无解码能力，服务不可用 */
    }

    s_mus.cmd_q = xQueueCreate(8, sizeof(music_msg_t)); /* 命令队列 */
    ESP_RETURN_ON_FALSE(s_mus.cmd_q, ESP_ERR_NO_MEM, TAG, "no mem q");
    s_mus.evt = xEventGroupCreate();                    /* 事件组 */
    ESP_RETURN_ON_FALSE(s_mus.evt, ESP_ERR_NO_MEM, TAG, "no mem evt");

    /* SD 卡挂载（失败不阻塞：网络电台不依赖卡，本地曲目自动为空） */
    esp_err_t sd_err = bsp_sd_mount();                  /* BSP 层一键挂载（/sdcard） */
    if (sd_err != ESP_OK) {                             /* 失败（没插卡） */
        ESP_LOGW(TAG, "SD 卡挂载失败（%s）：仅网络电台可用", esp_err_to_name(sd_err));
    } else {                                            /* 成功 */
        ESP_LOGI(TAG, "SD 卡已挂载: " MUSIC_DIR);       /* 日志 */
    }
    music_scan();                                       /* 开机即建表（电台 + 本地） */
    if (xTaskCreate(music_task, "music", MUSIC_TASK_STACK, NULL,
                    MUSIC_TASK_PRIO, NULL) != pdPASS) { /* 建任务 */
        ESP_LOGE(TAG, "创建 music 任务失败");           /* 报错 */
        return ESP_FAIL;                                /* 返回 */
    }
    s_mus.inited = true;                                /* 就绪 */
    ESP_LOGI(TAG, "音乐服务就绪（%d 首，目录 " MUSIC_DIR "）", s_mus.count);
    return ESP_OK;                                      /* 成功 */
}

esp_err_t music_play_index(int idx)
{
    if (!s_mus.inited || idx < 0 || idx >= s_mus.count) {       /* 参数防御 */
        return ESP_ERR_NOT_FOUND;                       /* 无此曲 */
    }
    music_msg_t m = { .cmd = MUS_CMD_PLAY, .param = idx };      /* 播放命令 */
    return xQueueSend(s_mus.cmd_q, &m, pdMS_TO_TICKS(500)) == pdTRUE
           ? ESP_OK : ESP_ERR_TIMEOUT;                  /* 入队 */
}

esp_err_t music_next(void)
{
    if (!s_mus.inited || s_mus.count == 0) {            /* 无歌可切 */
        return ESP_ERR_INVALID_STATE;                   /* 拒绝 */
    }
    music_msg_t m = { .cmd = MUS_CMD_NEXT, .param = 0 };        /* 下一首 */
    return xQueueSend(s_mus.cmd_q, &m, pdMS_TO_TICKS(500)) == pdTRUE
           ? ESP_OK : ESP_ERR_TIMEOUT;                  /* 入队 */
}

esp_err_t music_prev(void)
{
    if (!s_mus.inited || s_mus.count == 0) {            /* 无歌可切 */
        return ESP_ERR_INVALID_STATE;                   /* 拒绝 */
    }
    music_msg_t m = { .cmd = MUS_CMD_PREV, .param = 0 };        /* 上一首 */
    return xQueueSend(s_mus.cmd_q, &m, pdMS_TO_TICKS(500)) == pdTRUE
           ? ESP_OK : ESP_ERR_TIMEOUT;                  /* 入队 */
}

esp_err_t music_pause(void)
{
    if (!s_mus.inited || !s_mus.playing) {              /* 没在播 */
        return ESP_ERR_INVALID_STATE;                   /* 拒绝 */
    }
    s_mus.paused = true;                                /* 置暂停（任务轮询生效） */
    return ESP_OK;                                      /* 成功 */
}

esp_err_t music_resume(void)
{
    if (!s_mus.inited || !s_mus.playing || !s_mus.paused) {     /* 没有暂停中的歌 */
        return ESP_ERR_INVALID_STATE;                   /* 拒绝 */
    }
    s_mus.paused = false;                               /* 清暂停 */
    return ESP_OK;                                      /* 成功 */
}

esp_err_t music_stop(void)
{
    if (!s_mus.inited || !s_mus.playing) {              /* 没在播 */
        return ESP_OK;                                  /* 幂等成功 */
    }
    music_msg_t m = { .cmd = MUS_CMD_STOP, .param = 0 };        /* 停止命令 */
    xEventGroupClearBits(s_mus.evt, BIT0);              /* 清回执位 */
    xQueueSend(s_mus.cmd_q, &m, pdMS_TO_TICKS(100));    /* 入队（慢了也行，abort 兜底） */
    s_mus.abort_cur = true;                             /* 直接置中断（双保险） */
    /* 等任务退出播放循环并恢复 16k（≤2s） */
    for (int i = 0; i < 200 && s_mus.playing; i++) {    /* 轮询 */
        vTaskDelay(pdMS_TO_TICKS(10));                  /* 短睡 */
    }
    return ESP_OK;                                      /* 成功 */
}

esp_err_t music_set_volume(int volume)
{
    return bsp_audio_set_volume(volume);                /* 透传 BSP */
}

bool music_is_playing(void)
{
    return s_mus.playing && !s_mus.paused;              /* 播放且未暂停 */
}

const char *music_current_name(void)
{
    if (s_mus.cur < 0 || s_mus.cur >= s_mus.count) {    /* 无当前曲 */
        return NULL;                                    /* 空 */
    }
    return s_mus.tracks[s_mus.cur].name;                /* 曲名 */
}

int music_position_sec(void)
{
    return s_mus.pos_sec;                               /* 直读（进度 MVP 占位） */
}

void music_notify_voice_start(void)
{
    if (!s_mus.inited || !s_mus.playing) {              /* 没在播 */
        return;                                         /* 立即返回 */
    }
    music_stop();                                       /* 停止（内部等待退出） */
    ESP_LOGI(TAG, "语音会话开始，音乐已停");            /* 日志 */
}
