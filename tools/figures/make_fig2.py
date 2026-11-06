# -*- coding: utf-8 -*-
"""图2 台阶检测算法流程图（修版：去掉宋体缺字符的 − 与下标，放大判断菱形）。"""
import os

from PIL import Image, ImageDraw, ImageFont

OUT = os.path.dirname(os.path.abspath(__file__))
HEI = r"C:\Windows\Fonts\simhei.ttf"
SUN = r"C:\Windows\Fonts\simsun.ttc"


def font(path, size):
    try:
        return ImageFont.truetype(path, size)
    except Exception:
        return ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", size)


F = font(SUN, 21)
F_S = font(SUN, 19)

W, H = 1180, 1240
img = Image.new("RGB", (W, H), "white")
d = ImageDraw.Draw(img)
CX = 560


def proc(y1, y2, text, fill=(238, 244, 255), line=(70, 110, 190), x1=250, x2=870, fnt=None):
    fnt = fnt or F
    d.rounded_rectangle([x1, y1, x2, y2], radius=10, fill=fill, outline=line, width=2)
    lines = text.split("\n")
    hh = sum(d.textbbox((0, 0), ln, font=fnt)[3] + 7 for ln in lines)
    yy = y1 + (y2 - y1 - hh) / 2
    for ln in lines:
        bb = d.textbbox((0, 0), ln, font=fnt)
        d.text((x1 + (x2 - x1 - (bb[2] - bb[0])) / 2, yy), ln, font=fnt, fill=(25, 25, 25))
        yy += bb[3] + 7


def diamond(cx, cy, w, h, text):
    d.polygon([(cx, cy - h // 2), (cx + w // 2, cy), (cx, cy + h // 2), (cx - w // 2, cy)],
              fill=(255, 248, 228), outline=(200, 140, 40))
    d.multiline_text((cx, cy), text, font=F_S, fill=(25, 25, 25), anchor="mm", align="center", spacing=6)


def down(y1, y2, color=(70, 110, 190)):
    d.line([(CX, y1), (CX, y2 - 9)], fill=color, width=3)
    d.polygon([(CX, y2), (CX - 6, y2 - 11), (CX + 6, y2 - 11)], fill=color)


proc(20, 92, "读入一帧六轴原始值\n(gx, gy, gz, ax, ay, az)")
proc(122, 194, "量纲换算：a = LSB / 16384 (单位 g)\n陀螺：/ 114.285 (单位 °/s)")
proc(224, 306, "重力低通估计：g_new = (1-α)·g_prev + α·|a|\n动态加速度：dyn = |a| - g_new    (α = 0.03 @50 Hz)")
proc(336, 408, "对 dyn 做 100 ms 滑动平均，抑制高频抖动")
down(92, 122)
down(194, 224)
down(306, 336)
down(408, 448)

diamond(CX, 545, 620, 190,
        "f > 0.18 g  且\n已脱离迟滞（f < 0.09 g 才重新武装）\n且距上一步 >= 300 ms")

proc(660, 742, "计一级台阶：steps + 1\n刷新不应期计时，记录本步时刻", (228, 248, 234), (50, 150, 90))
proc(772, 844, "steps mod 11 = 0 → 中间平台事件\nsteps mod 22 = 0 → 整层事件（楼层 + 1）", (228, 248, 234), (50, 150, 90))
proc(874, 956, "RMS 能量 < 平台门限且持续 N 秒\n→ 判定为真停顿，输出暂停事件")
proc(986, 1078, "更新瞬时步频：取最近 8 步的时刻\nspm = (n-1) × 60 / (最新 - 最旧)")
proc(1108, 1180, "输出事件位图，等待下一帧（f < 0.09 g 时重新武装迟滞）", (245, 245, 245), (120, 120, 120))

down(408, 452, (50, 150, 90))
down(638, 660, (50, 150, 90))
d.text((CX + 14, 648), "是", font=F, fill=(50, 150, 90))

for a, b in ((742, 772), (844, 874), (956, 986), (1078, 1108)):
    down(a, b)

# 否 分支
d.line([(CX + 310, 545), (1040, 545), (1040, 1032), (872, 1032)], fill=(180, 90, 90), width=3)
d.polygon([(870, 1032), (882, 1026), (882, 1038)], fill=(180, 90, 90))
d.text((930, 515), "否（未触发）", font=F_S, fill=(180, 90, 90))

p = os.path.join(OUT, "fig2_flow.png")
img.save(p)
print("图2 ->", p, img.size)
