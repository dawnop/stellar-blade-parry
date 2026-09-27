# 生成程序图标 res/sbparry.ico（原创图案：斜向的剑 + 四芒星 + 弹反的偏折弧光）
# Generates res/sbparry.ico: an original mark — a slanted blade, a four-point star and a parry deflection arc.
# 用法 / usage:  python make_icon.py        （需要 Pillow）
import math
import os
from PIL import Image, ImageDraw, ImageFilter

S = 1024  # 高分辨率绘制，再缩小到各尺寸
HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "..", "..", "res")

BG_TOP, BG_BOT = (22, 28, 44), (6, 8, 14)
BLADE = (236, 242, 255)
GOLD = (255, 204, 92)
CYAN = (110, 220, 255)


def rotate(pts, deg, cx, cy):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    return [(cx + (x - cx) * c - (y - cy) * s, cy + (x - cx) * s + (y - cy) * c) for x, y in pts]


def star(cx, cy, r_long, r_short, rot=0):
    pts = []
    for i in range(8):
        r = r_long if i % 2 == 0 else r_short
        a = math.radians(rot + i * 45 - 90)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def glow(size, draw_fn, radius, alpha):
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(layer))
    layer = layer.filter(ImageFilter.GaussianBlur(radius))
    if alpha < 1:
        a = layer.getchannel("A").point(lambda v: int(v * alpha))
        layer.putalpha(a)
    return layer


def make(size=S):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    k = size / 1024

    # 圆角方形底，竖向渐变
    bg = Image.new("RGBA", (size, size))
    for y in range(size):
        t = y / (size - 1)
        col = tuple(int(BG_TOP[i] * (1 - t) + BG_BOT[i] * t) for i in range(3)) + (255,)
        ImageDraw.Draw(bg).line([(0, y), (size, y)], fill=col)
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, size - 1, size - 1], radius=int(210 * k), fill=255)
    img.paste(bg, (0, 0), mask)

    cx, cy = size * 0.5, size * 0.5

    # 弹反的偏折弧光：绕中心的一道弧，从细到粗汇向星芒（像刀光被弹开的轨迹）
    R = 330 * k
    def swoosh(d, scale, col):
        n = 60
        for i in range(n):
            t = i / (n - 1)
            a0 = math.radians(205 + 110 * t)
            a1 = math.radians(205 + 110 * min(1, t + 1.5 / n))
            w = (6 + 44 * t ** 1.4) * k * scale
            d.line([(cx + R * math.cos(a0), cy + R * math.sin(a0)), (cx + R * math.cos(a1), cy + R * math.sin(a1))],
                   fill=col, width=max(1, int(w)))
    img.alpha_composite(glow(size, lambda d: swoosh(d, 1.8, GOLD + (255,)), 36 * k, 0.55))
    swoosh(ImageDraw.Draw(img), 1.0, GOLD + (255,))

    # 剑：从左下到右上的细长刀身 + 护手 + 柄（先竖着画再旋转 45°）
    L, W = 720 * k, 58 * k
    blade = [(cx, cy - L * 0.55), (cx + W / 2, cy - L * 0.55 + W * 1.4), (cx + W / 2, cy + L * 0.28),
             (cx - W / 2, cy + L * 0.28), (cx - W / 2, cy - L * 0.55 + W * 1.4)]
    guard = [(cx - 150 * k, cy + L * 0.28), (cx + 150 * k, cy + L * 0.28), (cx + 150 * k, cy + L * 0.28 + 34 * k),
             (cx - 150 * k, cy + L * 0.28 + 34 * k)]
    grip = [(cx - 24 * k, cy + L * 0.28 + 34 * k), (cx + 24 * k, cy + L * 0.28 + 34 * k), (cx + 24 * k, cy + L * 0.45),
            (cx - 24 * k, cy + L * 0.45)]
    blade, guard, grip = (rotate(p, 45, cx, cy) for p in (blade, guard, grip))
    img.alpha_composite(glow(size, lambda d: d.polygon(blade, fill=CYAN + (255,)), 30 * k, 0.8))
    d = ImageDraw.Draw(img)
    d.polygon(blade, fill=BLADE + (255,))
    # 刀身中线（血槽）
    ridge = rotate([(cx, cy - L * 0.50), (cx, cy + L * 0.26)], 45, cx, cy)
    d.line(ridge, fill=(170, 190, 225, 255), width=max(1, int(8 * k)))
    d.polygon(guard, fill=GOLD + (255,))
    d.polygon(grip, fill=(150, 160, 185, 255))

    # 四芒星：压在剑与弧的交点上（弹反的火花），外发光
    sx, sy = cx + 150 * k, cy - 150 * k
    img.alpha_composite(glow(size, lambda d: d.polygon(star(sx, sy, 250 * k, 60 * k), fill=(255, 255, 255, 255)), 45 * k, 0.9))
    d = ImageDraw.Draw(img)
    d.polygon(star(sx, sy, 230 * k, 46 * k), fill=(255, 255, 255, 255))
    d.polygon(star(sx, sy, 120 * k, 26 * k, rot=45), fill=(255, 240, 200, 255))
    return img


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    big = make()
    sizes = [256, 128, 64, 48, 32, 24, 16]
    big.resize((256, 256), Image.LANCZOS).save(os.path.join(OUT_DIR, "sbparry.png"))
    # 小尺寸预览（托盘 / 任务栏）
    prev = Image.new("RGBA", (256 + 64 + 32 + 16 + 40, 256), (40, 40, 40, 255))
    x = 0
    for s in (256, 64, 32, 16):
        prev.paste(big.resize((s, s), Image.LANCZOS), (x, 0))
        x += s + 10
    prev.save(os.path.join(HERE, "preview.png"))
    big.resize((256, 256), Image.LANCZOS).save(os.path.join(OUT_DIR, "sbparry.ico"), sizes=[(s, s) for s in sizes])
    print("wrote", os.path.normpath(os.path.join(OUT_DIR, "sbparry.ico")))


if __name__ == "__main__":
    main()
