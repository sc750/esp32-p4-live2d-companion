# M05: 音频子系统

> **优先级**: P0 | **预估工时**: 1-2 周 | **依赖**: M01

---

## 1. 模块概述

管理 ESP32-P4-Function-EV-Board 的音频输入输出。板载 ES8311 音频编解码芯片通过 I2S + I2C 与 ESP32-P4 连接，板载 NS4150 3W D 类功放驱动扬声器，板载麦克风用于语音采集。为 AI 语音对话和音乐播放提供底层音频能力。

---

## 2. 架构

```
                    ┌──────────────────────────────┐
                    │         Audio Service          │
                    │  ┌──────────┐ ┌──────────┐   │
                    │  │ Mic Stream│ │ Speaker   │   │
                    │  │ Manager   │ │ Manager   │   │
                    │  └─────┬────┘ └─────┬────┘   │
                    │        │             │         │
                    │  ┌─────▼────┐ ┌─────▼────┐   │
                    │  │ Audio     │ │ Audio     │   │
                    │  │ Pipeline  │ │ Pipeline  │   │
                    │  │ (RX)      │ │ (TX)      │   │
                    │  └─────┬────┘ └─────┬────┘   │
                    └────────┼─────────────┼────────┘
                             │             │
                    ┌────────▼─────────────▼────────┐
                    │        Audio HAL               │
                    │  ┌──────────┐ ┌──────────┐   │
                    │  │ I2S PDM  │ │ I2S STD  │   │
                    │  │ RX       │ │ TX       │   │
                    │  │ (Mic)    │ │ (Spk)    │   │
                    │  └──────────┘ └──────────┘   │
                    └──────────────────────────────┘
```

---

## 3. 功能需求

| ID | 需求 | 优先级 |
|----|------|--------|
| AUD-01 | I2S PDM 麦克风采集 (16kHz 16bit) | P0 |
| AUD-02 | I2S STD 扬声器输出 (16kHz/24kHz) | P0 |
| AUD-03 | 环形缓冲区 (Ring Buffer) 管理 | P0 |
| AUD-04 | MP3 软解码播放 | P1 |
| AUD-05 | 音量控制 (0-100%) | P1 |
| AUD-06 | VAD (Voice Activity Detection) | P1 |
| AUD-07 | 音频流优先级管理 (对话 > 音乐) | P1 |
| AUD-08 | USB 音频输出 (可选) | P2 |
| AUD-09 | 音频增益/降噪预处理 | P2 |

---

## 4. 音频硬件

### 4.1 ES8311 音频编解码器

ES8311 是板载低功耗单声道音频编解码芯片：
- 单通道 ADC (麦克风输入)
- 单通道 DAC (扬声器输出)
- 低噪声前置放大器
- 数字音效处理
- 通过 **I2S** 传输音频数据, **I2C** 配置寄存器

### 4.2 I2S 配置 (标准模式)

```c
// ES8311 作为 I2S 从机
// ESP32-P4 作为 I2S 主机
i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);

i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),  // 16kHz
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_MONO   // ES8311 为单声道编解码器
    ),
    .gpio_cfg = {
        .mclk = I2S_MCLK_PIN,
        .bclk = I2S_BCLK_PIN,
        .ws   = I2S_WS_PIN,
        .dout = I2S_DOUT_PIN,  // → ES8311 DAC → NS4150 功放 → 扬声器
        .din  = I2S_DIN_PIN,   // ← ES8311 ADC ← 板载麦克风
    },
};
```

### 4.3 I2C 配置 (ES8311 寄存器控制)

```c
// I2C 用于配置 ES8311 芯片 (音量、增益、采样率等)
// 需要在初始化阶段通过 I2C 写入 ES8311 寄存器
i2s_channel_init_std_mode(tx_handle, &std_cfg);
i2s_channel_init_std_mode(rx_handle, &std_cfg);
// 然后通过 I2C 配置 ES8311:
// - 设置 ADC/DAC 采样率
// - 设置输入增益 (麦克风)
// - 设置输出音量
// - 使能/禁用耳机/扬声器输出
```

### 4.4 NS4150 功放

NS4150 是 3W 单声道 D 类音频功放：
- 输入: ES8311 DAC 模拟输出
- 输出: 驱动 4Ω 3W 扬声器
- 低 EMI 设计
- 通过 GPIO 控制使能/禁用 (省电)

---

## 5. 扬声器配置

## 5. 音频输出管线

```
TTS 音频流 / MP3 解码 / 系统音效
        │
        ▼
┌──────────────────┐
│ 混音器 (Mixer)    │  支持多流混合
│  - TTS 流 (最高)  │  优先级: 对话 > 音乐 > 音效
│  - Music 流      │
│  - SFX 流        │
└────────┬─────────┘
         │
    ┌────▼────┐
    │ 音量控制 │  乘以音量系数
    └────┬────┘
         │
    ┌────▼──────────┐
    │ PCM Ring Buffer│  DMA 连续读取
    └────┬──────────┘
         │
    ┌────▼──────────┐
    │ I2S TX DMA     │  → ES8311 DAC → NS4150 功放 → 扬声器
    └────────────────┘
```

---

## 6. 音频流优先级

| 优先级 | 流类型 | 说明 |
|--------|--------|------|
| 3 (最高) | TTS 语音 | AI 对话语音, 必须不被打断 |
| 2 | 系统音效 | 提示音、番茄钟铃声 |
| 1 | 音乐播放 | MP3 背景音乐 (可被 TTS 暂停) |
| 0 (最低) | 环境音 | 白噪音等 |

当高优先级流开始时：
- 音乐流自动降低音量 (ducking) 或暂停
- TTS 结束后恢复音乐

---

## 7. 环形缓冲区

```c
typedef struct {
    uint8_t *buffer;
    size_t   size;
    volatile size_t read_pos;
    volatile size_t write_pos;
    SemaphoreHandle_t mutex;
} audio_ring_buffer_t;

// 初始化
esp_err_t audio_ring_buf_init(audio_ring_buffer_t *buf, size_t size);

// 写入 (生产者: 麦克风 DMA 回调 / TTS 流)
size_t audio_ring_buf_write(audio_ring_buffer_t *buf, const void *data, size_t len);

// 读取 (消费者: ASR 处理 / 扬声器 DMA 回调)
size_t audio_ring_buf_read(audio_ring_buffer_t *buf, void *data, size_t len);

// 获取可读数据量
size_t audio_ring_buf_available(audio_ring_buffer_t *buf);
```

### 缓冲区大小估算

| 用途 | 采样率 | 时长 | 大小 |
|------|--------|------|------|
| 麦克风输入环形缓冲 | 16kHz 16bit mono | 2 秒 | 64 KB |
| TTS 输出环形缓冲 | 16kHz 16bit stereo | 1 秒 | 64 KB |
| I2S DMA 缓冲 | — | — | 8 KB |
| MP3 解码缓冲 | 44.1kHz 16bit stereo | 0.5 秒 | 88 KB |

---

## 8. MP3 解码

### 8.1 方案选择

| 方案 | 大小 | CPU 占用 | 质量 |
|------|------|---------|------|
| minimp3 (推荐) | ~20 KB 代码 | 低 | 高 |
| MAD | ~100 KB | 中 | 高 |
| Helix MP3 | ~30 KB | 低 | 中 |

**推荐 minimp3**: 轻量、纯 C、单头文件、适合嵌入式。

### 8.2 解码管线

```
SD 卡 / Flash
    │
    ▼
┌────────────┐
│ MP3 File    │  按帧读取
│ Reader      │
└─────┬──────┘
      │
┌─────▼──────┐
│ minimp3     │  解码为 PCM
│ Decoder     │
└─────┬──────┘
      │
┌─────▼──────┐
│ Resampler   │  (可选) 重采样到 16kHz
└─────┬──────┘
      │
┌─────▼──────┐
│ Mixer       │  混入输出流
└─────┬──────┘
      │
┌─────▼──────┐
│ I2S TX DMA │
└────────────┘
```

---

## 9. 对外接口

```c
// audio_service.h

esp_err_t audio_service_init(void);

// 麦克风
esp_err_t audio_service_mic_start(void);
esp_err_t audio_service_mic_stop(void);
size_t    audio_service_mic_read(int16_t *buf, size_t max_samples, uint32_t timeout_ms);

// 扬声器
esp_err_t audio_service_speaker_start(void);
esp_err_t audio_service_speaker_stop(void);
esp_err_t audio_service_speaker_write(const int16_t *data, size_t samples);

// 音量
esp_err_t audio_service_set_volume(int volume);  // 0-100
int       audio_service_get_volume(void);

// 播放控制
esp_err_t audio_service_play_mp3(const char *file_path);
esp_err_t audio_service_pause(void);
esp_err_t audio_service_resume(void);
esp_err_t audio_service_stop(void);
```

---

## 10. 测试用例

| ID | 测试项 | 预期结果 |
|----|--------|---------|
| TST-AUD-01 | 麦克风录音 | 16kHz PCM 数据, 无杂音/断流 |
| TST-AUD-02 | 扬声器播放 WAV | 16kHz 立体声, 无爆音 |
| TST-AUD-03 | MP3 播放 | 正确解码, 音质正常 |
| TST-AUD-04 | 音量调节 | 0% 静音, 100% 最大, 线性/对数 |
| TST-AUD-05 | TTS + 音乐混音 | TTS 播放时音乐自动降低 |
| TST-AUD-06 | 长时间录音 (30min) | 无内存泄漏/断流 |
| TST-AUD-07 | 同时录音+播放 | 全双工正常, 无干扰 |
