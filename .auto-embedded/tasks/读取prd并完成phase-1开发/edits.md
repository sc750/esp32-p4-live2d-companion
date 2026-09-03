# 编辑清单

| 文件 | 改动 | 验证标准 | 结果 | commit |
|---|---|---|---|---|
| `managed_components/espressif__esp32_p4_function_ev_board/esp32_p4_function_ev_board.c` | ~~待移除 ILI9881C 无超时 ID 读取~~ | COM41 启动日志无 Task WDT | **作废**：根因是 Kconfig 断裂误入 ILI9881C 分支，不做 managed_components 手术，改为重构依赖链（R1） | - |
| R1 | `main/idf_component.yml` 重写：+`esp32_p4_function_ev_board_noglib: 5.2.*`，删 `lock_managed_components` 与 `esp_lcd_lt8912b` 直依赖 | `CONFIG_BSP_LCD_TYPE_1024_600=y` 出现在 sdkconfig | ✅ reconfigure 后命中 | - |
| R1 | `user/CMakeLists.txt`：PRIV_REQUIRES → `espressif__esp32_p4_function_ev_board_noglib` | 组件名解析正确 | ✅ | - |
| R1 | 删除 `components/`（bsp_extra 双层 stub + esp_lcd_ek79007 手工副本）、`managed_components/`、`dependencies.lock`、`sdkconfig`、`sdkconfig.stale`；重构前快照 commit `3db7673` | 依赖树从注册表重建 | ✅ noglib/K79007/esp_video/esp_cam_sensor 全部解析 | - |
| R1 | `sdkconfig.defaults`：删 `ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN`，+`BSP_DISPLAY_BRIGHTNESS_LEDC_CH=1`、`BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL=y` | 空白 flash 可自动格式化 SPIFFS | ✅ 符号实名为 FORMAT_ON_MOUNT_FAIL（非 FORMAT_ON_MOUNT） | `0cac84a` |
| R1 | `build.bat`/`build.ps1`：补 `ESP_IDF_VERSION=5.5`（缺失→CONFIG_WIFI_RMT_* 静默丢失→esp_hosted 编译失败）与 `ESP_ROM_ELF_DIR`；支持任意 idf.py 参数 | idf.py build 通过 | ✅ | `0cac84a` |
| R2 | `user/bsp/bsp_init.cpp` 重写：显式 `dsi_bus.lane_bit_rate_mbps=BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS`；初始化顺序对齐官方；GBK→UTF-8 | COM41 日志 1024x600、无 Task WDT、屏幕点亮 UI | ✅ EK79007 生效/GT911 触摸 OK/背光 100%/Home+Chat 创建/动画启动 | `e910e95` |
| R2 | `build.bat`/`build.ps1` 注释转 ASCII | cmd GBK 解析 .bat 不再错乱 | ✅ | `e910e95` |
| R3 | 新增 `user/bsp/bsp_audio.h/.c`（L1）：ES8311 录放 + PA 托管；bsp_init 接入 open+self_test；命名 init→open 规避 BSP 冲突 | 日志 ES8311 双向 open OK + 用户确认听到自检音 | ✅ 麦克风峰值 8000/32767（通路闭环） | `64a493c` |
| R4 | 新增 `user/bsp/bsp_camera.h/.c`（L1）+ sdkconfig 摄像头段：SC2336 V4L2 USERPTR 取流 | /dev/video0 出图帧计数递增 | ✅ RGB565 1280x720 精确 30fps（600帧/20s）；三修复：sizeimage 回退/128B cache 对齐/QUERYBUF 定长 | `6e6d4a5` |
| R5 | 新增 `user/bsp/bsp_wifi.h/.c` + user/Kconfig.projbuild：STA 经 esp_hosted SDIO→C6；删除 bsp_init 中 wifi stub | got ip 日志 + get_ip 实装 | ✅ got ip 10.108.211.47（关联→DHCP 全链路 7.4s），与相机 30fps 并发 | `717cc2c` |
| R6 | ~~SD 卡模块~~ **用户取消**（手头无 SD 卡）；已创建的 bsp_sdcard.h/.c 未接线，已删除 | - | - | - |
