# -*- coding: utf-8 -*-
"""图1 系统总体架构（修版：层标题独占一行，避免与方框重叠）。"""
import math
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


F_LABEL = font(HEI, 26)
F_BOX = font(SUN, 22)

W = 1520
LAYER_H = 176
GAP = 16
TOP = 20
H = TOP + 4 * (LAYER_H + GAP)

img = Image.new("RGB", (W, H), "white")
d = ImageDraw.Draw(img)

LAYERS = [
    ("感知层", (228, 240, 255), (60, 120, 200), [
        "LSM9DS0 九轴传感器\n加速度 ±2 g / 陀螺 ±245 °/s\n16 位 ADC，I2C 接口",
        "STM32F407ZGT6\n168 MHz，I2C 走 PB6 / PB7\nTIM3 提供 50 Hz 采样节拍",
        "板载资源\n4.3 英寸 TFT、按键、蜂鸣器 PF8\nLED0 / LED1、板载 ST-Link 与 CH340",
    ]),
    ("端侧处理层", (228, 248, 234), (50, 150, 90), [
        "量纲换算\nint16 原始值 ÷ 16384 得 g\n陀螺 ÷ 114.285 得 °/s",
        "gait_algo 端侧算法\n重力低通 → 动态加速度\n迟滞判峰 → 台阶 / 楼层计数",
        "实时反馈\n每级台阶蜂鸣一次\n到层翻灯，串口输出六轴",
    ]),
    ("传输层", (255, 245, 224), (205, 145, 40), [
        "USART1 与 CH340\n115200 bps，8N1\n板载 USB 转串口",
        "串口桥 serial_ingest.py\n按 100 个样本分批\nPOST /api/ingest",
        "HTTP 接口\nX-Device-Key 设备鉴权\nJSON 报文，断线自动重连",
    ]),
    ("平台与应用层", (244, 233, 250), (140, 90, 190), [
        "服务端 server.exe（C 语言）\n算法复算 / 事件生成\n多设备并发与角色鉴权",
        "SQLite 数据库（7 张表）\nraw_samples / floor_events\nprogress_snapshots 等",
        "Web 前端与原始数据导出\n实时波形、楼层看板\n原始数据可离线复算",
    ]),
]

for i, (label, fill, line, boxes) in enumerate(LAYERS):
    y = TOP + i * (LAYER_H + GAP)
    d.rounded_rectangle([24, y, W - 24, y + LAYER_H], radius=12, fill=fill, outline=line, width=2)
    d.text((44, y + 6), label, font=F_LABEL, fill=line)

    by1, by2 = y + 46, y + LAYER_H - 14
    bw = (W - 48 - 2 * 44 - 2 * 40) // 3
    xs = [56, 56 + bw + 40, 56 + 2 * (bw + 40)]
    for x, text in zip(xs, boxes):
        d.rounded_rectangle([x, by1, x + bw, by2], radius=10, fill="white", outline=line, width=2)
        lines = text.split("\n")
        hh = sum(d.textbbox((0, 0), ln, font=F_BOX)[3] + 6 for ln in lines)
        yy = by1 + (by2 - by1 - hh) / 2
        for ln in lines:
            bb = d.textbbox((0, 0), ln, font=F_BOX)
            d.text((x + (bw - (bb[2] - bb[0])) / 2, yy), ln, font=F_BOX, fill=(25, 25, 25))
            yy += bb[3] + 6

    # 行内箭头
    for k in range(2):
        ax1 = xs[k] + bw
        ax2 = xs[k + 1]
        ay = (by1 + by2) // 2
        d.line([(ax1 + 6, ay), (ax2 - 8, ay)], fill=(110, 110, 110), width=3)
        d.polygon([(ax2 - 8, ay), (ax2 - 20, ay - 6), (ax2 - 20, ay + 6)], fill=(110, 110, 110))

    # 层间箭头
    if i < 3:
        cy = y + LAYER_H
        d.line([(W // 2, cy + 1), (W // 2, cy + GAP - 2)], fill=(110, 110, 110), width=3)
        d.polygon([(W // 2, cy + GAP - 1), (W // 2 - 6, cy + GAP - 11), (W // 2 + 6, cy + GAP - 11)],
                  fill=(110, 110, 110))

p = os.path.join(OUT, "fig1_arch.png")
img.save(p)
print("图1 ->", p, img.size)
