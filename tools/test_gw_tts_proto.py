# -*- coding: utf-8 -*-
"""模拟设备验证网关 TTS 协议：tts 文本 → 二进制 PCM 帧 → tts_end。"""
import asyncio
import json
import time

import websockets


async def main():
    async with websockets.connect("ws://127.0.0.1:8765", max_size=4 * 1024 * 1024) as ws:
        await ws.send(json.dumps({"type": "hello", "device": "fake-test"}))
        await ws.send(json.dumps({"type": "tts", "data": "你好，我是通过网关合成的测试语音。"}))
        t0 = time.time()
        first = None
        total = 0
        frames = 0
        while True:
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=20)
            except asyncio.TimeoutError:
                print("超时退出")
                break
            if isinstance(msg, bytes):
                frames += 1
                total += len(msg)
                if first is None:
                    first = (time.time() - t0) * 1000
                    print(f"首个 PCM 帧: {len(msg)}B @ {first:.0f}ms")
            else:
                msg = json.loads(msg)
                if msg.get("type") == "tts_end":
                    print(f"tts_end: {frames} 帧 / {total}B PCM / 首帧 {first:.0f}ms / "
                          f"总耗时 {(time.time() - t0) * 1000:.0f}ms")
                    break
                print("文本:", msg)

asyncio.run(main())
