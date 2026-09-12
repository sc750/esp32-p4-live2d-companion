# -*- coding: utf-8 -*-
"""步骤 3 端到端测试：对话 → 网关 TTS 下行 → 播放。"""
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

pump(9)
buf += p.read(262144)
for _ in range(60):                       # 等网关连接
    if "已连接网关".encode("utf-8") in buf:
        break
    pump(0.5)
buf = b""
p.write("chat 你好啊\r\n".encode("utf-8"))
pump(50)
p.close()

text = buf.decode("utf-8", errors="replace")
skip = ("rig_lvgl", "eh_wifi", "bsp_wifi", "esp-x509", "I2S_IF", "i2s_common",
        "Adev_Codec", "mem_mgr", "es_parser")
for line in text.splitlines():
    if not any(k in line for k in skip) and line.strip():
        print(line)
