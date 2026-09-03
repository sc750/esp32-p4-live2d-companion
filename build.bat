@echo off
rem === ESP-IDF 环境说明 ===
rem 本机为 Windows 安装器布局（工具在 C:\Espressif），export.bat 无法完整激活
rem （报 "tool xtensa-esp-elf-gdb has no installed versions"），故手工拼装环境。
rem 注意：ESP_IDF_VERSION 必须设置——esp_wifi_remote 组件的 Kconfig 用
rem `orsource "./Kconfig.idf_v$ESP_IDF_VERSION.in"` 按版本加载符号；
rem 缺失时 CONFIG_WIFI_RMT_* 全部丢失，esp_hosted 编译失败（eh_host_wifi.c）。
set MSYSTEM=
set IDF_PATH=D:\esp32idf\v5.5.4\esp-idf
set ESP_IDF_VERSION=5.5
set ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011\
set PATH=C:\Users\rosesc\.espressif\python_env\idf5.5_py3.12_env\Scripts;%PATH%
set PATH=C:\Espressif\tools\cmake\3.31.7\bin;C:\Espressif\tools\ninja\1.12.1;%PATH%
set PATH=C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20241119\riscv32-esp-elf\bin;%PATH%
cd /d D:\esp32Demo\project1
python D:\esp32idf\v5.5.4\esp-idf\tools\idf.py %*
