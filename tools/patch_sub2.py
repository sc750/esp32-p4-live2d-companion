# -*- coding: utf-8 -*-
"""字幕逐句同步（步骤 6）设备侧补丁 2：pcm 字段解析 + 喂 ring 推进句号。"""
p = "user/ai/voice_pipeline.c"
s = open(p, encoding="utf-8").read()

# 1) gw_msg_handler 的 reply_sentence 分支改为带 json 解析（pcm 字段）
old = """    } else if (strcmp(type, "reply_sentence") == 0) {   /* 步骤 6：句子入状态机（播到再显示） */
        if (s_sub.count < SUB_SENT_MAX) {
            strlcpy(s_sub.text[s_sub.count], data ? data : "", SUB_SENT_LEN);
            s_sub.pcm_bytes[s_sub.count] = 0;           /* 边界默认 0，pcm 字段解析覆盖 */
            s_sub.count++;
        }"""
new = """    } else if (strcmp(type, "reply_sentence") == 0) {   /* 步骤 6：句子入状态机（播到再显示） */
        if (s_sub.count < SUB_SENT_MAX) {
            strlcpy(s_sub.text[s_sub.count], data ? data : "", SUB_SENT_LEN);
            s_sub.count++;
        }"""
assert old in s, "1"
s = s.replace(old, new, 1)

# 2) gw_msg_handler 签名处：reply_sentence 带 raw json 解析 pcm 字段
#    实现：handler 拿不到 raw json——在 gw_client 分发处把 pcm 塞进 data？
#    不动协议：网关把 pcm_bytes 拼进 data 之外的字段设备解析不到。
#    简化协议：网关随句发的 data = "文本|pcm边界值"，设备按 '|' 拆。
old2 = """    } else if (strcmp(type, "reply_sentence") == 0) {   /* 步骤 6：句子入状态机（播到再显示） */
        if (s_sub.count < SUB_SENT_MAX) {
            strlcpy(s_sub.text[s_sub.count], data ? data : "", SUB_SENT_LEN);
            s_sub.count++;
        }"""
new2 = """    } else if (strcmp(type, "reply_sentence") == 0) {   /* 步骤 6：句子入状态机（播到再显示） */
        /* data 格式："文本|累计pcm字节"（网关把边界拼在尾部，'|' 分隔） */
        char sep[512];
        strlcpy(sep, data ? data : "", sizeof(sep));
        char *bar = strrchr(sep, '|');
        uint32_t boundary = 0;
        if (bar) {
            *bar = '\\0';
            boundary = (uint32_t)strtoul(bar + 1, NULL, 10);
        }
        if (s_sub.count < SUB_SENT_MAX) {
            strlcpy(s_sub.text[s_sub.count], sep, SUB_SENT_LEN);
            s_sub.pcm_bytes[s_sub.count] = boundary;    /* 本句播完时的累计边界 */
            s_sub.count++;
        }"""
assert old2 in s, "2"
s = s.replace(old2, new2, 1)

# 3) gw_tts_pcm_cb：喂 ring 累计 + 跨边界切字幕
old3 = """static void gw_tts_pcm_cb(const uint8_t *pcm, size_t bytes)
{
    if (s_spk.ring != NULL && !s_spk.synth_done && !s_spk.abort) {      /* 会话中且未被打断 */
        s_gw_tts_fed += bytes;
        on_tts_audio((const int16_t *)pcm, bytes / sizeof(int16_t), NULL);
    }
}"""
new3 = """static void gw_tts_pcm_cb(const uint8_t *pcm, size_t bytes)
{
    if (s_spk.ring != NULL && !s_spk.synth_done && !s_spk.abort) {      /* 会话中且未被打断 */
        s_gw_tts_fed += bytes;
        /* 字幕逐句同步（步骤 6）：喂入跨过第 shown+1 句边界 → 显示那句。
         * 字幕按"开始喂该句"切换，比声音早约半句，观感最顺。 */
        while (s_sub.shown < s_sub.count &&
               s_sub.pcm_bytes[s_sub.shown] > 0 &&
               s_gw_tts_fed >= s_sub.pcm_bytes[s_sub.shown]) {
            s_sub.shown++;
            ui_text(s_sub.text[s_sub.shown - 1]);       /* 单句显示（scr_home 已改单行） */
        }
        on_tts_audio((const int16_t *)pcm, bytes / sizeof(int16_t), NULL);
    }
}"""
assert old3 in s, "3"
s = s.replace(old3, new3, 1)

# 4) 首句显示：gw_dialog_round 在 ring 就位后把第一句立即上屏（不用等 PCM 到达才看到）
old4 = """    s_gw_sub[0] = '\\0';                                 /* 字幕累积复位 */"""
new4 = """    memset(&s_sub, 0, sizeof(s_sub));                   /* 字幕状态机复位（每轮） */
    s_gw_sub[0] = '\\0';                                 /* 字幕累积复位 */"""
assert old4 in s, "4"
s = s.replace(old4, new4, 1)

open(p, "w", encoding="utf-8", newline="\n").write(s)
print("part2 ok")
