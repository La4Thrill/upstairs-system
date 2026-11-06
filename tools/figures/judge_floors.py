# -*- coding: utf-8 -*-
"""判定 E:\\STM32-01_raw_20261105_142752.txt 爬了几层楼。

给出四个口径的结果，互相印证：
  ① 原样复算（gait_algo 原封不动）
  ② 剔除开头未上楼时的零碎台阶（从第一个 HALF 事件起算）
  ③ 剔除 1.96% 的加速度无效样本（前向保持）后复算
  ④ 阈值敏感性（0.18 / 0.20 / 0.22 / 0.24）
"""
import math
import os
import sys

FS = 50.0
ALPHA = 0.03
AC = 16384.0
THR_HI = 0.18
THR_LO = 0.09
REFRACT = 15
SMOOTH = 5
PER_FLOOR = 22
PER_HALF = 11

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "testdata", "walk_real.txt")


def load(path):
    rows = []
    for line in open(path, encoding="ascii", errors="replace"):
        p = line.split()
        if len(p) == 6:
            try:
                rows.append([int(x) for x in p])
            except ValueError:
                pass
    return rows


def run(rows, thr_hi=THR_HI, hold_bad=False, thr_lo_ratio=0.5):
    thr_lo = thr_hi * thr_lo_ratio
    lp = None
    armed, since = True, REFRACT
    buf = []
    steps_t, floor_t = [], []
    n_step, floor_no = 0, 1
    last_ok = None
    n_fix = 0

    for i, r in enumerate(rows):
        ax, ay, az = r[3] / AC, r[4] / AC, r[5] / AC
        a = math.sqrt(ax * ax + ay * ay + az * az)
        if hold_bad and a < 0.20 and last_ok is not None:
            a = last_ok
            n_fix += 1
        else:
            last_ok = a

        lp = a if lp is None else (1 - ALPHA) * lp + ALPHA * a
        buf.append(a - lp)
        if len(buf) > SMOOTH:
            buf.pop(0)
        f = sum(buf) / len(buf)

        t = i / FS
        since += 1
        if f > thr_hi and armed and since >= REFRACT:
            armed, since = False, 0
            n_step += 1
            steps_t.append(t)
            if n_step % PER_FLOOR == 0:
                floor_no += 1
                floor_t.append((t, floor_no, n_step))
        elif f < thr_lo:
            armed = True

    return steps_t, floor_t, n_fix


rows = load(SRC)
print(f"文件 {SRC}")
print(f"样本 {len(rows)}  时长 {len(rows)/FS:.1f} 秒\n")

# ---- ① 原样
st, fl, _ = run(rows)
print("① 原样复算")
print(f"   台阶 {len(st)} 级   判出楼层 {len(fl)} 层（当前 {1+len(fl)} 层）")

# ---- ② 从第一次 HALF 起算（排除开头零碎动作）
half_idx = []
for k in range(1, len(st) + 1):
    if k % PER_HALF == 0:
        half_idx.append(k)
t_half1 = st[PER_HALF - 1] if len(st) >= PER_HALF else None
print("\n② 只算真正爬楼的段（从第 11 级 HALF 事件起）")
if t_half1:
    real_steps = [t for t in st if t >= st[0] and t <= st[-1]]
    steps_from_half = len([t for t in st if t >= st[PER_HALF - 2]])
    print(f"   第一个 HALF 在 {t_half1:.1f}s；此后台阶 {steps_from_half} 级"
          f" → {steps_from_half/PER_FLOOR:.2f} 层")
else:
    print("   台阶不足 11 级")

# ---- ③ 剔除无效样本
st3, fl3, nfix = run(rows, hold_bad=True)
print("\n③ 剔除加速度无效样本后复算")
print(f"   修正 {nfix} 个无效样本（{nfix/len(rows)*100:.2f}%）")
print(f"   台阶 {len(st3)} 级   判出楼层 {len(fl3)} 层（当前 {1+len(fl3)} 层）")

# ---- ④ 阈值敏感性
print("\n④ 阈值敏感性（原样数据）")
for thr in (0.24, 0.22, 0.20, 0.18, 0.16, 0.14):
    s, f_, _ = run(rows, thr_hi=thr)
    print(f"   阈值 {thr:.2f} g → 台阶 {len(s):3d} 级  判出 {len(f_)} 层")

# ---- 到层明细
print("\n⑤ 到层明细（原样）")
prev_t, prev_s = 0.0, 0
for t, floor_no, ns in fl:
    print(f"   {t:6.1f}s  台阶 {ns:3d}  → 楼层 {floor_no}"
          f"   本层 {ns-prev_s:2d} 级 / {t-prev_t:5.1f}s = {(ns-prev_s)*60/max(0.1,t-prev_t):3.0f} 级/分")
    prev_t, prev_s = t, ns

print("\n⑥ 结论")
print(f"   原样：{len(st)} 级台阶 ÷ {PER_FLOOR} 级/层 = {len(st)/PER_FLOOR:.2f} 层 → 算法判 {len(fl)} 层")
print(f"   剔除无效样本后：{len(st3)} 级 ÷ {PER_FLOOR} = {len(st3)/PER_FLOOR:.2f} 层 → 算法判 {len(fl3)} 层")
