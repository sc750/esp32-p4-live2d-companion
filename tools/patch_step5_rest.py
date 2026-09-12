# -*- coding: utf-8 -*-
"""步骤 5 余下补丁：网关 tts_cancel / 设备打断钩子 / VAD 串口命令。"""
import re

# ---- 网关 tts_cancel ----
p = "tools/gateway/gateway.py"
s = open(p, encoding="utf-8").read()
old = """        self.asr: AsrSession | None = None
        self.send_lock = asyncio.Lock()  # LLM/TTS 多任务并发推送：所有发送串行化"""
new = old + "\n        self.tts_cancel = False         # 设备打断：停止 TTS 推流"
assert old in s, "g1"
s = s.replace(old, new, 1)
old = '''    async def stream_doubao(self, text: str):
        """豆包流式合成一段文本：chunked PCM 块逐帧推回设备（不收尾）。"""'''
new = old + """
        if self.tts_cancel:                             # 已被打断：不再开始
            return"""
assert old in s, "g2"
s = s.replace(old, new, 1)
old = """                        data = msg.get("data")
                        if data:                            # base64 PCM 块 → 二进制推回
                            await self.send(base64.b64decode(data))"""
new = """                        if self.tts_cancel:                 # 设备打断：立即停止推流
                            log.info("TTS 被设备打断，停止推流")
                            return
                        data = msg.get("data")
                        if data:                            # base64 PCM 块 → 二进制推回
                            await self.send(base64.b64decode(data))"""
assert old in s, "g3"
s = s.replace(old, new, 1)
old = '''        elif mtype == "chat":                           # 步骤 4：LLM+TTS 全流水线
            asyncio.create_task(self.run_chat(msg))'''
new = old + '''
        elif mtype == "tts_cancel":                     # 步骤 5：设备打断 TTS
            self.tts_cancel = True
            log.info("收到设备打断请求")'''
assert old in s, "g4"
s = s.replace(old, new, 1)
# run_chat / run_tts 开始时复位
old = '''    async def run_tts(self, text: str):
        """步骤 3 入口：整段文本流式合成，完成发 tts_end。"""
        if not text:'''
new = '''    async def run_tts(self, text: str):
        """步骤 3 入口：整段文本流式合成，完成发 tts_end。"""
        self.tts_cancel = False                         # 新会话复位打断标志
        if not text:'''
assert old in s, "g5"
s = s.replace(old, new, 1)
old = '''    async def run_chat(self, msg: dict):
        """步骤 4：流式 LLM → 断句 → 逐句豆包流式（LLM 生成与 TTS 合成真并行）。"""'''
new = old + """
        self.tts_cancel = False                         # 新会话复位打断标志"""
assert old in s, "g6"
s = s.replace(old, new, 1)
# worker 里打断时跳过合成
old = """            t0 = asyncio.get_event_loop().time()
            try:
                await self.stream_doubao(sent)"""
new = """            t0 = asyncio.get_event_loop().time()
            if self.tts_cancel:                         # 被打断：跳过剩余句子
                break
            try:
                await self.stream_doubao(sent)"""
assert old in s, "g7"
s = s.replace(old, new, 1)
open(p, "w", encoding="utf-8", newline="\n").write(s)
print("gateway ok")

# ---- 设备 main_app：按住说话时先打断 ----
p = "user/main_app.c"
s = open(p, encoding="utf-8").read()
m = re.search(r"static void on_voice_hold\(bool holding[^\{]*\{", s)
assert m, "d1"
insert_at = m.end()
code = "\n    voice_pipeline_barge_in();                  /* 步骤 5：说话时打断三玖播报 */"
s = s[:insert_at] + code + s[insert_at:]
open(p, "w", encoding="utf-8", newline="\n").write(s)
print("main_app ok")

# ---- voice_pipeline: VAD 开关 API ----
p = "user/ai/voice_pipeline.h"
s = open(p, encoding="utf-8").read()
old = "/** 打断当前播报（barge-in）：三玖说话时调用——立即停播+通知网关取消+可立即录音 */\nvoid voice_pipeline_barge_in(void);"
new = old + """

/** VAD 连续对话模式开关（步骤 5）：播完自动续听、说完静音自动断句 */
void voice_pipeline_set_vad(bool on);

/** VAD 人声 RMS 阈值调节（环境噪声大时上调；默认 250） */
void voice_pipeline_set_vad_thresh(int thresh);"""
assert old in s, "d2"
s = s.replace(old, new, 1)
open(p, "w", encoding="utf-8", newline="\n").write(s)
print("vp h ok")

# ---- voice_pipeline.c: 实现 ----
p = "user/ai/voice_pipeline.c"
s = open(p, encoding="utf-8").read()
anchor = "void voice_pipeline_barge_in(void)"
i = s.index(anchor)
# 找到函数结尾（下一个空行+}后）
j = s.index("\n}\n", i) + 3
code = """
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
"""
s = s[:j] + code + s[j:]
open(p, "w", encoding="utf-8", newline="\n").write(s)
print("vp c ok")
