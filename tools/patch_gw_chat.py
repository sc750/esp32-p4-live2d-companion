# -*- coding: utf-8 -*-
"""网关步骤 4：chat 流水线（流式 LLM → 断句 → 逐句豆包流式 → PCM 推回）。"""
p = "tools/gateway/gateway.py"
s = open(p, encoding="utf-8").read()

# 1) "tts" 分支旁加 "chat" 分支
old = '''        elif mtype == "tts":                            # 步骤 3：TTS 下行
            asyncio.create_task(self.run_tts(data))'''
new = old + '''
        elif mtype == "chat":                           # 步骤 4：LLM+TTS 全流水线
            asyncio.create_task(self.run_chat(msg))'''
assert old in s, "1"
s = s.replace(old, new, 1)

# 2) run_tts 的豆包推流段抽成 stream_doubao；run_tts 复用
old = """    async def run_tts(self, text: str):
        \"\"\"豆包流式 TTS：整段文本 → chunked PCM 块逐帧推回设备，完成发 tts_end。\"\"\"
        if not text:
            await self.send({"type": "tts_end"})
            return
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
        t0 = asyncio.get_event_loop().time()
        first = True
        try:
            async with aiohttp.ClientSession() as http:
                async with http.post(DOUBAO_TTS_URL, json=body,
                                     headers=headers,
                                     timeout=aiohttp.ClientTimeout(total=60)) as resp:
                    if resp.status != 200:
                        log.error("豆包 TTS HTTP %d: %.200s", resp.status,
                                  (await resp.text())[:200])
                        await self.send({"type": "tts_end"})
                        return
                    buf = b""
                    async for chunk in resp.content.iter_any():
                        buf += chunk
                        while b"\\n" in buf:                # 按行切（JSON 行式）
                            line, buf = buf.split(b"\\n", 1)
                            if not line.strip():
                                continue
                            msg = json.loads(line)
                            code = msg.get("code", 0)
                            if code != 0 and code != 20000000:
                                log.error("豆包 TTS 错误 %s: %s", code, msg.get("message"))
                                raise RuntimeError(f"doubao {code}")
                            data = msg.get("data")
                            if data:                        # base64 PCM 块 → 二进制推回
                                if first:
                                    log.info("TTS 首块延迟 %.0f ms",
                                             (asyncio.get_event_loop().time() - t0) * 1000)
                                    first = False
                                await self.ws.send(base64.b64decode(data))
        except Exception as e:
            log.error("TTS 流失败: %r", e)
        await self.send({"type": "tts_end"})                # 成功失败都收尾（防设备卡等）"""
new = '''    async def stream_doubao(self, text: str):
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
                    while b"\\n" in buf:                    # 按行切（JSON 行式）
                        line, buf = buf.split(b"\\n", 1)
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
        await self.send({"type": "tts_end"})                # 全部音频推完（设备排空后收尾）'''
assert old in s, "2"
s = s.replace(old, new, 1)

open(p, "w", encoding="utf-8", newline="\n").write(s)
print("chat flow ok")
