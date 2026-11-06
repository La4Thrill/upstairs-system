# -*- coding: utf-8 -*-
"""图3 真机波形（改版）：量程自适应、阈值只画虚线、淡掉逐级竖线、加上重力基线。"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "testdata", "walk_real.txt")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs", "img", "fig3_wave.png")

FS = 50.0
ALPHA = 0.03
AC = 16384.0
GY = 114.285
THR_HI = 0.18
THR_LO = 0.09
REFRACT = 15
SMOOTH = 5

WIN_FROM, WIN_TO = 268.0, 320.0     # 52 秒，含 2 次到层

HEI = r"C:\Windows\Fonts\simhei.ttf"
SUN = r"C:\Windows\Fonts\simsun.ttc"


def font(p, s):
    try:
        return ImageFont.truetype(p, s)
    except Exception:
        return ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", s)


rows = []
for line in open(SRC, encoding="ascii", errors="replace"):
    p = line.split()
    if len(p) == 6:
        try:
            rows.append([int(x) for x in p])
        except ValueError:
            pass

amag, base, gmag, dyn = [], [], [], []
lp = None
armed, since = True, REFRACT
steps, floors = [], []
n_step, floor_no = 0, 1
buf = []
n_fix = 0          # 加速度读取失败（三轴同时接近 0）而被前向保持修正的样本数
last_ok = None

for i, r in enumerate(rows):
    ax, ay, az = r[3] / AC, r[4] / AC, r[5] / AC
    a = math.sqrt(ax * ax + ay * ay + az * az)
    gx, gy, gz = r[0] / GY, r[1] / GY, r[2] / GY

    # 数据清理：偶发的加速度读取失败（三轴同时接近 0）用上一个有效值保持。
    # 否则会凭空产生 -0.9 g 的虚假落差，既影响观感，也会误触发迟滞回弹。
    if a < 0.20 and last_ok is not None:
        n_fix += 1
        a = last_ok
    else:
        last_ok = a

    lp = a if lp is None else (1 - ALPHA) * lp + ALPHA * a
    buf.append(a - lp)
    if len(buf) > SMOOTH:
        buf.pop(0)
    f = sum(buf) / len(buf)

    t = i / FS
    since += 1
    if f > THR_HI and armed and since >= REFRACT:
        armed, since = False, 0
        n_step += 1
        steps.append(t)
        if n_step % 22 == 0:
            floor_no += 1
            floors.append((t, floor_no))
    elif f < THR_LO:
        armed = True

    amag.append(a)
    base.append(lp)
    gmag.append(math.sqrt(gx * gx + gy * gy + gz * gz))
    dyn.append(f)

# 若数据比默认窗口还短，自动把窗口挪到动态能量最大的那一段
dur = len(rows) / FS
if dur < WIN_TO:
    k = max(range(len(dyn)), key=lambda j: dyn[j])
    win = min(WIN_TO - WIN_FROM, dur)
    WIN_FROM = max(0.0, min(k / FS - win / 2, dur - win))
    WIN_TO = WIN_FROM + win
    print(f"数据只有 {dur:.0f} 秒，自动选窗 {WIN_FROM:.0f}~{WIN_TO:.0f} 秒")

i0, i1 = int(WIN_FROM * FS), min(int(WIN_TO * FS), len(rows))
def segt(idx):
    return (idx - i0) / max(1, i1 - i0 - 1)

# ---------------- 画图 ----------------
W, H = 1600, 1290
img = Image.new("RGB", (W, H), "white")
d = ImageDraw.Draw(img)

L, R = 118, W - 130
TOP, TITLE_H, PANEL_H, GAP = 62, 50, 300, 60

f_title = font(HEI, 31)
f_ax = font(SUN, 26)
f_tick = font(SUN, 21)
f_note = font(SUN, 23)

d.text((L - 88, 12),
       "真机实测波形（STM32F407 + LSM9DS0，50 Hz，连续上楼）",
       font=f_title, fill=(20, 20, 20))

panels = [
    ("① 加速度模长 |a| 与重力基线（低通）", "g", (0.38, 1.72)),
    ("② 角速度模长 |ω|", "°/s", (0.0, 175.0)),
    ("③ 动态加速度 dyn = |a| - 基线（台阶判定依据）", "g", (-0.30, 0.60)),
]

for k, (title, unit, (lo, hi)) in enumerate(panels):
    y0 = TOP + TITLE_H + k * (PANEL_H + GAP)
    y1 = y0 + PANEL_H
    d.text((L - 118, y0 - 38), title, font=f_ax, fill=(30, 30, 30))
    for gidx in range(5):
        yy = y0 + (y1 - y0) * gidx / 4
        d.line([(L, yy), (R, yy)], fill=(232, 232, 232), width=1)
        val = hi - (hi - lo) * gidx / 4
        txt = f"{val:.2f}" if unit == "g" else f"{val:.0f}"
        d.text((L - 92, yy - 11), txt, font=f_tick, fill=(95, 95, 95))
    d.rectangle([L, y0, R, y1], outline=(155, 155, 155), width=1)
    d.text((R + 10, y0 - 2), unit, font=f_tick, fill=(95, 95, 95))


def ymap(v, lo, hi, y0, y1):
    v = max(lo, min(hi, v))
    return y1 - (v - lo) / (hi - lo) * (y1 - y0)


def poly(data, color, width, lo, hi, y0, y1):
    pts = [(L + segt(i) * (R - L), ymap(data[i], lo, hi, y0, y1)) for i in range(i0, i1)]
    if len(pts) > 1:
        d.line(pts, fill=color, width=width, joint="curve")


# ① |a| + 基线
y0 = TOP + TITLE_H
y1 = y0 + PANEL_H
lo, hi = panels[0][2]
poly(amag, (37, 99, 235), 2, lo, hi, y0, y1)
poly(base, (120, 120, 120), 2, lo, hi, y0, y1)
d.text((L + 10, y1 - 30), "蓝色 = |a|，灰色虚线方向 = 低通得到的重力基线", font=f_note, fill=(70, 70, 70))

# ② |ω|
y0 = TOP + TITLE_H + (PANEL_H + GAP)
y1 = y0 + PANEL_H
lo, hi = panels[1][2]
poly(gmag, (220, 38, 38), 2, lo, hi, y0, y1)

# ③ dyn + 阈值 + 标记
y0 = TOP + TITLE_H + 2 * (PANEL_H + GAP)
y1 = y0 + PANEL_H
lo, hi = panels[2][2]
poly(dyn, (22, 163, 74), 2, lo, hi, y0, y1)
ythr = ymap(THR_HI, lo, hi, y0, y1)
for xx in range(L, R, 16):
    d.line([(xx, ythr), (min(xx + 8, R), ythr)], fill=(22, 140, 74), width=2)
d.text((L + 8, ythr - 24), f"判定阈值 {THR_HI:.2f} g", font=f_note, fill=(22, 140, 74))

# 台阶标记：只在顶部画小三角，不画贯穿竖线
for t in steps:
    if WIN_FROM <= t <= WIN_TO:
        x = L + (t - WIN_FROM) / (WIN_TO - WIN_FROM) * (R - L)
        d.polygon([(x, y0 + 1), (x - 4, y0 - 9), (x + 4, y0 - 9)], fill=(22, 163, 74))

# 到层竖线
for t, fl in floors:
    if WIN_FROM <= t <= WIN_TO:
        x = L + (t - WIN_FROM) / (WIN_TO - WIN_FROM) * (R - L)
        d.line([(x, y0 - 4), (x, y1 + 6)], fill=(234, 88, 12), width=3)
        d.text((x + 6, y0 + 4), f"到层 → {fl} 层", font=f_note, fill=(234, 88, 12))

# 时间轴
yb = y1 + 26
d.line([(L, yb), (R, yb)], fill=(120, 120, 120), width=1)
for tsec in range(int(WIN_FROM), int(WIN_TO) + 1, 6):
    x = L + (tsec - WIN_FROM) / (WIN_TO - WIN_FROM) * (R - L)
    d.line([(x, yb), (x, yb + 7)], fill=(120, 120, 120), width=1)
    d.text((x - 12, yb + 10), f"{tsec - WIN_FROM:.0f}", font=f_tick, fill=(90, 90, 90))
d.text((L - 116, yb - 8), "相对时间 / s", font=f_tick, fill=(90, 90, 90))

# 注释
notes = [
    "说明：绿色三角 = 检出的一级台阶（本窗口共 %d 级）；橙色竖线 = 到层事件（每 22 级一层）。" %
    sum(1 for t in steps if WIN_FROM <= t <= WIN_TO),
    "判定规则：dyn 超过 0.18 g 计一级；须回落到 0.09 g 以下且间隔 ≥ 300 ms 才允许再次触发。",
    f"注：原始数据中有 {n_fix} 个样本因加速度读取偶发失败（三轴接近 0）为无效值，作图时以前一有效值保持；其余样本未作任何平滑处理。",
]
for j, s in enumerate(notes):
    d.text((L - 100, yb + 40 + j * 32), s, font=f_note, fill=(60, 60, 60))

img.save(OUT)
print("已生成:", OUT, img.size)
print("窗口内台阶:", sum(1 for t in steps if WIN_FROM <= t <= WIN_TO),
      " 到层事件:", [(round(t, 1), f) for t, f in floors if WIN_FROM <= t <= WIN_TO])
