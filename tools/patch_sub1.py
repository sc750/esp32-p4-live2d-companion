# -*- coding: utf-8 -*-
"""字幕逐句同步（步骤 6）设备侧补丁 1：状态机 + 消息处理。"""
p = "user/ai/voice_pipeline.c"
s = open(p, encoding="utf-8").read()

# 1) 字幕状态机变量
old = """static char *s_gw_reply = NULL;         /* 网关完整回复（WS 任务写入，管线任务取走） */
static char s_gw_sub[2048];             /* 流式字幕累积（reply_sentence 逐句追加） */
static volatile bool s_gw_chat_err;     /* 网关 LLM 失败标志 */"""
new = """static char *s_gw_reply = NULL;         /* 网关完整回复（WS 任务写入，管线任务取走） */
static char s_gw_sub[2048];             /* 完整回复累积（reply_done 定稿用） */

/* 字幕逐句同步状态机（步骤 6）：语音播到哪句，字幕显示哪句 */
#define SUB_SENT_MAX    8               /* 缓存句数（50 字限长下 3~4 句足够） */
#define SUB_SENT_LEN    128             /* 单句缓存字节数 */
static struct {
    char text[SUB_SENT_MAX][SUB_SENT_LEN];      /* 各句文本 */
    uint32_t pcm_bytes[SUB_SENT_MAX];           /* 各句累计 PCM 边界（网关随句下发） */
    int count;                                  /* 已收句数 */
    int shown;                                  /* 当前已显示句号（0=未显示） */
    uint32_t fed;                               /* 已喂入 ring 的累计 PCM */
} s_sub;

static volatile bool s_gw_chat_err;     /* 网关 LLM 失败标志 */"""
assert old in s, "1"
s = s.replace(old, new, 1)

# 2) reply_sentence / reply_done 处理改造
old = """    } else if (strcmp(type, "reply_sentence") == 0) {   /* 步骤 4：LLM 断句流式字幕 */
        strlcat(s_gw_sub, data ? data : "", sizeof(s_gw_sub));
        ui_text(s_gw_sub);                              /* 直接刷字幕（桥自持锁，任意任务安全） */
    } else if (strcmp(type, "reply_done") == 0) {       /* 步骤 4：完整回复到达 */
        free(s_gw_reply);
        s_gw_reply = (data && data[0]) ? strdup(data) : NULL;
        ui_text(s_gw_reply ? s_gw_reply : s_gw_sub);    /* 定稿字幕 */
        xEventGroupSetBits(s_vp.evt, EVT_REPLY_DONE);"""
new = """    } else if (strcmp(type, "reply_sentence") == 0) {   /* 步骤 6：句子入状态机（播到再显示） */
        if (s_sub.count < SUB_SENT_MAX) {
            strlcpy(s_sub.text[s_sub.count], data ? data : "", SUB_SENT_LEN);
            s_sub.pcm_bytes[s_sub.count] = 0;           /* 边界默认 0，pcm 字段解析覆盖 */
            s_sub.count++;
        }
    } else if (strcmp(type, "reply_done") == 0) {       /* 步骤 4：完整回复到达 */
        free(s_gw_reply);
        s_gw_reply = (data && data[0]) ? strdup(data) : NULL;
        xEventGroupSetBits(s_vp.evt, EVT_REPLY_DONE);"""
assert old in s, "2"
s = s.replace(old, new, 1)

open(p, "w", encoding="utf-8", newline="\n").write(s)
print("part1 ok")
