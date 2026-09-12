# -*- coding: utf-8 -*-
"""网关 AsrSession：连接重试 + 握手期间音频帧缓存。"""
p = "tools/gateway/gateway.py"
s = open(p, encoding="utf-8").read()

# 1) __init__ 加状态
old = """        self.failed = False
        self.done = asyncio.Event()     # 收到 status=2（最终结果）
        self.ready = asyncio.Event()    # 讯飞握手完成
        self.rx_task = None"""
new = """        self.failed = False
        self.done = asyncio.Event()     # 收到 status=2（最终结果）
        self.connected = False          # 讯飞握手完成且首帧已发
        self.pending = []               # 握手期间到达的音频帧（最多 150 帧≈15s）
        self.rx_task = None"""
assert old in s, "1"
s = s.replace(old, new, 1)

# 2) start(): 重试 + flush pending
old = """    async def start(self):
        self.ws = await websockets.connect(xfyun_auth_url(),
                                           ping_interval=None, max_size=None)
        first = {
            "common": {"app_id": XFYUN_APP_ID},
            "business": {"language": "zh_cn", "domain": "iat",
                         "accent": "mandarin", "vad_eos": 10000},
            "data": {"status": 0, "format": "audio/L16;rate=16000",
                     "encoding": "raw", "audio": ""},
        }
        await self.ws.send(json.dumps(first))
        self.rx_task = asyncio.create_task(self._rx_loop())
        # 注意：讯飞 IAT 没有欢迎帧——不发音频不会回话，连接成功即就绪
        log.info("讯飞会话已连接")"""
new = """    async def start(self):
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
        log.info("讯飞会话已连接（补发 %d 帧）", len(self.pending))"""
assert old in s, "2"
s = s.replace(old, new, 1)

# 3) feed(): 未连接时缓存
old = """        if not self.ws:                                 # 会话未连上：丢弃本帧
            return"""
new = """        if not self.connected:                          # 握手中：缓存待补发
            if len(self.pending) < 150:
                self.pending.append(pcm)
            return
        if not self.ws:                                 # 会话未连上：丢弃本帧
            return"""
assert old in s, "3"
s = s.replace(old, new, 1)

open(p, "w", encoding="utf-8", newline="\n").write(s)
print("gateway asr fix ok")
