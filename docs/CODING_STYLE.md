# CODING_STYLE.md — 编码规范速查表

> 写代码时查阅。架构设计见 [ARCHITECTURE.md](ARCHITECTURE.md)，Agent 速查见 [agent.md](../agent.md)。

---

## 1. 文件组织

```
main/
├── main.c                          # 入口，初始化 BSP，启动各层
├── app/
│   ├── app_state_machine.c/.h      # 状态机
│   └── app_events.c/.h             # 事件类型定义
├── services/                       # 业务服务
│   ├── audio_service.c/.h
│   ├── ai_agent.c/.h
│   ├── network_service.c/.h
│   ├── memory_service.c/.h
│   ├── diary_service.c/.h
│   ├── music_service.c/.h
│   ├── pomodoro_service.c/.h
│   └── vision_service.c/.h
├── bridges/                        # 服务状态 → LVGL 同步
│   ├── ui_bridge.c/.h
│   └── live2d_bridge.c/.h
├── engines/                        # 渲染引擎适配
│   ├── lvgl_engine.c/.h
│   ├── painterengine_adapter.c/.h
│   └── ppa_blend.c/.h
├── ui/                             # LVGL 页面与控件
│   ├── screens/scr_*.c/.h
│   ├── widgets/*.c/.h
│   └── themes/*.c/.h
├── core/                           # 基础设施
│   ├── event_bus.c/.h
│   ├── timer_manager.c/.h
│   ├── memory_manager.c/.h
│   └── ring_buffer.c/.h
├── ai/                             # AI 客户端
│   ├── asr_client.c/.h
│   ├── llm_client.c/.h
│   ├── tts_client.c/.h
│   ├── dialog_manager.c/.h
│   └── vision_agent.c/.h
└── drivers/                        # 非 BSP 驱动
    ├── es8311_codec.c/.h
    └── motion_detector.c/.h
```

**规则**：每个 `.c` 文件对应一个同名 `.h`，一个文件只做一件事，私有头文件用 `_internal.h` 后缀。

---

## 2. 命名规范

### 2.1 文件命名

| 类型 | 格式 | 示例 |
|------|------|------|
| 模块 | `snake_case.c/.h` | `audio_service.c` |
| BSP | `bsp_*.c/.h` | `bsp_display.c` |
| 私有头文件 | `*_internal.h` | `ai_agent_internal.h` |

### 2.2 函数命名

格式：`<模块名>_<动作>[_<对象>]`

```c
esp_err_t audio_service_init(void);                      // 初始化
esp_err_t audio_service_deinit(void);                    // 销毁
esp_err_t audio_service_mic_start(void);                 // 操作
esp_err_t audio_service_speaker_write(const int16_t *, size_t len);  // 写数据
bool      audio_service_is_speaking(void);               // 查询
int       audio_service_get_volume(void);                // getter
```

### 2.3 类型/变量命名

| 类型 | 风格 | 示例 |
|------|------|------|
| 结构体 | `snake_case_t` | `audio_config_t` |
| 枚举类型 | `snake_case_t` | `app_state_t` |
| 枚举值 | `UPPER_SNAKE_CASE` | `STATE_IDLE`, `STATE_TALKING` |
| 宏/常量 | `UPPER_SNAKE_CASE` | `MAX_DIALOG_HISTORY` |
| 局部变量 | `snake_case` | `frame_count` |
| 全局变量 | `g_` 前缀 | `g_event_bus` |
| 文件内 static | `s_` 前缀 | `s_mic_ring_buf` |
| 句柄 | `xxx_handle_t` | `audio_handle_t` |
| 回调函数 | `xxx_cb_t` | `audio_frame_cb_t` |

---

## 3. Doxygen 注释模板

### 3.1 文件头

```c
/**
 * @file audio_service.h
 * @brief 音频服务 — ES8311 编解码器输入输出管理
 *
 * 封装 ESP32-P4-Function-EV-Board 板载 ES8311 芯片的初始化、
 * 麦克风采集和扬声器播放。I2S 传数据，I2C 配置寄存器。
 *
 * @note 依赖 BSP 层的 I2S 和 I2C 初始化
 * @see bsp_audio.h
 */
```

### 3.2 公开 API 函数

```c
/**
 * @brief 初始化音频服务
 *
 * 1. I2C 配置 ES8311 (采样率/增益/音量)
 * 2. 初始化 I2S TX/RX 通道
 * 3. 使能 NS4150 功放
 * 4. 分配 DMA 双缓冲
 *
 * @param[in]  config  配置参数，传 NULL 使用默认值
 * @param[out] handle  返回的服务句柄
 * @return
 *   - ESP_OK: 成功
 *   - ESP_ERR_INVALID_ARG: 参数无效
 *   - ESP_ERR_NO_MEM: 内存不足
 *
 * @warning 必须在 bsp_init() 之后调用
 * @code
 *     audio_handle_t h;
 *     ESP_ERROR_CHECK(audio_service_init(NULL, &h));
 * @endcode
 */
esp_err_t audio_service_init(const audio_config_t *config, audio_handle_t *handle);
```

### 3.3 结构体

```c
/**
 * @brief 音频配置参数
 *
 * 传给 audio_service_init()，所有字段有默认值。
 */
typedef struct {
    int sample_rate;     /**< 采样率 (Hz)，默认 16000 */
    int volume;          /**< 初始音量 (0-100)，默认 70 */
} audio_config_t;
```

### 3.4 回调函数类型

```c
/**
 * @brief 音频帧就绪回调
 *
 * DMA 缓冲满时调用，应尽快返回，不要阻塞。
 *
 * @param[in] samples  PCM 样本数据
 * @param[in] count    样本数量
 * @param[in] ctx      用户上下文
 */
typedef void (*audio_frame_cb_t)(const int16_t *samples, size_t count, void *ctx);
```

### 3.5 复杂逻辑注释

```c
/*
 * 计算 RMS 能量用于 VAD 检测
 * 公式: RMS = sqrt(sum(sample^2) / count)
 * 用定点累加避免浮点开销，最后再开方
 */
int32_t sum = 0;
for (size_t i = 0; i < count; i++) {
    sum += (int32_t)samples[i] * samples[i];
}
int16_t rms = (int16_t)sqrtf((float)sum / count);
```

---

## 4. 中文注释规则

| 场景 | 规则 | 示例 |
|------|------|------|
| 变量声明 | 行内注释用途 | `int volume; /* 音量 0~100 */` |
| 枚举值/字段 | 必须注释 | `STATE_IDLE, /* 空闲 */` |
| 函数内步骤 | 注释做了什么 | `/* 第一步：初始化硬件 */` |
| if/case 分支 | 注释含义 | `case STATE_IDLE: /* 空闲 */` |
| 专业术语 | 保留英文 | LVGL, PSRAM, DMA, PPA |

**不写废话注释**：`i++  // i加1` 这种不写。

---

## 5. 错误处理

```c
// ✅ 检查返回值 + 记录日志
esp_err_t ret = audio_service_init(NULL, &handle);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "音频服务初始化失败: %s", esp_err_to_name(ret));
    return ret;
}

// ✅ 用宏简化
ESP_GOTO_ON_ERROR(audio_service_init(NULL, &handle), err, TAG, "初始化失败");

// ❌ 忽略返回值（禁止）
audio_service_init(NULL, &handle);
```

日志级别：`ESP_LOGE`(错误) / `LOGW`(警告) / `LOGI`(关键节点) / `LOGD`(调试)

---

## 6. 内存管理

```c
// 项目分配器（自动选择最佳策略）
void *buf = mem_alloc(size);           // 大块：池化分配
void *buf = mem_alloc_dma(size);       // DMA：PSRAM 对齐分配
// 小块(<1KB) 可用 malloc

// PSRAM DMA 分配
esp_dma_mem_info_t dma = { .extra_heap_caps = MALLOC_CAP_SPIRAM };
void *buf;
esp_dma_capable_calloc(1, size, &dma, &buf);

// 释放后置空
mem_free(buf);
buf = NULL;

// ❌ 禁止裸 malloc 大块
```

---

## 7. 事件系统

```c
// 发送事件（在 app_events.h 定义事件类型）
event_bus_post(EVENT_ASR_FINAL, &(event_data_t){
    .asr_text = "你好"
});

// 订阅事件（模块 init 时注册）
EVENT_SUBSCRIBE(EVENT_ASR_FINAL, on_asr_final_handler);

// ❌ 禁止跨模块直接调用（打破解耦）
audio_service_speaker_write(data, len);
```

---

## 8. 线程安全

```c
// 共享数据用互斥锁
xSemaphoreTake(ctx->mutex, portMAX_DELAY);
ctx->data = value;
xSemaphoreGive(ctx->mutex);

// ISR 用 FromISR 版本
BaseType_t xHigherPriorityTaskWoken = pdFALSE;
xSemaphoreGiveFromISR(ctx->sem, &xHigherPriorityTaskWoken);
portYIELD_FROM_ISR(xHigherPriorityTaskWoken);

// 任务间通信用队列（优于共享变量）
xQueueSend(ctx->queue, &event, pdMS_TO_TICKS(100));

// ❌ ISR 中禁止：malloc、阻塞、非 FromISR 信号量
```

---

## 9. 性能注意事项

| 规则 | 原因 |
|------|------|
| JPEG 用硬件编码器，不软编码 | 硬件 10ms vs 软件 100ms+ |
| PPA 用 DMA 异步，不阻塞 CPU | DMA 做融合，CPU 做其他事 |
| 网络请求走串行化队列 | mbedTLS 硬件加速器并发崩溃 |
| 音频 DMA 缓冲 ≥1024 samples | 小缓冲会导致断音 |
| LVGL 在 Core 0，Live2D 在 Core 1 | 避免 CPU 密集型互相抢占 |

---

## 10. Git 提交格式

```
<type>(<scope>): <中文描述>

type: feat / fix / docs / refactor / perf / style / test / chore / init
```

示例：
```
feat(audio): 新增 ES8311 麦克风采集功能
fix(network): 修复 mbedTLS 并发崩溃问题
docs(prd): 更新摄像头模块 PRD 文档
```
