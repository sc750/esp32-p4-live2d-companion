# 研究发现

| 关键词 | 来源 | 摘要 | 可信度 | 状态 |
|---|---|---|---|---|
| 显示初始化阻塞 | COM41 实机串口日志 + `addr2line` | CPU0 卡在 `panel_io_dbi_rx_param()`，由 ILI9881C 初始化中的 ID 寄存器读取触发；该路径无超时，导致 Task WDT。 | 高 | 待修复 |
| ILI9881C vendor 配置 | `esp_lcd_ili9881c.h` / 组件测试例 | 驱动要求 `vendor_config` 包含 DSI bus、DPI config 和 lane 数；当前代码已按该约定提供。 | 高 | 已验证编译 |
| 分辨率矛盾 | `docs/prd/00-overview.md` 与 BSP `display.h` | PRD 标称 1024x600；锁定 BSP/ILI9881C 组件配置为 1280x800（内部按旋转使用 800x1280），缺少实际面板型号或时序资料，不能据 PRD 直接改写。 | 高 | 已解决（见下） |
| BSP Kconfig 断裂（显示挂死根因） | 本项目 sdkconfig.defaults × managed BSP Kconfig × 官方 esp_brookesia_phone 对照 | 本项目 `CONFIG_BSP_LCD_TYPE_1024_600=y` 等 4 个符号在整棵依赖树无 Kconfig 定义（手工放置的 BSP 组件 Kconfig 被裁剪），被静默丢弃 → `display.h` 编译进 800x1280 ILI9881C 分支 → ILI9881C ID 读取挂死。即前两条"显示初始化阻塞"发现的真正根因。修复=恢复官方依赖链（espressif/esp32_p4_function_ev_board_noglib 5.2.*），不做 managed_components 手术。 | 高 | 待修复（PLAN R1） |
| dsi lane 速率未配置（第二处缺陷） | `user/bsp/bsp_init.cpp:53` vs 官方 `main.cpp:141-157` | 本项目 `bsp_display_config_t cfg = {};` 零初始化，未设 `dsi_bus.lane_bit_rate_mbps`（官方显式设 `BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS`=1000）；即使修好 Kconfig，DSI 总线创建仍会异常。 | 高 | 待修复（PLAN R2） |
| 官方 BSP 硬件参数基线（esp32_p4_function_ev_board_noglib 5.2.3） | 官方示例 managed_components 源码 | I2C1 GPIO7/8@400kHz 全板共享；I2S1 MCLK13/BCLK12/WS10/DOUT9/DSIN11；ES8311(0x18) 兼任录放（模拟麦克风，非 PDM），PA=GPIO53；EK79007 1024x600@60Hz RGB565，DSI 2-lane@1000Mbps，DPHY=片上 LDO3 2.5V，DPI 3 缓冲+DMA2D，背光 LEDC ch1/GPIO26；触摸 GT911(0x5D) mirror_x/y；SDMMC slot0 D0-D3=39/40/41/42 CMD44 CLK43 + LDO4 3.3V；摄像头 SC2336 MIPI-CSI V4L2 `/dev/video0` RAW8 1280x720@30 + P4 ISP；Wi-Fi=esp_wifi→esp_wifi_remote 1.6.2→esp_hosted 3.0.2(SDIO)→板载 C6。 | 高 | 冻结为基线 |
| 组件身份混乱 | dependencies.lock(1.2.0) vs managed_components 内容(5.2.3) vs components/bsp_extra 依赖(^1.0.0) | 手工放置 + `lock_managed_components: true` 锁定 + 外层 bsp_extra stub 拉旧版 BSP 三者叠加造成。修复=解除锁定、显式依赖 noglib 5.2.*、删除手工组件，重新求解依赖树。 | 高 | 待修复（PLAN R1） |
| 初始化顺序基准（官方） | 官方 `main.cpp` app_main | NVS → SPIFFS → SD/LDO → 音频 codec → 显示(BSP 裸句柄) → LVGL adapter → 背光 → UI；I2C 由 BSP 懒初始化；相机/Wi-Fi 独立延迟初始化。 | 高 | 冻结为基准 |
| 构建环境（本项目特有） | build.bat + 实测 | Windows 安装器布局下 export.bat 不可用；手工环境必须设 `ESP_IDF_VERSION=5.5`（缺失→CONFIG_WIFI_RMT_* 静默丢失→esp_hosted 编译失败）与 `ESP_ROM_ELF_DIR`；cmd 解析 .bat 按 GBK，.bat 内 UTF-8 中文注释会吞换行。已封装进 build.bat/build.ps1（支持任意 idf.py 参数）。 | 高 | 已固化 |
| C6 协处理器固件过旧 | COM41 启动日志 | fw=0x00000000/coprocessor=0.0.0 vs host 3.0.2，有 major version mismatch 告警；传输层功能正常（Wi-Fi 连接实测通过）。后续可用 esp_hosted OTA coprocessor from host 升级。 | 中 | 待办（非阻塞） |
