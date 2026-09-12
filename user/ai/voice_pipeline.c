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
#include "cJSON.h"              /* 网关 tts 消息组包 */
#include "freertos/FreeRTOS.h"          /* FreeRTOS 基础 */
#include "freertos/task.h"              /* 任务创建 */
#include "freertos/event_groups.h"      /* 事件组（按住说话沿） */
#include "freertos/stream_buffer.h"     /* 流缓冲（播放环形缓冲） */
#include "freertos/queue.h"             /* 队列（句子/就绪短语） */

#include "bsp_wifi.h"           /* WiFi 连接状态查询 */
#include "bsp_audio.h"          /* 扬声器播放（bsp_audio_play） */
#include "music_service.h"      /* Phase4：TTS 抢占音乐（PRD M10 方案 A） */
#include "voice_rec.h"          /* 录音器 */
#include "asr_client.h"         /* ASR（讯飞/MiMo 双后端） */
#include "tts_client.h"         /* TTS（MiMo 流式合成） */
#include "doubao_tts.h"         /* TTS（豆包真流式，2026-09 方案 A） */
#include "gw_client.h"         /* 语音网关（步骤 2 流式 ASR 上行） */
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
#define EVT_ASR_RESULT  BIT2    /* 网关流式 ASR 结果到达（步骤 2） */
#define EVT_TTS_DONE    BIT3    /* 网关 TTS 推流结束（步骤 3） */
#define EVT_REPLY_DONE  BIT4    /* 网关 LLM 完整回复到达（步骤 4） */

/* 网关全流水线的回复状态（步骤 4） */
static char *s_gw_reply = NULL;         /* 网关完整回复（WS 任务写入，管线任务取走） */
static char s_gw_sub[2048];             /* 流式字幕累积（reply_sentence 逐句追加） */
static volatile bool s_gw_chat_err;     /* 网关 LLM 失败标志 */

/* 串口文本播报的播放状态（ring buffer + 重采样残样 + 诊断计数）。
 * 定义提前到 process_wav 之前——网关 TTS 路径（步骤 3）在 process_wav
 * 里直接复用 ring/player， Late 定义会编译不过 */
static struct {
    StreamBufferHandle_t ring;      /* 流缓冲句柄（SSE→播放 解耦） */
    uint8_t *ring_mem;              /* 环形缓冲存储（PSRAM） */
    StaticStreamBuffer_t ring_ctl;  /* 静态创建的控制块 */
    volatile bool synth_done;       /* TTS 拉流结束 */
    volatile bool abort;            /* 打断标志（barge-in：立即停播） */
    volatile bool player_done;      /* 播放任务排空退出 */
    volatile bool player_started;   /* 预缓冲足够后才启动播放 */
    volatile uint32_t underflows;   /* 诊断：播放时缓冲耗尽次数 */
    volatile uint32_t send_failures;/* 诊断：SSE 塞缓冲失败次数 */
    uint32_t max_feed_gap_ms;       /* 诊断：SSE 块间最大间隔 */
    int64_t last_feed_us;           /* 上次喂缓冲时刻 */
    int16_t resample_tail[3];       /* 24k 源 PCM 不足三帧时跨 SSE 块续上 */
    size_t resample_tail_count;     /* 残样数量 */
} s_spk;


/* 流式字幕刷新节流：两个 token 之间至少隔 100ms 才刷一次 LVGL */
#define FLUSH_MIN_US    (100 * 1000)

/* 管线主状态（按住说话 + 一轮防重入） */
static struct {
    bool inited;                /* 初始化完成标志（幂等闸门） */
    EventGroupHandle_t evt;     /* 事件组：EVT_HOLD_START/STOP */
    voice_ui_cb_t ui;           /* 注入的 UI 回调（状态点/字幕） */
    bool busy;                  /* 一轮管线进行中（防重入） */
} s_vp;

/* ---- 网关流式 ASR（步骤 2）：音频边录边上送，松手只等识别收尾 ---- */
static volatile bool s_use_gw_asr;      /* 本轮是否走网关 ASR（录音开始时按连接状态决定） */
static char *s_gw_asr_text = NULL;      /* 网关识别文本（WS 任务写入，管线任务取走并释放） */
static volatile bool s_gw_tts_end;      /* 网关 TTS 推流结束标志（WS 任务置位） */
static volatile bool s_gw_asr_err;      /* 网关 ASR 失败标志（区别于静音空结果） */

/* 前置声明（定义在文件后段，网关播报路径先用到） */
static void on_tts_audio(const int16_t *pcm, size_t samples, void *ctx);
static esp_err_t spk_ring_begin(void);
static void gw_tts_pcm_cb(const uint8_t *pcm, size_t bytes);
static void ui_text(const char *t);
static uint32_t s_gw_tts_fed = 0;   /* 诊断：本轮已喂 ring 的 PCM 字节 */
static void player_task(void *arg);
static volatile bool s_vad_mode = false;    /* VAD 连续对话模式（串口 vad on/off） */
static int s_vad_thresh = 250;              /* 语音 RMS 阈值（环境噪声上调） */

/** 网关文本消息处理器（WS 客户端任务上下文）：asr_result/tts_end → 置事件位 */
static void gw_msg_handler(const char *type, const char *data)
{
    if (strcmp(type, "asr_result") == 0) {              /* 识别结果到达 */
        free(s_gw_asr_text);                            /* 丢弃上一轮残留 */
        s_gw_asr_text = (data && data[0]) ? strdup(data) : NULL;        /* 拷贝（空结果=NULL） */
        xEventGroupSetBits(s_vp.evt, EVT_ASR_RESULT);   /* 唤醒等待方 */
    } else if (strcmp(type, "asr_error") == 0) {        /* 网关 ASR 失败（如讯飞握手抖动） */
        s_gw_asr_err = true;                            /* 让等待方立即回退本地识别，别干等 5s */
        free(s_gw_asr_text);
        s_gw_asr_text = NULL;
        xEventGroupSetBits(s_vp.evt, EVT_ASR_RESULT);
    } else if (strcmp(type, "reply_sentence") == 0) {   /* 步骤 4：LLM 断句流式字幕 */
        strlcat(s_gw_sub, data ? data : "", sizeof(s_gw_sub));
        ui_text(s_gw_sub);                              /* 直接刷字幕（桥自持锁，任意任务安全） */
    } else if (strcmp(type, "reply_done") == 0) {       /* 步骤 4：完整回复到达 */
        free(s_gw_reply);
        s_gw_reply = (data && data[0]) ? strdup(data) : NULL;
        ui_text(s_gw_reply ? s_gw_reply : s_gw_sub);    /* 定稿字幕 */
        xEventGroupSetBits(s_vp.evt, EVT_REPLY_DONE);
    } else if (strcmp(type, "chat_error") == 0) {       /* 步骤 4：网关 LLM 失败 */
        s_gw_chat_err = true;
        xEventGroupSetBits(s_vp.evt, EVT_REPLY_DONE);
    } else if (strcmp(type, "tts_end") == 0) {          /* 网关 TTS 推流结束（步骤 3） */
        s_gw_tts_end = true;                            /* 管线任务据它置 synth_done */
        xEventGroupSetBits(s_vp.evt, EVT_TTS_DONE);     /* 唤醒等待方 */
    }
}

static volatile bool s_vad_spoke = false;   /* 本轮录音里检测到人声 */
static bool s_vad_round = false;            /* 本轮是否 VAD 自动模式（无人声则跳过 ASR 等待） */
static volatile int64_t s_vad_last_voice = 0;   /* 最后一次人声时刻（ms，esp_timer） */

/** 录音块上行回调（voice_rec 块粒度 100ms/3200B → WS 二进制帧） */
static void gw_chunk_uplink(const int16_t *mono, size_t samples)
{
    gw_client_send_binary(mono, samples * sizeof(int16_t));     /* 块即帧，直接发 */
    if (s_vad_mode) {                                   /* VAD 连续模式：块级能量检测 */
        int64_t acc = 0;
        for (size_t i = 0; i < samples; i++) {
            int32_t v = mono[i];
            acc += (int64_t)v * v;
        }
        double rms = sqrt((double)acc / (samples ? samples : 1));
        if (rms >= s_vad_thresh) {                      /* 检测到人声 */
            s_vad_spoke = true;
            s_vad_last_voice = esp_timer_get_time() / 1000;
        }
    }
}

/** 网关 ASR 收尾：发 asr_stop 并等结果（最多 5s）；成功返回识别文本（调用方 free） */
static esp_err_t gw_asr_collect(char **text_out)
{
    *text_out = NULL;
    xEventGroupClearBits(s_vp.evt, EVT_ASR_RESULT);     /* 清残留位 */
    gw_client_send_text("{\"type\":\"asr_stop\"}");     /* 通知网关：录音结束 */
    EventBits_t bits = xEventGroupWaitBits(s_vp.evt, EVT_ASR_RESULT,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(12000));       /* 收尾+最后帧合成
                                           （网关连讯飞重试最长 ~17s，12s 内多数能回） */
    if ((bits & EVT_ASR_RESULT) && s_gw_asr_text) {     /* 拿到非空识别 */
        *text_out = s_gw_asr_text;                      /* 所有权转移 */
        s_gw_asr_text = NULL;
        return ESP_OK;
    }
    free(s_gw_asr_text);                                /* 清理 */
    s_gw_asr_text = NULL;
    if (bits & EVT_ASR_RESULT) {
        if (s_gw_asr_err) {
            s_gw_asr_err = false;
            return ESP_ERR_INVALID_STATE;               /* 网关报错：回退本地识别 */
        }
        return ESP_ERR_NOT_FOUND;                       /* 网关明确空结果（静音）：不回退 */
    }
    s_gw_asr_err = false;
    return ESP_ERR_TIMEOUT;                             /* 真超时：允许回退本地识别 */
}

/**
 * @brief 网关全流水线一轮（步骤 4）：LLM+TTS 都在网关侧，真并行
 *
 * 流程：上下文组装上行 → 网关流式 LLM（断句即流式字幕 + 逐句豆包合成）
 * → PCM 帧下行喂 ring。设备只管放音，LLM 生成期间 TTS 已在跑。
 *
 * @param user_text 用户输入（识别结果；仅读取）
 * @param reply_out 成功时输出完整回复文本（堆上，调用方 free）
 * @return true 全流程成功；false 失败（调用方回退本地流程）
 */
static bool gw_dialog_round(const char *user_text, char **reply_out)
{
    *reply_out = NULL;
    cJSON *ctx = dialog_build_gw_context();             /* 设备组装上下文（状态源在板上） */
    if (!ctx) {
        return false;
    }
    cJSON_AddStringToObject(ctx, "type", "chat");
    cJSON_AddStringToObject(ctx, "text", user_text);
    char *jtxt = cJSON_PrintUnformatted(ctx);
    cJSON_Delete(ctx);
    if (!jtxt) {
        return false;
    }
    s_gw_sub[0] = '\0';                                 /* 字幕累积复位 */
    s_gw_chat_err = false;
    free(s_gw_reply);                                   /* 上轮残留防御 */
    s_gw_reply = NULL;
    if (spk_ring_begin() != ESP_OK) {                   /* ring + 播放器先就位（PCM 随到随播） */
        cJSON_free(jtxt);
        return false;
    }
    music_notify_voice_start();                         /* TTS 抢占音乐 */
    esp_err_t serr = gw_client_send_text(jtxt);         /* 一条消息触发网关 LLM+TTS 流水线 */
    cJSON_free(jtxt);
    if (serr != ESP_OK) {
        ESP_LOGW(TAG, "网关 chat 上行失败");
        return false;
    }
    /* 等 TTS 推流结束（字幕在期间流式刷新；上限 90s 覆盖长回复） */
    EventBits_t bits = xEventGroupWaitBits(s_vp.evt, EVT_TTS_DONE,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(90000));
    bool ok = (bits & EVT_TTS_DONE) && !s_gw_chat_err && s_gw_reply;
    if (ok) {
        dialog_commit_gw_round(user_text, s_gw_reply);  /* 历史入环 + 摘要/日记素材 */
        *reply_out = s_gw_reply;                        /* 所有权转移 */
        s_gw_reply = NULL;
        ESP_LOGI(TAG, "网关 TTS 已喂 ring %uB", (unsigned)s_gw_tts_fed);
        s_spk.synth_done = true;                        /* 播放器排空 ring 后自退 */
        s_gw_tts_end = false;
        int wait_ms = 30000;                            /* 排空上限（防御） */
        while (!s_spk.player_done && wait_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait_ms -= 50;
        }
    } else {
        ESP_LOGW(TAG, "网关流水线未完成（err=%d reply=%p）",
                 s_gw_chat_err, s_gw_reply);
        free(s_gw_reply);
        s_gw_reply = NULL;
        s_spk.synth_done = true;                        /* 强制收尾播放器 */
        s_gw_tts_end = false;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (s_spk.ring) {                                   /* 会话资源清理 */
        vStreamBufferDelete(s_spk.ring);
        s_spk.ring = NULL;
    }
    if (s_spk.ring_mem) {
        free(s_spk.ring_mem);
        s_spk.ring_mem = NULL;
    }
    return ok;
}

/* ---- 按句 TTS 预取流水线的常量与数据结构 ---- */
#define TTS_SENTENCE_MAX  512   /* 单句文本上限（bytes，UTF-8） */
#define TTS_QUEUE_DEPTH   8     /* 句子队列深度（LLM → TTS 拉流） */
#define TTS_READY_DEPTH    4    /* 就绪队列深度（TTS → 播放，按序） */

/** 一句待合成的文本（LLM 断句回调的产物，在队列里排队） */
typedef struct {
    char text[TTS_SENTENCE_MAX];    /* 句子文本（'\0' 结尾） */
} tts_sentence_t;

/* 攒批合成（2026-09-12 用户实测"短句一句一句往外蹦"）：
 * 每句单独请求 TTS 首块要 ~2.4s，短句只播 1~2s，句间必出空洞。
 * 攒多句合成一批（首批 ~30 字保首响，后续 ~60 字），单批可播 10 秒+，
 * 下一批在播放期间早已就绪，听感连续。 */
#define TTS_BATCH_FIRST_BYTES   90      /* 首批阈值（≈30 字，兼顾首响延迟） */
#define TTS_BATCH_BYTES         180     /* 后续批次阈值（≈60 字） */
static char s_tts_batch[TTS_SENTENCE_MAX];      /* 批次累积缓冲（未凑满批的句子先攒着） */
static size_t s_tts_batch_len;                  /* 批次当前长度 */
static int s_tts_batch_count;                   /* 已发批次数（决定首启阈值） */

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
static volatile bool s_tts_cancel;      /* 音乐点播抢占：未播 TTS 全部丢弃（三处闸门共用） */
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

/** 把攒好的批次推入 TTS 队列（on_reply_sentence 攒批 / process_wav 收尾共用） */
static void tts_batch_flush(void)
{
    if (s_tts_batch_len == 0 || s_tts_cancel) {         /* 空批或已被音乐点播取消 */
        s_tts_batch_len = 0;                            /* 批次作废 */
        return;
    }
    tts_sentence_t item = {0};                          /* 构造句子队列项 */
    memcpy(item.text, s_tts_batch, s_tts_batch_len + 1);    /* 批次文本（攒入时已保证放得下） */
    __atomic_fetch_add(&s_tts_pending, 1, __ATOMIC_RELAXED);    /* 未播计数 +1（先加防竞态） */
    if (xQueueSend(s_tts_queue, &item, pdMS_TO_TICKS(50)) == pdPASS) {  /* 投递到句子队列 */
        ESP_LOGI(TAG, "TTS 批次入队 (%uB): %.60s",      /* 打印入队的批次 */
                 (unsigned)s_tts_batch_len, item.text);
    } else {                                            /* 队列满（50ms 都没等到空位） */
        __atomic_fetch_sub(&s_tts_pending, 1, __ATOMIC_RELAXED);        /* 回滚计数 */
        ESP_LOGW(TAG, "TTS 短句队列已满，本批未播报");   /* 告警丢批 */
    }
    s_tts_batch_len = 0;                                /* 批次清空 */
    s_tts_batch_count++;                                /* 批次数推进（后续用大批阈值） */
}

/** 句末回调在 LLM 网络任务上下文执行，只投递，绝不在这里请求 TTS。 */
static void on_reply_sentence(const char *sentence, void *ctx)
{
    (void)ctx;                                          /* 未使用上下文 */
    /* 空句/纯空白句过滤：空白句进了 TTS 队列会触发 "text cannot be
     * empty" 的无效调用（M6 实测），且没有任何播报价值 */
    const char *p = sentence;                           /* 扫描指针 */
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {       /* 跳过前导空白 */
        p++;                                            /* 前进 */
    }
    if (p[0] == '\0' || !s_tts_queue) {                 /* 全空白或管线未初始化 */
        return;                                         /* 直接忽略 */
    }
    if (s_tts_cancel) {                                 /* 用户已显式点播音乐 */
        return;                                         /* 本句及其后句子一律不入队 */
    }
    /* M3 延迟量化：LLM 首句耗时（只记一次） */
    if (s_t_first_sentence == 0) {                      /* 首句只记一次时刻 */
        s_t_first_sentence = esp_timer_get_time();      /* 记录首句时刻 */
        ESP_LOGI(TAG, "⏱ LLM 首句: %lldms",             /* 打印松手→首句延迟 */
                 (s_t_first_sentence - s_t_release) / 1000);    /* 毫秒换算 */
    }
    tts_sentence_t item = {0};                          /* 构造句子队列项 */
    /* 攒批优先：句子先进批次缓冲，凑满阈值或 LLM 收尾时才整批发送。
     * 单句超长兜底：放不进批次缓冲时按旧逻辑直接入队（strlcpy 截断保护）。 */
    size_t slen = strlen(sentence);                     /* 本句字节数 */
    if (s_tts_batch_len + slen >= sizeof(s_tts_batch) - 1) {    /* 再放一句就溢出 */
        tts_batch_flush();                              /* 先把现有批次发走腾位置 */
    }
    if (s_tts_batch_len + slen < sizeof(s_tts_batch) - 1) {     /* 批次攒得下 */
        memcpy(s_tts_batch + s_tts_batch_len, sentence, slen);  /* 追加本句 */
        s_tts_batch_len += slen;                        /* 长度推进 */
        s_tts_batch[s_tts_batch_len] = '\0';            /* 维护结尾 */
    } else {                                            /* 单句就超缓冲：直接入队 */
        strlcpy(item.text, sentence, sizeof(item.text));    /* 拷贝句子（截断保护） */
        __atomic_fetch_add(&s_tts_pending, 1, __ATOMIC_RELAXED);    /* 未播计数 +1（先加防竞态） */
        if (xQueueSend(s_tts_queue, &item, pdMS_TO_TICKS(50)) == pdPASS) {  /* 投递到句子队列 */
            ESP_LOGI(TAG, "LLM 超长句直发入队: %.80s", item.text);      /* 打印入队的句子 */
        } else {                                        /* 队列满（50ms 都没等到空位） */
            __atomic_fetch_sub(&s_tts_pending, 1, __ATOMIC_RELAXED);        /* 回滚计数 */
            ESP_LOGW(TAG, "TTS 短句队列已满，本句未播报");   /* 告警丢句 */
        }
    }
    size_t threshold = (s_tts_batch_count == 0) ? TTS_BATCH_FIRST_BYTES : TTS_BATCH_BYTES;   /* 首批小阈值保首响 */
    if (s_tts_batch_len >= threshold) {                 /* 攒够一批了 */
        tts_batch_flush();                              /* 整批发送 */
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
        if (s_tts_cancel) {                             /* 音乐点播抢占：丢弃未播短语 */
            free(phrase->pcm);                          /* 释放 PCM */
            free(phrase);                               /* 释放控制块 */
            __atomic_fetch_sub(&s_tts_pending, 1, __ATOMIC_RELAXED);    /* 未播计数 -1 */
            continue;                                   /* 取下一条（同样会被丢弃） */
        }
        xSemaphoreTake(s_speak_lock, portMAX_DELAY);    /* 拿扬声器互斥锁（防两路同时播） */
        /* Phase4：TTS 抢占音乐（PRD M10 方案 A）——第一句播出前停掉音乐并等其
         * 恢复 16k 采样率（音乐播放时 I2S 可能切在 44.1k/48k，直接播 TTS 会变调）。
         * 无音乐时该调用立即返回，零开销。 */
        music_notify_voice_start();
        ui_state(DIALOG_STATE_SPEAKING);                /* 绿点亮起（说话中） */
        ESP_LOGI(TAG, "TTS playback start: buffered=%uB (complete phrase)",     /* 打印短语信息 */
                 (unsigned)phrase->len);                /* 预取完成的字节数 */
        size_t offset = 0;                              /* 播放偏移游标 */
        while (offset < phrase->len) {                  /* 循环播完整个短语 */
            size_t chunk = phrase->len - offset;        /* 剩余字节数 */
            if (chunk > 4096) {                         /* 每次 4KB 喂 codec */
                chunk = 4096;                           /* 限块大小（DMA 友好） */
            }
            if (s_tts_cancel) {                         /* 中途被音乐点播打断：立即停口 */
                ESP_LOGI(TAG, "TTS 播报被音乐点播打断");  /* 诊断日志 */
                break;                                  /* 跳出写循环（下方统一释放） */
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
        if (s_tts_cancel) {                             /* 音乐点播抢占：不再请求合成 */
            __atomic_fetch_sub(&s_tts_pending, 1, __ATOMIC_RELAXED);    /* 未播计数 -1 */
            continue;                                   /* 取下一句（同样会被丢弃） */
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
    /* 清掉残留的松开沿：上一轮"按下时 WiFi 未连接被拒"的场景里，
     * 松手沿（STOP 位）置位后无人消费——不清除会让下一轮录音
     * 刚 begin 就"秒松手"，只录到 100ms 静音（M5 用户实测 bug） */
    xEventGroupClearBits(s_vp.evt, EVT_HOLD_STOP);
    voice_rec_begin();                                  /* 录音器复位并开始 */
    /* 步骤 2：网关在线则走流式 ASR——块回调即 WS 上行，识别与录音并行 */
    s_use_gw_asr = gw_client_is_connected();
    if (s_use_gw_asr) {
        xEventGroupClearBits(s_vp.evt, EVT_ASR_RESULT); /* 清上轮结果位 */
        s_gw_asr_err = false;
        gw_client_send_text("{\"type\":\"asr_start\",\"fmt\":\"pcm16k\"}");
        voice_rec_set_chunk_cb(gw_chunk_uplink);        /* 挂块上行回调 */
        ESP_LOGI(TAG, "本轮 ASR 走网关流式");
    }
    ui_state(DIALOG_STATE_LISTENING);                   /* 蓝点亮起（听） */
    ui_text("在听呢……（说完松手）");                     /* 字幕提示用户 */
    uint32_t start = (uint32_t)(esp_timer_get_time() / 1000);   /* 记录开始时刻（ms） */
    s_vad_round = s_vad_mode && s_use_gw_asr;           /* VAD 连续模式（仅网关 ASR 时启用） */
    bool vad_round = s_vad_round;
    s_vad_spoke = false;                                /* 本轮人声标志复位 */
    s_vad_last_voice = 0;
    if (vad_round) {
        max_ms = 8000;                                  /* 单轮监听上限 8s（无声自动收） */
    }
    while ((uint32_t)(esp_timer_get_time() / 1000) - start < max_ms) {  /* 未到上限就继续 */
        EventBits_t bits = xEventGroupWaitBits(s_vp.evt, EVT_HOLD_STOP, /* 等"松开"事件 */
                                               pdTRUE, pdFALSE,         /* 取位后清除 */
                                               pdMS_TO_TICKS(100));     /* 100ms 超时分块 */
        voice_rec_chunk();      /* 100ms 一块，落在等待超时的缝隙里 */
        if (bits & EVT_HOLD_STOP) {                     /* 用户松手了 */
            break;                                      /* 结束录音循环 */
        }
        if (vad_round && s_vad_spoke &&                 /* VAD：说完静音 800ms 自动断句 */
            (esp_timer_get_time() / 1000) - s_vad_last_voice > 800) {
            ESP_LOGI(TAG, "VAD 断句（说完静音 800ms）");
            break;
        }
    }
    if (s_use_gw_asr) {                                 /* 录音结束：撤回调 + 通知网关 */
        voice_rec_set_chunk_cb(NULL);
        gw_client_send_text("{\"type\":\"asr_stop\"}");
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
    esp_err_t err;
    if (s_use_gw_asr && s_vad_round && !s_vad_spoke) {  /* VAD 静音轮：没人说话，不等网关 */
        ESP_LOGI(TAG, "VAD 静音轮：无人声，直接续听");
        free(wav);
        ui_text("……（安静着呢，说话我就接）");
        ui_state(DIALOG_STATE_IDLE);
        return;
    }
    if (s_use_gw_asr) {                                 /* 网关流式 ASR（步骤 2）：音频已边录边传 */
        esp_err_t gerr = gw_asr_collect(&text);
        if (gerr == ESP_OK && text && text[0]) {
            err = ESP_OK;                               /* 网关识别成功 */
        } else if (gerr == ESP_ERR_INVALID_STATE) {     /* 网关报错：回退本地识别 */
            free(text);
            free(wav);                                  /* WAV 用完释放 */
            ui_text("……好像没听到我说话？再说一遍？");   /* 字幕提示重试 */
            ui_state(DIALOG_STATE_IDLE);                /* 状态点熄灭 */
            ESP_LOGW(TAG, "网关 ASR 空结果（静音），跳过本地回退");
            return;                                     /* 结束本轮（不白等本地识别 6s） */
        } else {                                        /* 真超时/故障：回退本地 HTTP 识别 */
            free(text);
            text = NULL;
            ESP_LOGW(TAG, "网关 ASR 超时，回退本地 HTTP 识别");
            err = asr_recognize(wav, wav_len, &text);
        }
    } else {                                            /* 网关不在线：原有整段 HTTP 识别 */
        err = asr_recognize(wav, wav_len, &text);
    }
    free(wav);                                          /* WAV 数据用完释放 */
    if (err != ESP_OK || text == NULL) {                /* 识别失败或空结果 */
        ui_text("……没听清呢，再说一遍？");               /* 字幕提示重试 */
        ui_state(DIALOG_STATE_IDLE);                    /* 状态点熄灭 */
        return;                                         /* 结束本轮 */
    }
    ESP_LOGI(TAG, "识别: %s", text);                     /* 打印识别全文 */
    ESP_LOGI(TAG, "⏱ ASR: %lldms（含上传）",             /* 打印 ASR 分段耗时 */
             (esp_timer_get_time() - t0) / 1000);       /* 毫秒换算 */

    /* ---- 步骤 4：网关全流水线（LLM+TTS 在网关侧，真并行）优先 ---- */
    char *reply = NULL;
    bool done_gw = false;
    if (gw_client_is_connected()) {
        ui_state(DIALOG_STATE_THINKING);                /* 橙色（LLM 思考中） */
        done_gw = gw_dialog_round(text, &reply);
    }
    if (!done_gw) {                                     /* 本地回退：原 dialog + 播报全流程 */
        s_reply_len = 0;                                /* 回复累积游标复位 */
        s_reply_text[0] = '\0';                         /* 回复缓冲清零 */
        s_tts_batch_len = 0;                            /* 批次缓冲复位（上轮残留防御） */
        s_tts_batch_count = 0;                          /* 批次数复位（首批回到小阈值） */
        __atomic_store_n(&s_tts_pending, 0, __ATOMIC_RELAXED);      /* 未播计数清零 */
        s_tts_input_done = false;                       /* LLM 输入未完成标志 */
        s_tts_cancel = false;                           /* 复位取消闸门（上轮音乐抢占遗留） */
        s_tts_stream_active = true;                     /* 按句 TTS 流水线激活 */
        s_reply_last_flush_us = esp_timer_get_time();   /* 字幕节流基准复位 */
        /* 豆包启用时走环形缓冲真流式整句播报（边合成边播，首声 ~1s）；
         * 按句流水线对豆包无意义（TLS 锁下 TTS 本来就要等 LLM 结束）。 */
        bool db_stream = doubao_tts_configured();
        reply = dialog_ask_stream(text, on_reply_token,
                                  db_stream ? NULL : on_reply_sentence, NULL);        /* 流式对话 */
        if (reply) {                                    /* LLM 回复成功 */
            ui_text(reply);                             /* 字幕显示完整回复 */
            if (db_stream) {                            /* 豆包：整句流式播报 */
                s_tts_stream_active = false;            /* 未走按句流水线 */
                ui_state(DIALOG_STATE_SPEAKING);        /* 绿点亮起 */
                music_notify_voice_start();             /* TTS 抢占音乐（与按句路径同语义） */
                bool played = false;                    /* 本轮是否已成功走网关播报 */
                s_gw_tts_fed = 0;
                if (spk_ring_begin() == ESP_OK) {       /* ring + 播放器就位 */
                    xEventGroupClearBits(s_vp.evt, EVT_TTS_DONE);
                    s_gw_tts_end = false;
                    cJSON *jroot = cJSON_CreateObject();    /* 组 tts 消息（cJSON 转义防中文/引号） */
                    cJSON_AddStringToObject(jroot, "type", "tts");
                    cJSON_AddStringToObject(jroot, "data", reply);
                    char *jtxt = cJSON_PrintUnformatted(jroot);
                    cJSON_Delete(jroot);
                    esp_err_t serr = gw_client_send_text(jtxt);     /* 文本上行（触发网关合成） */
                    cJSON_free(jtxt);
                    if (serr == ESP_OK) {                   /* 等网关推流完成（45s 上限） */
                        EventBits_t bits = xEventGroupWaitBits(s_vp.evt, EVT_TTS_DONE,
                                                               pdTRUE, pdFALSE,
                                                               pdMS_TO_TICKS(45000));
                        if (bits & EVT_TTS_DONE) {          /* 推流完：等播放器排空 ring */
                            ESP_LOGI(TAG, "网关 TTS 已喂 ring %uB", (unsigned)s_gw_tts_fed);
                            s_spk.synth_done = true;        /* 播放器排空后自退 */
                            s_gw_tts_end = false;
                            int wait_ms = 30000;            /* 排空上限（2MB ring ≈ 32s，防御） */
                            while (!s_spk.player_done && wait_ms > 0) {
                                vTaskDelay(pdMS_TO_TICKS(50));
                                wait_ms -= 50;
                            }
                            played = true;
                        } else {                            /* 超时：强制收尾 */
                            ESP_LOGW(TAG, "网关 TTS 超时");
                            s_spk.synth_done = true;
                            s_gw_tts_end = false;
                            vTaskDelay(pdMS_TO_TICKS(200)); /* 给播放器一点排空时间 */
                        }
                    }
                    if (s_spk.ring) {                       /* 会话资源清理 */
                        vStreamBufferDelete(s_spk.ring);
                        s_spk.ring = NULL;
                    }
                    if (s_spk.ring_mem) {
                        free(s_spk.ring_mem);
                        s_spk.ring_mem = NULL;
                    }
                }
                if (!played) {                              /* 网关路径失败：回退本地豆包流式 */
                    ESP_LOGW(TAG, "网关 TTS 不可用，回退本地合成");
                    voice_pipeline_speak(reply);            /* 环形缓冲边合成边播（阻塞至播完） */
                }
            } else {                                    /* MiniMax/MiMo：按句流水线 */
                tts_batch_flush();                      /* 收尾：把没凑满批的尾巴发走 */
                s_tts_input_done = true;                /* 标记 LLM 输出完毕（不再有新句） */
                /* 等语音任务播完已入队短句；LLM 和第一个 TTS 已在此前并行。 */
                while (__atomic_load_n(&s_tts_pending, __ATOMIC_RELAXED) > 0) {     /* 还有句子没播完 */
                    vTaskDelay(pdMS_TO_TICKS(50));      /* 50ms 轮询等待 */
                }
            }
        } else {                                        /* LLM 失败 */
            s_tts_batch_len = 0;                        /* 未成批的残句一并作废 */
            s_tts_stream_active = false;                /* 流水线停用 */
            ui_text("……脑子突然一片空白，再说一次？");    /* 字幕提示 */
            ui_state(DIALOG_STATE_IDLE);                /* 状态点熄灭 */
        }
    }
    free(text);                                         /* 识别文本用完释放 */
    if (s_vad_mode && gw_client_is_connected()) {       /* VAD 连续对话：自动进入下一轮监听 */
        ui_text("……（我在听，直接说就好）");             /* 字幕提示免按钮 */
        xEventGroupSetBits(s_vp.evt, EVT_HOLD_START);   /* 自动开启新一轮录音 */
    }
    ESP_LOGI(TAG, "⏱ 全程: %lldms（松手→播完）",         /* 打印全程延迟 */
             (esp_timer_get_time() - t0) / 1000);       /* 毫秒换算 */
    rig_rig_set_mouth(RIG_MOUTH_CLOSED);                /* 播完闭嘴 */
    rig_rig_set_mouth(RIG_MOUTH_AUTO);                  /* 释放口型控制交还 idle */
    s_tts_stream_active = false;                        /* 流水线停用 */
    ui_state(DIALOG_STATE_IDLE);                        /* 状态点熄灭 */
    free(reply);                                        /* 释放完整回复（网关/本地共用） */
}

/* ---------- TTS 播报（M2 串口文本路径，ring buffer 架构） ---------- */

/*
 * 播放架构（M2 教训：在 SSE 回调里同步 bsp_audio_play 会阻塞网络读取，
 * 服务端流式推送超时掐流——实测整条流只剩 1 块 160ms 音频）。
 * 这块 C6 Wi-Fi 到云端的 SSE 音频块存在长间隙，且长句的平均下发速度
 * 可能低于实时播放速度。语音按短语切分，每段必须完整预取后才稳定播出；
 * 下一段在上一段播放期间继续下载，避免网络抖动直接传到扬声器。
 */
/*
 * 环形缓冲容量（2026-09-10 上板实测后由 512KB 提到 2MB）：
 * 512KB 只够 8 秒 16kHz/双声道 PCM16，而三玖正常一条回复就能合成出
 * 400~690KB PCM（10 秒以上）。生产端（TTS 拉流）比实时快得多，
 * 8 秒的余量一眨眼就填满 → xStreamBufferSend 等 2 秒写不进 →
 * "TTS ring write timeout: 20480/20480B dropped" 成片出现，
 * 同时 playback stats 报 send_timeouts=20 / max_feed_gap=5698ms。
 * PSRAM 有 14MB 富余，直接给到 2MB（≈32 秒），把网络抖动、
 * 渲染抢占、长句整体预取全部吸收进来，不再丢样。
 */
#define SPK_RING_SIZE         (2 * 1024 * 1024) /* 约 32 秒 16kHz/双声道 PCM16 */
#define SPK_RECV_TIMEOUT_MS   40           /* 播放任务取数据短超时（欠载探测粒度） */


/** 网关 TTS PCM 帧（WS 任务上下文）：24k/mono 分块 → 复用 speak 重采样进 ring */
static void gw_tts_pcm_cb(const uint8_t *pcm, size_t bytes)
{
    if (s_spk.ring != NULL && !s_spk.synth_done && !s_spk.abort) {      /* 会话中且未被打断 */
        s_gw_tts_fed += bytes;
        on_tts_audio((const int16_t *)pcm, bytes / sizeof(int16_t), NULL);
    }
}

/** 网关播报的 ring + 播放器初始化（从 voice_pipeline_speak 抽出复用） */
static esp_err_t spk_ring_begin(void)
{
    s_spk.ring_mem = heap_caps_malloc(SPK_RING_SIZE, MALLOC_CAP_SPIRAM);        /* PSRAM 分配 */
    if (!s_spk.ring_mem) {                          /* 分配失败 */
        ESP_LOGW(TAG, "播放缓冲（PSRAM）分配失败");
        return ESP_ERR_NO_MEM;
    }
    s_spk.ring = xStreamBufferCreateStatic(SPK_RING_SIZE, 1,        /* 静态创建流缓冲 */
                                           s_spk.ring_mem, &s_spk.ring_ctl);
    s_spk.synth_done = false;                       /* 推流未结束 */
    s_spk.abort = false;                            /* 清打断标志 */
    s_spk.player_done = false;                      /* 播放未完成 */
    s_spk.player_started = true;                    /* 播放器立即启动（帧到前欠载等待） */
    s_spk.underflows = 0;                           /* 诊断计数复位 */
    s_spk.send_failures = 0;
    s_spk.max_feed_gap_ms = 0;
    s_spk.last_feed_us = 0;
    s_spk.resample_tail_count = 0;
    if (xTaskCreate(player_task, "spk_play", 4 * 1024, NULL, 5, NULL) != pdPASS) {
        free(s_spk.ring_mem);                       /* 任务创建失败回滚 */
        s_spk.ring_mem = NULL;
        s_spk.ring = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

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
        } else if (s_spk.abort) {               /* 被打断：立即退出（残帧丢弃） */
            break;
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

void voice_pipeline_tts_cancel(void)
{
    /* 音乐页显式点播的抢占入口：三处闸门（入队/拉流/播放）见 s_tts_cancel，
     * 标志保持置位直到下一轮对话在 process_wav 里复位——这期间即使
     * 在途的 TTS 请求姗姗来迟，短语也会在播放任务门口被丢弃并释放。 */
    s_tts_cancel = true;                                /* 立闸门 */
    s_tts_batch_len = 0;                                /* 未成批的残句一并作废 */
    if (s_tts_queue != NULL) {                          /* 句子队列：值拷贝，直接清 */
        xQueueReset(s_tts_queue);
    }
    if (s_tts_ready_queue != NULL) {                    /* 就绪队列：堆指针，逐个释放防泄漏 */
        tts_pcm_phrase_t *phrase = NULL;                /* 待释放短语 */
        while (xQueueReceive(s_tts_ready_queue, &phrase, 0) == pdPASS) {
            free(phrase->pcm);                          /* 释放 PCM */
            free(phrase);                               /* 释放控制块 */
            __atomic_fetch_sub(&s_tts_pending, 1, __ATOMIC_RELAXED);    /* 未播计数 -1 */
        }
    }
    ESP_LOGI(TAG, "TTS 未播内容已全部取消（音乐点播）"); /* 诊断日志 */
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
    gw_client_set_msg_handler(gw_msg_handler);      /* 步骤 2：注册网关消息处理器（ASR 结果） */
    gw_client_set_binary_handler(gw_tts_pcm_cb);    /* 步骤 3：注册 PCM 帧处理器（漏注册=喂 ring 0B，2026-09-12 实测） */
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

void voice_pipeline_barge_in(void)
{
    if (!s_vp.inited) {                             /* 管线未初始化 */
        return;
    }
    if (s_spk.ring == NULL) {                       /* 没有播报在进行 */
        return;
    }
    ESP_LOGI(TAG, "打断！停止播报");                /* 日志 */
    s_spk.abort = true;                             /* 播放器立即退出（残帧丢弃） */
    s_spk.synth_done = true;
    s_gw_tts_end = false;
    xEventGroupSetBits(s_vp.evt, EVT_TTS_DONE);     /* 唤醒等待方（尽快收尾本轮） */
    gw_client_send_text("{\"type\":\"tts_cancel\"}");        /* 通知网关停止合成推流 */
    while (xStreamBufferReset(s_spk.ring) != pdPASS) {      /* 丢弃未播残帧 */
        vTaskDelay(pdMS_TO_TICKS(2));               /* 播放器 40ms 粒度阻塞，很快让出 */
    }
}

void voice_pipeline_set_vad(bool on)
{
    s_vad_mode = on;                                /* 模式切换（下一轮生效） */
    ESP_LOGI(TAG, "VAD 连续对话: %s", on ? "开" : "关");
}

void voice_pipeline_set_vad_thresh(int thresh)
{
    s_vad_thresh = thresh;                          /* 阈值调节 */
    ESP_LOGI(TAG, "VAD 阈值: %d", thresh);
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
    /* 同步版：录 ms 毫秒后走管线。直接复用 rec_until_stop_or——
     * 网关流式 ASR 的启停/上行都在那里（2026-09-12：record_ms 自带
     * 循环曾绕过网关分支，导致串口 rec 命令不走网关 ASR） */
    rec_until_stop_or(ms);
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
