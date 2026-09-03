@echo off
rem === ESP-IDF env (Windows-installer layout: tools in C:\Espressif) ===
rem export.bat CANNOT fully activate here (gdb tools "not installed"), so we
rem hand-roll the environment. ESP_IDF_VERSION is REQUIRED: esp_wifi_remote's
rem Kconfig loads symbols via `orsource "./Kconfig.idf_v$ESP_IDF_VERSION.in"`.
rem Without it CONFIG_WIFI_RMT_* vanish and esp_hosted fails to compile.
rem ESP_ROM_ELF_DIR silences the per-build gen_gdbinit warning.
rem Usage: build.bat <idf.py args>, e.g. build.bat build / build.bat flash monitor
set MSYSTEM=
set IDF_PATH=D:\esp32idf\v5.5.4\esp-idf
set ESP_IDF_VERSION=5.5
set ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011\
set PATH=C:\Users\rosesc\.espressif\python_env\idf5.5_py3.12_env\Scripts;%PATH%
set PATH=C:\Espressif\tools\cmake\3.31.7\bin;C:\Espressif\tools\ninja\1.12.1;%PATH%
set PATH=C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20241119\riscv32-esp-elf\bin;%PATH%
cd /d D:\esp32Demo\project1
python D:\esp32idf\v5.5.4\esp-idf\tools\idf.py %*
