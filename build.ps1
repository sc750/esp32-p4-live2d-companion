# === ESP-IDF 环境说明 ===
# 本机为 Windows 安装器布局（工具在 C:\Espressif），export.bat 无法完整激活，
# 故手工拼装环境。ESP_IDF_VERSION 必须设置——esp_wifi_remote 的 Kconfig 用
# `orsource "./Kconfig.idf_v$ESP_IDF_VERSION.in"` 按版本加载符号，缺失时
# CONFIG_WIFI_RMT_* 全部丢失，esp_hosted 编译失败（eh_host_wifi.c）。
$env:MSYSTEM = ""
$env:IDF_PATH = "D:\esp32idf\v5.5.4\esp-idf"
$env:ESP_IDF_VERSION = "5.5"
$env:ESP_ROM_ELF_DIR = "C:\Espressif\tools\esp-rom-elfs\20241011\"
$env:PATH = "C:\Users\rosesc\.espressif\python_env\idf5.5_py3.12_env\Scripts;" + $env:PATH
$env:PATH = "C:\Espressif\tools\cmake\3.31.7\bin;C:\Espressif\tools\ninja\1.12.1;" + $env:PATH
$env:PATH = "C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20241119\riscv32-esp-elf\bin;" + $env:PATH
Set-Location "D:\esp32Demo\project1"
python "D:\esp32idf\v5.5.4\esp-idf\tools\idf.py" @args
