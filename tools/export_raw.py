# -*- coding: utf-8 -*-
"""从服务端导出最近 N 个样本的原始数据（默认 1 分钟）。

用途：验收要求提交「一分钟的测试视频 + 对应的传感器原始数据」，
而网页按钮导出的是固定 20000 行（400 秒），不便于和视频一一对应。
本脚本按样本数精确导出：50 Hz × 60 s = 3000 行。

用法:
    python tools/export_raw.py [--device STM32-01] [--seconds 60]
                              [--url http://127.0.0.1:8080] [--out 目录]
"""
import argparse
import datetime
import json
import os
import sys
import urllib.request

FS = 50.0


def post_json(url, payload):
    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read().decode("utf-8", "replace"))


def get_text(url, token):
    req = urllib.request.Request(url, method="GET")
    req.add_header("X-Auth-Token", token)
    with urllib.request.urlopen(req, timeout=30) as resp:
        return resp.read().decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", default="STM32-01")
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="admin123")
    ap.add_argument("--url", default="http://127.0.0.1:8080")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    samples = int(round(args.seconds * FS))
    base = args.url.rstrip("/")

    print(f"[1/3] 登录 {base} ...")
    try:
        r = post_json(base + "/api/login", {"username": args.user, "password": args.password})
    except Exception as e:
        print(f"[ERR] 连不上服务端：{e}")
        print("      先确认 server.exe 在跑，且网页能打开")
        return 3
    token = r.get("token")
    if not token:
        print("[ERR] 登录失败:", r)
        return 3

    print(f"[2/3] 导出 {args.device} 最近 {samples} 个样本（≈{args.seconds:g} 秒）...")
    url = f"{base}/api/export/raw?device_id={args.device}&limit={samples}"
    try:
        txt = get_text(url, token)
    except Exception as e:
        print("[ERR] 导出失败:", e)
        return 4

    lines = [ln for ln in txt.splitlines() if ln.strip()]
    if not lines:
        print("[ERR] 这台设备还没有原始数据")
        return 5

    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    outdir = args.out or os.getcwd()
    name = f"{args.device}_raw_{stamp}_{len(lines)}samples.txt"
    path = os.path.join(outdir, name)
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines) + "\n")

    print(f"[3/3] 已保存: {path}")
    print(f"      行数 {len(lines)}  时长 {len(lines)/FS:.1f} 秒 @50Hz")
    print(f"      校验：algo_check {os.path.basename(path)} 1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
