# ESP32-P4 Live2D 桌面 AI 陪伴系统 — 产品需求文档 (PRD)

> **版本**: v1.0  
> **日期**: 2026-09-01  
> **状态**: 设计阶段

---

## 1. 产品概述

### 1.1 产品定位

以二次元虚拟角色为载体的桌面 AI 陪伴系统，集成实时语音对话、Live2D 角色动画、长期记忆与主动日记功能，运行于 ESP32-P4 嵌入式平台，提供有温度的桌面陪伴与效率辅助体验。

### 1.2 核心价值

| 维度 | 价值 |
|------|------|
| 情感陪伴 | 角色具备表情/动作反馈，记住用户喜好，以第一人称写日记 |
| 效率辅助 | 番茄钟、语音快捷指令、音乐播放 |
| 语言学习 | 中日双语字幕 + 语音朗读，辅助口语练习 |
| 技术验证 | ESP32-P4 上实现 20FPS Live2D + 流式语音对话的端到端方案 |

### 1.3 目标用户

- 二次元文化爱好者
- 需要桌面陪伴/效率工具的开发者、学生
- 嵌入式 AIoT 技术学习者

---

## 2. 硬件平台

### 2.1 核心硬件：ESP32-P4-Function-EV-Board

| 组件 | 规格 |
|------|------|
| **主控** | ESP32-P4 双核 RISC-V, 400 MHz |
| **内存** | 768 KB 内部 SRAM + 32 MB PSRAM |
| **显示屏** | 1024×600 MIPI-DSI 电容触摸屏 (2-lane, ≤1.5Gbps), FPC 15Pin |
| **摄像头** | 200万像素 MIPI CSI 摄像头 (可选配件), FPC 15Pin |
| **Wi-Fi** | 板载 ESP32-C6-MINI-1 模组通过 SDIO 连接 (ESP-Hosted) |
| **以太网** | RJ45 10/100 Mbps 自适应 (板载以太网 PHY) |
| **音频** | ES8311 Codec (I2S+I2C) + NS4150 3W D类功放 + 板载麦克风 + 扬声器输出 |
| **USB** | USB Serial/JTAG + USB Full-speed + USB 2.0 OTG High-Speed (Type-C/Type-A 二选一) |
| **存储** | 16 MB SPI Flash + MicroSD 卡槽 (4-bit SDMMC) |
| **加速器** | PPA (像素处理加速器)、2D-DMA、JPEG 硬件编解码 |
| **扩展** | 所有 GPIO 引出至 J1 排针 |

### 2.2 关键硬件约束

| 约束 | 说明 |
|------|------|
| PSRAM 带宽 | MIPI-DSI 需 PSRAM ≥200MHz，否则可能闪蓝屏 |
| PPA 限制 | ESP32-P4 的 PPA Blend 支持 ARGB8888 前景 + RGB565/RGB888 背景 |
| mbedTLS 并发 | 共享 SHA/AES 硬件加速器，多 TLS 连接会崩溃，需串行化 |
| I2S MCLK | PDM 模式下 I2S0 支持 PCM↔PDM 硬件转换 |
| DSI 仅 Video 模式 | 不支持 Command 模式，必须连续刷新 |

---

## 3. 系统架构

### 3.1 分层架构总览

```
┌─────────────────────────────────────────────────────────────────┐
│                     Application Layer                           │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐          │
│  │  Screen   │ │  Screen   │ │  Screen   │ │  Screen   │         │
│  │  Idle     │ │  Chat     │ │  Music    │ │  Pomodoro │         │
│  └────┬─────┘ └────┬─────┘ └────┬─────┘ └────┬─────┘          │
│       └─────────────┴────────────┴─────────────┘                │
│                         │                                       │
├─────────────────────────┼───────────────────────────────────────┤
│                   State Machine Layer                           │
│  ┌──────────────────────────────────────────────────────┐      │
│  │  app_state_machine: 管理界面切换、交互状态、超时       │      │
│  │  events: UI_EVENT, VOICE_EVENT, TIMER_EVENT ...       │      │
│  └──────────────────────┬───────────────────────────────┘      │
│                         │                                       │
├─────────────────────────┼───────────────────────────────────────┤
│                   Runtime / Event Bus Layer                     │
│  ┌──────────────────┐  ┌─────────────┐  ┌────────────────┐    │
│  │  event_bus        │  │  timer_mgr   │  │  power_mgr     │    │
│  │  (发布-订阅)      │  │  (节拍定时器) │  │  (休眠/唤醒)   │    │
│  └────────┬─────────┘  └──────┬──────┘  └───────┬────────┘    │
│           └───────────────────┴──────────────────┘              │
│                         │                                       │
├─────────────────────────┼───────────────────────────────────────┤
│                      Service Layer                              │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐          │
│  │  audio_   │ │  network  │ │  ai_      │ │  storage  │         │
│  │  service  │ │  service  │ │  agent    │ │  service  │         │
│  ├──────────┤ ├──────────┤ ├──────────┤ ├──────────┤          │
│  │  music_   │ │  pomodoro │ │  memory   │ │  diary    │         │
│  │  player   │ │  service  │ │  service  │ │  service  │         │
│  └────┬─────┘ └────┬─────┘ └────┬─────┘ └────┬─────┘          │
│       └─────────────┴────────────┴─────────────┘                │
│                         │                                       │
├─────────────────────────┼───────────────────────────────────────┤
│                   UI Bridge Layer                               │
│  ┌──────────────────────────────────────────────────────┐      │
│  │  ui_bridge: 将各 Service 的状态快照 → LVGL widget 更新  │      │
│  │  live2d_bridge: PainterEngine 渲染 → PPA overlay 融合   │      │
│  └──────────────────────┬───────────────────────────────┘      │
│                         │                                       │
├─────────────────────────┼───────────────────────────────────────┤
│                   Rendering Engine Layer                        │
│  ┌──────────────────────┐  ┌──────────────────────────┐       │
│  │  LVGL v9              │  │  PainterEngine            │       │
│  │  (UI 渲染, RGB565)    │  │  (Live2D 渲染, ARGB8888)  │       │
│  │  → MIPI-DSI 帧缓冲   │  │  → 双槽 overlay 缓冲      │       │
│  └──────────┬───────────┘  └──────────┬───────────────┘       │
│             │                         │                         │
│             └────────┬────────────────┘                         │
│                      │                                          │
│              ┌───────▼────────┐                                 │
│              │  PPA Blend      │                                 │
│              │  (硬件图层融合)  │                                 │
│              └───────┬────────┘                                 │
│                      │                                          │
├──────────────────────┼──────────────────────────────────────────┤
│               Hardware Abstraction Layer                        │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐      │
│  │MIPI-DSI│ │I2S/PDM │ │PPA     │ │SDIO    │ │Flash/  │      │
│  │LCD     │ │Audio   │ │2D-DMA  │ │ESP-H.  │ │SPIFFS  │      │
│  └────────┘ └────────┘ └────────┘ └────────┘ └────────┘      │
│               (ESP-IDF v5.5+ / BSP)                            │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 双核任务分配

```
Core 0 (Protocol Core)              Core 1 (Application Core)
┌──────────────────────┐           ┌──────────────────────┐
│  FreeRTOS Tasks:      │           │  FreeRTOS Tasks:      │
│                       │           │                       │
│  • lvgl_task (高优先) │           │  • live2d_task (高)   │
│    - LVGL timer_handler│          │    - PainterEngine 更新│
│    - UI 渲染           │          │    - Live2D 物理模拟   │
│    - 触摸输入处理       │          │                       │
│                       │           │  • audio_task          │
│  • network_task       │           │    - I2S DMA 回调      │
│    - ESP-Hosted 管理   │          │    - 音频编解码         │
│    - HTTP/TLS 通信     │          │                       │
│                       │           │  • ai_agent_task       │
│  • storage_task       │           │    - 语音识别流水线     │
│    - Flash/SPIFFS      │          │    - LLM 推理调度       │
│    - SD 卡操作         │          │    - TTS 合成           │
│                       │           │                       │
│  • main_task          │           │  • ppa_blend_task      │
│    - 事件总线分发      │           │    - 图层融合           │
│    - 状态机推进        │           │                       │
│    - 定时器管理        │           │                       │
└──────────────────────┘           └──────────────────────┘
```

### 3.3 核心设计模式

#### 3.3.1 事件驱动架构

系统核心采用发布-订阅模式的事件总线：

```c
// 事件类型定义
typedef enum {
    // UI 事件
    EVENT_TOUCH_DOWN, EVENT_TOUCH_UP, EVENT_TOUCH_MOVE,
    EVENT_SCREEN_TAP, EVENT_SCREEN_LONG_PRESS,
    
    // 语音事件
    EVENT_ASR_PARTIAL, EVENT_ASR_FINAL,
    EVENT_LLM_TOKEN, EVENT_LLM_DONE, EVENT_LLM_TOOL_CALL,
    EVENT_TTS_CHUNK, EVENT_TTS_DONE,
    
    // 定时器事件
    EVENT_POMODORO_TICK, EVENT_POMODORO_DONE,
    EVENT_DIARY_TIME, EVENT_SLEEP_TIMEOUT,
    
    // 系统事件
    EVENT_WIFI_CONNECTED, EVENT_WIFI_DISCONNECTED,
    EVENT_LOW_MEMORY, EVENT_THEME_CHANGE,
    
    // Live2D 事件
    EVENT_L2D_EXPRESSION, EVENT_L2D_MOTION,
} event_type_t;

typedef struct {
    event_type_t type;
    uint32_t timestamp;
    union {
        struct { int x, y; } touch;
        struct { const char *text; } asr;
        struct { const char *token; } llm;
        struct { int expression_id; } l2d;
        // ...
    } payload;
} app_event_t;
```

#### 3.3.2 状态机层

```c
typedef enum {
    STATE_IDLE,         // 主界面：角色待机 + 时钟/天气
    STATE_LISTENING,    // 语音收集中
    STATE_THINKING,     // AI 推理中
    STATE_SPEAKING,     // TTS 播放中
    STATE_MUSIC,        // 音乐播放界面
    STATE_POMODORO,     // 番茄钟界面
    STATE_DIARY,        // 日记阅读界面
    STATE_MEMORY,       // 记忆/设置界面
    STATE_SLEEP,        // 息屏休眠
} app_state_t;

// 状态转移表
static const state_transition_t transitions[] = {
    { STATE_IDLE,      EVENT_SCREEN_TAP,      STATE_LISTENING },
    { STATE_IDLE,      EVENT_DIARY_TIME,      STATE_DIARY },
    { STATE_IDLE,      EVENT_SLEEP_TIMEOUT,   STATE_SLEEP },
    { STATE_LISTENING, EVENT_ASR_FINAL,       STATE_THINKING },
    { STATE_THINKING,  EVENT_LLM_TOKEN,       STATE_SPEAKING },
    { STATE_SPEAKING,  EVENT_TTS_DONE,        STATE_IDLE },
    // ...
};
```

#### 3.3.3 渲染流水线 (Producer/Consumer)

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│  Slot A (LVGL)   │────▶│  PPA Blend       │────▶│  MIPI-DSI       │
│  Slot B (Live2D) │     │  硬件融合         │     │  DMA 连续输出    │
└─────────────────┘     └─────────────────┘     └─────────────────┘

帧节奏: VSync 中断驱动
- LVGL 在非活跃 slot 渲染 UI (RGB565)
- PainterEngine 在 overlay buffer 渲染 Live2D (ARGB8888)
- PPA 将 ARGB8888 overlay 融合到 RGB565 背景
- VSync 到来时 swap slot
```

---

## 4. 模块分解与子文档索引

| 编号 | 模块 | 子文档 | 优先级 | 依赖 |
|------|------|--------|--------|------|
| M01 | 硬件抽象与 BSP | [01-hardware-bsp.md](01-hardware-bsp.md) | P0 | 无 |
| M02 | LVGL UI 框架 | [02-lvgl-ui.md](02-lvgl-ui.md) | P0 | M01 |
| M03 | PainterEngine Live2D | [03-painterengine-live2d.md](03-painterengine-live2d.md) | P0 | M01 |
| M04 | PPA 图层融合 | [04-ppa-blending.md](04-ppa-blending.md) | P0 | M02, M03 |
| M05 | 音频子系统 | [05-audio-subsystem.md](05-audio-subsystem.md) | P0 | M01 |
| M06 | 网络与 Wi-Fi | [06-network-wifi.md](06-network-wifi.md) | P0 | M01 |
| M07 | AI Agent (ASR/LLM/TTS) | [07-ai-agent.md](07-ai-agent.md) | P0 | M05, M06 |
| M08 | 长期记忆与人设系统 | [08-memory-persona.md](08-memory-persona.md) | P1 | M07 |
| M09 | 日记生成系统 | [09-diary-system.md](09-diary-system.md) | P1 | M08 |
| M10 | 音乐播放器 | [10-music-player.md](10-music-player.md) | P1 | M05, M06 |
| M11 | 番茄钟 | [11-pomodoro.md](11-pomodoro.md) | P2 | M02 |
| M12 | 电源管理与昼夜主题 | [12-power-theme.md](12-power-theme.md) | P2 | M02 |
| M13 | 内存管理与性能优化 | [13-memory-perf.md](13-memory-perf.md) | P0 | M01 |
| M14 | 摄像头视觉系统 (AI之眼) | [14-camera-vision.md](14-camera-vision.md) | P2 | M06, M07 |

---

## 5. 技术选型

| 领域 | 技术方案 | 理由 |
|------|---------|------|
| **固件框架** | ESP-IDF v5.5+ | 官方支持, PPA/MIPI-DSI 驱动成熟 |
| **BSP** | esp-bsp (esp32_p4_function_ev_board) | 已适配板级硬件 |
| **UI 框架** | LVGL v9 | ESP LVGL Adapter 支持 PPA 融合 |
| **Live2D 引擎** | PainterEngine Live2D | 已验证的嵌入式 Live2D 方案 |
| **图层融合** | PPA BLEND (ARGB8888+RGB565) | 硬件加速, 零 CPU 开销 |
| **Wi-Fi** | ESP-Hosted (SDIO + ESP32-C6) | 板载方案, 4-bit SDIO |
| **TLS** | mbedTLS (串行化请求) | IDF 内置, 避免硬件并发问题 |
| **ASR** | 流式 WebSocket/HTTP (如 Whisper API) | 低延迟, 流式传输 |
| **LLM** | OpenAI 兼容 API (function calling) | 标准协议, 工具调用能力 |
| **TTS** | 流式 HTTP (如 Edge TTS / VITS) | 边生成边播放, 低首包延迟 |
| **文件系统** | SPIFFS / FAT (SD卡) | Flash 存储固件资源; SD 存储音乐/日记 |
| **内存池** | 自定义大块池化分配器 | 抑制 32MB PSRAM 碎片 |

---

## 6. 内存预算 (32 MB PSRAM)

| 用途 | 大小 | 说明 |
|------|------|------|
| MIPI-DSI 帧缓冲 (×2) | ~1.2 MB | 1024×600×RGB565×2 |
| LVGL 渲染缓冲 | ~1.2 MB | ARGB8888 模式下的 PPA 前景缓冲 |
| Live2D overlay (×2) | ~5 MB | 1024×600×ARGB8888×2 (双槽) |
| Live2D 模型数据 | ~8 MB | 模型文件 + 纹理贴图 |
| 摄像头帧缓冲 | ~1.5 MB | MIPI-CSI 720p YUV422×2 + JPEG 编码缓冲 |
| 音频缓冲 | ~1 MB | I2S DMA + PCM 环形缓冲 + MP3 解码 |
| HTTP/TLS 缓冲 | ~2 MB | mbedTLS 上下文 + 收发缓冲 |
| LLM 上下文缓冲 | ~4 MB | 流式推理 token 缓冲 |
| SPIFFS 缓存 | ~512 KB | 文件系统缓存 |
| 碎片预留/其他 | ~7.5 MB | 栈空间、动态分配、系统开销 |
| **合计** | **~32 MB** | |

---

## 7. 开发路线图

### Phase 1: 基础平台 (P0, 2-3 周)
- [x] 硬件验证 (用户已完成)
- [ ] M01: BSP 初始化 (MIPI-DSI, I2S, SDIO, Flash)
- [ ] M13: 内存池管理器
- [ ] M02: LVGL v9 UI 框架搭建
- [ ] M06: ESP-Hosted Wi-Fi 连接

### Phase 2: 渲染管线 (P0, 2-3 周)
- [ ] M03: PainterEngine 移植与 Live2D 渲染
- [ ] M04: PPA 硬件图层融合
- [ ] M12: 昼夜主题切换

### Phase 3: 语音对话 (P0, 3-4 周)
- [ ] M05: 音频子系统 (PDM 麦克风 + I2S 扬声器)
- [ ] M07: AI Agent 全链路 (ASR → LLM → TTS)
- [ ] M07: TLS 串行化方案

### Phase 4: 智能功能 (P1, 2-3 周)
- [ ] M08: 长期记忆与人设系统
- [ ] M09: AI 日记自动生成
- [ ] M10: MP3 音乐播放

### Phase 5: 体验完善 (P2, 1-2 周)
- [ ] M11: 番茄钟
- [ ] M12: 息屏休眠与电源管理
- [ ] 性能调优与稳定性测试

### Phase 6: 视觉感知 (P2, 2 周)
- [ ] M14: 摄像头初始化 (MIPI-CSI + JPEG 硬件编码)
- [ ] M14: 视觉 AI Agent (Vision LLM 调用)
- [ ] M14: 本地运动检测 + 手势识别
- [ ] M14: 视觉 MCP 工具集成到 AI Agent
- [ ] M14: 表情识别与场景理解

---

## 8. 非功能需求

| 指标 | 目标 |
|------|------|
| Live2D 帧率 | 稳定 ≥20 FPS |
| 语音对话端到端延迟 | ≤4 秒 (ASR→LLM→TTS 首包) |
| UI 触摸响应延迟 | ≤50 ms |
| 连续运行稳定性 | ≥24 小时无堆损坏/内存泄漏 |
| 启动到就绪时间 | ≤10 秒 |
| 功耗 (显示开启) | ≤3W (典型) |

---

## 9. 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| PSRAM 带宽竞争导致画面撕裂 | 显示异常 | PPA VSync 同步 + 优先级隔离 |
| mbedTLS 并发崩溃 | 系统死机 | HTTP 请求串行化队列 |
| PSRAM 碎片化 | 长期运行 OOM | 大块池化 + 页面 composite 缓存 |
| Live2D + UI + 网络 CPU 竞争 | 卡顿 | 双核严格分工 + 优先级配置 |
| TTS 流式播放断音 | 体验差 | 预缓冲 + 环形缓冲区 |
| ESP-Hosted SDIO 偶发断连 | 网络中断 | 自动重连 + 指数退避 + 以太网备选 |
| Vision API 延迟过高 | 视觉交互卡顿 | 降低分辨率/质量 + 异步分析 + 本地预检测 |
| 摄像头 FPC 连接松动 | 无法出图 | 确保 FPC 排线牢固 + 重启恢复 |
| Vision API 图片隐私 | 用户隐私泄露 | 软件开关 + 仅按需拍照 + 不本地存储 |

---

## 10. 附录

### 10.1 参考项目
- PainterEngine: https://github.com/matrixcascade/PainterEngine
- ESP-BSP: https://github.com/espressif/esp-bsp
- ESP-Hosted: https://github.com/espressif/esp-hosted
- ESP LVGL Adapter: https://components.espressif.com/components/espressif/esp_lvgl_adapter

### 10.2 术语表

| 术语 | 定义 |
|------|------|
| PPA | Pixel Processing Accelerator, ESP32-P4 像素处理加速器 |
| PDM | Pulse-Density Modulation, 脉冲密度调制 (数字麦克风接口) |
| MIPI-DSI | Mobile Industry Processor Interface - Display Serial Interface |
| ARGB8888 | 含 Alpha 通道的 32 位色彩格式 |
| RGB565 | 16 位色彩格式 (UI 渲染用) |
| Live2D | 2D 角色动画技术, 支持物理模拟和表情系统 |
| ASR | Automatic Speech Recognition, 自动语音识别 |
| TTS | Text-to-Speech, 文本转语音 |
| iTWT | Wi-Fi 6 目标唤醒时间, 节省功耗 |
