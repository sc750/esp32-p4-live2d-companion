# Agent Guidelines — ESP32-P4 Live2D 桌面 AI 陪伴系统

> 本文档定义项目的编码规范、架构原则和协作约定。
> 所有参与本项目的 AI Agent 和开发者都应遵循这些规范。

---

## 1. 项目概述

基于 ESP32-P4 的 Live2D 桌面 AI 陪伴系统，采用分层事件驱动架构，
集成实时语音对话、Live2D 角色动画、视觉感知、长期记忆与日记功能。

**技术栈**: ESP-IDF v5.5+ / LVGL v9 / PainterEngine / C 语言

---

## 2. 架构原则

### 2.1 分层架构（严格遵守）

```
Application Layer    → 页面/场景逻辑
State Machine Layer  → 界面状态管理
Runtime Layer        → 事件总线/定时器/电源
Service Layer        → 业务服务（音频/AI/网络/存储）
UI Bridge Layer      → 服务状态 → LVGL widget 同步
Rendering Layer      → LVGL + PainterEngine + PPA
HAL Layer            → 硬件抽象（BSP）
```

**规则**: 上层可以调用下层接口，**严禁反向调用**。同层之间通过事件总线通信。

### 2.2 高内聚、低耦合

- **每个 .c 文件只做一件事**，文件名清晰反映职责
- **模块间通过头文件（接口）交互**，不直接访问其他模块的内部变量
- **使用不透明指针（opaque pointer）隐藏实现细节**：
  ```c
  // audio_service.h — 对外只暴露类型声明
  typedef struct audio_service_ctx *audio_service_handle_t;
  
  // audio_service.c — 内部定义结构体
  struct audio_service_ctx {
      i2s_chan_handle_t tx_handle;
      // ...内部成员
  };
  ```
- **每个模块有独立的 .h 和 .c**，不要把多个模块塞进一个文件

### 2.3 依赖方向

```
app_main.c
    ↓
app_state_machine.c
    ↓
services/*.c  (audio_service, ai_agent, network_service, ...)
    ↓
bridges/*.c   (ui_bridge, live2d_bridge)
    ↓
engines/*.c   (lvgl_engine, painterengine_adapter)
    ↓
bsp/*.c       (硬件初始化)
```

---

## 3. 命名规范

### 3.1 文件命名

| 类型 | 格式 | 示例 |
|------|------|------|
| 模块 | `snake_case.c/.h` | `audio_service.c`, `audio_service.h` |
| 头文件 | 与 .c 同名 | `ai_agent.c` → `ai_agent.h` |
| BSP 组件 | `bsp_*.c` | `bsp_display.c`, `bsp_audio.c` |
| 私有头文件 | `*_internal.h` | `ai_agent_internal.h` |

### 3.2 函数命名

格式: `<模块名>_<动作>[_<对象>]`

```c
// 初始化/销毁
esp_err_t audio_service_init(void);
esp_err_t audio_service_deinit(void);

// 操作
esp_err_t audio_service_mic_start(void);
esp_err_t audio_service_speaker_write(const int16_t *data, size_t len);

// 查询
bool audio_service_is_speaking(void);
int  audio_service_get_volume(void);
```

### 3.3 变量/类型命名

| 类型 | 风格 | 示例 |
|------|------|------|
| 结构体 | `snake_case_t` | `audio_service_ctx_t`, `dialog_message_t` |
| 枚举 | `UPPER_SNAKE_CASE` + 类型 `snake_case_t` | `STATE_IDLE`, `app_state_t` |
| 宏/常量 | `UPPER_SNAKE_CASE` | `MAX_DIALOG_HISTORY`, `DMA_BUF_SIZE` |
| 局部变量 | `snake_case` | `frame_count`, `is_running` |
| 全局变量 | `g_` 前缀 | `g_event_bus`, `g_audio_handle` |
| 函数内 static | `s_` 前缀 | `s_mic_ring_buf` |
| 句柄 | `xxx_handle_t` | `audio_handle_t`, `camera_handle_t` |
| 回调函数 | `xxx_cb_t` | `audio_frame_cb_t`, `vsync_cb_t` |

---

## 4. Doxygen 注释规范

### 4.1 文件头

```c
/**
 * @file audio_service.h
 * @brief 音频服务模块 — 管理 ES8311 编解码器的输入输出
 *
 * 本模块封装了 ESP32-P4-Function-EV-Board 板载 ES8311 音频编解码芯片
 * 的初始化、麦克风采集和扬声器播放功能。通过 I2S 接口传输音频数据，
 * 通过 I2C 接口配置芯片寄存器。
 *
 * @author 项目组
 * @date 2026-09-01
 * @version 1.0
 *
 * @note 本模块依赖 BSP 层的 I2S 和 I2C 初始化
 * @see bsp_audio.h
 */
```

### 4.2 函数注释

```c
/**
 * @brief 初始化音频服务
 *
 * 完成以下初始化步骤:
 * 1. 通过 I2C 配置 ES8311 寄存器 (采样率、增益、音量)
 * 2. 初始化 I2S TX 和 RX 通道
 * 3. 使能 NS4150 功放
 * 4. 分配 DMA 双缓冲
 *
 * @param[in]  config  音频配置参数，传 NULL 使用默认配置
 * @param[out] handle  返回音频服务句柄
 *
 * @return
 *      - ESP_OK: 成功
 *      - ESP_ERR_INVALID_ARG: 参数无效
 *      - ESP_ERR_NO_MEM: 内存分配失败
 *
 * @warning 必须在 bsp_init() 之后调用
 * @code
 *     audio_service_handle_t handle;
 *     ESP_ERROR_CHECK(audio_service_init(NULL, &handle));
 * @endcode
 */
esp_err_t audio_service_init(const audio_config_t *config, audio_service_handle_t *handle);
```

### 4.3 结构体/枚举注释

```c
/**
 * @brief 音频服务配置参数
 *
 * 用于 audio_service_init() 的初始化配置。
 * 所有字段都有合理默认值，可以只设置需要修改的字段。
 */
typedef struct {
    int sample_rate;        /**< 采样率 (Hz)，默认 16000 */
    int bits_per_sample;    /**< 位宽，默认 16 */
    int dma_buf_count;      /**< DMA 缓冲数量，默认 4 */
    int dma_buf_size;       /**< 每个 DMA 缓冲大小 (字节)，默认 2048 */
    int volume;             /**< 初始音量 (0-100)，默认 70 */
} audio_config_t;

/**
 * @brief 音频帧回调函数类型
 *
 * 当麦克风 DMA 缓冲满时调用此回调。
 * 回调函数应尽量简短，避免阻塞。
 *
 * @param[in] samples  PCM 音频样本数据
 * @param[in] count    样本数量
 * @param[in] ctx      用户上下文指针
 */
typedef void (*audio_frame_cb_t)(const int16_t *samples, size_t count, void *ctx);
```

### 4.4 复杂逻辑注释

```c
// 计算 RMS 能量用于 VAD 检测
// RMS = sqrt(sum(sample^2) / count)
// 使用定点运算避免浮点开销: 先累加平方和，最后再开方
int32_t sum = 0;
for (size_t i = 0; i < count; i++) {
    sum += (int32_t)samples[i] * samples[i];
}
int16_t rms = (int16_t)sqrtf((float)sum / count);
```

---

## 5. 错误处理规范

```c
// ✅ 正确: 检查每个关键调用的返回值
esp_err_t ret = audio_service_init(NULL, &handle);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "音频服务初始化失败: %s", esp_err_to_name(ret));
    return ret;
}

// ✅ 正确: 使用 ESP_GOTO_ON_ERROR 宏简化错误处理
ESP_GOTO_ON_ERROR(audio_service_init(NULL, &handle), err, TAG, "初始化失败");

// ❌ 错误: 忽略返回值
audio_service_init(NULL, &handle);  // 不要这样做
```

### 日志级别

| 级别 | 用途 |
|------|------|
| `ESP_LOGE` | 错误：功能不可用，需要关注 |
| `ESP_LOGW` | 警告：可以工作但有异常 |
| `ESP_LOGI` | 信息：正常运行的关键节点 |
| `ESP_LOGD` | 调试：开发调试用，发布版可关闭 |
| `ESP_LOGV` | 详细：非常详细，通常不使用 |

---

## 6. 内存管理规范

```c
// ✅ 使用项目统一的内存分配器
void *buf = mem_alloc(size);           // 智能分配 (池/页面/malloc)
void *buf = mem_alloc_dma(size);       // DMA 对齐分配 (PSRAM)

// ✅ PSRAM 分配使用 DMA 对齐
esp_dma_mem_info_t dma_mem = {
    .extra_heap_caps = MALLOC_CAP_SPIRAM,
};
void *buf;
esp_dma_capable_calloc(1, size, &dma_mem, &buf);

// ✅ 释放后置空指针
mem_free(buf);
buf = NULL;

// ❌ 禁止直接使用 malloc/free 分配大块内存
void *buf = malloc(1024 * 1024);  // 不要这样做，使用 mem_alloc
```

---

## 7. 事件系统规范

```c
// ✅ 使用统一的事件类型和事件总线
event_bus_post(EVENT_ASR_FINAL, &(event_data_t){
    .asr_text = "你好"
});

// ✅ 订阅者只处理自己关心的事件
EVENT_SUBSCRIBE(EVENT_ASR_FINAL, on_asr_final_handler);

// ❌ 不要直接调用其他模块的函数
audio_service_speaker_write(data, len);  // 在 UI 模块中不要这样做
// ✅ 应该发送事件，让音频模块自己处理
event_bus_post(EVENT_TTS_DATA, &tts_data);
```

---

## 8. 线程安全规范

```c
// ✅ 共享数据必须用互斥锁保护
xSemaphoreTake(ctx->mutex, portMAX_DELAY);
ctx->shared_data = new_value;
xSemaphoreGive(ctx->mutex);

// ✅ ISR 中使用 FromISR 版本
BaseType_t xHigherPriorityTaskWoken = pdFALSE;
xSemaphoreGiveFromISR(ctx->done_sem, &xHigherPriorityTaskWoken);
portYIELD_FROM_ISR(xHigherPriorityTaskWoken);

// ✅ 任务间通信优先使用队列
xQueueSend(ctx->event_queue, &event, pdMS_TO_TICKS(100));

// ❌ 不要在 ISR 中做耗时操作
// ❌ 不要在 ISR 中获取非 FromISR 的信号量
```

---

## 9. 文件组织结构

```
main/
├── CMakeLists.txt
├── main.c                         # 入口, 初始化 BSP, 启动各层
│
├── app/                           # 应用层
│   ├── app_state_machine.c/.h     # 状态机
│   └── app_events.c/.h            # 事件类型定义
│
├── services/                      # 服务层
│   ├── audio_service.c/.h         # 音频管理
│   ├── ai_agent.c/.h              # AI 对话流水线
│   ├── network_service.c/.h       # 网络通信
│   ├── memory_service.c/.h        # 长期记忆
│   ├── diary_service.c/.h         # 日记生成
│   ├── music_service.c/.h         # 音乐播放
│   ├── pomodoro_service.c/.h      # 番茄钟
│   └── vision_service.c/.h        # 摄像头视觉
│
├── bridges/                       # 桥接层
│   ├── ui_bridge.c/.h             # 服务状态 → LVGL widget
│   └── live2d_bridge.c/.h         # Live2D 渲染桥接
│
├── engines/                       # 渲染引擎适配
│   ├── lvgl_engine.c/.h           # LVGL v9 初始化与适配
│   ├── painterengine_adapter.c/.h # PainterEngine 平台适配
│   └── ppa_blend.c/.h             # PPA 图层融合
│
├── ui/                            # UI 页面
│   ├── screens/
│   │   ├── scr_home.c/.h          # 主界面
│   │   ├── scr_chat.c/.h          # 对话界面
│   │   ├── scr_music.c/.h         # 音乐界面
│   │   ├── scr_pomodoro.c/.h      # 番茄钟界面
│   │   ├── scr_diary.c/.h         # 日记界面
│   │   └── scr_settings.c/.h      # 设置界面
│   ├── widgets/                   # 自定义控件
│   │   ├── wave_indicator.c/.h    # 语音波形
│   │   └── subtitle_bar.c/.h      # 字幕栏
│   └── themes/
│       ├── theme_day.c/.h         # 日间主题
│       └── theme_night.c/.h       # 夜间主题
│
├── core/                          # 核心基础设施
│   ├── event_bus.c/.h             # 事件总线
│   ├── timer_manager.c/.h         # 定时器管理
│   ├── memory_manager.c/.h        # 内存池管理
│   └── ring_buffer.c/.h           # 环形缓冲区
│
├── ai/                            # AI 相关
│   ├── asr_client.c/.h            # 语音识别客户端
│   ├── llm_client.c/.h            # 大模型客户端
│   ├── tts_client.c/.h            # 语音合成客户端
│   ├── dialog_manager.c/.h        # 对话上下文管理
│   └── vision_agent.c/.h          # 视觉理解
│
└── drivers/                       # 驱动适配 (非 BSP)
    ├── es8311_codec.c/.h          # ES8311 芯片驱动
    └── motion_detector.c/.h       # 运动检测
```

---

## 10. Git 提交规范

### 10.1 提交信息格式

```
<类型>(<范围>): <简短描述>

<详细描述（可选）>
```

### 10.2 类型

| 类型 | 说明 |
|------|------|
| `feat` | 新功能 |
| `fix` | 修复 bug |
| `docs` | 文档更新 |
| `refactor` | 代码重构（不改变功能） |
| `perf` | 性能优化 |
| `style` | 代码格式调整（不影响逻辑） |
| `test` | 测试相关 |
| `chore` | 构建/工具链相关 |
| `init` | 项目初始化 |

### 10.3 示例

```
init: 项目初始化，添加 PRD 文档和架构设计

- 新增 15 个 PRD 文档 (docs/prd/)
- 定义分层事件驱动架构
- 添加 BSP 组件 (esp32_p4_function_ev_board)
- 创建 agent.md 编码规范
```

---

## 11. 性能注意事项

1. **避免在 ISR 中分配内存**
2. **PSRAM 大块分配使用池化分配器**，小块(<1KB)可用标准 malloc
3. **JPEG 编码使用硬件加速器**，不要软编码
4. **PPA 图层融合使用 DMA 异步模式**，不阻塞 CPU
5. **网络请求通过串行化队列**，避免 mbedTLS 并发
6. **LVGL 渲染控制在 Core 0**，Live2D 控制在 Core 1
7. **音频 DMA 缓冲不要小于 1024 samples**，避免断音

---

## 12. 文档要求

1. **所有公开 API 必须有 Doxygen 注释**
2. **复杂算法必须有中文注释说明思路**
3. **每个模块文件头必须有 @file 和 @brief**
4. **关键数据结构必须有字段说明**
5. **README.md 描述项目整体架构和快速上手**
6. **每个子模块有独立文档说明设计思路**
