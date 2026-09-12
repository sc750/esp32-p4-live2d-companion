#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gateway.py —— 语音网关骨架（步骤 1：WS 通道打通）

当前职责（步骤 1，只做通道验证）：
  - 监听 ws://0.0.0.0:8765，接受 ESP32 设备连接
  - 文本帧：解析 JSON（type/data），ping→pong，text→echo 回显
  - 二进制帧：仅统计并打印（后续步骤承载音频上行）
  - 协议级心跳由 websockets 库自带 ping_interval 保证

后续步骤（占位，不实现）：
  - 步骤 2：音频上行 → 流式 ASR（Silero VAD 断句）
  - 步骤 3：TTS 下行（音频帧推回设备）
  - 步骤 4：流式 LLM 编排（persona/历史/记忆注入）

用法：
  pip install websockets
  py tools/gateway/gateway.py
"""
import asyncio
import json
import logging

import websockets

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("gateway")

HOST = "0.0.0.0"
PORT = 8765

# 已连接的设备集合（当前只预期一台 ESP32，多连也不拒绝）
clients: set = set()

# 累计统计（诊断用）
stats = {"text_rx": 0, "binary_rx": 0, "binary_bytes": 0}


async def handle_device(ws):
    """单个设备连接的会话循环。"""
    peer = ws.remote_address
    clients.add(ws)
    log.info("设备上线: %s（当前在线 %d）", peer, len(clients))
    try:
        # 握手欢迎语：设备收到即视为通道可用
        await ws.send(json.dumps({"type": "welcome", "msg": "gateway step1 ready"}))
        async for message in ws:
            if isinstance(message, str):
                stats["text_rx"] += 1
                await handle_text(ws, message)
            else:
                # 二进制帧：步骤 2 起为音频上行，这里先只统计
                stats["binary_rx"] += 1
                stats["binary_bytes"] += len(message)
                if stats["binary_rx"] % 50 == 1:        # 限流打印，防刷屏
                    log.info("二进制帧 #%d: %d B（累计 %d B）",
                             stats["binary_rx"], len(message), stats["binary_bytes"])
    except websockets.ConnectionClosed as e:
        log.info("设备下线: %s（code=%s）", peer, e.code)
    finally:
        clients.discard(ws)
        log.info("连接清理: %s（当前在线 %d）", peer, len(clients))


async def handle_text(ws, message: str):
    """文本帧：约定 JSON {"type": ..., "data": ...}；解析失败原样回显。"""
    try:
        msg = json.loads(message)
    except json.JSONDecodeError:
        log.warning("非 JSON 文本帧: %.120s", message)
        return

    mtype = msg.get("type", "")
    data = msg.get("data", "")

    if mtype == "hello":
        log.info("设备握手: device=%s version=%s",
                 msg.get("device", "?"), msg.get("version", "?"))
        await ws.send(json.dumps({"type": "welcome", "msg": "step1 channel ok"}))
    elif mtype == "ping":
        await ws.send(json.dumps({"type": "pong"}))
    elif mtype == "text":
        log.info("设备文本: %s", data)
        await ws.send(json.dumps({"type": "echo", "data": data}))
    elif mtype == "stats":
        await ws.send(json.dumps({"type": "stats", "data": stats}))
    else:
        log.info("未知类型 %r: %.80s", mtype, data)


async def periodic_report():
    """每 60 秒打印一次概览（在线数/流量统计）。"""
    while True:
        await asyncio.sleep(60)
        log.info("概览: 在线=%d 文本帧=%d 二进制帧=%d(%dB)",
                 len(clients), stats["text_rx"],
                 stats["binary_rx"], stats["binary_bytes"])


async def main():
    async with websockets.serve(handle_device, HOST, PORT,
                                ping_interval=15, ping_timeout=10):
        log.info("语音网关（步骤 1）已启动: ws://%s:%d", HOST, PORT)
        await periodic_report()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log.info("网关退出")
