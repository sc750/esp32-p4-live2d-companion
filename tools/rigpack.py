#!/usr/bin/env py
# -*- coding: utf-8 -*-
"""
rigpack.py — 类 Live2D 角色资源打包工具（M03 R1）

用法:
  py tools/rigpack.py placeholder [输出.rigbin]          # 生成程序合成 placeholder 角色
  py tools/rigpack.py pack <素材目录> [输出.rigbin]       # 从真实立绘打包（base.png + 变体图）
  py tools/rigpack.py verify <文件.rigbin>               # 校验并打印 rigbin 内容

rigbin v1 格式（小端，MCU 直接映射，零解码）:
  Header 32B:
    magic[4]="RIG1" ver:u16 flags:u16 canvas_w:u16 canvas_h:u16
    layer_count:u16 param_count:u16 anim_count:u16 reserved:u32 total_size:u32
  Layer 表 ×layer_count（每条 32B）:
    name[12] parent:i16 atlas_x:u16 atlas_y:u16 atlas_w:u16 atlas_h:u16
    base_x:i16 base_y:i16 z:i16 flags:u16 reserved:u32
    flags bit0=physics_chain（Verlet 物理，如呆毛）
  Atlas: w:u16 h:u16 后接 w*h*4 RGBA8888（直通 alpha，非预乘）
  参数/动画表: 预留 v2（v1 先置 0）

素材约定（pack 模式，目录内）:
  base.png          全身立绘（纯白背景，长边>=1024）
  eyes_closed.png   同图闭眼版（仅眼睛区域与 base 不同）
  mouth_half.png    嘴微张变体
  mouth_open.png    嘴张开变体
  parts.json        可选：{"neck_ratio":0.42} 头/身切分线（占图高比例）
切层原理:
  - 变体图与 base 做像素差分 → 变化区域包围盒 = 该部件（眼/嘴）图块，坐标与 base 对齐
  - 头/身在 neck_ratio 处横切，头部带 overlap 边距防接缝
"""
import struct
import sys
import os
from PIL import Image, ImageDraw

MAGIC = b"RIG1"
VERSION = 1
HEADER_FMT = "<4sHHHHHHHII"         # 4+2*7+4*2 = 26B，补齐到 32B 用 padding
LAYER_FMT = "<12shHHHHhhhHIH"        # 12+2+2*4+2*3+2+2+4+2 = 36B（含对齐 padding）

# 图层名约定（固件按名索引）
L_BODY, L_HEAD, L_EYE_L, L_EYE_R, L_MOUTH, L_AHOGE, L_HAIR_FRONT = (
    "body", "head", "eye_l", "eye_r", "mouth", "ahoge", "hair_front")
FLAG_PHYSICS = 0x0001


# ---------------------------------------------------------------- rigbin 写出
def write_rigbin(path, canvas_w, canvas_h, layers, atlas_img):
    """layers: [{name,parent,atlas_x,atlas_y,atlas_w,atlas_h,base_x,base_y,z,flags}]"""
    layer_count = len(layers)
    atlas_w, atlas_h = atlas_img.size
    total = 32 + 36 * layer_count + 4 + atlas_w * atlas_h * 4

    out = bytearray()
    out += struct.pack(HEADER_FMT, MAGIC, VERSION, 0,
                       canvas_w, canvas_h, layer_count, 0, 0, 0, total)
    out += b"\0" * 6                                   # 26B 补齐 32B 头
    for ly in layers:
        name = ly["name"].encode("ascii")[:12].ljust(12, b"\0")
        out += struct.pack(LAYER_FMT, name, ly.get("parent", -1),
                           ly["atlas_x"], ly["atlas_y"], ly["atlas_w"], ly["atlas_h"],
                           ly["base_x"], ly["base_y"], ly["z"], ly.get("flags", 0), 0, 0)
    out += struct.pack("<HH", atlas_w, atlas_h)
    out += atlas_img.convert("RGBA").tobytes()
    assert len(out) == total, f"size mismatch {len(out)} != {total}"
    with open(path, "wb") as f:
        f.write(out)
    print(f"[rigpack] 写出 {path}: canvas={canvas_w}x{canvas_h} "
          f"layers={layer_count} atlas={atlas_w}x{atlas_h} total={total}B")


# ---------------------------------------------------------------- rigbin 校验
def verify_rigbin(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, ver, flags, cw, ch, nl, nparam, nanim, _r, total = struct.unpack_from(HEADER_FMT, data, 0)
    assert magic == MAGIC, f"magic 错: {magic}"
    assert total == len(data), f"total_size 不符: {total} != {len(data)}"
    print(f"[verify] {os.path.basename(path)}: v{ver} canvas={cw}x{ch} "
          f"layers={nl} params={nparam} anims={nanim} size={total}B")
    off = 32
    for i in range(nl):
        (name, parent, ax, ay, aw, ah, bx, by, z, fl, _r, _r2) = struct.unpack_from(LAYER_FMT, data, off)
        off += 36
        nm = name.split(b"\0")[0].decode("ascii")
        print(f"  layer[{i}] {nm:<12} "
              f"parent={parent:<3} atlas=({ax},{ay} {aw}x{ah}) base=({bx},{by}) z={z} "
              f"flags={fl:#06x}{' [physics]' if fl & FLAG_PHYSICS else ''}")
    aw, ah = struct.unpack_from("<HH", data, off)
    off += 4
    n_px = aw * ah
    px = data[off:off + n_px * 4]
    alpha_nonzero = sum(1 for i in range(3, len(px), 4) if px[i] > 0)
    print(f"  atlas={aw}x{ah} 非透明像素占比 {alpha_nonzero * 100 // max(1, n_px)}%")
    print("[verify] PASS")


# ---------------------------------------------------------------- placeholder
def placeholder(path):
    """程序合成 6 层角色（三玖风格占位：粉发/蓝耳机/米色开衫），画布 480x640。"""
    W, H = 480, 640
    canvas_atlas = Image.new("RGBA", (W, H), (0, 0, 0, 0))

    def new_layer():
        return Image.new("RGBA", (W, H), (0, 0, 0, 0))

    PINK = (247, 168, 192, 255)
    PINK_D = (226, 138, 168, 255)
    SKIN = (255, 227, 205, 255)
    SKIN_D = (235, 196, 172, 255)
    BLUE = (70, 130, 200, 255)
    UNIFORM = (235, 225, 205, 255)
    UNIFORM_D = (205, 192, 168, 255)
    BLACK = (60, 52, 60, 255)
    RED = (215, 90, 90, 255)

    # ---- body（躯干+校服，含脖颈） ----
    body = new_layer()
    d = ImageDraw.Draw(body)
    d.rounded_rectangle([180, 300, 300, 600], 40, fill=UNIFORM)          # 开衫
    d.rounded_rectangle([196, 300, 284, 590], 34, outline=UNIFORM_D, width=6)
    d.polygon([(225, 300), (255, 300), (240, 380)], fill=(255, 255, 255, 255))  # 领口
    d.rectangle([228, 270, 252, 310], fill=SKIN_D)                       # 脖颈
    body_atlas = (170, 260, 140, 350)  # atlas 内区域(左上x,左上y,宽,高)

    # ---- head（脸+头发+耳机） ----
    head = new_layer()
    d = ImageDraw.Draw(head)
    d.rounded_rectangle([150, 60, 330, 300], 80, fill=PINK)              # 后发
    d.ellipse([170, 110, 310, 290], fill=SKIN)                           # 脸
    d.polygon([(170, 150), (310, 150), (310, 120), (170, 120)], fill=PINK)  # 齐刘海
    d.pieslice([150, 60, 330, 240], 160, 380, fill=PINK)
    d.arc([170, 110, 310, 290], 200, 340, fill=SKIN_D, width=4)
    d.rounded_rectangle([160, 130, 190, 230], 14, fill=BLUE)             # 左耳机
    d.rounded_rectangle([290, 130, 320, 230], 14, fill=BLUE)             # 右耳机
    head_atlas = (10, 10, 330, 300)

    # ---- 眼睛（开） ----
    def eye(x):
        e = new_layer()
        d = ImageDraw.Draw(e)
        d.ellipse([x, 190, x + 34, 226], outline=BLACK, width=4, fill=(255, 255, 255, 255))
        d.ellipse([x + 10, 198, x + 26, 220], fill=(60, 120, 200, 255))
        d.ellipse([x + 14, 202, x + 20, 208], fill=(255, 255, 255, 255))
        return e

    eye_l, eye_r = eye(200), eye(252)

    # ---- 眼睛（闭，占同一 atlas 位，运行时换图用独立图层实现更简单：
    #      v1 用参数控制眼睛 scaleY=0.1 模拟闭眼，因此不出闭眼图层） ----

    # ---- mouth（微笑） ----
    mouth = new_layer()
    d = ImageDraw.Draw(mouth)
    d.arc([228, 240, 262, 264], 20, 160, fill=RED, width=5)
    mouth_atlas_src = (222, 234, 46, 36)

    # ---- ahoge（呆毛，physics 链演示） ----
    ahoge = new_layer()
    d = ImageDraw.Draw(ahoge)
    d.line([(240, 62), (232, 30), (252, 12), (244, 4)], fill=PINK_D, width=9, joint="curve")
    ahoge_atlas_src = (220, 0, 44, 66)

    # ---- 组装 atlas（简单 shelf 排布）并导出图层区域 ----
    slots = {}
    x = 0
    for name, img, (sx, sy, sw, sh) in [
        (L_BODY, body, body_atlas), (L_HEAD, head, head_atlas),
        (L_EYE_L, eye_l, (196, 186, 42, 44)), (L_EYE_R, eye_r, (248, 186, 42, 44)),
        (L_MOUTH, mouth, mouth_atlas_src), (L_AHOGE, ahoge, ahoge_atlas_src),
    ]:
        slots[name] = (x, 0, sw, sh)
        canvas_atlas.paste(img.crop((sx, sy, sx + sw, sy + sh)), (x, 0))
        x += sw + 4

    layers = [
        dict(name=L_BODY,  parent=-1, base_x=170, base_y=260, z=0, flags=0,
             atlas_x=slots[L_BODY][0], atlas_y=0, atlas_w=slots[L_BODY][2], atlas_h=slots[L_BODY][3]),
        dict(name=L_HEAD,  parent=-1, base_x=10,  base_y=10,  z=2, flags=0,
             atlas_x=slots[L_HEAD][0], atlas_y=0, atlas_w=slots[L_HEAD][2], atlas_h=slots[L_HEAD][3]),
        dict(name=L_EYE_L, parent=1,  base_x=196, base_y=186, z=3, flags=0,
             atlas_x=slots[L_EYE_L][0], atlas_y=0, atlas_w=slots[L_EYE_L][2], atlas_h=slots[L_EYE_L][3]),
        dict(name=L_EYE_R, parent=1,  base_x=248, base_y=186, z=3, flags=0,
             atlas_x=slots[L_EYE_R][0], atlas_y=0, atlas_w=slots[L_EYE_R][2], atlas_h=slots[L_EYE_R][3]),
        dict(name=L_MOUTH, parent=1,  base_x=222, base_y=234, z=3, flags=0,
             atlas_x=slots[L_MOUTH][0], atlas_y=0, atlas_w=slots[L_MOUTH][2], atlas_h=slots[L_MOUTH][3]),
        dict(name=L_AHOGE, parent=1,  base_x=220, base_y=0,   z=4, flags=FLAG_PHYSICS,
             atlas_x=slots[L_AHOGE][0], atlas_y=0, atlas_w=slots[L_AHOGE][2], atlas_h=slots[L_AHOGE][3]),
    ]
    write_rigbin(path, W, H, layers, canvas_atlas)


# ---------------------------------------------------------------- 真实素材打包
def pack(src_dir, path):
    """从 assets/art/<char>/ 打包：base 图必需，变体图差分切层。"""
    import json

    base_p = None
    for cand in ("base.png", "Base.png", "base.jpg", "Base.jpg", "base.jpeg", "Base.jpeg"):
        p = os.path.join(src_dir, cand)
        if os.path.exists(p):
            base_p = p
            break
    if not base_p:
        sys.exit(f"[rigpack] {src_dir} 中找不到 base.png/jpg")
    cfg_p = os.path.join(src_dir, "parts.json")
    neck_ratio, max_h, white_thr = 0.42, 800, 245
    if os.path.exists(cfg_p):
        cfg = json.load(open(cfg_p, encoding="utf-8"))
        neck_ratio = cfg.get("neck_ratio", neck_ratio)
        max_h = cfg.get("max_height", max_h)
        white_thr = cfg.get("white_threshold", white_thr)

    def extract(img):
        """白底抠图 v2——数学正确消白边（R5a 重写，替代阈值/腐蚀补丁）。

        1) 洪泛：从四角填充分离"外部白底"与"内部白物"（白衬衫不被误抠）
        2) 边缘带 matting：带内像素 P = αF + (1-α)W，
           α = clip(d(P)/d_ref, 0, 1)，d = 与白的色距，d_ref 取邻域最饱和色
        3) 颜色反解 F = (P - (1-α)W)/α —— 还原真实角色色，边缘零白渍
        """
        import numpy as np
        from PIL import ImageDraw, ImageFilter

        W0, H0 = img.size
        # ---- 1. 外部背景掩码：PIL floodfill 从四角注入品红（thresh 判白） ----
        work = img.convert("RGB").copy()
        MAGENTA = (255, 0, 254)
        for seed in [(0, 0), (W0 - 1, 0), (0, H0 - 1), (W0 - 1, H0 - 1)]:
            ImageDraw.floodfill(work, seed, MAGENTA, thresh=14)
        arr = np.asarray(work, dtype=np.int16)
        bg = (arr[:, :, 0] == 255) & (arr[:, :, 1] == 0) & (arr[:, :, 2] == 254)

        # ---- 1b. 开运算清噪点孤岛（JPG 残渣；细发丝 ~10px 不受 5x5 影响） ----
        a_bin = Image.fromarray(((~bg) * 255).astype(np.uint8), "L")
        a_open = np.asarray(a_bin.filter(ImageFilter.MinFilter(5)).filter(
            ImageFilter.MaxFilter(5)), dtype=bool)
        bg = bg | (~a_open)

        # ---- 2. 距白场距离 + 边缘带 ----
        rgb = np.asarray(img.convert("RGB"), dtype=np.float32)
        d = np.sqrt(((255.0 - rgb) ** 2).sum(axis=2))       # 白=0，色越浓越大
        d_img = Image.fromarray(np.clip(d, 0, 255).astype(np.uint8), "L")
        bg_img = Image.fromarray((bg * 255).astype(np.uint8), "L")

        # 背景膨胀 2px ∧ 非背景 = 边缘带（含抗锯齿过渡像素）
        dil = np.asarray(bg_img.filter(ImageFilter.MaxFilter(5)), dtype=bool)
        band = dil & (~bg)

        # 参考饱和度：9×9 邻域内非背景像素的最大 d（= 附近"最浓"的角色色）
        d_opaque = np.where(~bg, d, 0).astype(np.uint8)
        d_ref = np.asarray(Image.fromarray(d_opaque, "L").filter(
            ImageFilter.MaxFilter(9)), dtype=np.float32)
        d_ref = np.maximum(d_ref, 48.0)                     # 防小分母

        # ---- 3. 合成 alpha ----
        alpha = np.where(bg, 0, 255).astype(np.float32)
        # 边缘带：d<20 的近白像素（JPEG 振铃形成的"背景环"）直接归零；
        # 其余按饱和度比例给 α，但下限64/255（≈0.25）——颜色反解除以近零 α 会爆炸产生黑边。
        # 内部白物（衬衫）不在边缘带，不受影响。
        a_band = np.clip((d - 20.0) / np.maximum(d_ref - 20.0, 1.0), 0.0, 1.0) * 255.0
        a_band[d < 20.0] = 0.0
        a_band[band] = np.maximum(a_band[band], 64.0)   # α 下限防黑边
        alpha[band] = a_band[band]

        # ---- 4. 边缘带颜色反解（α≥64/255 数值稳定，无黑边） ----
        a_n = alpha / 255.0
        f = (rgb - (1.0 - a_n[..., None]) * 255.0) / np.maximum(a_n[..., None], 0.25)
        f = np.clip(f, 0, 255)

        out = np.dstack([f.astype(np.uint8), alpha.astype(np.uint8)])
        return Image.fromarray(out, "RGBA")

    base = extract(Image.open(base_p))
    bbox = base.getbbox()
    base = base.crop(bbox)
    W, H = base.size

    # 降采样（atlas 内存可控，渲染按 1:1 贴 overlay）
    if H > max_h:
        scale = max_h / H
        W, H = int(W * scale), max_h
        base = base.resize((W, H), Image.LANCZOS)
    print(f"[rigpack] base={os.path.basename(base_p)} 处理后 {W}x{H}")

    neck_y = int(H * neck_ratio)
    overlap = max(4, int(H * 0.02))

    variants = {}
    for fn, name in [("eyes_closed.png", "eyes_closed"),
                     ("mouth_half.png", "mouth_half"), ("mouth_open.png", "mouth_open")]:
        p = os.path.join(src_dir, fn)
        if os.path.exists(p):
            var = extract(Image.open(p))
            vb = var.getbbox()
            if vb:
                var = var.crop(vb)      # 按自身角色包围盒对齐（消除整图平移/尺寸差）
            variants[name] = var.resize((W, H), Image.LANCZOS)

    # 差分求部件包围盒（相对全图坐标）。
    # 变体图与 base 常有 1~2px 的重采样全局差 → 先高斯模糊压掉边缘噪声，
    # 并把搜索范围限制在颈线以上（眼/嘴只会在头区）。
    def diff_bbox(var, y_limit=None):
        from PIL import ImageChops, ImageFilter
        d = ImageChops.difference(base, var).convert("L")
        if y_limit:
            d = d.crop((0, 0, W, y_limit))
        d = d.filter(ImageFilter.GaussianBlur(2.5))
        d = d.point(lambda v: 255 if v > 40 else 0)
        bb = d.getbbox()
        if bb and y_limit:
            bb = (bb[0], bb[1], bb[2], min(bb[3] + 8, H))
        return bb

    layers = []
    atlas = Image.new("RGBA", (W, H * (2 + len(variants))), (0, 0, 0, 0))
    ly = 0  # atlas 行游标

    def push(name, img, region, base_x, base_y, z, parent=-1, flags=0):
        nonlocal ly
        cx, cy, cw, ch = region
        atlas.paste(img.crop(region), (0, ly))
        layers.append(dict(name=name, parent=parent, atlas_x=0, atlas_y=ly,
                           atlas_w=cw, atlas_h=ch, base_x=base_x, base_y=base_y,
                           z=z, flags=flags))
        ly += ch

    # body: 颈线以下（含 overlap）
    push(L_BODY, base, (0, neck_y - overlap, W, H - neck_y + overlap), 0, neck_y - overlap, 0)
    # head: 颈线以上（含 overlap），底边羽化——头移动时切线渐变不露硬边
    head_img = base.crop((0, 0, W, neck_y + overlap))
    feather = head_img.load()
    fw, fh = head_img.size
    for row in range(fh - overlap, fh):
        k = (row - (fh - overlap)) / max(1, overlap)   # 0→1
        mul = int(255 * (1.0 - k))
        for col in range(fw):
            r, g, b, a = feather[col, row]
            if a:
                feather[col, row] = (r, g, b, a * mul // 255)
    push(L_HEAD, head_img, (0, 0, W, fh), 0, 0, 2)
    # 变体部件：差分掩膜 + bbox 裁剪，z 高于 head；固件在参数驱动时叠画。
    # 掩膜外的像素 alpha 强制清零——补丁只含"真变化"（眼/嘴），
    # 发丝间隙的背景残留不再随矩形补丁出现（R5c 实测教训）。
    from PIL import ImageChops, ImageFilter
    import numpy as np
    for i, (name, var) in enumerate(variants.items()):
        diff = ImageChops.difference(base, var).convert("L")
        diff = diff.filter(ImageFilter.GaussianBlur(2.0)).point(lambda v: 255 if v > 30 else 0)
        diff = diff.filter(ImageFilter.MaxFilter(9))       # 掩膜外扩 ~4px 保覆盖
        mask = np.asarray(diff, dtype=np.float32) / 255.0
        va = np.asarray(var, dtype=np.uint8).copy()
        va[:, :, 3] = (va[:, :, 3].astype(np.float32) * mask).astype(np.uint8)
        var = Image.fromarray(va, "RGBA")

        bb = diff_bbox(var, y_limit=neck_y)
        if not bb:
            print(f"[rigpack] 警告: {name} 与 base 无差异，跳过")
            continue
        if (bb[2] - bb[0]) * (bb[3] - bb[1]) > 0.5 * W * H:
            print(f"[rigpack] 警告: {name} 差分区域过大（构图不一致？），跳过")
            continue
        pad = 6
        rx0 = max(0, bb[0] - pad); ry0 = max(0, bb[1] - pad)
        rx1 = min(W, bb[2] + pad); ry1 = min(H, bb[3] + pad)
        # 变体是脸部补丁，挂到 head 层（index=1）——转头时随头动
        push(name, var, (rx0, ry0, rx1, ry1), rx0, ry0, 3 + i, parent=1)
        print(f"[rigpack] {name}: bbox=({rx0},{ry0})-({rx1},{ry1})")

    write_rigbin(path, W, H, layers, atlas)


# ---------------------------------------------------------------- 入口
def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    cmd = sys.argv[1]
    if cmd == "placeholder":
        out = sys.argv[2] if len(sys.argv) > 2 else "assets/character_01.rigbin"
        placeholder(out)
    elif cmd == "pack":
        if len(sys.argv) < 3:
            sys.exit("[rigpack] 用法: rigpack.py pack <素材目录> [输出]")
        src = sys.argv[2]
        out = sys.argv[3] if len(sys.argv) > 3 else "assets/character_01.rigbin"
        pack(src, out)
    elif cmd == "verify":
        verify_rigbin(sys.argv[2] if len(sys.argv) > 2 else "assets/character_01.rigbin")
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
