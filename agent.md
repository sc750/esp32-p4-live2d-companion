# Agent Guidelines — ESP32-P4 Live2D 桌面 AI 陪伴系统

> AI Agent 速查手册。完整规范：[ARCHITECTURE.md](ARCHITECTURE.md) | [CODING_STYLE.md](CODING_STYLE.md)

---

## 一句话描述

ESP32-P4 上的 Live2D 桌面 AI 陪伴设备，事件驱动架构，C 语言 + ESP-IDF。

## 代码怎么写（速查）

| 事项 | 规则 |
|------|------|
| 文件命名 | `snake_case.c/.h`，模块名与文件名一致 |
| 函数命名 | `<模块名>_<动作>`，如 `audio_service_init()` |
| 句柄类型 | `xxx_handle_t`，用不透明指针隐藏内部结构 |
| 错误返回 | 所有公开 API 返回 `esp_err_t` |
| 日志 | `ESP_LOGI/W/E`，不用 `printf`，TAG 在文件顶部定义 |
| 内存 | 大块用 `mem_alloc()`，DMA 用 `mem_alloc_dma()`，禁止裸 `malloc` |
| 注释语言 | 中文，变量/枚举/结构体字段必须行内注释用途 |
| Doxygen | 公开 API 必须 `@brief / @param / @return`，文件头 `@file` |

## 架构约束（不能违反）

```
Application → State Machine → Service → UI Bridge → Rendering → HAL(BSP)
```

- **严禁反向调用**（上层可调下层，下层不可调上层）
- **同层通信**通过事件总线（`event_bus_post` + `EVENT_SUBSCRIBE`），不直接调用
- **双核分工**：Core 0 = LVGL/网络/存储，Core 1 = Live2D/音频/AI

## 事件系统速查

```c
// 发送事件
event_bus_post(EVENT_ASR_FINAL, &(event_data_t){ .asr_text = "你好" });

// 订阅事件
EVENT_SUBSCRIBE(EVENT_ASR_FINAL, on_asr_final_handler);
```

## 提交信息格式

`<type>(<scope>): <中文描述>`，type: `feat / fix / docs / refactor / perf / chore / init`

## 重要坑点

| 问题 | 规避 |
|------|------|
| mbedTLS 并发崩溃 | 所有 HTTPS 请求走串行化队列 |
| PSRAM 碎片化 | 大块用池化分配器，小块(<1KB)用 malloc |
| LVGL/Live2D 跨核 | LVGL 锁 `bsp_display_lock()` / 解锁 `bsp_display_unlock()` |
| GPIO35 冲突 | BOOT 按钮和以太网 RMII TXD1 共享，不能同时用 |
| MIPI-DSI underrun | PSRAM 时钟 ≥200MHz，开启 `CONFIG_SPIRAM_XIP_FROM_PSRAM` |

## 工具禁用

**本项目禁止使用智谱（Zhipu / zhipuai）的任何 MCP 工具。**

## 完整参考

- [ARCHITECTURE.md](ARCHITECTURE.md) — 为什么这样设计（分层、数据流、模块职责）
- [CODING_STYLE.md](CODING_STYLE.md) — 怎么写代码（命名细则、Doxygen 模板、错误处理模板）
