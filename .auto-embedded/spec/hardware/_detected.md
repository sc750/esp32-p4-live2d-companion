# 自动探测结果（草案，待人工确认）

> 由 `aemb init` 扫描工程文件名/扩展名推断，**可能误判**。
> 确认无误后，请把相关信息手工并入 `index.md` 与 `hw-lock.yaml`，再删除本文件。

- 疑似芯片: ESP32, ESP32P4, ESP32C3, ESP32XX, ESP32C2, ESP32C5, ESP32C6, ESP32C61, ESP32S2, ESP32S3, RT-THREAD, RTTHREAD
- 疑似框架: ESP-IDF
- 构建系统: CMake, Makefile

## 证据
- CMake ← CMakeLists.txt
- ESP-IDF ← Kconfig.projbuild
- ESP32 ← esp32_p4_function_ev_board.c
- ESP32P4 ← sdkconfig.defaults.esp32p4
- ESP32C3 ← eh_cp_feat_bt_uart_esp32c3_s3.c
- ESP32XX ← eh_cp_feat_bt_uart_esp32xx.c
- ESP32C2 ← sdkconfig.defaults.esp32c2
- ESP32C5 ← sdkconfig.defaults.esp32c5
- ESP32C6 ← sdkconfig.defaults.esp32c6
- ESP32C61 ← sdkconfig.defaults.esp32c61
- ESP32S2 ← sdkconfig.defaults.esp32s2
- ESP32S3 ← sdkconfig.defaults.esp32s3
- Makefile ← Makefile
- RT-THREAD ← rt-thread.rst
- RTTHREAD ← lv_rtthread.c
