# 硬件资源 spec（hardware）

> 全工程共享的"硬件事实基线"。RESEARCH 阶段冻结，后续以此为准；改动留变更记录。
> 机器可读锁定区在 `hw-lock.yaml`，REVIEW 的 ARCH-8 与冲突检测据此运行。

## 芯片与框架
- 芯片型号：ESP32-P4（chip rev v3.2, 双核 RISC-V @400MHz），板卡 ESP32-P4-Function-EV-Board（含板载 ESP32-C6 Wi-Fi 从机，rev v3.2 识别为 chip revision v3.2 / efuse v1.2）
- 开发框架：ESP-IDF v5.5.4 + esp32_p4_function_ev_board_noglib 5.2.*（官方 BSP，无图形库变体）+ esp_lvgl_adapter 0.5.*（LVGL 9.4）+ esp_video 2.0.x + esp_hosted 3.0.2
- 主频 / 时钟树：CPU 400MHz；PSRAM 250MHz（32MB hex PSRAM，XIP 已启用）；Flash 16MB QIO@40MHz

## 引脚 / DMA / 中断（人类视图）
> 详细分配维护在 `hw-lock.yaml`（机器可读）；此处写设计理由与变更记录。

- **共享 I2C1（GPIO7/8, 400kHz）**：GT911 触摸(0x5D) + ES8311 音频(0x18) + SC2336 相机 SCCB 共总线，由官方 BSP 懒初始化统一持有。
- **显示**：EK79007 1024x600@60Hz RGB565，MIPI-DSI 2-lane @1000Mbps/lane，DPHY 由片上 LDO3 供 2.5V，DPI 3 帧缓冲 + DMA2D，背光 LEDC ch1/GPIO26。LVGL 经 esp_lvgl_adapter 接管（20 行缓冲）。
- **音频**：ES8311 兼任录放（模拟麦克风，非 PDM），I2S1 全双工，PA=GPIO53 由 codec 驱动托管。
- **相机**：SC2336 MIPI-CSI，RAW8 1280x720@30 传感器格式，P4 ISP 转 RGB565 输出，V4L2 `/dev/video0`。
- **Wi-Fi**：标准 esp_wifi API → esp_wifi_remote → esp_hosted SDIO 4-bit → 板载 C6。

## 变更记录
| 日期 | 改了什么 | 原因 |
|---|---|---|
| 2026-09-03 | BSP 重构：从手工放置的 5.2.3 副本（Kconfig 被裁剪）切换为注册表 noglib 5.2.* 正式依赖 | 手工副本 Kconfig 断裂导致 CONFIG_BSP_LCD_TYPE_1024_600 失效，误入 ILI9881C 分支挂死 |
| 2026-09-03 | 冻结引脚/DMA 基线（hw-lock.yaml 回填） | BSP 重构完成后基线确立 |

## 沉淀（promote 回流）
> 本板踩过的硬件坑（如某 strap 脚、某外设时钟门）沉淀于此。

1. **USERPTR 必须按 L2 cache line 对齐**：本工程 `CONFIG_CACHE_L2_CACHE_LINE_128B=y`，esp_video 要求用户帧缓冲地址按 128B 对齐（64B 对齐会在 QBUF 报 EINVAL errno=22）。用 `esp_cache_get_alignment(MALLOC_CAP_SPIRAM)`（esp_private/esp_cache_private.h）运行时查询。
2. **G_FMT 不回填 sizeimage**：esp_video 的 VIDIOC_G_FMT 可能返回 sizeimage=0；帧大小以 QUERYBUF 返回的 buf.length 为准，或回退 w*h*2。
3. **MIPI-DSI lane 速率必须显式设置**：`bsp_display_config_t` 零初始化会让 lane_bit_rate_mbps=0，须显式 `BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS`(1000)。
4. **sdkconfig 粘性**：改 sdkconfig.defaults 后，旧 sdkconfig 中已存在的显式条目（含 `is not set`）会压住新 defaults 值；改 defaults 必须删 sdkconfig 重新生成。
5. **C6 协处理器固件过旧**（fw=0.0.0 vs host 3.0.2，启动有 major version mismatch 告警）：传输层工作正常（SDIO 流模式 + Wi-Fi 连接 OK），后续可用 esp_hosted 的 OTA coprocessor from host 升级。
