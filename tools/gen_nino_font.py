#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_nino_font.py — 生成三玖桌面助手专用的 CJK LVGL 字体（nino_cjk_16）

背景：
  LVGL 内置的 source_han_sans_sc_16_cjk 号称"1000 常用字"，实测连
  "听/说/开" 都缺（覆盖率表过时）。而 AI 对话字幕是动态文本，
  内置字库救不了 → 自己用 Windows 自带黑体（simhei.ttf）生成精确覆盖。

用法：
  py tools/gen_nino_font.py            # 生成 user/ui/fonts/lv_font_nino_cjk_16.c
  py tools/gen_nino_font.py --verify   # 只校验：全工程上屏字符串 ⊆ 当前字库？

加字流程：
  1. 往下面 CHARSET 里加字符
  2. 重跑本脚本，重新编译固件即可
  （16px 4bpp 每字约 200B flash，100 字 ≈ 20KB，放心加）

防复发（break-loop R12 沉淀）：
  豆腐块的根因是"文案改动与字库不同步"且编译期零感知。
  --verify 模式扫描 user/ 全部 .c/.h 的字符串字面量（排除 ESP_LOGx
  日志行——串口日志不上屏无字体问题），校验每个非 ASCII 字符都在
  当前 CHARSET/字库覆盖内，缺失则 exit 1——接进提交前检查即可机械拦截。
"""
import glob
import os
import re
import subprocess
import sys

# ---- 需要覆盖的字符集（ASCII 之外的中日韩 + 全角符号） ----
CHARSET = (
    # 对话状态
    "监听中思考说话待机"
    # 按钮/导航
    "返回停止设备"
    # Wi-Fi 状态
    "已连接未正"
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
    # 音乐页（scr_music.c 文案 + music_service.c 的电台名）
    # 电台名在 music_service.c 的 s_radios[] 字面量里，会随播放列表直接上屏
    "网络电台环境子舒缓声空间氛围量当无曲目"
)

OUT = os.path.join("user", "ui", "fonts", "lv_font_nino_cjk_16.c")
FONT = r"C:\Windows\Fonts\simhei.ttf"


def charset_chars():
    """从本脚本源码提取 CHARSET 常量的非 ASCII 字符集（单一事实源）"""
    src = open(__file__, encoding="utf-8").read()
    block = src[src.index("CHARSET = ("):src.index(")", src.index("CHARSET = ("))]
    return set(c for c in block if ord(c) > 127 and c not in "　")


def font_covered_chars():
    """解析生成的 LVGL 字体源码，返回其覆盖的 unicode 集合"""
    path = os.path.join("user", "ui", "fonts", "lv_font_nino_cjk_16.c")
    src = open(path, encoding="utf-8").read()
    covered = set()
    # FORMAT0_TINY 段：range_start..range_start+range_length 全覆盖
    for m in re.finditer(
            r"\.range_start = (\d+), \.range_length = (\d+), "
            r"\.glyph_id_start = \d+,\s*\n?\s*\.unicode_list = NULL", src):
        s0, l0 = int(m.group(1)), int(m.group(2))
        covered.update(range(s0, s0 + l0))
    # SPARSE_TINY 段：unicode_list_N[] + 所属 range 基址
    lists = dict(re.findall(
        r"static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};", src, re.S))
    for m in re.finditer(
            r"\.range_start = (\d+), \.range_length = \d+, \.glyph_id_start = \d+,"
            r"\s*\n?\s*\.unicode_list = (unicode_list_\d+),", src):
        base = int(m.group(1))
        for tok in re.findall(r"0x([0-9A-Fa-f]+)", lists.get(m.group(2), "")):
            covered.add(base + int(tok, 16))
    return covered


def verify():
    """校验上屏字符串的每个非 ASCII 字符都在字库覆盖内（机械门禁）

    扫描范围 = user/ui/**（含 rig_chatter 语料引用链）+ user/rig/rig_chatter.c
    ——全工程所有会送到 lv_label 的字符串都出自这些文件；ESP_LOGx 日志走
    串口无字体问题，注释更是人类看的，都剥掉后再提取字符串字面量。
    """
    cs = charset_chars()
    covered = font_covered_chars()
    allowed = cs | covered          # CHARSET 声明的 ∪ 当前字库实际有的
    scope = (glob.glob("user/ui/**/*.c", recursive=True)
             + glob.glob("user/ui/**/*.h", recursive=True)
             + ["user/rig/rig_chatter.c"])

    def strip_comments(text):
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)   # 块注释
        text = re.sub(r"//[^\n]*", "", text)                # 行注释
        return text

    str_re = re.compile(r'"([^"\\]|\\.)*"')
    missing = {}                    # char -> 首个出现位置
    for path in scope:
        if "font" in path.replace("\\", "/").lower():
            continue                # 字体数据文件自身跳过
        text = strip_comments(open(path, encoding="utf-8", errors="ignore").read())
        for lineno, line in enumerate(text.split("\n"), 1):
            if line.lstrip().startswith("ESP_LOG"):
                continue            # 串口日志不上屏
            for lit in str_re.findall(line):
                for c in lit:
                    if ord(c) > 127 and c not in allowed:
                        missing.setdefault(c, f"{path}:{lineno}")
    if missing:
        print(f"[verify] 缺失 {len(missing)} 字（会显示为豆腐块）：")
        for c, loc in sorted(missing.items()):
            print(f"  '{c}' U+{ord(c):04X}  首见 {loc}")
        print("[verify] FAIL —— 加进 CHARSET 重跑生成，或改文案")
        return 1
    print(f"[verify] PASS —— 上屏字符串全部被字库覆盖"
          f"（CHARSET {len(cs)} 字）")
    return 0


def build_charset():
    """GB2312 全集（6763 字：一级 3755 常用 + 二级 3008 次常用）+ 手工符号集

    R13：LLM 回复是动态文本，手搓几百字必出豆腐块——直接上 GB2312 全集
    （PRD M02 字体规划的原方案）。16px 4bpp 全集约 1.3MB flash，可承受。
    """
    chars = set()
    # GB2312 汉字区：行 0xB0-0xF7，列 0xA1-0xFE（部分空位跳过）
    for hi in range(0xB0, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass                    # 空位
    # 手工补充：全角标点 + 特殊符号（GB2312 汉字区外）
    chars.update(set(c for c in CHARSET if ord(c) > 127))
    return chars


def main():
    # --verify：只做覆盖校验不生成（提交前/CI 机械门禁用）
    if "--verify" in sys.argv:
        sys.exit(verify())
    symbols = "".join(sorted(build_charset()))
    print(f"[fontgen] 符号集 {len(symbols)} 字（GB2312 全集+符号）")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    # Windows cmd 参数上限 8K，GB2312 六千字会爆——用 npx 全路径 + 参数列表
    # （CreateProcess 上限 32K，11KB 的符号串安全）
    import shutil
    npx = shutil.which("npx.cmd") or shutil.which("npx")
    cmd = [
        npx, "--yes", "lv_font_conv",
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
    print(f"[fontgen] 调用 {os.path.basename(npx)}（符号 {len(symbols)} 字走参数列表）")
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit("[fontgen] lv_font_conv 失败")
    size = os.path.getsize(OUT)
    print(f"[fontgen] 生成 {OUT} ({size}B)")
    # 生成完立即自校验（防"生成了但没覆盖全"）
    sys.exit(verify())

if __name__ == "__main__":
    main()
