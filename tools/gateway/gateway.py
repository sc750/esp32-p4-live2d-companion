#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gateway.py —— 语音网关（步骤 2：音频上行 + 网关侧讯飞流式听写）

协议（设备 ↔ 网关）：
  文本帧 JSON：
    设备→网关 {"type":"asr_start","fmt":"pcm16k"}   开始一轮听写
    设备→网关 {"type":"asr_stop"}                    录音结束，等最终结果
    设备→网关 {"type":"text","data":...} / {"type":"ping"} / hello（步骤 1 保留）
    网关→设备 {"type":"asr_result","data":"识别文本"}
    网关→设备 {"type":"asr_error","data":"原因"} / welcome / pong / echo
  二进制帧：16k/16bit/mono PCM（设备侧 100ms/3200B 一帧，仅 asr_start 后有效）

讯飞侧：IAT v2 wss 流式听写，1280B 子帧重切，status=2 收尾。

用法：
  pip install websockets
  py tools/gateway/gateway.py
"""
import asyncio
import base64
import datetime
import hashlib
import hmac
import json
import logging
import urllib.parse
from urllib.parse import urlencode

import websockets
import aiohttp

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("gateway")

HOST = "0.0.0.0"
PORT = 8765

# 讯飞流式听写凭据（与设备端 sdkconfig 同一套账号）
XFYUN_APP_ID = "REDACTED_XFYUN_APPID"
XFYUN_API_KEY = "REDACTED_XFYUN_KEY"
XFYUN_API_SECRET = "REDACTED_XFYUN_SECRET"
XFYUN_URL = "wss://iat-api.xfyun.cn/v2/iat"

# 讯飞帧规格：推荐 1280B/帧（40ms），设备 3200B/帧需重切
XFYUN_FRAME_BYTES = 1280

# 豆包 TTS（V3 HTTP chunked 单向流式，与设备端 doubao_tts 同一接口；
# 网关请求 pcm 裸流——局域网带宽充裕，设备端免解码直接喂播放器）
DOUBAO_TTS_URL = "https://openspeech.bytedance.com/api/v3/tts/unidirectional"
DOUBAO_API_KEY = "REDACTED_DOUBAO_KEY"
DOUBAO_RESOURCE = "seed-tts-2.0"
DOUBAO_VOICE = "zh_female_vv_uranus_bigtts"

# DeepSeek LLM（步骤 4：流式生成 + 断句 + 逐句 TTS 编排）
DEEPSEEK_URL = "https://api.deepseek.com/chat/completions"
DEEPSEEK_KEY = "REDACTED_DEEPSEEK_KEY"
DEEPSEEK_MODEL = "deepseek-flash"
TERMINAL_PUNCT = "。！？!?；;\n"          # 断句终止符（软断句交给标点密度，这里求稳）


def xfyun_auth_url() -> str:
    """讯飞 IAT v2 鉴权 URL（HMAC-SHA256 签名，官方规则）。"""
    date = datetime.datetime.strftime(
        datetime.datetime.utcnow(), "%a, %d %b %Y %H:%M:%S GMT")
    origin = f"host: iat-api.xfyun.cn\ndate: {date}\nGET /v2/iat HTTP/1.1"
    sig = base64.b64encode(
        hmac.new(XFYUN_API_SECRET.encode(), origin.encode(),
                 hashlib.sha256).digest()).decode()
    auth_origin = (f'api_key="{XFYUN_API_KEY}", algorithm="hmac-sha256", '
                   f'headers="host date request-line", signature="{sig}"')
    auth = base64.b64encode(auth_origin.encode()).decode()
    return f"{XFYUN_URL}?{urlencode({'authorization': auth, 'date': date, 'host': 'iat-api.xfyun.cn'})}"


class AsrSession:
    """一轮讯飞流式听写会话：start → feed* → stop → 等最终文本。"""

    def __init__(self):
        self.ws = None                  # 讯飞侧连接
        self.text_parts: list[str] = [] # 累计识别文本
        self.failed = False
        self.done = asyncio.Event()     # 收到 status=2（最终结果）
        self.connected = False          # 讯飞握手完成且首帧已发
        self.pending = []               # 握手期间到达的音频帧（最多 150 帧≈15s）
        self.rx_task = None

    async def start(self):
        last_err = None
        for attempt in range(2):                        # 热点网络抖动：握手失败重试一次
            try:
                self.ws = await asyncio.wait_for(
                    websockets.connect(xfyun_auth_url(),
                                       ping_interval=None, max_size=None),
                    timeout=8)
                break
            except Exception as e:
                last_err = e
                log.warning("讯飞握手失败(第 %d 次): %r", attempt + 1, e)
                await asyncio.sleep(0.5)
        if not self.ws:
            raise last_err
        first = {
            "common": {"app_id": XFYUN_APP_ID},
            "business": {"language": "zh_cn", "domain": "iat",
                         "accent": "mandarin", "vad_eos": 10000},
            "data": {"status": 0, "format": "audio/L16;rate=16000",
                     "encoding": "raw", "audio": ""},
        }
        await self.ws.send(json.dumps(first))
        self.rx_task = asyncio.create_task(self._rx_loop())
        self.connected = True
        # 补发握手期间缓存的音频帧（设备边录边发，连接慢时不能丢）
        for pcm in self.pending:
            await self.feed(pcm)
        self.pending.clear()
        # 注意：讯飞 IAT 没有欢迎帧——不发音频不会回话，连接成功即就绪
        log.info("讯飞会话已连接（补发 %d 帧）", len(self.pending))

    async def _rx_loop(self):
        """讯飞结果接收循环：拼字；status=2 触发 done。"""
        try:
            async for raw in self.ws:
                msg = json.loads(raw)
                code = msg.get("code", -1)
                if code != 0:
                    log.error("讯飞错误 %s: %s", code, msg.get("message"))
                    self.failed = True
                    self.done.set()
                    return
                data = msg.get("data") or {}
                result = data.get("result")
                if result:
                    for row in result.get("ws", []):        # 每行拼 cw.w
                        self.text_parts.append(
                            "".join(cw.get("w", "") for cw in row.get("cw", [])))
                if data.get("status") == 2:                 # 最终结果
                    self.done.set()
                    return
        except websockets.ConnectionClosed:
            pass
        finally:
            self.ready.set()                                # 连接异常也放行等待方
            self.done.set()

    async def feed(self, pcm: bytes):
        """上行 PCM 重切成 1280B 子帧发讯飞（status=1 中间帧）。"""
        if not self.connected:                          # 握手中：缓存待补发
            if len(self.pending) < 150:
                self.pending.append(pcm)
            return
        if not self.ws:                                 # 会话未连上：丢弃本帧
            return
        for pos in range(0, len(pcm), XFYUN_FRAME_BYTES):
            chunk = pcm[pos:pos + XFYUN_FRAME_BYTES]
            frame = {"data": {"status": 1,
                              "format": "audio/L16;rate=16000",
                              "encoding": "raw",
                              "audio": base64.b64encode(chunk).decode()}}
            await self.ws.send(json.dumps(frame))

    async def stop(self) -> str:
        """发尾帧（status=2），等最终结果并返回文本。"""
        end = {"data": {"status": 2, "format": "audio/L16;rate=16000",
                        "encoding": "raw", "audio": ""}}
        await self.ws.send(json.dumps(end))
        await asyncio.wait_for(self.done.wait(), timeout=8)
        try:
            await self.ws.close()
        except Exception:
            pass
        if self.rx_task:
            self.rx_task.cancel()
        return "".join(self.text_parts)


class DeviceSession:
    """一台设备的会话：设备 WS ↔ 讯飞 ASR 的编排。"""

    def __init__(self, ws):
        self.ws = ws                     # 设备侧连接
        self.asr: AsrSession | None = None
        self.send_lock = asyncio.Lock()  # LLM/TTS 多任务并发推送：所有发送串行化

    async def send(self, obj):
        # dict → JSON；str/bytes 原样。加锁防并发 send 帧交错。
        data = json.dumps(obj) if isinstance(obj, (dict, list)) else obj
        async with self.send_lock:
            await self.ws.send(data)

    async def handle_text(self, message: str):
        try:
            msg = json.loads(message)
        except json.JSONDecodeError:
            log.warning("非 JSON 文本帧: %.120s", message)
            return
        mtype, data = msg.get("type", ""), msg.get("data", "")

        if mtype == "hello":
            log.info("设备握手: device=%s version=%s",
                     msg.get("device", "?"), msg.get("version", "?"))
            await self.send({"type": "welcome", "msg": "step2 asr ready"})
        elif mtype == "ping":
            await self.send({"type": "pong"})
        elif mtype == "asr_start":
            if self.asr:                                # 上一轮没收尾：强制清理
                await self._abort_asr()
            self.asr = AsrSession()
            try:
                await self.asr.start()
                log.info("ASR 会话开始")
            except Exception as e:
                log.error("讯飞连接失败: %s: %r", type(e).__name__, e)
                self.asr = None
                await self.send({"type": "asr_error", "data": f"xfyun connect: {e}"})
        elif mtype == "asr_stop":
            if not self.asr:
                return
            asr, self.asr = self.asr, None
            try:
                text = await asr.stop()
                log.info("ASR 结果: %s", text if text else "(空)")
                await self.send({"type": "asr_result", "data": text})
            except Exception as e:
                log.error("ASR 收尾失败: %s", e)
                await self.send({"type": "asr_error", "data": f"stop: {e}"})
            finally:
                self.asr = None                         # 会话已消费/作废，必须清引用
        elif mtype == "tts":                            # 步骤 3：TTS 下行
            asyncio.create_task(self.run_tts(data))
        elif mtype == "chat":                           # 步骤 4：LLM+TTS 全流水线
            asyncio.create_task(self.run_chat(msg))
        elif mtype == "text":
            log.info("设备文本: %s", data)
            await self.send({"type": "echo", "data": data})
        else:
            log.info("未知类型 %r", mtype)

    async def handle_binary(self, pcm: bytes):
        if self.asr:                                    # 仅会话内的音频帧有效
            await self.asr.feed(pcm)

    async def stream_doubao(self, text: str):
        """豆包流式合成一段文本：chunked PCM 块逐帧推回设备（不收尾）。"""
        headers = {
            "X-Api-Key": DOUBAO_API_KEY,
            "X-Api-Resource-Id": DOUBAO_RESOURCE,
        }
        body = {
            "user": {"uid": "sanjiu_esp32p4"},
            "req_params": {
                "text": text,
                "speaker": DOUBAO_VOICE,
                "audio_params": {"format": "pcm", "sample_rate": 24000, "channel": 1},
            },
        }
        async with aiohttp.ClientSession() as http:
            async with http.post(DOUBAO_TTS_URL, json=body, headers=headers,
                                 timeout=aiohttp.ClientTimeout(total=60)) as resp:
                if resp.status != 200:
                    raise RuntimeError(f"doubao HTTP {resp.status}: "
                                       f"{(await resp.text())[:150]}")
                buf = b""
                async for chunk in resp.content.iter_any():
                    buf += chunk
                    while b"\n" in buf:                     # 按行切（JSON 行式）
                        line, buf = buf.split(b"\n", 1)
                        if not line.strip():
                            continue
                        msg = json.loads(line)
                        code = msg.get("code", 0)
                        if code != 0 and code != 20000000:
                            log.error("豆包 TTS 错误 %s: %s", code, msg.get("message"))
                            raise RuntimeError(f"doubao {code}")
                        data = msg.get("data")
                        if data:                            # base64 PCM 块 → 二进制推回
                            await self.send(base64.b64decode(data))

    async def run_tts(self, text: str):
        """步骤 3 入口：整段文本流式合成，完成发 tts_end。"""
        if not text:
            await self.send({"type": "tts_end"})
            return
        try:
            await self.stream_doubao(text)
        except Exception as e:
            log.error("TTS 流失败: %r", e)
        await self.send({"type": "tts_end"})                # 成功失败都收尾（防设备卡等）

    @staticmethod
    def _split_sentence(buf: str):
        """断句：扫到终止标点即切一句（含标点）；没有返回 None。"""
        for i, ch in enumerate(buf):
            if ch in TERMINAL_PUNCT:
                return buf[:i + 1]
        return None

    async def run_chat(self, msg: dict):
        """步骤 4：流式 LLM → 断句 → 逐句豆包流式（LLM 生成与 TTS 合成真并行）。"""
        messages = [{"role": "system", "content": msg.get("sys", "")}]
        messages += msg.get("history", [])
        messages.append({"role": "user", "content": msg.get("text", "")})
        q: asyncio.Queue = asyncio.Queue()
        full = {"text": ""}

        async def llm_stream():
            headers = {"Authorization": f"Bearer {DEEPSEEK_KEY}",
                       "Content-Type": "application/json"}
            body = {"model": DEEPSEEK_MODEL, "messages": messages, "stream": True}
            buf = ""
            try:
                async with aiohttp.ClientSession() as http:
                    async with http.post(DEEPSEEK_URL, json=body, headers=headers,
                                         timeout=aiohttp.ClientTimeout(total=90)) as resp:
                        if resp.status != 200:
                            raise RuntimeError(f"deepseek {resp.status}: "
                                               f"{(await resp.text())[:150]}")
                        while True:
                            line = await resp.content.readline()
                            if not line:
                                break
                            line = line.decode("utf-8", "replace").strip()
                            if not line.startswith("data:"):
                                continue
                            payload = line[5:].strip()
                            if payload == "[DONE]":
                                break
                            delta = json.loads(payload)["choices"][0]["delta"]
                            piece = delta.get("content") or ""
                            if not piece:
                                continue
                            full["text"] += piece
                            buf += piece
                            while True:                     # 断句即入队（TTS 边合成）
                                sent = self._split_sentence(buf)
                                if sent is None:
                                    break
                                buf = buf[len(sent):]
                                await q.put(sent)
                                await self.send({"type": "reply_sentence",
                                                 "data": sent})     # 设备字幕流式刷新
                    if buf:                                 # 尾句
                        await q.put(buf)
            except Exception as e:
                log.error("LLM 流失败: %r", e)
                await self.send({"type": "chat_error", "data": str(e)})
            finally:
                await q.put(None)                           # 通知 TTS 工匠收工
                if full["text"]:
                    await self.send({"type": "reply_done", "data": full["text"]})

        async def tts_worker():
            while True:
                sent = await q.get()
                if sent is None:
                    break
                t0 = asyncio.get_event_loop().time()
                try:
                    await self.stream_doubao(sent)
                    log.info("句 TTS 完成 (%.0f ms): %.30s",
                             (asyncio.get_event_loop().time() - t0) * 1000, sent)
                except Exception as e:
                    log.error("句 TTS 失败: %r", e)          # 单句失败不拖垮整段

        worker = asyncio.create_task(tts_worker())
        await llm_stream()                                  # LLM 完成后 reply_done 已发
        await worker                                        # 等 TTS 队列排空
        await self.send({"type": "tts_end"})                # 全部音频推完（设备排空后收尾）

async def handle_device(ws):
    """单个设备连接的会话循环。"""
    peer = ws.remote_address
    sess = DeviceSession(ws)
    log.info("设备上线: %s", peer)
    try:
        await sess.send({"type": "welcome", "msg": "gateway step2 asr ready"})
        async for message in ws:
            if isinstance(message, str):
                await sess.handle_text(message)
            else:
                await sess.handle_binary(message)       # 音频上行（步骤 2）
    except websockets.ConnectionClosed as e:
        log.info("设备下线: %s（code=%s）", peer, e.code)
    finally:
        if sess.asr:                                    # 连接断开时清理在途会话
            await sess._abort_asr()
        log.info("连接清理: %s", peer)


async def xfyun_selftest():
    """启动自检：验证进程内能否连上讯飞（定位环境差异）。"""
    try:
        ws = await asyncio.wait_for(websockets.connect(xfyun_auth_url(),
                                                       ping_interval=None), timeout=10)
        await ws.close()
        log.info('自检: 讯飞连接 OK')
    except Exception as e:
        log.error('自检: 讯飞连接失败 %s: %r', type(e).__name__, e)


async def main():
    async with websockets.serve(handle_device, HOST, PORT,
                                ping_interval=15, ping_timeout=10,
                                max_size=4 * 1024 * 1024):
        log.info("语音网关（步骤 2：讯飞流式听写）已启动: ws://%s:%d", HOST, PORT)
        await xfyun_selftest()
        await asyncio.Event().wait()                    # 常驻


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log.info("网关退出")
