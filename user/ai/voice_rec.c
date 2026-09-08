/**
 * @file    voice_rec.c
 * @brief   录音器实现——2MB PSRAM 常驻缓冲，立体声采集→单声道抽取，出 WAV
 *
 * 采集格式固定 16k/16bit/2ch（BSP 官方验证格式，绝不中途重开 codec）；
 * 单声道化在应用层做（M3：上传体积减半）；AGC 用整段峰值做受限增益。
 *
 * @date    2026-09-06
 * @version 1.1.0  M3：单声道抽取 + AGC 统计
 */

#include "voice_rec.h"      /* 本模块公开接口 */

#include <string.h>         /* memcpy */
#include <stdlib.h>         /* 标准 */
#include <math.h>           /* sqrt（RMS 统计） */

#include "esp_log.h"        /* ESP_LOGx 日志 */
#include "esp_check.h"      /* ESP_RETURN_ON_* 检查宏 */
#include "esp_heap_caps.h"  /* heap_caps_malloc（PSRAM 分配） */

#include "bsp_audio.h"      /* 底层录音接口（bsp_audio_record） */

#define TAG "voice_rec"     /* 本模块日志标签 */

#define CHUNK_BYTES     (6400)      /* 100ms @ 16k/16bit/2ch（BSP 采集格式，不变） */
#define MONO_CHUNK_BYTES (3200)     /* 左声道抽取后：100ms @ 16k/16bit/1ch */
#define REC_BUF_SIZE    (VOICE_REC_MAX_SEC * 32000) /* 30s 单声道 = 937KB */
#define AGC_TARGET_PEAK  (12000)    /* 约 -8.7dBFS，给语音峰值留出余量 */
#define AGC_MAX_GAIN_X1000 (8000)   /* 最多 8 倍，避免把底噪放成爆音 */
#define AGC_MIN_PEAK     (200)      /* 低于此值通常是静音，不作增益 */

/* 立体声采集暂存（内部 RAM，I2S 块直读；3200 个样本） */
static int16_t s_staging[CHUNK_BYTES / sizeof(int16_t)];

/* 录音器状态（仅管线任务上下文访问，无锁） */
static struct {
    bool inited;                /* 初始化完成标志（幂等闸门） */
    char *buf;                  /* PSRAM 常驻缓冲：[0,44)=WAV头占位，[44,…)=PCM */
    size_t pcm_len;             /* 已录单声道 PCM 字节数 */
    bool recording;             /* 是否正在录音 */
    int32_t peak;               /* 本段最大采样幅值（AGC 依据） */
    uint64_t energy;            /* 样本平方和（算 RMS 用） */
    size_t sample_count;        /* 样本总数（算 RMS 用） */
} s_rec;

/**
 * 初始化录音器：预分配 937KB PSRAM 缓冲（幂等）
 * @return ESP_OK 就绪；ESP_ERR_NO_MEM 分配失败
 */
esp_err_t voice_rec_init(void)
{
    if (s_rec.inited) {                                 /* 幂等闸门：已初始化直接返回 */
        return ESP_OK;                                  /* 重复调用无害 */
    }
    s_rec.buf = heap_caps_malloc(REC_BUF_SIZE, MALLOC_CAP_SPIRAM);      /* PSRAM 缓冲一次到位 */
    ESP_RETURN_ON_FALSE(s_rec.buf, ESP_ERR_NO_MEM, TAG, "2MB 录音缓冲分配失败");   /* 分配失败即报错 */
    s_rec.inited = true;                                /* 置就绪标志 */
    ESP_LOGI(TAG, "录音器就绪（上限 %ds = %dKB PSRAM）",         /* 打印容量信息 */
             VOICE_REC_MAX_SEC, REC_BUF_SIZE / 1024);   /* 秒数与 KB 数 */
    return ESP_OK;                                      /* 初始化成功 */
}

/**
 * 开始一轮录音：游标与 AGC 统计全部复位
 * @return ESP_OK 就绪；ESP_ERR_INVALID_STATE 上一段没取走或已在录
 */
esp_err_t voice_rec_begin(void)
{
    /* 状态检查：必须已初始化且不在录音中 */
    ESP_RETURN_ON_FALSE(s_rec.inited, ESP_ERR_INVALID_STATE, TAG, "not init");
    ESP_RETURN_ON_FALSE(!s_rec.recording, ESP_ERR_INVALID_STATE, TAG, "already rec");

    /* 音频 BSP 从启动起固定为官方已验证的 16k/16bit/双声道。
     * TTS 的 24k/单声道 PCM 在应用层转换，录音时绝不重开共享 codec。 */
    s_rec.pcm_len = 0;                                  /* PCM 游标归零 */
    s_rec.peak = 0;                                     /* 峰值统计归零 */
    s_rec.energy = 0;                                   /* 能量统计归零 */
    s_rec.sample_count = 0;                             /* 样本计数归零 */
    s_rec.recording = true;                             /* 置录音中标志 */
    return ESP_OK;                                      /* 开始录音 */
}

/** 录一个 100ms 块：立体声采集 → 左声道抽取 → AGC 统计（阻塞式） */
void voice_rec_chunk(void)
{
    if (!s_rec.recording) {                             /* 不在录音状态 */
        return;                                         /* 直接忽略本次调用 */
    }
    if (s_rec.pcm_len + MONO_CHUNK_BYTES > REC_BUF_SIZE - 44) { /* 超过 30s 容量上限 */
        return;                                         /* 静默停写（保留头部空间） */
    }
    /* 读立体声块到暂存（BSP 固定 16k/16bit/2ch 采集，绝不重开 codec） */
    if (bsp_audio_record(s_staging, CHUNK_BYTES) != ESP_OK) {   /* 阻塞等满 100ms 块 */
        ESP_LOGW(TAG, "录音块读取失败");                 /* I2S 读取失败告警 */
        return;                                         /* 本块丢弃，下块继续 */
    }
    /* M3 单声道化：ES8311 是单声道麦克风，右声道纯冗余——
     * 只保留左声道，上传体积减半（ASR 上传时间近似减半） */
    int16_t *dst = (int16_t *)(s_rec.buf + 44 + s_rec.pcm_len); /* 写入位置（44 头之后） */
    const size_t mono_samples = CHUNK_BYTES / 2 / sizeof(int16_t);      /* 本块单声道样本数 */
    for (size_t i = 0; i < mono_samples; i++) {         /* 逐样本抽取与统计 */
        int32_t sample = s_staging[i * 2];              /* 偶数下标 = 左声道 */
        dst[i] = (int16_t)sample;                       /* 单声道数据写入缓冲 */
        int32_t magnitude = sample >= 0 ? sample : -sample;     /* 取绝对值 */
        if (magnitude > s_rec.peak) {                   /* 更新本段峰值 */
            s_rec.peak = magnitude;                     /* 记录最大幅值 */
        }
        s_rec.energy += (uint64_t)((int64_t)sample * sample);   /* 累计平方和（RMS 用） */
    }
    s_rec.sample_count += mono_samples;                 /* 样本计数累加 */
    s_rec.pcm_len += MONO_CHUNK_BYTES;                  /* PCM 游标前进 3200B */
}

/** 小工具：往缓冲写一个 32 位小端值（WAV 头字段用） */
static void wav_put_u32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);                                   /* 小端机直接内存拷贝 */
}

/** 小工具：往缓冲写一个 16 位小端值（WAV 头字段用） */
static void wav_put_u16(uint8_t *p, uint16_t v)
{
    memcpy(p, &v, 2);                                   /* 小端机直接内存拷贝 */
}

/**
 * 停止录音并取出完整 WAV（44B 头 + 单声道 PCM，带 AGC 增益）
 * @param wav_out  输出 WAV 缓冲（堆上，调用方 free）
 * @param len_out  输出总长度（44 + PCM）
 * @return ESP_OK 成功；ESP_ERR_INVALID_SIZE 录音太短
 */
esp_err_t voice_rec_end_and_get(char **wav_out, size_t *len_out)
{
    /* 状态检查：必须在录音中 */
    ESP_RETURN_ON_FALSE(s_rec.recording, ESP_ERR_INVALID_STATE, TAG, "not rec");
    s_rec.recording = false;                            /* 结束录音状态 */

    /* 最短 200ms：太短给 ASR 是浪费一次调用（200ms 单声道 = 6400B） */
    ESP_RETURN_ON_FALSE(s_rec.pcm_len >= 6400, ESP_ERR_INVALID_SIZE,
                        TAG, "录音太短 (%ums)", (unsigned)(s_rec.pcm_len / 32));

    /* 内部缓冲常驻复用；拷出有效段（44 头 + PCM）交调用方 */
    size_t total = 44 + s_rec.pcm_len;                  /* WAV 总长 = 头 + PCM */
    char *out = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);     /* 输出缓冲（PSRAM） */
    ESP_RETURN_ON_FALSE(out, ESP_ERR_NO_MEM, TAG, "WAV 输出分配失败");      /* 分配失败即报错 */

    uint8_t *h = (uint8_t *)out;                        /* 头部写指针（字节视角） */
    memcpy(h, "RIFF", 4);                               /* RIFF 魔数 */
    wav_put_u32(h + 4, (uint32_t)(36 + s_rec.pcm_len)); /* 文件总长 -8 */
    memcpy(h + 8, "WAVE", 4);                           /* WAVE 标识 */
    memcpy(h + 12, "fmt ", 4);                          /* fmt 块标识 */
    wav_put_u32(h + 16, 16);                    /* fmt 块长 */
    wav_put_u16(h + 20, 1);                     /* PCM */
    wav_put_u16(h + 22, 1);                     /* 单声道（M3） */
    wav_put_u32(h + 24, 16000);                 /* 采样率 */
    wav_put_u32(h + 28, 16000 * 1 * 2);         /* 字节率 */
    wav_put_u16(h + 32, 1 * 2);                 /* 块对齐 */
    wav_put_u16(h + 34, 16);                    /* 位深 */
    memcpy(h + 36, "data", 4);                          /* data 块标识 */
    wav_put_u32(h + 40, (uint32_t)s_rec.pcm_len);       /* PCM 数据长度 */
    memcpy(out + 44, s_rec.buf + 44, s_rec.pcm_len);    /* 拷贝有效 PCM 数据 */

    /* 板载麦克风在远距离说话时原始幅度很低，云端 ASR 会将其判为静音。
     * 用整段峰值做受限 AGC；采集格式不变，只提升有效语音的量化幅度。 */
    uint32_t gain_x1000 = 1000;                         /* 初始增益 = 1.0 倍（千分比表示） */
    if (s_rec.peak >= AGC_MIN_PEAK && s_rec.peak < AGC_TARGET_PEAK) {   /* 峰值偏低且非静音 */
        gain_x1000 = (uint32_t)((uint64_t)AGC_TARGET_PEAK * 1000 / s_rec.peak); /* 算目标增益 */
        if (gain_x1000 > AGC_MAX_GAIN_X1000) {          /* 增益超上限 */
            gain_x1000 = AGC_MAX_GAIN_X1000;            /* 封顶 8 倍 */
        }
    }
    if (gain_x1000 > 1000) {                            /* 增益 >1 倍才需要处理 */
        int16_t *out_pcm = (int16_t *)(out + 44);       /* PCM 数据指针（跳过 44B 头） */
        size_t sample_count = s_rec.pcm_len / sizeof(*out_pcm); /* 单声道样本总数 */
        for (size_t i = 0; i < sample_count; i++) {     /* 逐样本施加增益 */
            int32_t scaled = (int32_t)((int64_t)out_pcm[i] * gain_x1000 / 1000); /* 放大样本 */
            if (scaled > INT16_MAX) {                   /* 上限削顶保护 */
                scaled = INT16_MAX;                     /* 钳到 int16 最大 */
            } else if (scaled < INT16_MIN) {            /* 下限削顶保护 */
                scaled = INT16_MIN;                     /* 钳到 int16 最小 */
            }
            out_pcm[i] = (int16_t)scaled;               /* 写回增益后样本 */
        }
    }

    *wav_out = out;                                     /* 交出输出缓冲 */
    *len_out = total;                                   /* 交出总长度 */
    uint32_t rms = s_rec.sample_count                   /* RMS = 能量/样本数 开方 */
                   ? (uint32_t)sqrt((double)s_rec.energy / s_rec.sample_count) : 0;    /* 防除零 */
    ESP_LOGI(TAG, "录音完成: %ums / %uKB, peak=%ld, rms=%lu, agc=%lux",  /* 打印录音统计 */
             (unsigned)(s_rec.pcm_len / 32), (unsigned)(total / 1024),  /* 时长与大小 */
             (long)s_rec.peak, (unsigned long)rms,                      /* 峰值与 RMS */
             (unsigned long)gain_x1000 / 1000);         /* 实际增益倍数 */
    return ESP_OK;                                      /* 成功返回 */
}

/** 放弃当前录音（内部缓冲留着复用，不转移所有权） */
void voice_rec_abort(void)
{
    if (!s_rec.recording) {                             /* 没在录音就无事可做 */
        return;                                         /* 直接返回 */
    }
    s_rec.recording = false;                            /* 仅清除录音标志 */
    /* buf 留着复用（abort 不转移所有权） */
}

/** 当前已录时长（毫秒，单声道 32 字节/ms） */
uint32_t voice_rec_elapsed_ms(void)
{
    return s_rec.pcm_len / 32;                  /* 32 字节/ms（单声道） */
}
