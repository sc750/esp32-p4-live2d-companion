# -*- coding: utf-8 -*-
"""语音网关步骤 2 设备侧三 bug 修复补丁。"""
p = "user/ai/voice_pipeline.c"
s = open(p, encoding="utf-8").read()

# A: init 注册二进制处理器
old = "    gw_client_set_msg_handler(gw_msg_handler);      /* 步骤 2：注册网关消息处理器（ASR 结果） */"
new = old + "\n    gw_client_set_binary_handler(gw_tts_pcm_cb);    /* 步骤 3：注册 PCM 帧处理器（漏注册=喂 ring 0B，2026-09-12 实测） */"
assert old in s, "A"
s = s.replace(old, new, 1)

# B1: asr_error 处理
old = '    } else if (strcmp(type, "tts_end") == 0) {          /* 网关 TTS 推流结束（步骤 3） */'
new = ('''    } else if (strcmp(type, "asr_error") == 0) {        /* 网关 ASR 失败（如讯飞握手抖动） */
        s_gw_asr_err = true;                            /* 让等待方立即回退本地识别，别干等 5s */
        free(s_gw_asr_text);
        s_gw_asr_text = NULL;
        xEventGroupSetBits(s_vp.evt, EVT_ASR_RESULT);
    } else if (strcmp(type, "tts_end") == 0) {          /* 网关 TTS 推流结束（步骤 3） */''')
assert old in s, "B1"
s = s.replace(old, new, 1)

# B2: 错误标志
old = "static volatile bool s_gw_tts_end;      /* 网关 TTS 推流结束标志（WS 任务置位） */"
new = old + "\nstatic volatile bool s_gw_asr_err;      /* 网关 ASR 失败标志（区别于静音空结果） */"
assert old in s, "B2"
s = s.replace(old, new, 1)

# B3: collect 区分三种结局
old = """    free(s_gw_asr_text);                                /* 清理 */
    s_gw_asr_text = NULL;
    if (bits & EVT_ASR_RESULT) {
        return ESP_ERR_NOT_FOUND;                       /* 网关明确给出空结果（静音）——
                                                           不必再回退本地 HTTP 白等一轮 */
    }
    return ESP_ERR_TIMEOUT;                             /* 真超时：允许回退本地识别 */"""
new = """    free(s_gw_asr_text);                                /* 清理 */
    s_gw_asr_text = NULL;
    if (bits & EVT_ASR_RESULT) {
        if (s_gw_asr_err) {
            s_gw_asr_err = false;
            return ESP_ERR_INVALID_STATE;               /* 网关报错：回退本地识别 */
        }
        return ESP_ERR_NOT_FOUND;                       /* 网关明确空结果（静音）：不回退 */
    }
    s_gw_asr_err = false;
    return ESP_ERR_TIMEOUT;                             /* 真超时：允许回退本地识别 */"""
assert old in s, "B3"
s = s.replace(old, new, 1)

# B4: 录音开始清残留标志
old = "        xEventGroupClearBits(s_vp.evt, EVT_ASR_RESULT); /* 清上轮结果位 */"
new = old + "\n        s_gw_asr_err = false;"
assert old in s, "B4"
s = s.replace(old, new, 1)

# B5: process_wav 报错分支走本地回退
old = "        } else if (gerr == ESP_ERR_NOT_FOUND) {         /* 网关明确空结果（静音）——直接判没听清 */"
new = "        } else if (gerr == ESP_ERR_INVALID_STATE) {     /* 网关报错：回退本地识别 */"
assert old in s, "B5"
s = s.replace(old, new, 1)

open(p, "w", encoding="utf-8", newline="\n").write(s)
print("all 6 patches ok")
