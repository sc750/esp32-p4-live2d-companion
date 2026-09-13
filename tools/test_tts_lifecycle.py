# -*- coding: utf-8 -*-
"""全程监控：完整对话周期（chat -> TTS 播完 -> 存活检查），排查崩溃 + 验证队列排空。"""
import serial, time

p = serial.Serial("COM41", 115200, timeout=1)
p.dtr = False
p.rts = True
time.sleep(0.1)
p.rts = False
buf = b""

def pump(sec):
    global buf
    end = time.time() + sec
    while time.time() < end:
        c = p.read(8192)
        if c:
            buf += c

pump(8)                      # 等启动
buf = p.read(262144)         # 丢弃启动日志
p.write("chat 给我讲三个冷笑话\r\n".encode("utf-8"))
deadline = time.time() + 180  # 最多等 3 分钟（TTS 长播报）
done = False
while time.time() < deadline:
    pump(1)
    t = buf.decode("utf-8", errors="replace")
    if "全程:" in t or "脑子突然一片空白" in t:     # process_wav 收尾标志
        done = True
        break
print("管线收尾标志:", done)
pump(5)                      # 再观察 5 秒看有没有 panic
p.write(b"music status\r\n") # 控制台应已恢复 = 任务未卡死
pump(2)
p.close()

text = buf.decode("utf-8", errors="replace")
skip = ("rig_lvgl", "eh_wifi", "bsp_wifi", "esp-x509", "I2S_IF", "i2s_common", "Adev_Codec", "mem_mgr")
panic_kw = ("panic", "abort", "Guru", "assert", "Backtrace", "stack overflow", " watchdog")
for line in text.splitlines():
    if any(k.lower() in line.lower() for k in panic_kw):
        print("[PANIC?]", line)
    elif not any(k in line for k in skip) and line.strip():
        print(line)
