# 研究发现（Phase 3 语音对话 MVP）

| 关键词 | 来源 | 摘要 | 可信度 | 状态 |
|---|---|---|---|---|
| 音频 BSP 现状 | user/bsp/bsp_audio.c/.h | ES8311 已通：open/volume/set_fs(运行时切采样率)/record(阻塞读)/play(阻塞写)。缺：ring buffer 流式封装、PDM 路径（实际板子走 ES8311 ADC，PRD 写 PDM 有误）、双工并发验证 | 高（代码实证） | 已确认 |
| 采样率 | bsp_audio.h | 默认 16k/16bit/2ch；set_fs 可切。ASR 输入 16k ✓，TTS 输出若 mp3 需解码 | 高 | 已确认 |
| 状态机契约 | app_state_machine.c | LISTENING/THINKING/SPEAKING 转移规则已备（Phase 2 拆了入口），Phase 3 只需接事件源 | 高 | 已确认 |
| 状态层规格 | task phase2 prd.md | 主页对话状态层：字幕升高+波形条+状态点（蓝听/橙思/绿说），ui_bridge 已具备跨任务更新能力 | 高 | 已确认 |
| 口型联动 | rig_rig.h | rig_rig_set_mouth(RIG_MOUTH_*) 外部驱动接口已留；说话优先级 TTS>闲聊>idle | 高 | 已确认 |
| 网络栈 | R12 实测 | WiFi 稳定 + 自动重连；mbedTLS 需串行化（PRD 风险表 + 硬件约束：共享 SHA/AES 加速器）；HTTP/TLS 缓冲预算 ~2MB PSRAM | 高 | 已确认 |
| LLM 接口 | PRD M07 §6 | OpenAI 兼容 /v1/chat/completions + SSE 流式 + system prompt 人设 + function calling(P0 但 MVP 可缓) | 高 | ✅ 定：DeepSeek（key 文档已给） |
| ASR 接口 | docs/key与说明文档.md | **小米 mimo-v2.5-asr 走 OpenAI chat.completions 格式**！音频 wav/mp3 → base64 → `content[].input_audio.data`（data URL 前缀 `data:audio/wav;base64,`），`asr_options.language:"zh"`；base64≤10MB（≈16k16bit 录音 230s，够）；**返回标准 completion 文本**。统一 HTTP 客户端可行！ | 高（官方文档） | ✅ 定：mimo 首选/百度 fallback |
| TTS 接口 | docs/key与说明文档.md | **同为 chat.completions 格式**：合成文本放 `role:assistant`，风格指令放 `role:user`，`audio:{format,voice}`；流式 format=pcm16 → **24kHz PCM16LE mono** base64 分块（delta.audio.data）；音色"冰糖/茉莉"（中文女声）；限时免费。voiceclone/voicedesign 后期可玩（甚至克隆三玖声线） | 高（官方文档） | ✅ 定：mimo-v2.5-tts |
| 端点 | docs/key与说明文档.md | DeepSeek: api.deepseek.com（OpenAI 兼容）；MiMo token plan: **token-plan-cn.xiaomimimo.com/v1**（文档示例的 api.xiaomimimo.com 是通用端点，token plan 必须用专属 URL）；模型 deepseek-v4-flash / mimo-v2.5-asr / mimo-v2.5-tts | 高 | 已确认 |
| key 存放 | 项目惯例 | 三家 key 走 WiFi 凭证同款模式：Kconfig 定义 CONFIG_AI_*，真实值填 gitignore 的本地 sdkconfig；docs/key与说明文档.md 已加 .gitignore（防真实 key 入库） | 高 | 已落实 |
| VAD | PRD M07 §4 | 能量阈值 VAD 极简（~30 行）；MVP 先按住说话（push-to-talk）绕开 | 高 | M3 后置 |

## MVP 分解建议（迭代式，每步可验）

- **M0 文本链路**（先通脑子）：LLM HTTP(SSE) 客户端 + 人设 prompt + 对话历史 → 字幕显示回复。
  验证：不发声，字幕看到三玖的 AI 回复。此步不需要 ASR/TTS。
- **M1 装耳朵**：录音（按住说话）→ ASR REST → 文本进 LLM。
  验证：说话 → 字幕先显识别文本再显回复。
- **M2 装嘴巴**：TTS → mp3 解码 → 流式播放 + 口型联动 + 状态点。
  验证：完整语音对话，端到端延迟记录（PRD 目标 ≤4s）。
- **M3 打磨**（后置）：流式 TTS、VAD 自动断句、function calling、表情标签联动。

## 候选方案对比（待用户拍板）

| 环节 | 候选 | 优点 | 缺点 |
|---|---|---|---|
| LLM | DeepSeek / 通义 / Kimi / OpenAI / 中转 | 均为 OpenAI 兼容，代码同一套 | 价格/网络可达性差异 |
| ASR | OpenAI audio/transcriptions | REST 一发一收，最简 | 需国际网络 |
| ASR | 百度短语音 REST | 国内直连，免费额度 | key 申请 |
| ASR | 讯飞流式 WS | 延迟低、partial 流式 | 协议复杂，MVP 超纲 |
| TTS | OpenAI TTS (mp3) | REST 最简，音质好 | 需国际网络 |
| TTS | Edge TTS | 免费 | WS 私有协议，P4 移植风险 |
| TTS | 讯飞/火山 HTTP | 国内直连 | key/计费 |
