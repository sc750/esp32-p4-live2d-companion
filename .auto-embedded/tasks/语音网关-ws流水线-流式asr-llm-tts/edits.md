# 编辑清单

| 文件 | 改动 | 验证标准 | 结果 | commit |
|---|---|---|---|---|
| tools/gateway/gateway.py（新增） | 网关骨架：WS 服务/握手/echo/统计/断线清理 | 板连上+握手+回显 | ✅ | 待提交 |
| user/ai/gw_client.c/.h（新增） | 设备 WS 客户端：自动连接/重连/hello/收发 | gw 命令显示已连接，回环通 | ✅ | 待提交 |
| user/ai/ai_http.c | http_common_setup 支持自定义头（open 前） | 豆包 X-Api-* 生效 | ✅（上轮） | 2f5c2f9 |
| user/Kconfig | GW_URL/GW_RECONNECT_MS | 默认 ws://10.56.204.52:8765 | ✅ | 待提交 |
| user/main_app.c | 4e. gw_client_init() | 启动即连 | ✅ | 待提交 |
| user/ai/chat_console.c | gw / gw send 命令 | 回环测试通过 | ✅ | 待提交 |

## 步骤 2 编辑清单（2026-09-12）

| 文件 | 改动 | 结果 |
|---|---|---|
| tools/gateway/gateway.py | 重写为会话类：AsrSession（讯飞 IAT v2/iat wss 流式，HMAC 签名，1280B 重切）+ DeviceSession 编排（asr_start/二进制帧/asr_stop→asr_result） | ✅ |
| user/ai/voice_rec.c/.h | 录音器加 100ms 块回调 tap（voice_rec_set_chunk_cb） | ✅ |
| user/ai/gw_client.c/.h | 加 send_binary + set_msg_handler（type/data 分发） | ✅ |
| user/ai/voice_pipeline.c | rec_until_stop_or 挂网关分支（asr_start/上行/asr_stop）；gw_asr_collect 等结果（5s 超时，空结果=NOT_FOUND 不回退）；process_wav 网关优先；record_ms 统一走 rec_until_stop_or（修绕过 bug） | ✅ |

### 实测证据
- 环境语音识别成功：识别「我把他被暴打了，好绝望。」ASR 全程 **1302ms**（旧整段 HTTP 路径 31017ms，24 倍）
- 静音场景：网关明确空结果 → 直接「没听清」（跳过本地回退，省 6s）
- 网关断开自动回退本地 HTTP 识别（降级链验证通过）

## 步骤 3 编辑清单（2026-09-12）

| 文件 | 改动 | 结果 |
|---|---|---|
| tools/gateway/gateway.py | run_tts：豆包 V3 chunked（format=pcm 24k，aiohttp 流式），base64 解码后二进制帧推回，tts_end 收尾 | ✅ |
| user/ai/gw_client.c/.h | 二进制帧（op_code 0x02）分发 → set_binary_handler | ✅ |
| user/ai/voice_pipeline.c | spk_ring_begin（ring+播放器初始化抽出复用）；EVT_TTS_DONE；process_wav 网关分支：tts 文本上行→PCM 帧喂 ring→排空→清理；失败回退本地豆包 | ✅ |

### 实测证据
- 模拟设备协议测试：tts 请求 → 首帧 PCM 561ms → 9 帧 152KB（≈4.8s 音频）927ms 推完 → tts_end
- 设备侧待语音实测（serial chat 走直连文本通道不经过 process_wav，故串口 chat 不触发网关 TTS）

## 步骤 4 编辑清单（2026-09-12）

| 文件 | 改动 | 结果 |
|---|---|---|
| user/ai/dialog_manager.h/.c | 新增 dialog_build_gw_context（sys+历史 JSON，设备保持状态源）/ dialog_commit_gw_round（历史+摘要+日记素材落账，不调 LLM） | ✅ |
| tools/gateway/gateway.py | run_chat：DeepSeek 流式（SSE）→ 断句 → reply_sentence/reply_done → asyncio.Queue → 逐句豆包流式 PCM 推回；DeviceSession.send 全局串行锁（LLM/TTS 双任务并发推送防帧交错）；chat_error 处理 | ✅ |
| user/ai/voice_pipeline.c | gw_dialog_round（chat 上行 → EVT_TTS_DONE 等待 90s → 落账 → ring 排空清理）；process_wav 重构：网关流水线优先，本地全流程整体回退；首块 PCM 切 SPEAKING 状态 | ✅ |

### 协议实测（模拟设备）
- chat → 首句字幕 1671ms / reply_done 1673ms / 首帧 PCM 2217ms / tts_end 2862ms（240KB PCM）
- 网关编排已实现"LLM 生成与 TTS 合成真并行"（无 TLS 锁限制）

### 预期板上效果
松手 → ASR 1.3s → LLM 首句+TTS 首包（网关并行 ~2s）→ 首声 ≈ 3.5s（对比直连版 25-35s）

## 构建证据（2026-09-12）
- 端到端：板上 gw send → 网关「设备文本: hello-gateway-step1」→ 网关 echo →
  板上「网关→: {"type":"echo",...}」；断线（1006）后 3s 自动重连握手
