# M07: AI Agent (ASR/LLM/TTS)

> **优先级**: P0 | **预估工时**: 3-4 周 | **依赖**: M05, M06

---

## 1. 模块概述

实现全双工语音对话的完整 AI 流水线：流式 ASR (语音识别) → LLM (大模型推理含 function calling) → 流式 TTS (语音合成)。这是系统的核心智能模块。

---

## 2. 对话流水线架构

```
麦克风音频流
    │
    ▼
┌──────────────┐
│  VAD 检测     │  检测用户开始/停止说话
│  (本地)       │
└──────┬───────┘
       │ 语音帧
       ▼
┌──────────────┐    WebSocket/HTTP    ┌──────────────┐
│  流式 ASR     │ ──────────────────▶ │  ASR 云端     │
│  (客户端)     │ ◀── partial text ── │  (Whisper等)  │
└──────┬───────┘                      └──────────────┘
       │ final text
       ▼
┌──────────────┐    HTTPS (SSE)       ┌──────────────┐
│  LLM Client   │ ──────────────────▶ │  LLM API      │
│  (推理调度)    │ ◀── token stream ── │  (OpenAI兼容)  │
│               │                      │               │
│  Function     │ ◀── tool_call ──── │               │
│  Calling      │ ──▶ 执行工具 ──────▶ │               │
└──────┬───────┘                      └──────────────┘
       │ response text
       ▼
┌──────────────┐    HTTPS              ┌──────────────┐
│  流式 TTS     │ ──────────────────▶ │  TTS 云端     │
│  (客户端)     │ ◀── audio chunk ─── │  (Edge TTS等) │
└──────┬───────┘                      └──────────────┘
       │ PCM audio chunks
       ▼
┌──────────────┐
│  音频播放     │  → 扬声器
│  (流式输出)   │
└──────────────┘
```

### 端到端时序 (目标 ≤4 秒)

```
0.0s  用户停止说话
0.0-0.5s  VAD 确认 + 最后音频帧上传
0.5-1.5s  ASR 处理 → 返回最终文本
1.5-3.0s  LLM 推理 (首 token ~0.5s, 流式输出)
3.0-4.0s  TTS 首包音频到达 → 开始播放
4.0s+     TTS 持续流式播放, 同时 LLM 继续输出
```

---

## 3. 功能需求

### 3.1 ASR (自动语音识别)

| ID | 需求 | 优先级 |
|----|------|--------|
| ASR-01 | 流式语音识别 (WebSocket 或 streaming HTTP) | P0 |
| ASR-02 | 支持中文识别 | P0 |
| ASR-03 | VAD 本地检测 (开/关判断) | P0 |
| ASR-04 | 中间结果 (partial) 实时显示 | P0 |
| ASR-05 | 最终结果 (final) 触发 LLM | P0 |
| ASR-06 | 采样率转换 (16kHz → API 要求) | P1 |

### 3.2 LLM (大语言模型)

| ID | 需求 | 优先级 |
|----|------|--------|
| LLM-01 | OpenAI 兼容 API 调用 | P0 |
| LLM-02 | 流式 token 接收 (SSE) | P0 |
| LLM-03 | Function Calling (工具调用) | P0 |
| LLM-04 | System Prompt (角色人设) | P0 |
| LLM-05 | 上下文管理 (对话历史) | P0 |
| LLM-06 | 情感标签输出 (用于表情切换) | P1 |
| LLM-07 | 中日双语输出 (可选) | P2 |

### 3.3 TTS (文本转语音)

| ID | 需求 | 优先级 |
|----|------|--------|
| TTS-01 | 流式 TTS (边生成边播放) | P0 |
| TTS-02 | 中文语音合成 | P0 |
| TTS-03 | 日文语音合成 (可选) | P2 |
| TTS-04 | 低首包延迟 (≤1s) | P0 |
| TTS-05 | PCM 16kHz 输出 | P0 |

---

## 4. VAD (Voice Activity Detection)

### 4.1 方案选择

| 方案 | 说明 | 推荐 |
|------|------|------|
| 简单能量阈值 | 计算帧能量, 超过阈值=有语音 | ✅ 首选, 资源极少 |
| WebRTC VAD | Google 开源, 高精度 | 备选, 需要额外 ROM |
| 云端 VAD | 由 ASR 服务判断 | 不推荐, 延迟高 |

### 4.2 简单能量 VAD

```c
typedef struct {
    int16_t  energy_threshold;   // 动态调整的阈值
    int      speech_frames;     // 连续语音帧计数
    int      silence_frames;    // 连续静音帧计数
    bool     is_speaking;       // 当前状态
    int      hangover_frames;   // 语音结束后保留帧数 (避免截尾)
} vad_state_t;

// 检测一帧 (1024 samples = 64ms @ 16kHz)
bool vad_detect(vad_state_t *vad, const int16_t *samples, size_t count) {
    // 计算 RMS 能量
    int32_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += samples[i] * samples[i];
    }
    int16_t rms = (int16_t)sqrtf((float)sum / count);
    
    if (rms > vad->energy_threshold) {
        vad->speech_frames++;
        vad->silence_frames = 0;
        vad->is_speaking = true;
    } else {
        vad->silence_frames++;
        if (vad->silence_frames > vad->hangover_frames) {
            vad->speech_frames = 0;
            vad->is_speaking = false;
        }
    }
    
    return vad->is_speaking;
}
```

---

## 5. ASR 流式客户端

### 5.1 WebSocket 方案 (推荐)

```c
// asr_client.h

typedef struct {
    ws_handle_t ws;
    char *partial_text;         // 中间结果
    char *final_text;           // 最终结果
    bool  is_final;             // 是否为最终结果
    SemaphoreHandle_t done_sem;
} asr_session_t;

// 启动 ASR 会话
esp_err_t asr_session_start(asr_session_t *session);

// 发送音频帧 (16kHz 16bit PCM)
esp_err_t asr_session_send_audio(asr_session_t *session, 
                                 const int16_t *samples, size_t count);

// 接收识别结果 (阻塞, 超时返回)
esp_err_t asr_session_receive(asr_session_t *session, 
                              char **text, bool *is_final, 
                              uint32_t timeout_ms);

// 结束会话
esp_err_t asr_session_stop(asr_session_t *session);
```

### 5.2 典型 API 格式

```json
// 发送: 音频帧 (binary WebSocket frame)
// 接收: 识别结果
{
    "text": "你好，请问今天天气怎么样",
    "is_final": true,
    "confidence": 0.95
}
```

---

## 6. LLM 客户端

### 6.1 请求格式

```json
POST /v1/chat/completions
{
    "model": "gpt-4o-mini",
    "stream": true,
    "messages": [
        {
            "role": "system",
            "content": "你是一个可爱的二次元桌面助手，名叫小星。你说话温柔可爱，会用颜文字和语气词..."
        },
        {
            "role": "user", 
            "content": "你好啊"
        }
    ],
    "tools": [
        {
            "type": "function",
            "function": {
                "name": "set_alarm",
                "description": "设置闹钟",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "time": { "type": "string", "description": "时间 HH:MM" }
                    }
                }
            }
        }
    ]
}
```

### 6.2 SSE 流式处理

```c
// llm_client.h

typedef struct {
    char *token;                // 当前 token
    char *full_response;        // 完整响应
    bool  is_done;              // 是否结束
    bool  has_tool_call;        // 是否有工具调用
    struct {
        char *name;             // 函数名
        char *arguments;        // JSON 参数
    } tool_call;
} llm_stream_event_t;

typedef void (*llm_stream_callback_t)(const llm_stream_event_t *event, void *ctx);

// 流式 LLM 请求
esp_err_t llm_stream_request(const char *system_prompt,
                             const char *user_message,
                             const llm_tool_def_t *tools,
                             int tool_count,
                             llm_stream_callback_t on_event,
                             void *user_ctx);
```

### 6.3 Function Calling 流程

```
LLM 返回 tool_call
    │
    ├── 1. 解析函数名 + 参数
    ├── 2. 执行本地函数 (如设置闹钟、查天气、控制音乐)
    ├── 3. 将结果格式化为 tool_result
    ├── 4. 发送回 LLM (继续对话)
    └── 5. LLM 生成最终回复
```

### 6.4 支持的工具列表

| 工具名 | 功能 | 参数 |
|--------|------|------|
| `set_alarm` | 设置闹钟 | time: "HH:MM" |
| `play_music` | 播放音乐 | query: "歌名/歌手" |
| `stop_music` | 停止音乐 | — |
| `set_pomodoro` | 启动番茄钟 | duration: 分钟数 |
| `get_weather` | 查询天气 | city: "城市名" |
| `set_reminder` | 设置提醒 | time: "HH:MM", content: "内容" |
| `read_diary` | 朗读日记 | date: "YYYY-MM-DD" (可选) |
| `adjust_volume` | 调节音量 | level: 0-100 |
| `change_expression` | 切换角色表情 | expression: "happy/sad/..." |
| `sleep_mode` | 进入休眠 | — |
| `see_surroundings` | 摄像头看周围 (M14) | focus: "user/desk/screen/general" |
| `take_photo` | 拍照保存 (M14) | save: true/false |
| `check_expression` | 查看用户表情 (M14) | — |

---

## 7. TTS 流式客户端

### 7.1 Edge TTS 方案

```c
// tts_client.h

typedef void (*tts_audio_callback_t)(const int16_t *pcm_data, 
                                     size_t samples, void *ctx);

// 流式 TTS (文本 → PCM 音频流)
esp_err_t tts_stream_synthesize(const char *text,
                                const char *voice,  // "zh-CN-XiaoxiaoNeural"
                                tts_audio_callback_t on_audio,
                                void *user_ctx);
```

### 7.2 音频回调处理

```c
void tts_audio_callback(const int16_t *pcm_data, size_t samples, void *ctx) {
    // 直接写入扬声器环形缓冲区
    audio_ring_buf_write(&speaker_ring_buf, pcm_data, samples * sizeof(int16_t));
    
    // 同时更新 Live2D 口型
    live2d_set_mouth_open(compute_mouth_value(pcm_data, samples));
}
```

---

## 8. 对话管理器

```c
// dialog_manager.h

#define MAX_DIALOG_HISTORY    20
#define MAX_MESSAGE_LENGTH    512

typedef struct {
    char role[16];       // "system", "user", "assistant"
    char content[MAX_MESSAGE_LENGTH];
} dialog_message_t;

typedef struct {
    dialog_message_t history[MAX_DIALOG_HISTORY];
    int count;
    char system_prompt[2048];
    
    // 状态
    bool is_listening;
    bool is_thinking;
    bool is_speaking;
    
    // 当前回合
    char *current_user_input;
    char *current_assistant_reply;
} dialog_context_t;

// 开始新一轮对话
esp_err_t dialog_begin_round(dialog_context_t *ctx, const char *user_input);

// 处理 LLM 流式 token
esp_err_t dialog_on_llm_token(dialog_context_t *ctx, const char *token);

// 处理 tool_call
esp_err_t dialog_on_tool_call(dialog_context_t *ctx, const char *name, const char *args);

// 完成一轮对话
esp_err_t dialog_end_round(dialog_context_t *ctx);

// 从 NVS 持久化/恢复上下文
esp_err_t dialog_save_context(dialog_context_t *ctx);
esp_err_t dialog_load_context(dialog_context_t *ctx);
```

---

## 9. 对话状态与 Live2D 联动

| 对话阶段 | Live2D 表情 | Live2D 动作 | 字幕状态 |
|---------|------------|------------|---------|
| 等待用户说话 | normal | idle | 隐藏 |
| 用户说话中 | listening | 微微前倾 | 显示 ASR partial |
| AI 思考中 | thinking | 思考手势 | 显示 "..." |
| AI 说话中 | speaking | 口型同步 | 显示回复文本 |
| AI 开心 | happy | 欢快动作 | 显示回复文本 |
| AI 安慰 | sad | 关切表情 | 显示回复文本 |

---

## 10. API 配置

```c
// ai_config.h

// ASR 配置
#define ASR_API_URL        "wss://api.example.com/v1/streaming"
#define ASR_API_KEY        "sk-xxx"
#define ASR_LANGUAGE       "zh"

// LLM 配置
#define LLM_API_URL        "https://api.openai.com/v1/chat/completions"
#define LLM_API_KEY        "sk-xxx"
#define LLM_MODEL          "gpt-4o-mini"
#define LLM_MAX_TOKENS     512
#define LLM_TEMPERATURE    0.8

// TTS 配置
#define TTS_API_URL        "https://api.example.com/tts"
#define TTS_VOICE          "zh-CN-XiaoxiaoNeural"
#define TTS_RATE           "+0%"
#define TTS_PITCH          "+0Hz"
```

---

## 11. 测试用例

| ID | 测试项 | 预期结果 |
|----|--------|---------|
| TST-AI-01 | ASR 中文识别 | "你好" → 正确识别 |
| TST-AI-02 | LLM 流式回复 | Token 逐个到达, 无卡顿 |
| TST-AI-03 | TTS 流式播放 | 首包 ≤1s, 无断音 |
| TST-AI-04 | 端到端对话 | 语音→文字→回复→语音 ≤4s |
| TST-AI-05 | Function Calling | "设个闹钟" → 调用 set_alarm |
| TST-AI-06 | 多轮对话 | 上下文正确保持 |
| TST-AI-07 | 表情联动 | "开心" → happy 表情 |
| TST-AI-08 | 网络断开恢复 | 自动重连后继续对话 |
| TST-AI-09 | 连续对话 (10 轮) | 无崩溃/内存泄漏 |
| TST-AI-10 | 并发 (边听边说) | 正确处理打断 |
