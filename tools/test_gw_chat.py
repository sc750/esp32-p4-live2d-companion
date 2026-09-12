# -*- coding: utf-8 -*-
"""模拟设备验证步骤 4 chat 流水线：chat → reply_sentence* → reply_done → PCM* → tts_end。"""
import asyncio
import json
import time

import websockets

SYS = "你是中野三玖，傲娇但温柔。回复不超过两句话。"
HIST = [{"role": "user", "content": "你好"},
        {"role": "assistant", "content": "……哼，你好。"}]


async def main():
    async with websockets.connect("ws://127.0.0.1:8765",
                                  max_size=4 * 1024 * 1024) as ws:
        await ws.send(json.dumps({"type": "hello", "device": "fake-chat"}))
        await ws.send(json.dumps({
            "type": "chat", "sys": SYS, "history": HIST, "text": "今天吃什么好"}))
        t0 = time.time()
        first_sentence = None
        first_pcm = None
        pcm_total = 0
        while True:
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=60)
            except asyncio.TimeoutError:
                print("超时退出")
                break
            if isinstance(msg, bytes):
                pcm_total += len(msg)
                if first_pcm is None:
                    first_pcm = (time.time() - t0) * 1000
                    print(f"首个 PCM 帧 @ {first_pcm:.0f}ms")
                continue
            msg = json.loads(msg)
            mtype = msg.get("type")
            if mtype == "reply_sentence":
                if first_sentence is None:
                    first_sentence = (time.time() - t0) * 1000
                    print(f"首句 @ {first_sentence:.0f}ms: {msg.get('data')}")
                else:
                    print(f"续句 @ {(time.time() - t0) * 1000:.0f}ms: {msg.get('data')}")
            elif mtype == "reply_done":
                print(f"reply_done @ {(time.time() - t0) * 1000:.0f}ms: "
                      f"{msg.get('data', '')[:60]}")
            elif mtype == "tts_end":
                print(f"tts_end @ {(time.time() - t0) * 1000:.0f}ms / "
                      f"PCM 共 {pcm_total}B")
                break
            else:
                print("其他:", msg)

asyncio.run(main())
