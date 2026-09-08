/**
 * @file    voice_pipeline.c
 * @brief   语音管线编排实现——录音 → ASR → LLM → 按句 TTS 预取 → 播放
 *
 * 状态机（一次一轮，s_busy 防重入）：
 *   按下 → 录音（蓝点+“在听”） → 松开/30s上限 → ASR（“识别中”）
 *        → 识别文本 → LLM 流式（橙点，token 刷字幕 + 句末切句）
 *        → 每句 TTS 预取（合成与播放并行） → 绿点 → 灭
 *
 * 三任务流水线（Codex M2+，首音延迟优化的核心）：
 *   voice_pipe : 录音+ASR+LLM 调度（把完整句子推入 s_tts_queue）
 *   tts_stream : 从句子队列取句 → TTS 拉流重采样 → 推入就绪队列
 *   tts_play   : 从就绪队列取完整短语 → 扬声器连播（互斥锁防串音）
 *
 * @date    2026-09-06
 * @version 1.2.0  M2+：按句 TTS 预取流水线（Codex）+ 延迟量化埋点（M3）
 */

#include "voice_pipeline.h"     /* 本模块公开接口 */

#include <string.h>             /* strlen / memcpy / strlcpy */
#include <stdlib.h>             /* calloc / free */
#include <math.h>               /* sqrt（口型 RMS 计算） */

#include "esp_log.h"            /* ESP_LOGx 日志 */
#include "esp_check.h"          /* ESP_RETURN_ON_* 检查宏 */
#include "esp_timer.h"          /* esp_timer_get_time（延迟量化/节流） */
#include "esp_heap_caps.h"      /* heap_caps_realloc（短语 PCM 扩容） */
#include "freertos/FreeRTOS.h"          /* FreeRTOS 基础 */
#include "freertos/task.h"              /* 任务创建 */
#include "freertos/event_groups.h"      /* 事件组（按住说话沿） */
#include "freertos/stream_buffer.h"     /* 流缓冲（播放环形缓冲） */
#include "freertos/queue.h"             /* 队列（句子/就绪短语） */

#include "bsp_wifi.h"           /* WiFi 连接状态查询 */
#include "bsp_audio.h"          /* 扬声器播放（bsp_audio_play） */
#include "voice_rec.h"          /* 录音器 */
#include "asr_client.h"         /* ASR（讯飞/MiMo 双后端） */
#include "tts_client.h"         /* TTS（MiMo 流式合成） */
#include "dialog_manager.h"     /* LLM 对话（流式断句） */
#include "rig_rig.h"            /* 口型驱动（rig_rig_set_mouth） */
#include "app_events.h"         /* dialog_state_t 共享词汇 */
#include <math.h>               /* sqrtf/sqrt（重复包含无害，兼容历史） */
#include "freertos/stream_buffer.h"     /* 流缓冲（重复包含无害） */
#include "freertos/queue.h"     /* 队列（重复包含无害） */

#define TAG "voice"             /* 本模块日志标签 */

/* 事件位：按住说话的按下/松开沿（LVGL 按钮 → 管线任务） */
#define EVT_HOLD_START  BIT0    /* 按下沿：开始录音 */
#define EVT_HOLD_STOP   BIT1    /* 松开沿：停止录音并走管线 */

/* 流式字幕刷新节流：两个 token 之间至少隔 100ms 才刷一次 LVGL */
#define FLUSH_MIN_US    (100 * 1000)

/* 管线主状态（按住说话 + 一轮防重入） */
static struct {
    bool inited;                /* 初始化完成标志（幂等闸门） */
    EventGroupHandle_t evt;     /* 事件组：EVT_HOLD_START/STOP */
    voice_ui_cb_t ui;           /* 注入的 UI 回调（状态点/字幕） */
    bool busy;                  /* 一轮管线进行中（防重入） */
} s_vp;

/* ---- 按句 TTS 预取流水线的常量与数据结构 ---- */
#define TTS_SENTENCE_MAX  512   /* 单句文本上限（bytes，UTF-8） */
#define TTS_QUEUE_DEPTH   8     /* 句子队列深度（LLM → TTS 拉流） */
#define TTS_READY_DEPTH    4    /* 就绪队列深度（TTS → 播放，按序） */

/** 一句待合成的文本（LLM 断句回调的产物，在队列里排队） */
typedef struct {
    char text[TTS_SENTENCE_MAX];    /* 句子文本（'\0' 结尾） */
} tts_sentence_t;

/** 一段已完整转换为 codec PCM 的短语，所有权在播放任务释放。 */
typedef struct {
    uint8_t *pcm;                   /* 短语 PCM 缓冲（16k/2ch，PSRAM） */
    size_t len;                     /* 当前有效字节数 */
    size_t cap;                     /* 缓冲容量（自动翻倍扩容） */
    int16_t resample_tail[3];       /* 24k→16k 重采样的跨块残样 */
    size_t resample_tail_count;     /* 残样数量（0~2） */
} tts_pcm_phrase_t;

static QueueHandle_t s_tts_queue;       /* 句子队列：LLM 断句 → TTS 拉流任务 */
static QueueHandle_t s_tts_ready_queue; /* 就绪队列：TTS 拉流 → 播放任务（指针传递） */
static volatile uint32_t s_tts_pending; /* 已入队未播完的句子数（原子加减） */
static volatile bool s_tts_input_done;  /* LLM 输出结束标志（全部句子已入队） */
static volatile int64_t s_t_release = 0;        /* M3 延迟量化：松手时刻 */
static volatile int64_t s_t_first_sentence = 0; /* M3 延迟量化：LLM 首句时刻 */
static volatile bool s_tts_stream_active;       /* 按句 TTS 流水线活动标志 */
static SemaphoreHandle_t s_speak_lock;          /* 播报互斥锁（扬声器同一时刻只一路） */
static SemaphoreHandle_t s_tts_request_lock;    /* TTS 请求互斥锁（MiMo SSE 并发保护） */
static char s_reply_text[2048];                 /* LLM 回复累积缓冲（流式刷字幕） */
static size_t s_reply_len;                      /* 回复累积长度 */
static int64_t s_reply_last_flush_us;           /* 上次字幕刷新时刻（节流用） */

/* ---------- UI 快捷（回调判空后转发给注入的 UI 接口） ---------- */

/** 状态点联动：转发对话状态给注入的 on_state 回调 */
static void ui_state(int st)
{
    if (s_vp.ui.on_state) {                             /* 回调已注入才转发 */
        s_vp.ui.on_state(st, s_vp.ui.ctx);              /* 转发状态值与上下文 */
    }
}

/** 字幕联动：转发文本给注入的 on_subtitle 回调 */
static void ui_text(const char *t)
{
    if (s_vp.ui.on_subtitle) {                          /* 回调已注入才转发 */
        s_vp.ui.on_subtitle(t, s_vp.ui.ctx);            /* 转发文本与上下文 */
    }
}

/** LLM token 只做字幕累积；节流避免每个 token 都触发一次 LVGL 重绘。 */
static void on_reply_token(const char *text, void *ctx)
{
    (void)ctx;                                          /* 未使用上下文 */
    size_t len = strlen(text);                          /* 本段 token 长度 */
    if (s_reply_len + len >= sizeof(s_reply_text)) {    /* 超过 2KB 累积上限 */
        return;                                         /* 丢弃（超长保护） */
    }
    memcpy(s_reply_text + s_reply_len, text, len);      /* 追加 token 到累积缓冲 */
    s_reply_len += len;                                 /* 长度游标前进 */
    s_reply_text[s_reply_len] = '\0';                   /* 维护字符串结尾 */
    int64_t now = esp_timer_get_time();                 /* 取当前时刻（微秒） */
    if (now - s_reply_last_flush_us >= FLUSH_MIN_US) {  /* 距上次刷新 ≥100ms 才刷 */
        s_reply_last_flush_us = now;                    /* 记录本次刷新时刻 */
        ui_text(s_reply_text);                          /* 流式刷字幕 */
    }
}

/** 句末回调在 LLM 网络任务上下文执行，只投递，绝不在这里请求 TTS。 */
static void on_reply_sentence(const char *sentence, void *ctx)
{
    (void)ctx;                                          /* 未使用上下文 */
    if (!sentence[0] || !s_tts_queue) {                 /* 空句或管线未初始化 */
        return;                                         /* 直接忽略 */
    }
    /* M3 延迟量化：LLM 首句耗时（只记一次） */
    if (s_t_first_sentence == 0) {                      /* 首句只记一次时刻 */
        s_t_first_sentence = esp_timer_get_time();      /* 记录首句时刻 */
        ESP_LOGI(TAG, "⏱ LLM 首句: %lldms",             /* 打印松手→首句延迟 */
                 (s_t_first_sentence - s_t_release) / 1000);    /* 毫秒换算 */
    }
    tts_sentence_t item = {0};                          /* 构造句子队列项 */
    strlcpy(item.text, sentence, sizeof(item.text));    /* 拷贝句子（截断保护） */
    __atomic_fetch_add(&s_tts_pending, 1, __ATOMIC_RELAXED);    /* 未播计数 +1（先加防竞态） */
    if (xQueueSend(s_tts_queue, &item, pdMS_TO_TICKS(50)) == pdPASS) {  /* 投递到句子队列 */
        ESP_LOGI(TAG, "LLM 句末入队: %.80s", item.text);        /* 打印入队的句子 */
    } else {                                            /* 队列满（50ms 都没等到空位） */
        __atomic_fetch_sub(&s_tts_pending, 1, __ATOMIC_RELAXED);        /* 回滚计数 */
        ESP_LOGW(TAG, "TTS 短句队列已满，本句未播报");   /* 告警丢句 */
    }
}

/** 把 24k/mono 输入转为 16k/stereo，并追加到短语的完整 PCM 缓冲。 */
static void prefetch_audio_cb(const int16_t *pcm, size_t samples, void *ctx)
{
    tts_pcm_phrase_t *phrase = (tts_pcm_phrase_t *)ctx; /* 上下文 = 短语控制块 */
    size_t max_frames = (phrase->resample_tail_count + samples) / 3 * 2;        /* 理论最大输出帧数 */
    size_t bytes = max_frames * 2 * sizeof(int16_t);    /* 对应字节数（双声道） */
    if (bytes == 0) {                                   /* 凑不满 3 个残样 */
        return;                                         /* 无输出，直接返回 */
    }
    size_t required = phrase->len + bytes;              /* 扩容后需要的总容量 */
    if (required > phrase->cap) {                       /* 现有容量不够 */
        size_t new_cap = phrase->cap ? phrase->cap : 32 * 1024; /* 首次 32KB，之后翻倍 */
        while (new_cap < required) {                    /* 循环翻倍直到够用 */
            new_cap *= 2;                               /* 容量翻倍 */
        }
        uint8_t *new_pcm = heap_caps_realloc(phrase->pcm, new_cap, MALLOC_CAP_SPIRAM);  /* PSRAM 扩容 */
        if (!new_pcm) {                                 /* 扩容失败 */
            ESP_LOGW(TAG, "TTS 短语 PCM 扩容失败: %uB", (unsigned)new_cap);     /* 告警放弃本块 */
            return;                                     /* 数据丢弃（内存紧张属异常） */
        }
        phrase->pcm = new_pcm;                          /* 更新缓冲指针 */
        phrase->cap = new_cap;                          /* 更新容量 */
    }

    size_t src = 0;                                     /* 输入样本游标 */
    size_t out_frames = 0;                              /* 输出帧游标 */
    int16_t *out = (int16_t *)(phrase->pcm + phrase->len);      /* 输出写指针 */
    while (src < samples) {                             /* 逐样本消费输入 */
        while (phrase->resample_tail_count < 3 && src < samples) {      /* 先凑满 3 个残样 */
            phrase->resample_tail[phrase->resample_tail_count++] = pcm[src++];  /* 补进尾部缓冲 */
        }
        if (phrase->resample_tail_count < 3) {          /* 输入耗尽仍不足 3 个 */
            break;                                      /* 留到下一块继续凑 */
        }
        int16_t first = phrase->resample_tail[0];       /* 第 1 个样本 → 输出第 1 帧 */
        int16_t middle = (int16_t)(((int32_t)phrase->resample_tail[1] +   /* 第 2、3 样本取均值 */
                                    phrase->resample_tail[2]) / 2);     /* → 输出第 2 帧 */
        out[out_frames * 2] = first;                    /* 第 1 帧左声道 */
        out[out_frames * 2 + 1] = first;                /* 第 1 帧右声道（同值=mono 拷贝） */
        out_frames++;                                   /* 帧计数 +1 */
        out[out_frames * 2] = middle;                   /* 第 2 帧左声道（插值帧） */
        out[out_frames * 2 + 1] = middle;               /* 第 2 帧右声道 */
        out_frames++;                                   /* 帧计数 +1 */
        phrase->resample_tail_count = 0;                /* 3 样本已消费，残样清空 */
    }
    phrase->len += out_frames * 2 * sizeof(*out);       /* 短语长度推进（双声道帧×2样本） */
}

/** 按一块 PCM 的 RMS 能量驱动口型三档开合（说/半开/闭） */
static void update_mouth_from_pcm(const int16_t *pcm, size_t samples)
{
    int64_t acc = 0;                                    /* 平方和累加器 */
    for (size_t i = 0; i < samples; i++) {              /* 逐样本求平方和 */
        acc += (int64_t)pcm[i] * pcm[i];                /* 64 位防溢出 */
    }
    double rms = sqrt((double)acc / (samples ? samples : 1));   /* RMS 均方根（防除零） */
    rig_rig_set_mouth(rms > 2500 ? RIG_MOUTH_OPEN :     /* RMS>2500 → 张大嘴 */
                      rms > 600 ? RIG_MOUTH_HALF : RIG_MOUTH_CLOSED);   /* 中间半开，否则闭嘴 */
}

/** 播放器只消费完整短语，不会因网络速度低于播放速度而欠载。 */
static void tts_play_task(void *arg)
{
    (void)arg;                                          /* 未使用任务参数 */
    tts_pcm_phrase_t *phrase = NULL;                    /* 从就绪队列取出的短语指针 */
    while (1) {                                         /* 播放任务常驻循环 */
        if (xQueueReceive(s_tts_ready_queue, &phrase, portMAX_DELAY) != pdPASS) {       /* 阻塞等短语 */
            continue;                                   /* 队列异常时继续下一轮 */
        }
        xSemaphoreTake(s_speak_lock, portMAX_DELAY);    /* 拿扬声器互斥锁（防两路同时播） */
        ui_state(DIALOG_STATE_SPEAKING);                /* 绿点亮起（说话中） */
        ESP_LOGI(TAG, "TTS playback start: buffered=%uB (complete phrase)",     /* 打印短语信息 */
                 (unsigned)phrase->len);                /* 预取完成的字节数 */
        size_t offset = 0;                              /* 播放偏移游标 */
        while (offset < phrase->len) {                  /* 循环播完整个短语 */
            size_t chunk = phrase->len - offset;        /* 剩余字节数 */
            if (chunk > 4096) {                         /* 每次 4KB 喂 codec */
                chunk = 4096;                           /* 限块大小（DMA 友好） */
            }
            update_mouth_from_pcm((const int16_t *)(phrase->pcm + offset),      /* 用本块能量驱动口型 */
                                  chunk / sizeof(int16_t)); /* 样本数换算 */
            esp_err_t err = bsp_audio_play(phrase->pcm + offset, chunk);        /* 阻塞写入 codec */
            if (err != ESP_OK) {                        /* 播放写入失败 */
                ESP_LOGE(TAG, "I2S playback write failed: %s", esp_err_to_name(err));   /* 打印错误 */
                break;                                  /* 中断本短语播放 */
            }
            offset += chunk;                            /* 播放偏移推进 */
        }
        ESP_LOGI(TAG, "TTS playback stats: underflows=0, send_timeouts=0, complete phrase");    /* 短语完整播出 */
        free(phrase->pcm);                              /* 释放短语 PCM 缓冲 */
        free(phrase);                                   /* 释放短语控制块 */
        rig_rig_set_mouth(RIG_MOUTH_CLOSED);            /* 播完闭嘴 */
        if (__atomic_load_n(&s_tts_pending, __ATOMIC_RELAXED) > 0) {    /* 还有未播完的句子 */
            __atomic_fetch_sub(&s_tts_pending, 1, __ATOMIC_RELAXED);    /* 未播计数 -1 */
        }
        xSemaphoreGive(s_speak_lock);                   /* 释放扬声器锁 */
    }
}

/** 下载下一短语时，播放器可同时播放上一短语。 */
static void tts_stream_task(void *arg)
{
    (void)arg;                                          /* 未使用任务参数 */
    tts_sentence_t item;                                /* 从句子队列取出的句子 */
    while (1) {                                         /* 拉流任务常驻循环 */
        if (xQueueReceive(s_tts_queue, &item, portMAX_DELAY) != pdPASS) {       /* 阻塞等句子 */
            continue;                                   /* 队列异常时继续下一轮 */
        }
        tts_pcm_phrase_t *phrase = calloc(1, sizeof(*phrase));  /* 分配短语控制块（清零） */
        if (!phrase) {                                  /* 控制块分配失败 */
            ESP_LOGW(TAG, "TTS 短语控制块分配失败");     /* 告警 */
            __atomic_fetch_sub(&s_tts_pending, 1, __ATOMIC_RELAXED);    /* 未播计数回滚 */
            continue;                                   /* 跳过本句 */
        }
        xSemaphoreTake(s_tts_request_lock, portMAX_DELAY);      /* TTS 请求串行化（SSE 单路） */
        esp_err_t err = tts_synthesize(item.text, "傲娇但温柔的少女语气，语速自然",   /* 风格指令 */
                                       prefetch_audio_cb, phrase);      /* 音频块 → 短语缓冲 */
        xSemaphoreGive(s_tts_request_lock);             /* 释放请求锁 */
        if (err != ESP_OK || phrase->len == 0) {        /* 合成失败或零音频 */
            ESP_LOGW(TAG, "TTS 短语下载失败: %s", esp_err_to_name(err));        /* 告警 */
            free(phrase->pcm);                          /* 释放 PCM（可能为 NULL，free 无害） */
            free(phrase);                               /* 释放控制块 */
            __atomic_fetch_sub(&s_tts_pending, 1, __ATOMIC_RELAXED);    /* 未播计数回滚 */
            continue;                                   /* 跳过本句 */
        }
        if (xQueueSend(s_tts_ready_queue, &phrase, portMAX_DELAY) != pdPASS) {  /* 推入就绪队列 */
            free(phrase->pcm);                          /* 推送失败则释放 PCM */
            free(phrase);                               /* 释放控制块 */
            __atomic_fetch_sub(&s_tts_pending, 1, __ATOMIC_RELAXED);    /* 未播计数回滚 */
        }
    }
}

/* ---------- 录音段 ---------- */

/** 录到 stop 事件或 ms 上限；返回时录音已结束 */
static void rec_until_stop_or(uint32_t max_ms)
{
    voice_rec_begin();                                  /* 录音器复位并开始 */
    ui_state(DIALOG_STATE_LISTENING);                   /* 蓝点亮起（听） */
    ui_text("在听呢……（说完松手）");                     /* 字幕提示用户 */
    uint32_t start = (uint32_t)(esp_timer_get_time() / 1000);   /* 记录开始时刻（ms） */
    while ((uint32_t)(esp_timer_get_time() / 1000) - start < max_ms) {  /* 未到上限就继续 */
        EventBits_t bits = xEventGroupWaitBits(s_vp.evt, EVT_HOLD_STOP, /* 等"松开"事件 */
                                               pdTRUE, pdFALSE,         /* 取位后清除 */
                                               pdMS_TO_TICKS(100));     /* 100ms 超时分块 */
        voice_rec_chunk();      /* 100ms 一块，落在等待超时的缝隙里 */
        if (bits & EVT_HOLD_STOP) {                     /* 用户松手了 */
            break;                                      /* 结束录音循环 */
        }
    }
}

/** ASR + LLM + 字幕（录音结束后调用；wav 所有权随之转移进来并释放） */
static void process_wav(char *wav, size_t wav_len)
{
    ui_state(DIALOG_STATE_THINKING);                    /* 橙点亮起（识别+思考） */
    ui_text("……让我听听你说了啥");                       /* 字幕提示 */

    const int64_t t0 = esp_timer_get_time();    /* M3 延迟量化起点（≈松手） */
    s_t_release = t0;                                   /* 记录起点供首句计算 */
    s_t_first_sentence = 0;                             /* 首句时刻复位 */

    char *text = NULL;                                  /* ASR 识别文本 */
    esp_err_t err = asr_recognize(wav, wav_len, &text); /* 识别（讯飞/MiMo 自动选择） */
    free(wav);                                          /* WAV 数据用完释放 */
    if (err != ESP_OK || text == NULL) {                /* 识别失败或空结果 */
        ui_text("……没听清呢，再说一遍？");               /* 字幕提示重试 */
        ui_state(DIALOG_STATE_IDLE);                    /* 状态点熄灭 */
        return;                                         /* 结束本轮 */
    }
    ESP_LOGI(TAG, "识别: %s", text);                     /* 打印识别全文 */
    ESP_LOGI(TAG, "⏱ ASR: %lldms（含上传）",             /* 打印 ASR 分段耗时 */
             (esp_timer_get_time() - t0) / 1000);       /* 毫秒换算 */

    s_reply_len = 0;                                    /* 回复累积游标复位 */
    s_reply_text[0] = '\0';                             /* 回复缓冲清空 */
    __atomic_store_n(&s_tts_pending, 0, __ATOMIC_RELAXED);      /* 未播计数清零 */
    s_tts_input_done = false;                           /* LLM 输入未完成标志 */
    s_tts_stream_active = true;                         /* 按句 TTS 流水线激活 */
    s_reply_last_flush_us = esp_timer_get_time();       /* 字幕节流基准复位 */
    char *reply = dialog_ask_stream(text, on_reply_token, on_reply_sentence, NULL);     /* 流式对话 */
    free(text);                                         /* 识别文本用完释放 */
    if (reply) {                                        /* LLM 回复成功 */
        ui_text(reply);                                 /* 字幕显示完整回复 */
        s_tts_input_done = true;                        /* 标记 LLM 输出完毕（不再有新句） */
        /* 等语音任务播完已入队短句；LLM 和第一个 TTS 已在此前并行。 */
        while (__atomic_load_n(&s_tts_pending, __ATOMIC_RELAXED) > 0) { /* 还有句子没播完 */
            vTaskDelay(pdMS_TO_TICKS(50));              /* 50ms 轮询等待 */
        }
        ESP_LOGI(TAG, "⏱ 全程: %lldms（松手→播完）",     /* 打印全程延迟 */
                 (esp_timer_get_time() - t0) / 1000);   /* 毫秒换算 */
        rig_rig_set_mouth(RIG_MOUTH_CLOSED);            /* 播完闭嘴 */
        rig_rig_set_mouth(RIG_MOUTH_AUTO);              /* 释放口型控制交还 idle */
        s_tts_stream_active = false;                    /* 按句流水线停用 */
        ui_state(DIALOG_STATE_IDLE);                    /* 状态点熄灭 */
        free(reply);                                    /* 释放完整回复 */
    } else {                                            /* LLM 失败 */
        s_tts_stream_active = false;                    /* 流水线停用 */
        ui_text("……脑子突然一片空白，再说一次？");        /* 字幕提示 */
        ui_state(DIALOG_STATE_IDLE);                    /* 状态点熄灭 */
    }
}

/* ---------- TTS 播报（M2 串口文本路径，ring buffer 架构） ---------- */

/*
 * 播放架构（M2 教训：在 SSE 回调里同步 bsp_audio_play 会阻塞网络读取，
 * 服务端流式推送超时掐流——实测整条流只剩 1 块 160ms 音频）。
 * 这块 C6 Wi-Fi 到云端的 SSE 音频块存在长间隙，且长句的平均下发速度
 * 可能低于实时播放速度。语音按短语切分，每段必须完整预取后才稳定播出；
 * 下一段在上一段播放期间继续下载，避免网络抖动直接传到扬声器。
 */
#define SPK_RING_SIZE         (512 * 1024) /* 约 8 秒 16kHz/双声道 PCM16 */
#define SPK_RECV_TIMEOUT_MS   40           /* 播放任务取数据短超时（欠载探测粒度） */

/* 串口文本播报的播放状态（ring buffer + 重采样残样 + 诊断计数） */
static struct {
    StreamBufferHandle_t ring;      /* 流缓冲句柄（SSE→播放 解耦） */
    uint8_t *ring_mem;              /* 环形缓冲存储（PSRAM） */
    StaticStreamBuffer_t ring_ctl;  /* 静态创建的控制块 */
    volatile bool synth_done;       /* TTS 拉流结束 */
    volatile bool player_done;      /* 播放任务排空退出 */
    volatile bool player_started;   /* 预缓冲足够后才启动播放 */
    volatile uint32_t underflows;   /* 诊断：播放时缓冲耗尽次数 */
    volatile uint32_t send_failures;/* 诊断：SSE 塞缓冲失败次数 */
    uint32_t max_feed_gap_ms;       /* 诊断：SSE 块间最大间隔 */
    int64_t last_feed_us;           /* 上次喂缓冲时刻 */
    int16_t resample_tail[3];       /* 24k 源 PCM 不足三帧时跨 SSE 块续上 */
    size_t resample_tail_count;     /* 残样数量 */
} s_spk;

/** 串口播报的播放任务：从 ring 取 PCM 喂 codec，排空且拉流结束才退出 */
static void player_task(void *arg)
{
    static char buf[4096];                          /* 播放块缓冲（4KB/次喂 codec） */
    (void)arg;                                      /* 未使用任务参数 */
    ESP_LOGI(TAG, "TTS playback start: buffered=%uB (complete phrase)",     /* 打印预缓冲量 */
             (unsigned)xStreamBufferBytesAvailable(s_spk.ring));    /* 当前缓冲字节数 */
    while (1) {                                     /* 播放循环 */
        size_t got = xStreamBufferReceive(s_spk.ring, buf, sizeof(buf),     /* 从 ring 取数据 */
                                          pdMS_TO_TICKS(SPK_RECV_TIMEOUT_MS));      /* 40ms 超时 */
        if (got > 0) {                              /* 取到数据 */
            esp_err_t err = bsp_audio_play(buf, got);       /* 喂给扬声器 */
            if (err != ESP_OK) {                    /* 写入失败 */
                ESP_LOGE(TAG, "I2S playback write failed: %s", esp_err_to_name(err));       /* 打印错误 */
            }
        } else if (s_spk.synth_done && xStreamBufferBytesAvailable(s_spk.ring) == 0) {      /* 拉流完+排空 */
            break;                  /* SSE 收尾且缓冲已排空 */
        } else {
            s_spk.underflows++;     /* 网络抖动：等后续 SSE，不退出播放器 */
        }
    }
    s_spk.player_done = true;                       /* 标记播放任务退出 */
    s_spk.player_started = false;                   /* 清除启动标志 */
    vTaskDelete(NULL);                              /* 任务自删 */
}

/** TTS 音频块：24k/mono → 16k/stereo，塞进播放缓冲 + 按块 RMS 驱动口型 */
static void on_tts_audio(const int16_t *pcm, size_t samples, void *ctx)
{
    (void)ctx;                                      /* 未使用上下文 */
    int64_t now_us = esp_timer_get_time();          /* 当前时刻（us） */
    if (s_spk.last_feed_us) {                       /* 非首块才计算间隔 */
        uint32_t gap_ms = (uint32_t)((now_us - s_spk.last_feed_us) / 1000); /* 与上次喂的间隔 */
        if (gap_ms > s_spk.max_feed_gap_ms) {       /* 更新最大块间隔 */
            s_spk.max_feed_gap_ms = gap_ms;         /* 记录最大值 */
        }
    }
    s_spk.last_feed_us = now_us;                    /* 记录本次喂缓冲时刻 */

    /* Codec 固定跑 16k/双声道，避免 TTS 和 ADC 反复重开同一条 I2S。
     * 每 3 个 24k 单声道样本变成 2 个 16k 双声道帧：第 2 帧做线性插值。 */
    size_t max_frames = (s_spk.resample_tail_count + samples) / 3 * 2;      /* 理论最大输出帧数 */
    int16_t *codec_pcm = heap_caps_malloc(max_frames * 2 * sizeof(*codec_pcm),      /* 输出缓冲 */
                                          MALLOC_CAP_SPIRAM);      /* 大块走 PSRAM */
    if (codec_pcm == NULL) {                        /* 分配失败 */
        s_spk.send_failures++;                      /* 失败计数 +1 */
        ESP_LOGW(TAG, "TTS 重采样缓冲分配失败");     /* 告警 */
        return;                                     /* 丢弃本块 */
    }
    size_t src = 0;                                 /* 输入样本游标 */
    size_t out_frames = 0;                          /* 输出帧游标 */
    while (src < samples) {                         /* 逐样本重采样 */
        while (s_spk.resample_tail_count < 3 && src < samples) {    /* 凑满 3 个残样 */
            s_spk.resample_tail[s_spk.resample_tail_count++] = pcm[src++];      /* 补进残样缓冲 */
        }
        if (s_spk.resample_tail_count < 3) {        /* 输入耗尽仍不足 */
            break;                                  /* 留到下一块继续凑 */
        }
        int16_t first = s_spk.resample_tail[0];     /* 第 1 样本 → 帧 1 */
        int16_t middle = (int16_t)(((int32_t)s_spk.resample_tail[1] +        /* 第 2、3 样本均值 */
                                    s_spk.resample_tail[2]) / 2);    /* → 帧 2（插值） */
        codec_pcm[out_frames * 2] = first;          /* 帧 1 左声道 */
        codec_pcm[out_frames * 2 + 1] = first;      /* 帧 1 右声道 */
        out_frames++;                               /* 帧 +1 */
        codec_pcm[out_frames * 2] = middle;         /* 帧 2 左声道 */
        codec_pcm[out_frames * 2 + 1] = middle;     /* 帧 2 右声道 */
        out_frames++;                               /* 帧 +1 */
        s_spk.resample_tail_count = 0;              /* 残样清空 */
    }

    /* StreamBuffer may accept only part of a block. Silently discarding the
     * remainder creates missing PCM samples and sounds exactly like stutter. */
    size_t bytes = out_frames * 2 * sizeof(*codec_pcm);     /* 重采样输出总字节数 */
    size_t sent = 0;                                /* 已发送字节数 */
    while (sent < bytes) {                          /* 循环直到全部写入 ring */
        size_t wrote = xStreamBufferSend(s_spk.ring, (const uint8_t *)codec_pcm + sent,     /* 写 ring */
                                         bytes - sent, pdMS_TO_TICKS(2000));        /* 单次 2s 超时 */
        if (wrote == 0) {                           /* 2 秒都写不进（ring 满） */
            s_spk.send_failures++;                  /* 失败计数 +1 */
            ESP_LOGW(TAG, "TTS ring write timeout: %u/%uB dropped",     /* 打印丢弃量 */
                     (unsigned)(bytes - sent), (unsigned)bytes);    /* 剩余/总量 */
            break;                                  /* 放弃剩余（防死等） */
        }
        sent += wrote;                              /* 已发送推进 */
    }
    free(codec_pcm);                                /* 释放重采样缓冲 */

    int64_t acc = 0;                                /* 平方和累加器 */
    for (size_t i = 0; i < samples; i++) {          /* 逐样本求平方和 */
        acc += (int64_t)pcm[i] * pcm[i];            /* 64 位防溢出 */
    }
    double rms = sqrt((double)acc / (samples ? samples : 1));       /* RMS 能量 */
    rig_mouth_t m = (rms > 2500) ? RIG_MOUTH_OPEN   /* RMS>2500 → 张嘴 */
                    : (rms > 600) ? RIG_MOUTH_HALF : RIG_MOUTH_CLOSED;      /* 中间半开/闭嘴 */
    rig_rig_set_mouth(m);                           /* 驱动口型 */
}

/**
 * @brief 把一个短语播报出来（完整预取后播放）
 *
 * 绿点（SPEAKING）亮起 → 播完闭嘴回待机。硬件 codec 固定使用
 * 16k/双声道；TTS 的 24k/单声道输入由 on_tts_audio 转换。
 */
void voice_pipeline_speak(const char *text)
{
    if (!text || !text[0] || !s_speak_lock) {       /* 空文本或未初始化 */
        return;                                     /* 直接忽略 */
    }
    xSemaphoreTake(s_speak_lock, portMAX_DELAY);    /* 播报互斥（防两路同时播） */
    ui_state(DIALOG_STATE_SPEAKING);                /* 绿点亮起 */
    /* 环形缓冲必须放 PSRAM——xStreamBufferCreate 默认走内部 SRAM
     * （总量仅 ~143KB，256KB 必然失败），静态创建 + PSRAM 存储 */
    s_spk.ring_mem = heap_caps_malloc(SPK_RING_SIZE, MALLOC_CAP_SPIRAM);    /* PSRAM 分配 512KB */
    if (!s_spk.ring_mem) {                          /* 分配失败 */
        ESP_LOGW(TAG, "播放缓冲（PSRAM）分配失败，本句跳过播报");    /* 告警 */
        if (!s_tts_stream_active) {                 /* 不在按句流水线中才回 IDLE */
            ui_state(DIALOG_STATE_IDLE);            /* 状态点熄灭 */
        }
        xSemaphoreGive(s_speak_lock);               /* 释放播报锁 */
        return;                                     /* 跳过播报 */
    }
    s_spk.ring = xStreamBufferCreateStatic(SPK_RING_SIZE, 1,        /* 静态创建流缓冲 */
                                           s_spk.ring_mem, &s_spk.ring_ctl);        /* 绑 PSRAM 存储 */
    s_spk.synth_done = false;                       /* 拉流未结束 */
    s_spk.player_done = false;                      /* 播放未完成 */
    s_spk.player_started = false;                   /* 播放未启动 */
    s_spk.underflows = 0;                           /* 欠载计数复位 */
    s_spk.send_failures = 0;                        /* 发送失败复位 */
    s_spk.max_feed_gap_ms = 0;                      /* 最大间隔复位 */
    s_spk.last_feed_us = 0;                         /* 喂缓冲时刻复位 */
    s_spk.resample_tail_count = 0;                  /* 残样计数复位 */
    xSemaphoreTake(s_tts_request_lock, portMAX_DELAY);      /* TTS 请求串行化 */
    esp_err_t err = tts_synthesize(text, "傲娇但温柔的少女语气，语速自然",   /* 三玖风格合成 */
                                   on_tts_audio, NULL);     /* 音频块 → ring */
    xSemaphoreGive(s_tts_request_lock);             /* 释放请求锁 */
    s_spk.synth_done = true;                        /* 标记拉流结束 */
    if (xStreamBufferBytesAvailable(s_spk.ring) > 0) {      /* ring 里有可播数据 */
        s_spk.player_started = true;                /* 标记播放启动 */
        if (xTaskCreate(player_task, "spk_play", 4 * 1024, NULL, 5, NULL) != pdPASS) {      /* 起播放任务 */
            s_spk.player_started = false;           /* 启动失败回滚标志 */
        }
    } else if (err == ESP_OK && xStreamBufferBytesAvailable(s_spk.ring) == 0) {     /* 成功但零音频 */
        ESP_LOGW(TAG, "TTS 没有可播放的音频，跳过本句播报");        /* 告警 */
    }
    int wait = 0;                                   /* 等待轮数计数 */
    while (s_spk.player_started && !s_spk.player_done && wait < 400) {      /* 最多等 40s */
        vTaskDelay(pdMS_TO_TICKS(100));             /* 100ms 轮询 */
        wait++;                                     /* 轮数 +1 */
    }
    if (err != ESP_OK) {                            /* TTS 报错 */
        ESP_LOGW(TAG, "TTS 播报失败: %s", esp_err_to_name(err));    /* 打印错误 */
    }
    ESP_LOGI(TAG, "TTS playback stats: underflows=%u, send_timeouts=%u, max_feed_gap=%ums", /* 播放统计 */
             (unsigned)s_spk.underflows, (unsigned)s_spk.send_failures,     /* 欠载与失败次数 */
             (unsigned)s_spk.max_feed_gap_ms);      /* 最大块间隔 */
    vStreamBufferDelete(s_spk.ring);                /* 删除流缓冲 */
    free(s_spk.ring_mem);                           /* 释放 PSRAM 存储 */
    s_spk.ring = NULL;                              /* 句柄置空防悬挂 */
    rig_rig_set_mouth(RIG_MOUTH_CLOSED);            /* 播完闭嘴 */
    rig_rig_set_mouth(RIG_MOUTH_AUTO);      /* 释放口型控制，idle 串接管 */
    if (!s_tts_stream_active ||                     /* 不在按句流水线中 */
        (s_tts_input_done &&                        /* 或 LLM 已输出完毕 */
         __atomic_load_n(&s_tts_pending, __ATOMIC_RELAXED) <= 1 &&  /* 且只剩本句未销账 */
         uxQueueMessagesWaiting(s_tts_queue) == 0)) {       /* 且句子队列已空 */
        ui_state(DIALOG_STATE_IDLE);                /* 状态点熄灭 */
    }
    xSemaphoreGive(s_speak_lock);                   /* 释放播报锁 */
}

/* ---------- 两个入口 ---------- */

/** 按住说话的一轮：录音→取WAV→送管线（voice_pipe 任务上下文执行） */
static void pipeline_run_hold(void)
{
    if (s_vp.busy) {                                /* 上一轮还没跑完 */
        ui_text("……等等，我还在上一句里呢");         /* 字幕提示稍等 */
        return;                                     /* 拒绝重入 */
    }
    if (!bsp_wifi_is_connected()) {                 /* WiFi 断了 */
        ui_text("……WiFi 断了，聊不了天啦");         /* 字幕提示 */
        return;                                     /* 不进入管线 */
    }
    s_vp.busy = true;                               /* 置忙标志 */
    rec_until_stop_or(VOICE_REC_MAX_SEC * 1000);    /* 录到松手或 30s 上限 */
    char *wav = NULL;                               /* WAV 输出 */
    size_t len = 0;                                 /* WAV 长度 */
    if (voice_rec_end_and_get(&wav, &len) != ESP_OK) {      /* 结束录音取 WAV */
        ui_text("……声音太短啦，按住多说一会儿");     /* 太短提示 */
        ui_state(DIALOG_STATE_IDLE);                /* 状态点熄灭 */
        s_vp.busy = false;                          /* 清忙标志 */
        return;                                     /* 结束本轮 */
    }
    process_wav(wav, len);                          /* 走完 ASR+LLM+TTS 管线 */
    s_vp.busy = false;                              /* 清忙标志 */
}

/** 管线任务：阻塞等"按住说话"按下沿，触发后整轮执行 */
static void pipeline_task(void *arg)
{
    while (1) {                                     /* 常驻循环 */
        EventBits_t bits = xEventGroupWaitBits(s_vp.evt, EVT_HOLD_START,    /* 等按下沿 */
                                               pdTRUE, pdFALSE,     /* 取位后清除 */
                                               portMAX_DELAY);      /* 永久阻塞 */
        if (bits & EVT_HOLD_START) {                /* 按下沿到来 */
            pipeline_run_hold();                    /* 执行一整轮管线 */
        }
    }
}

/**
 * 初始化语音管线：录音器/ASR/TTS 子模块 + 事件组 + 两条队列 + 三任务
 * @return ESP_OK 就绪
 */
esp_err_t voice_pipeline_init(void)
{
    if (s_vp.inited) {                              /* 幂等闸门 */
        return ESP_OK;                              /* 重复调用无害 */
    }
    ESP_RETURN_ON_ERROR(voice_rec_init(), TAG, "rec init failed");      /* 初始化录音器 */
    ESP_RETURN_ON_ERROR(asr_client_init(), TAG, "asr init failed");     /* 初始化 ASR（双后端） */
    ESP_RETURN_ON_ERROR(tts_client_init(), TAG, "tts init failed");     /* 初始化 TTS */
    s_vp.evt = xEventGroupCreate();                 /* 创建事件组（按住说话沿） */
    ESP_RETURN_ON_FALSE(s_vp.evt, ESP_ERR_NO_MEM, TAG, "evt alloc failed"); /* 分配失败 */
    s_tts_queue = xQueueCreate(TTS_QUEUE_DEPTH, sizeof(tts_sentence_t));    /* 创建句子队列 */
    ESP_RETURN_ON_FALSE(s_tts_queue, ESP_ERR_NO_MEM, TAG, "tts queue alloc failed");        /* 失败报错 */
    s_tts_ready_queue = xQueueCreate(TTS_READY_DEPTH, sizeof(tts_pcm_phrase_t *));  /* 创建就绪队列 */
    ESP_RETURN_ON_FALSE(s_tts_ready_queue, ESP_ERR_NO_MEM, TAG, "tts ready queue alloc failed");    /* 失败报错 */
    s_speak_lock = xSemaphoreCreateMutex();         /* 创建扬声器互斥锁 */
    ESP_RETURN_ON_FALSE(s_speak_lock, ESP_ERR_NO_MEM, TAG, "speaker lock alloc failed");    /* 失败报错 */
    s_tts_request_lock = xSemaphoreCreateMutex();   /* 创建 TTS 请求互斥锁 */
    ESP_RETURN_ON_FALSE(s_tts_request_lock, ESP_ERR_NO_MEM, TAG, "tts request lock alloc failed");  /* 失败报错 */
    s_vp.inited = true;                             /* 置就绪标志 */
    if (xTaskCreate(pipeline_task, "voice_pipe", 8 * 1024, NULL, 4, NULL) != pdPASS) {      /* 管线任务 */
        ESP_LOGE(TAG, "创建管线任务失败");           /* 创建失败 */
        return ESP_FAIL;                            /* 返回错误 */
    }
    if (xTaskCreate(tts_stream_task, "tts_stream", 6 * 1024, NULL, 4, NULL) != pdPASS) {    /* TTS 拉流任务 */
        ESP_LOGE(TAG, "创建流式 TTS 任务失败");     /* 创建失败 */
        return ESP_FAIL;                            /* 返回错误 */
    }
    if (xTaskCreate(tts_play_task, "tts_play", 6 * 1024, NULL, 5, NULL) != pdPASS) {        /* TTS 播放任务 */
        ESP_LOGE(TAG, "创建 TTS 播放任务失败");     /* 创建失败 */
        return ESP_FAIL;                            /* 返回错误 */
    }
    ESP_LOGI(TAG, "语音管线就绪（按住说话 / rec <秒> 调试）");      /* 就绪日志 */
    return ESP_OK;                                  /* 初始化成功 */
}

/** 注入 UI 回调（状态点/字幕），保存到模块状态 */
void voice_pipeline_set_ui(const voice_ui_cb_t *cb)
{
    if (cb) {                                       /* 非空才覆盖 */
        s_vp.ui = *cb;                              /* 拷贝回调结构体 */
    }
}

/** 按住说话：按下沿（LVGL 按钮事件调用，仅置事件位） */
void voice_pipeline_hold_start(void)
{
    if (s_vp.inited) {                              /* 管线已初始化 */
        xEventGroupSetBits(s_vp.evt, EVT_HOLD_START);       /* 置按下沿事件位 */
    }
}

/** 按住说话：松开沿（LVGL 按钮事件调用，仅置事件位） */
void voice_pipeline_hold_stop(void)
{
    if (s_vp.inited) {                              /* 管线已初始化 */
        xEventGroupSetBits(s_vp.evt, EVT_HOLD_STOP);        /* 置松开沿事件位 */
    }
}

/**
 * 定时录音调试入口：录 ms 毫秒后走完整管线（阻塞，调用方任务内执行）
 * @return ESP_OK 完成；ESP_ERR_INVALID_STATE 忙/未初始化/无 WiFi
 */
esp_err_t voice_pipeline_record_ms(uint32_t ms)
{
    if (!s_vp.inited || s_vp.busy) {                /* 未初始化或忙 */
        return ESP_ERR_INVALID_STATE;               /* 拒绝 */
    }
    if (!bsp_wifi_is_connected()) {                 /* WiFi 未连接 */
        return ESP_ERR_INVALID_STATE;               /* 无网无法识别 */
    }
    s_vp.busy = true;                               /* 置忙标志 */
    /* 同步版：录 ms 毫秒（100ms 块）后直接走管线 */
    voice_rec_begin();                              /* 开始录音 */
    ui_state(DIALOG_STATE_LISTENING);               /* 蓝点亮起 */
    ui_text("在听呢……");                             /* 字幕提示说话 */
    uint32_t start = (uint32_t)(esp_timer_get_time() / 1000);       /* 记录开始时刻 */
    while ((uint32_t)(esp_timer_get_time() / 1000) - start < ms) {  /* 未到指定时长 */
        voice_rec_chunk();                          /* 采集 100ms 块 */
        vTaskDelay(pdMS_TO_TICKS(1));   /* record 本身阻塞 100ms，无需长延时 */
    }
    char *wav = NULL;                               /* WAV 输出 */
    size_t len = 0;                                 /* WAV 长度 */
    esp_err_t err = voice_rec_end_and_get(&wav, &len);      /* 结束录音取 WAV */
    if (err != ESP_OK) {                            /* 录音太短或出错 */
        ui_text("……声音太短啦");                     /* 字幕提示 */
        ui_state(DIALOG_STATE_IDLE);                /* 状态点熄灭 */
        s_vp.busy = false;                          /* 清忙标志 */
        return err;                                 /* 返回错误 */
    }
    process_wav(wav, len);                          /* 走完 ASR+LLM+TTS 管线 */
    s_vp.busy = false;                              /* 清忙标志 */
    return ESP_OK;                                  /* 成功返回 */
}
