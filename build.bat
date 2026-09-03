@echo off
set IDF_PATH=D:\esp32idf\v5.5.4\esp-idf
set PATH=C:\Users\rosesc\.espressif\python_env\idf5.5_py3.12_env\Scripts;%PATH%
set PATH=C:\Espressif\tools\cmake\3.31.7\bin;C:\Espressif\tools\ninja\1.12.1;%PATH%
set PATH=C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20241119\riscv32-esp-elf\bin;%PATH%
cd /d D:\esp32Demo\project1
python D:\esp32idf\v5.5.4\esp-idf\tools\idf.py set-target esp32p4
