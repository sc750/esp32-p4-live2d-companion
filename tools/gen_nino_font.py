#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_nino_font.py — 生成三玖桌面助手专用的 CJK LVGL 字体（nino_cjk_16）

背景：
  LVGL 内置的 source_han_sans_sc_16_cjk 号称"1000 常用字"，实测连
  "听/说/开" 都缺（覆盖率表过时）。而 AI 对话字幕是动态文本，
  内置字库救不了 → 自己用 Windows 自带黑体（simhei.ttf）生成精确覆盖。

用法：
  py tools/gen_nino_font.py          # 生成 user/ui/fonts/lv_font_nino_cjk_16.c

加字流程：
  1. 往下面 CHARSET 里加字符
  2. 重跑本脚本，重新编译固件即可
  （16px 4bpp 每字约 200B flash，100 字 ≈ 20KB，放心加）
"""
import os
import subprocess
import sys

# ---- 需要覆盖的字符集（ASCII 之外的中日韩 + 全角符号） ----
CHARSET = (
    # 对话状态
    "监听中思考说话待机"
    # 按钮/导航
    "返回停止设备"
    # Wi-Fi 状态
    "已连接未"
    # 默认字幕/提示语
    "你好呀我是三玖点击屏幕开始聊天吧等待语音输入请"
    "角色触摸"
    # 标点（全角）
    "！？。，：～、·—…（）"
    # 常用对话储备（TTS 字幕提前铺路）
    "天气日期时间早上午下午夜晚安早安音乐播放暂停上下一首"
    "闹钟设置日记番茄钟钟表度分秒"
    "谢不客气对不起没关系打扰了喜欢高兴开心难过生气"
    "大小高低多少前后左右开关红黄蓝绿黑白亮暗"
    "人名字叫什么呢吗呢啊哦嗯啦咯喽呗"
    # 三玖闲聊轮播语料（rig_chatter.c LINES 表用字，改语料记得同步）
    "个久也习事五今以休会作做儿再别力加努劲去可吃哪哼嘛困在坏太头学宵就工"
    "己干弄得怎息想意才明晨最有来样梦歌油深熬特疼的盯直看眯眼着睛睡神算精"
    "糊累耳胎胖胞自要让许还那醒里长陪面饭"
    # 触摸反应语料（rig_chatter.c TOUCH_LINES 表用字）
    "半又戳把按服然痒突舒钮"
)

OUT = os.path.join("user", "ui", "fonts", "lv_font_nino_cjk_16.c")
FONT = r"C:\Windows\Fonts\simhei.ttf"

def main():
    # 去重去 ASCII
    symbols = "".join(sorted(set(c for c in CHARSET if ord(c) > 127)))
    print(f"[fontgen] 符号集 {len(symbols)} 字: {symbols}")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    cmd = [
        "npx", "--yes", "lv_font_conv",
        "--font", FONT,
        "--size", "16",
        "--bpp", "4",
        "--format", "lvgl",
        "--lv-font-name", "nino_cjk_16",
        "--range", "0x20-0x7F",          # ASCII（WiFi:/数字等混排）
        "--symbols", symbols,
        "--no-compress",
        "-o", OUT,
    ]
    print("[fontgen]", " ".join(cmd))
    r = subprocess.run(cmd, shell=(os.name == "nt"))
    if r.returncode != 0:
        sys.exit("[fontgen] lv_font_conv 失败")
    size = os.path.getsize(OUT)
    print(f"[fontgen] 生成 {OUT} ({size}B)")

if __name__ == "__main__":
    main()
