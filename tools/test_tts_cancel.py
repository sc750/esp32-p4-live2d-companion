# -*- coding: utf-8 -*-
"""串口实测 v2：等 TTS 真正开播后再点播音乐，验证 tts_cancel 抢占链路。"""
import serial, time

p = serial.Serial("COM41", 115200, timeout=1)
buf = b""

def pump(sec):
    global buf
    end = time.time() + sec
    while time.time() < end:
        c = p.read(8192)
        if c:
            buf += c

def wait_for(keyword, timeout):
    global buf
    end = time.time() + timeout
    while time.time() < end:
        if keyword in buf.decode("utf-8", errors="replace"):
            return True
        pump(0.5)
    return False

pump(2)
p.write(b"music stop\r\n")          # 清掉上一轮可能排队的播放
pump(2)
p.write("chat 讲三个简短的冷笑话吧\r\n".encode("utf-8"))
ok = wait_for("TTS playback start", 90)   # 等第一句 TTS 真正开播
print("TTS 开播:", ok)
time.sleep(2)                        # 让它播一小段
p.write(b"music play 0\r\n")         # 点播音乐（应触发取消）
pump(12)
p.write(b"music status\r\n")
pump(2)
p.write(b"music stop\r\n")
pump(1)
p.close()

text = buf.decode("utf-8", errors="replace")
skip = ("rig_lvgl", "eh_wifi", "bsp_wifi", "esp-x509", "I2S_IF", "i2s_common", "Adev_Codec", "es_parser")
seen = False
for line in text.splitlines():
    if not any(k in line for k in skip) and line.strip():
        print(line)
