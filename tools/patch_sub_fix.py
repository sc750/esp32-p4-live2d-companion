# -*- coding: utf-8 -*-
p = "user/ai/voice_pipeline.c"
s = open(p, encoding="utf-8").read()
old = '''    } else if (strcmp(type, "reply_sentence") == 0) {   /* 步骤 6：句子入状态机（播到再显示） */
        if (s_sub.count < SUB_SENT_MAX) {
            strlcpy(s_sub.text[s_sub.count], data ? data : "", SUB_SENT_LEN);
            s_sub.pcm_bytes[s_sub.count] = 0;           /* 边界默认 0，pcm 字段解析覆盖 */
            s_sub.count++;
        }'''
new = '''    } else if (strcmp(type, "reply_sentence") == 0) {   /* 步骤 6：句子入状态机（播到再显示） */
        /* data 格式："文本|累计pcm字节"（网关把边界拼在尾部，'|' 分隔） */
        char sep[512];
        strlcpy(sep, data ? data : "", sizeof(sep));
        char *bar = strrchr(sep, '|');
        uint32_t boundary = 0;
        if (bar) {
            *bar = '\0';
            boundary = (uint32_t)strtoul(bar + 1, NULL, 10);
        }
        if (s_sub.count < SUB_SENT_MAX) {
            strlcpy(s_sub.text[s_sub.count], sep, SUB_SENT_LEN);
            s_sub.pcm_bytes[s_sub.count] = boundary;    /* 本句播完时的累计边界 */
            s_sub.count++;
            /* 时序补丁：网关在该句 PCM 推完后才发消息——第一句到达时播放
             * 刚要开始，立即显示；最后一句到达后再无 PCM 帧，推进循环
             * 永不触发，也在此补显。 */
            if (s_sub.count == 1 || boundary <= s_gw_tts_fed) {
                s_sub.shown = s_sub.count;
                ui_text(s_sub.text[s_sub.shown - 1]);
            }
        }'''
assert old in s, "anchor"
open(p, "w", encoding="utf-8", newline="\n").write(s.replace(old, new, 1))
print("ok")
