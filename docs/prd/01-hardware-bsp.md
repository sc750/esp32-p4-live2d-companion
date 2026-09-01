# M01: 硬件抽象与 BSP

> **优先级**: P0 | **预估工时**: 1 周 | **依赖**: 无

---

## 1. 模块概述

基于 ESP-BSP 提供的板级支持包，完成 ESP32-P4-Function-EV-Board 所有外设的初始化与抽象，为上层模块提供统一的硬件访问接口。

---

## 2. 功能需求

### 2.1 显示子系统

| ID | 需求 | 验收标准 |
|----|------|---------|
| HW-DISP-01 | MIPI-DSI 初始化, 1024×600@60Hz | 屏幕点亮, 无蓝屏/花屏 |
| HW-DISP-02 | 电容触摸屏初始化 (I2C) | 触摸坐标正确, 多点触控支持 |
| HW-DISP-03 | 帧缓冲分配 (双缓冲) | 2×(1024×600×RGB565) = 2.4MB PSRAM |
| HW-DISP-04 | VSync 中断回调注册 | 可感知垂直同步事件 |
| HW-DISP-05 | PSRAM 时钟 ≥200MHz 配置 | `CONFIG_SPIRAM_SPEED_200M=y` |

### 2.2 音频子系统

| ID | 需求 | 验收标准 |
|----|------|---------|
| HW-AUD-01 | ES8311 Codec 初始化 (I2S + I2C) | I2S 数据通路正常, I2C 寄存器可读写 |
| HW-AUD-02 | 板载麦克风采集 | 16kHz 16bit PCM 数据流输出 |
| HW-AUD-03 | NS4150 功放 + 扬声器输出 | 16kHz/24kHz 16bit 立体声播放, 3W 输出 |
| HW-AUD-04 | DMA 双缓冲音频流 | 无断音/爆音 |

### 2.3 摄像头子系统

| ID | 需求 | 验收标准 |
|----|------|---------|
| HW-CAM-01 | MIPI-CSI 摄像头初始化 | 200万像素摄像头正常出图 |
| HW-CAM-02 | JPEG 硬件编码 | 720p 帧 JPEG 压缩 ≤15ms |
| HW-CAM-03 | 摄像头帧率 | 720p@24fps 或 1080p@15fps |
| HW-CAM-04 | 帧缓冲管理 | 双缓冲, PSRAM DMA 对齐 |

### 2.3 网络子系统

| ID | 需求 | 验收标准 |
|----|------|---------|
| HW-NET-01 | ESP-Hosted SDIO 初始化 | Wi-Fi STA 模式连接成功 |
| HW-NET-02 | SDIO 4-bit 总线, 40MHz | 吞吐量 ≥20 Mbps |
| HW-NET-03 | 自动重连机制 | 断线后 ≤10s 重连 |

### 2.4 存储子系统

| ID | 需求 | 验收标准 |
|----|------|---------|
| HW-STOR-01 | SPIFFS 初始化 (Flash) | 读写固件资源文件 |
| HW-STOR-02 | SD 卡初始化 (FATFS) | 读写音乐/日记文件 |
| HW-STOR-03 | NVS 初始化 | 持久化用户设置 |

---

## 3. 硬件引脚映射

> 基于 ESP32-P4-Function-EV-Board 原理图

### 3.1 MIPI-DSI

| 信号 | GPIO | 说明 |
|------|------|------|
| DSI_CLK_P | 内部 | 差分时钟正 |
| DSI_CLK_N | 内部 | 差分时钟负 |
| DSI_DAT0_P | 内部 | 数据通道 0 正 |
| DSI_DAT0_N | 内部 | 数据通道 0 负 |
| DSI_DAT1_P | 内部 | 数据通道 1 正 |
| DSI_DAT1_N | 内部 | 数据通道 1 负 |
| DPHY_VDD | LDO 2.5V | 由内部 LDO 供电 |

### 3.2 I2S 音频 (ES8311 Codec)

| 信号 | GPIO | 说明 |
|------|------|------|
| I2S_MCLK | TBD | 主时钟 (ES8311 需要) |
| I2S_BCLK | TBD | 位时钟 |
| I2S_WS | TBD | 字选择 (LRCK) |
| I2S_DOUT | TBD | 数据输出 → ES8311 DAC → NS4150 → 扬声器 |
| I2S_DIN | TBD | 数据输入 ← ES8311 ADC ← 板载麦克风 |
| I2C_SDA | TBD | ES8311 寄存器配置 (I2C) |
| I2C_SCL | TBD | ES8311 寄存器配置 (I2C) |
| PA_EN | TBD | NS4150 功放使能控制 (GPIO) |

### 3.2b MIPI-CSI 摄像头

> **注意**: BSP (`espp/esp32-p4-function-ev-board` v1.2.0) 中 `initialize_camera()` 仅为 stub
> 实现，返回 false。M14 模块需自行通过 `esp-video` / `esp-cam` 组件初始化摄像头。

| 信号 | GPIO | 说明 |
|------|------|------|
| CSI_CLK_P/N | 内部 | MIPI 时钟差分对 |
| CSI_DAT0_P/N | 内部 | 数据通道 0 |
| CSI_DAT1_P/N | 内部 | 数据通道 1 |
| CAM_XCLK | TBD | 外部时钟输出 |
| CAM_I2C_SDA | 7 (共用 I2C) | 摄像头 SCCB/I2C 配置 |
| CAM_I2C_SCL | 8 (共用 I2C) | 摄像头 SCCB/I2C 配置 |
| CAM_PWDN | TBD | 摄像头电源控制 |
| CAM_RESET | TBD | 摄像头复位 |

> FPC 连接器: 1.0K-GT-15PB, 15Pin, 间距 1.0mm
> 配件: 200万像素 MIPI CSI 摄像头模组

### 3.3 网络 (Wi-Fi + 以太网)

#### ESP-Hosted SDIO (Wi-Fi via ESP32-C6-MINI-1)

| 信号 | GPIO | 说明 |
|------|------|------|
| SDIO_CLK | 18 | SDIO 时钟 |
| SDIO_CMD | 19 | SDIO 命令 |
| SDIO_D0 | 14 | 数据线 0 |
| SDIO_D1 | 15 | 数据线 1 |
| SDIO_D2 | 16 | 数据线 2 |
| SDIO_D3 | 17 | 数据线 3 |
| SLAVE_RST | 54 | ESP32-C6-MINI-1 复位 |

#### 以太网 (板载 PHY + RJ45)

| 接口 | 说明 |
|------|------|
| EMAC RMII | ESP32-P4 内置 MAC |
| RJ45 端口 | 10/100 Mbps 自适应 |
| 以太网 PHY IC | 板载, 连接 EMAC RMII |

> 以太网可作为 Wi-Fi 的备选/补充网络方案，提供更稳定的连接

### 3.4 SD 卡 (SDMMC Slot 0)

| 信号 | GPIO | 说明 |
|------|------|------|
| SD_CMD | 44 | SD 命令 |
| SD_CLK | 43 | SD 时钟 |
| SD_D0 | 39 | 数据线 0 |
| SD_D1 | 40 | 数据线 1 |
| SD_D2 | 41 | 数据线 2 |
| SD_D3 | 42 | 数据线 3 |

---

## 4. 初始化顺序

```
bootloader
  │
  ▼
main() 
  ├── 1. 系统初始化 (时钟、Flash、PSRAM)
  │     └── 确认 PSRAM ≥32MB, 时钟 ≥200MHz
  │
  ├── 2. NVS 初始化
  │     └── 加载用户配置 (Wi-Fi SSID/密码、主题、音量)
  │
  ├── 3. SPIFFS 挂载
  │     └── 加载 Live2D 模型文件、字体、UI 资源
  │
  ├── 4. 内存池初始化
  │     └── 大块池化分配器 (详见 M13)
  │
  ├── 5. MIPI-DSI + 触摸屏初始化
  │     └── 分配帧缓冲, 注册 VSync 回调
  │
  ├── 6. ES8311 音频初始化
  │     ├── I2C 配置 ES8311 寄存器 (采样率/增益/音量)
  │     ├── I2S TX/RX 通道初始化
  │     └── NS4150 功放使能
  │
  ├── 7. MIPI-CSI 摄像头初始化
  │     ├── 配置 SCCB (I2C) 通信
  │     ├── 设置分辨率/帧率/JPEG 模式
  │     └── 分配帧缓冲 (PSRAM DMA 对齐)
  │
  ├── 8. SDMMC (SD 卡) 初始化
  │     └── 挂载 FAT 文件系统
  │
  ├── 8. ESP-Hosted Wi-Fi 初始化
  │     └── 连接到 AP, 获取 IP
  │
  ├── 9. LVGL 初始化
  │     └── 创建 UI (详见 M02)
  │
  ├── 10. PainterEngine 初始化
  │     └── Live2D 引擎启动 (详见 M03)
  │
  ├── 11. PPA Blend 初始化
  │     └── 图层融合启动 (详见 M04)
  │
  └── 12. 服务层启动
        └── 音频/AI/网络/存储服务
```

---

## 5. 关键配置 (sdkconfig.defaults)

```ini
# === PSRAM ===
CONFIG_SPIRAM=y
CONFIG_SPIRAM_SPEED_200M=y
CONFIG_SPIRAM_MODE_QUAD=y

# === Cache 优化 ===
CONFIG_CACHE_L2_CACHE_256KB=y
CONFIG_CACHE_L2_CACHE_LINE_128B=y

# === 编译优化 ===
CONFIG_COMPILER_OPTIMIZATION_PERF=y

# === MIPI-DSI ===
CONFIG_LCD_MIPI_DSI_LANE_NUM=2

# === ESP-Hosted ===
CONFIG_ESP_HOSTED_HOST_FEAT_RPC=y
CONFIG_ESP_HOSTED_HOST_FEAT_RPC_EXT_V2=y
CONFIG_ESP_HOSTED_HOST_FEAT_WIFI=y

# === Camera (MIPI-CSI) ===
CONFIG_CAM_ENABLE=y

# === LVGL ===
CONFIG_LV_USE_DRAW_MASKS=y
CONFIG_LV_COLOR_DEPTH_16=y

# === FreeRTOS ===
CONFIG_FREERTOS_HZ=1000
```

---

## 6. 对外接口 (API)

```c
// display_hal.h
esp_err_t display_hal_init(void);
esp_err_t display_hal_get_framebuffer(uint8_t **buf1, uint8_t **buf2);
esp_err_t display_hal_register_vsync_callback(vsync_cb_t cb);
esp_err_t touch_hal_read(int *x, int *y, bool *pressed);

// audio_hal.h
esp_err_t audio_hal_init(void);
esp_err_t audio_hal_mic_start(audio_frame_cb_t cb);
esp_err_t audio_hal_speaker_write(const int16_t *data, size_t len);
esp_err_t audio_hal_speaker_start(void);
esp_err_t audio_hal_speaker_stop(void);
esp_err_t audio_hal_set_volume(int volume);  // 0-100, 通过 ES8311 I2C

// camera_hal.h
esp_err_t camera_hal_init(void);
esp_err_t camera_hal_capture_frame(uint8_t **jpeg_buf, size_t *jpeg_size);
esp_err_t camera_hal_release_frame(void);
esp_err_t camera_hal_set_resolution(int width, int height, int fps);

// network_hal.h
esp_err_t network_hal_init(void);
esp_err_t network_hal_wifi_connect(const char *ssid, const char *password);
esp_err_t network_hal_eth_connect(void);       // 有线以太网连接
bool      network_hal_is_connected(void);
esp_err_t network_hal_get_ip(char *ip_str, size_t len);

// storage_hal.h
esp_err_t storage_hal_init(void);
FILE*     storage_hal_open(const char *path, const char *mode);
esp_err_t storage_hal_spiffs_init(void);
esp_err_t storage_hal_sd_init(void);
```

---

## 7. 测试用例

| ID | 测试项 | 预期结果 |
|----|--------|---------|
| TST-HW-01 | 上电后 MIPI-DSI 显示 | 屏幕正常显示 LVGL 默认画面 |
| TST-HW-02 | 触摸屏点击 | 控制台输出正确坐标 |
| TST-HW-03 | ES8311 麦克风录音 | 16kHz PCM 数据流输出 |
| TST-HW-04 | ES8311 + NS4150 扬声器播放 | 播放测试 WAV 无杂音 |
| TST-HW-05 | MIPI-CSI 摄像头出图 | JPEG 帧正确输出 |
| TST-HW-06 | Wi-Fi 连接 | DHCP 获取 IP, ping 通网关 |
| TST-HW-07 | 以太网连接 | RJ45 link up, ping 通网关 |
| TST-HW-08 | SD 卡读写 | 创建/读取/删除文件正常 |
| TST-HW-09 | 长时间显示 (1h) | 无蓝屏/underrun |
