$env:IDF_PATH = "D:\esp32idf\v5.5.4\esp-idf"
$env:MSYSTEM = ""
$env:PATH = "C:\Users\rosesc\.espressif\python_env\idf5.5_py3.12_env\Scripts;" + $env:PATH
$env:PATH = "C:\Espressif\tools\cmake\3.31.7\bin;C:\Espressif\tools\ninja\1.12.1;" + $env:PATH
$env:PATH = "C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20241119\riscv32-esp-elf\bin;" + $env:PATH
Set-Location "D:\esp32Demo\project1"
python "D:\esp32idf\v5.5.4\esp-idf\tools\idf.py" set-target esp32p4
