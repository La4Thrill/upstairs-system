#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tools/replay_ingest.py —— 把九轴数据 txt 按真实采样率回放进 /api/ingest

用途：**STM32 还没接好之前，就能把整条"设备 → 协议 → 服务端 → 数据库 → 网页"链路验证完。**
它模拟的正是 STM32 该做的事：50 Hz 采样、每 N 个样本打一个 JSON、带 X-Device-Key 上报。

只依赖 Python 3 标准库，不用装任何东西。

用法
----
  # 最基本：把上三层楼数据按 50 Hz 打到本机服务端，跑完打印汇总
  python tools/replay_ingest.py "D:\\数据文件\\数据文件\\九轴数据文件\\上三层楼数据.txt"

  # 用真实终端的设备号（这样网页上能看到一台叫 STM32-01 的"真实设备"）
  python tools/replay_ingest.py upstairs.txt --device STM32-01 --user collector1

  # 4 倍速快跑（只验证逻辑，不追求实时）
  python tools/replay_ingest.py upstairs.txt --speed 4

  # 只看不发（检查解析和分块对不对）
  python tools/replay_ingest.py upstairs.txt --dry-run

  # 循环发（压测多终端/多用户）
  python tools/replay_ingest.py upstairs.txt --loop 3

预期结果
--------
最后一行的 `climbed_floors` 应该是 3，`steps` 68 上下，`events` 里出现 3 次 floor。
如果对不上，说明协议/换算/服务端某一环有问题，**不用等硬件**。
"""

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request


def load_samples(path):
    """读 6 列 int16（gx gy gz ax ay az），跳过表头/坏行。"""
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as fp:
        for line in fp:
            parts = line.replace("\t", " ").split()
            if len(parts) < 6:
                continue
            try:
                v = [int(x) for x in parts[:6]]
            except ValueError:
                continue          # 表头或注释行
            rows.append([((x + 0x8000) & 0xFFFF) - 0x8000 for x in v])
    return rows


def post_json(url, payload, headers, timeout=10.0):
    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    req = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    for k, v in headers.items():
        if v:
            req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8", "replace"))
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        try:
            return e.code, json.loads(raw)
        except Exception:
            return e.code, {"error": raw[:200]}
    except Exception as e:                     # 连接被拒 / 超时
        return 0, {"error": str(e)}


def main():
    ap = argparse.ArgumentParser(description="把九轴数据回放进 /api/ingest")
    ap.add_argument("data", help="数据文件路径（gx gy gz ax ay az，六列整数）")
    ap.add_argument("--url", default="http://127.0.0.1:8080",
                    help="服务端地址（默认 http://127.0.0.1:8080）")
    ap.add_argument("--device", default="STM32-REPLAY", help="设备ID（默认 STM32-REPLAY）")
    ap.add_argument("--name", default="", help="设备显示名（可选）")
    ap.add_argument("--user", default="", help="绑定采集对象（可选，如 collector1）")
    ap.add_argument("--fs", type=float, default=50.0, help="采样率（默认 50）")
    ap.add_argument("--batch", type=int, default=100, help="每批样本数（默认 100 = 2 s）")
    ap.add_argument("--speed", type=float, default=1.0, help="回放倍速（默认 1.0 = 实时）")
    ap.add_argument("--loop", type=int, default=1, help="重复回放次数（默认 1）")
    ap.add_argument("--key", default="", help="设备密钥（首次上报会自动下发）")
    ap.add_argument("--dry-run", action="store_true", help="只解析不发")
    args = ap.parse_args()

    if not os.path.isfile(args.data):
        print(f"[ERR] 找不到数据文件: {args.data}")
        return 2

    rows = load_samples(args.data)
    if not rows:
        print("[ERR] 文件里没有可用的数据行")
        return 2

    duration = len(rows) / args.fs
    n_batches = (len(rows) + args.batch - 1) // args.batch
    print(f"文件      : {args.data}")
    print(f"样本      : {len(rows)}  ({duration:.1f} s @ {args.fs:g} Hz)")
    print(f"分批      : {n_batches} × {args.batch} 样本")
    print(f"目标      : {args.url}/api/ingest   设备 {args.device}")
    print(f"回放      : {args.loop} 次, {args.speed:g} 倍速")
    print("-" * 66)

    if args.dry_run:
        for i in range(0, len(rows), args.batch):
            chunk = rows[i:i + args.batch]
            if i % (args.batch * 10) == 0:
                print(f"  batch @{i:5d}: {len(chunk)} 样本, 首行 {chunk[0]}")
        print("-" * 66)
        print("dry-run 结束，未发送任何请求")
        return 0

    ingest_url = args.url.rstrip("/") + "/api/ingest"
    key = args.key
    interval = (args.batch / args.fs) / args.speed
    ok_batches = fail_batches = 0
    floor_events = 0
    last = {}

    t0 = time.time()
    for rep in range(args.loop):
        if args.loop > 1:
            print(f"=== 第 {rep + 1}/{args.loop} 轮 ===")
        for off in range(0, len(rows), args.batch):
            chunk = rows[off:off + args.batch]
            payload = {
                "device_id": args.device,
                "fs": args.fs,
                "samples": chunk,
            }
            if args.name:
                payload["name"] = args.name
            if args.user and rep == 0 and off == 0:
                payload["user"] = args.user

            status, body = post_json(ingest_url, payload, {"X-Device-Key": key})
            if status == 0:
                print(f"[ERR] 连不上服务端: {body.get('error')}")
                print("      确认 server.exe 在跑，且端口对（默认 8080）")
                return 3

            if status == 200:
                ok_batches += 1
                if body.get("provisioned") and body.get("device_key"):
                    key = body["device_key"]
                    print(f"[KEY] 首次登记，设备密钥 = {key}")
                    print("      真实 STM32 要把这个 key 存进 Flash，之后每次都带上")
                evs = body.get("events") or []
                floor_events += evs.count("floor")
                if evs:
                    print(f"  [{off / args.fs:6.1f}s] events={evs} "
                          f"floor={body.get('current_floor')} "
                          f"steps={body.get('steps')} "
                          f"speed={body.get('speed')}")
                last = body
            elif status == 403:
                print(f"[ERR] 403 {body.get('error')} —— 设备已登记但密钥不对。")
                print("      删掉 stair.db 里的该设备，或换个 --device 名字重来。")
                return 4
            else:
                fail_batches += 1
                print(f"[ERR] HTTP {status} {body}")

            # 按实时节奏发（--speed 1 时整段约等于 duration 秒）
            if args.speed > 0 and interval > 0:
                target = t0 + (rep * len(rows) + off) / args.fs / args.speed
                wait = target - time.time()
                if wait > 0:
                    time.sleep(wait)

    elapsed = time.time() - t0
    print("-" * 66)
    print(f"完成      : {ok_batches} 批成功, {fail_batches} 批失败, 用时 {elapsed:.1f} s")
    print(f"floor 事件: {floor_events} 次")
    if last:
        print(f"最终状态  : 已爬 {last.get('climbed_floors')} 层 / 当前 {last.get('current_floor')} 层"
              f" / {last.get('steps')} 级台阶 / 步频 {last.get('speed')}")
        print(f"            half_flights={last.get('half_flights')}")
    print()
    print("期望：上三层楼数据应得到 已爬 3 层 / 68 级台阶 / floor 事件 3 次。")
    print("      对不上就说明协议或换算有问题，先在软件侧修，别带去硬件阶段。")
    return 0 if fail_batches == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
