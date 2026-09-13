# -*- coding: utf-8 -*-
"""串口实测：mp3 TTS + 攒批 + 解码全链路（抓 MiniMax 合成/批次/播放日志）。"""
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

pump(9)                      # 等启动
buf += p.read(262144)
# 等 Wi-Fi + SNTP 就绪
for _ in range(60):
    if "时间已同步" in buf.decode("utf-8", errors="replace"):
        break
    pump(0.5)
buf = b""                    # 清掉启动段，只留对话
p.write("chat 给我讲三个冷笑话吧\r\n".encode("utf-8"))
pump(90)                     # 对话+TTS 全程
p.close()

text = buf.decode("utf-8", errors="replace")
skip = ("rig_lvgl", "eh_wifi", "bsp_wifi", "esp-x509", "I2S_IF", "i2s_common", "Adev_Codec", "mem_mgr", "es_parser")
panic_kw = ("panic", "abort", "Guru", "Backtrace", "stack overflow", "watchdog")
for line in text.splitlines():
    if any(k.lower() in line.lower() for k in panic_kw):
        print("[PANIC?]", line)
    elif not any(k in line for k in skip) and line.strip():
        print(line)
