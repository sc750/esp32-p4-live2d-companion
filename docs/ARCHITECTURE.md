# ARCHITECTURE.md — 分层架构与设计决策

> 本文档解释项目**为什么这样设计**。编码规范见 [CODING_STYLE.md](CODING_STYLE.md)。

---

## 1. 系统总览

```
┌─────────────────────────────────────────────────────────┐
│                     Application Layer                    │
│  scr_home  scr_chat  scr_music  scr_pomodoro  scr_diary │  LVGL 页面
├─────────────────────────────────────────────────────────┤
│                  State Machine Layer                     │
│              app_state_machine (状态流转)                 │  管理用户交互
├─────────────────────────────────────────────────────────┤
│                    Runtime Layer                         │
│     event_bus  timer_manager  power_manager              │  基础设施
├─────────────────────────────────────────────────────────┤
│                   Service Layer                          │
│  audio_service  ai_agent  network  music  vision  ...   │  业务逻辑
├─────────────────────────────────────────────────────────┤
│                  UI Bridge Layer                         │
│     ui_bridge  live2d_bridge                             │  状态→控件同步
├─────────────────────────────────────────────────────────┤
│                  Rendering Layer                         │
│  lvgl_engine  painterengine_adapter  ppa_blend          │  画面合成
├─────────────────────────────────────────────────────────┤
│                      HAL Layer                           │
│  bsp_audio  bsp_display  bsp_camera  bsp_network        │  硬件抽象
└─────────────────────────────────────────────────────────┘
```

---

## 2. 为什么这样分层

### 2.1 上下分层（依赖方向）

```
app_main.c
    ↓
app_state_machine.c
    ↓
services/*.c
    ↓
bridges/*.c
    ↓
engines/*.c
    ↓
bsp/*.c
```

**设计理由**：
- **BSP 层**独立于上层逻辑，更换硬件只需改 BSP，不影响业务代码
- **Service 层**只关心"业务流程"，不关心数据怎么渲染到屏幕上
- **Bridge 层**负责"服务状态 → LVGL 控件"的单向同步，UI 和业务彻底解耦
- **反向依赖是被禁止的**：如果 Service 层直接调用 LVGL API，换 UI 框架时 Service 全废

### 2.2 同层通信

同层模块**不直接调用彼此函数**，而是通过事件总线：

```c
// ❌ 错误：AI Agent 直接调用音频服务
audio_service_speaker_write(tts_data, len);

// ✅ 正确：通过事件总线解耦
event_bus_post(EVENT_TTS_DATA, &(event_data_t){ .tts = ... });
```

**设计理由**：直接调用会导致模块间强耦合，改一个模块连带改多个文件。事件总线让发送方和接收方互不感知，新增功能只需订阅事件。

### 2.3 双核分工

```
Core 0: LVGL 渲染 + 网络请求 + SD 卡读写
Core 1: Live2D 动画 + 音频处理 + AI 推理
```

**设计理由**：
- ESP32-P4 有 2 个 400MHz 核心，分开用充分利用算力
- LVGL 和 Live2D 都是 CPU 密集型，放在同一核会互相抢占
- 网络 TLS 操作 (mbedTLS) 耗时且需要独占，放在 Core 0 避免干扰音频
- 两个核通过 FreeRTOS 队列/信号量安全通信

---

## 3. 数据流

### 3.1 语音对话流（核心交互）

```
用户说话 → [Core 1] 麦克风采集 (ES8311 ADC)
         → VAD 语音活动检测
         → ASR 语音识别 (WebSocket 流式)
         → [Core 0] LLM 大模型推理 (HTTPS 请求串行化队列)
         → 工具调用 (记忆/番茄钟/Vision...)
         → 流式文本输出
         → TTS 语音合成 (HTTP 流式)
         → [Core 1] 音频播放 (ES8311 DAC + NS4150 功放)
         → Live2D 根据情感标签切换表情
         → 字幕显示中文
```

### 3.2 渲染管线

```
[Core 1]                          [Core 0]
Live2D 动画                       LVGL 界面
(ARGB8888 overlay)               (RGB565 背景)
        │                                │
        ▼                                ▼
    ┌──────────────────────────────────────┐
    │        PPA Blend (硬件)              │
    │  ARGB8888 + RGB565 → RGB565 帧缓冲   │
    └──────────────────────────────────────┘
                    │
                    ▼
            MIPI-DSI → 屏幕
```

**设计理由**：
- Live2D 需要 ARGB8888（带透明通道），LVGL 用 RGB565（省内存）
- PPA 硬件融合 = 零 CPU 开销，比软件混合快 10 倍
- 双缓冲 + VSync 同步 = 无撕裂，20FPS 稳定

### 3.3 网络请求串行化

```
请求队列 (FIFO)
┌─────────┬─────────┬─────────┐
│ ASR 请求 │ LLM 请求 │ TTS 请求 │ → TLS 处理 → 释放 → 下一个
└─────────┴─────────┴─────────┘
```

**设计理由**：ESP32-P4 的 mbedTLS 使用硬件 SHA/AES 加速器，多个 TLS 连接并发时会共享硬件导致崩溃。串行化是唯一可靠方案。

---

## 4. 模块职责表

| 模块 | 职责 | 核心文件 |
|------|------|---------|
| event_bus | 全局事件发布/订阅 | `core/event_bus.c` |
| audio_service | ES8311 音频输入输出 | `services/audio_service.c` |
| ai_agent | 语音对话全流程编排 | `services/ai_agent.c` |
| network_service | HTTP/TLS/WebSocket | `services/network_service.c` |
| memory_service | 长期记忆存储/检索 | `services/memory_service.c` |
| vision_service | 摄像头视觉 AI | `services/vision_service.c` |
| ui_bridge | 服务状态 → LVGL 控件同步 | `bridges/ui_bridge.c` |
| live2d_bridge | 表情/动作 → Live2D 控制 | `bridges/live2d_bridge.c` |
| ppa_blend | PPA 硬件图层融合 | `engines/ppa_blend.c` |
| mem_alloc | 内存池化分配器 | `core/memory_manager.c` |

---

## 5. 内存布局 (32 MB PSRAM)

```
┌────────────────────────────────────────┐
│          内存池 (16 MB)                 │
│  Live2D 模型+纹理 (8MB)                │
│  帧缓冲: DSI(1.2) + LVGL(1.2)          │
│  + Live2D overlay(5MB)                  │
├────────────────────────────────────────┤
│        页面缓存区 (8 MB)               │
│  HTTP/TLS 缓冲 (2MB)                   │
│  LLM 上下文 (4MB)                      │
│  音频缓冲 (1MB)                         │
│  摄像头帧 (1.5MB)                       │
├────────────────────────────────────────┤
│        标准 malloc 区 (8 MB)           │
│  小块分配 / 栈空间 / 系统开销            │
└────────────────────────────────────────┘
```

**设计理由**：
- 池化分配器（16MB）：大块分配不会碎片化，适合 Live2D 模型和帧缓冲
- 页面缓存（8MB）：固定大小的缓冲区，避免反复 malloc/free
- 标准 malloc（8MB）：小块临时分配，用完即还
- 三层管理让内存可控，长时间运行不 OOM
