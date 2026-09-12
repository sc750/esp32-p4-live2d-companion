# -*- coding: utf-8 -*-
"""步骤 5：打断（barge-in）+ VAD 连续对话（设备侧）。"""
p = "user/ai/voice_pipeline.c"
s = open(p, encoding="utf-8").read()

# 1) s_spk 加 abort
old = "    volatile bool synth_done;       /* TTS 拉流结束 */"
new = old + "\n    volatile bool abort;            /* 打断标志（barge-in：立即停播） */"
assert old in s, "1"
s = s.replace(old, new, 1)

# 2) 前置声明
old = "static void player_task(void *arg);"
new = old + "\nstatic volatile bool s_vad_mode = false;    /* VAD 连续对话模式（串口 vad on/off） */\nstatic int s_vad_thresh = 250;              /* 语音 RMS 阈值（环境噪声上调） */"
assert old in s, "2"
s = s.replace(old, new, 1)

# 3) spk_ring_begin 复位 abort
old = """    s_spk.synth_done = false;                       /* 推流未结束 */
    s_spk.player_done = false;                      /* 播放未完成 */
    s_spk.player_started = true;                    /* 播放器立即启动（帧到前欠载等待） */"""
new = """    s_spk.synth_done = false;                       /* 推流未结束 */
    s_spk.abort = false;                            /* 清打断标志 */
    s_spk.player_done = false;                      /* 播放未完成 */
    s_spk.player_started = true;                    /* 播放器立即启动（帧到前欠载等待） */"""
assert old in s, "3"
s = s.replace(old, new, 1)

# 4) player_task 欠载分支支持 abort
old = """        } else if (s_spk.synth_done && xStreamBufferBytesAvailable(s_spk.ring) == 0) {      /* 拉流完+排空 */
            break;                  /* SSE 收尾且缓冲已排空 */
        } else {
            s_spk.underflows++;     /* 网络抖动：等后续 SSE，不退出播放器 */
        }"""
new = """        } else if (s_spk.abort) {               /* 被打断：立即退出（残帧丢弃） */
            break;
        } else if (s_spk.synth_done && xStreamBufferBytesAvailable(s_spk.ring) == 0) {      /* 拉流完+排空 */
            break;                  /* SSE 收尾且缓冲已排空 */
        } else {
            s_spk.underflows++;     /* 网络抖动：等后续 SSE，不退出播放器 */
        }"""
assert old in s, "4"
s = s.replace(old, new, 1)

# 5) gw_tts_pcm_cb 打断后丢帧
old = "    if (s_spk.ring != NULL && !s_spk.synth_done) {      /* 播放会话进行中才喂 */"
new = "    if (s_spk.ring != NULL && !s_spk.synth_done && !s_spk.abort) {      /* 会话中且未被打断 */"
assert old in s, "5"
s = s.replace(old, new, 1)

# 6) VAD 状态 + 上行回调里做能量检测
old = """/** 录音块上行回调（voice_rec 块粒度 100ms/3200B → WS 二进制帧） */
static void gw_chunk_uplink(const int16_t *mono, size_t samples)
{
    gw_client_send_binary(mono, samples * sizeof(int16_t));     /* 块即帧，直接发 */
}"""
new = """static volatile bool s_vad_spoke = false;   /* 本轮录音里检测到人声 */
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
}"""
assert old in s, "6"
s = s.replace(old, new, 1)

# 7) rec_until_stop_or：VAD 自动断句（说完静音 800ms / 8s 无声自动收）
old = """    uint32_t start = (uint32_t)(esp_timer_get_time() / 1000);   /* 记录开始时刻（ms） */
    while ((uint32_t)(esp_timer_get_time() / 1000) - start < max_ms) {  /* 未到上限就继续 */
        EventBits_t bits = xEventGroupWaitBits(s_vp.evt, EVT_HOLD_STOP, /* 等"松开"事件 */
                                               pdTRUE, pdFALSE,         /* 取位后清除 */
                                               pdMS_TO_TICKS(100));     /* 100ms 超时分块 */
        voice_rec_chunk();      /* 100ms 一块，落在等待超时的缝隙里 */
        if (bits & EVT_HOLD_STOP) {                     /* 用户松手了 */
            break;                                      /* 结束录音循环 */
        }
    }"""
new = """    uint32_t start = (uint32_t)(esp_timer_get_time() / 1000);   /* 记录开始时刻（ms） */
    bool vad_round = s_vad_mode && s_use_gw_asr;        /* VAD 连续模式（仅网关 ASR 时启用） */
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
    }"""
assert old in s, "7"
s = s.replace(old, new, 1)

# 8) process_wav 尾部：VAD 模式自动续听（在 free(text) 之前的状态机之后）
old = """    free(text);                                         /* 识别文本用完释放 */
    ESP_LOGI(TAG, "⏱ 全程: %lldms（松手→播完）",         /* 打印全程延迟 */"""
new = """    free(text);                                         /* 识别文本用完释放 */
    if (s_vad_mode && gw_client_is_connected()) {       /* VAD 连续对话：自动进入下一轮监听 */
        ui_text("……（我在听，直接说就好）");             /* 字幕提示免按钮 */
        xEventGroupSetBits(s_vp.evt, EVT_HOLD_START);   /* 自动开启新一轮录音 */
    }
    ESP_LOGI(TAG, "⏱ 全程: %lldms（松手→播完）",         /* 打印全程延迟 */"""
assert old in s, "8"
s = s.replace(old, new, 1)

open(p, "w", encoding="utf-8", newline="\n").write(s)
print("step5 device core ok")
