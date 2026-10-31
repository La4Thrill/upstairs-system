#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tools/serial_ingest.py —— 串口 → /api/ingest 桥接

作用：**不用 WiFi 模块、不用改一行服务端代码**，就能让 STM32 的数据进到系统里。

    STM32 ──串口(115200)──► 本脚本 ──HTTP POST──► server.exe /api/ingest ──► 网页 + SQLite

这正是硬件阶段第一天最省事的联调方式：
  · 端侧只需要 printf 六个数（gx gy gz ax ay az），不用自己拼 JSON、不用连网；
  · 服务端的算法、数据库、网页全都用得上，和"真实设备"走的是同一个 /api/ingest。

依赖：
    pip install pyserial

用法：
    # 真实串口（每行 6 个整数，最好带个可选的帧头，脚本会自动找数字）
    python tools/serial_ingest.py --port COM3 --baud 115200 --device STM32-01

    # 端侧如果用 ASCII 帧，例如 "$GW,1234,-56,..." 也能work：脚本只取一行里的整数
    python tools/serial_ingest.py --port COM3 --fields 6

    # 不接板子也能测：- 表示从标准输入读
    Get-Content 上三层楼数据.txt | python tools/serial_ingest.py --port - --speed 0

    # 只看解析结果，不发
    python tools/serial_ingest.py --port COM3 --dry-run
"""

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

try:
    import serial
except ImportError:
    serial = None

INT_RE = re.compile(r"-?\d+")


def wrap16(v):
    """把任意整数折成有符号 16 位（端侧 printf %d 出来可能是无符号的 65533）。"""
    v &= 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v


# ---------------------------------------------------------------- 设备密钥持久化
# 服务端只在"首次登记"和"密钥丢失"时下发 key，之后必须自己带上来。
# 这里把 key 存到脚本旁边的 .device_keys.json，下次启动自动读取，
# 免得每次都要手工 --key（也避免忘了 key 导致 403 断采）。
KEY_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".device_keys.json")

# 服务端重启时的重连策略：每 0.5s 试一次，最多 60 次（约 30 秒）
RETRY_TIMES = 60
RETRY_INTERVAL = 0.5


def load_saved_key(device_id):
    try:
        with open(KEY_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
        return data.get(device_id, "")
    except Exception:
        return ""


def save_key(device_id, key):
    try:
        data = {}
        if os.path.exists(KEY_FILE):
            with open(KEY_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)
        data[device_id] = key
        with open(KEY_FILE, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
    except Exception as e:
        print(f"[WARN] 保存设备密钥失败：{e}")


def parse_line(line, fields):
    """从一行里取前 `fields` 个整数；不够就返回 None。

    注意：从串口读出来的是 bytes，从 stdin 读出来的是 str，这里统一成 str，
    否则 re 会报 "cannot use a string pattern on a bytes-like object"。
    """
    if isinstance(line, (bytes, bytearray)):
        line = line.decode("ascii", "replace")
    nums = INT_RE.findall(line)
    if len(nums) < fields:
        return None
    try:
        return [wrap16(int(x)) for x in nums[:fields]]
    except ValueError:
        return None


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
    except Exception as e:
        return 0, {"error": str(e)}


def open_source(port, baud):
    if port == "-":
        return sys.stdin, None
    try:
        import serial  # pyserial
    except ImportError:
        print("[ERR] 需要 pyserial：pip install pyserial")
        print("      （不想装的话加 --port - 从标准输入读，可以先把串口数据重定向过来）")
        sys.exit(2)
    # 显式关闭 DTR/RTS 再打开：
    # 板子的 CH340 接了一键下载电路（DTR→RESET，RTS→BOOT0），
    # serial.Serial(port, baud) 在 Windows 上会 assert DTR，
    # 可能把 MCU 按在复位里 —— 端口能开、但永远读不到数据。
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 1
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser, ser


def main():
    ap = argparse.ArgumentParser(description="串口 -> /api/ingest 桥接")
    ap.add_argument("--port", default="COM3", help="串口号；'-' 表示从标准输入读")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--url", default="http://127.0.0.1:8080")
    ap.add_argument("--device", default="STM32-01")
    ap.add_argument("--name", default="真实终端")
    ap.add_argument("--user", default="")
    ap.add_argument("--fs", type=float, default=50.0, help="端侧实际采样率（必须填真值）")
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--fields", type=int, default=6, help="每行取几个整数（默认 6：gx gy gz ax ay az）")
    ap.add_argument("--key", default="", help="设备密钥（首次上报会自动下发）")
    ap.add_argument("--realtime", action="store_true",
                    help="按 fs 实时发送（默认攒满一批就发，适合高速采集）")
    ap.add_argument("--speed", type=float, default=0.0, help="配合 --realtime 的倍速")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    src, ser = open_source(args.port, args.baud)
    ingest = args.url.rstrip("/") + "/api/ingest"
    key = args.key or load_saved_key(args.device)
    if not args.key and key:
        print(f"[KEY] 使用上次保存的密钥：{key[:6]}…")
    batch = []
    ok = fail = 0
    last = {}
    lines = 0
    skipped = 0
    t0 = time.time()

    print(f"来源    : {args.port}" + (f" @ {args.baud}" if args.port != '-' else " (stdin)"))
    print(f"目标    : {ingest}   设备 {args.device}   fs={args.fs:g}")
    print(f"每行取  : 前 {args.fields} 个整数" + ("   [dry-run]" if args.dry_run else ""))
    print("-" * 62)

    def flush():
        nonlocal ok, fail, last, key
        if not batch:
            return
        payload = {"device_id": args.device, "fs": args.fs,
                   "samples": [list(s) for s in batch]}
        if args.name and not key:
            payload["name"] = args.name
        if args.user and not key:
            payload["user"] = args.user
        # 服务端可能正在重启：重试一段时间，别让整个采集直接挂掉。
        # （以前这里是 raise SystemExit(3)，服务端崩一次桥就死了，必须人工重开。）
        status, body = 0, {}
        for attempt in range(1, RETRY_TIMES + 1):
            status, body = post_json(ingest, payload, {"X-Device-Key": key})
            if status != 0:
                if attempt > 1:
                    print(f"[OK] 服务端恢复（重试 {attempt - 1} 次后连上）")
                break
            if attempt == 1:
                print("[WAIT] 服务端暂时连不上，正在重试……")
            time.sleep(RETRY_INTERVAL)
        if status == 0:
            fail += 1
            print(f"[ERR] 服务端一直连不上，丢弃本批 {len(batch)} 个样本（继续采集）")
            batch.clear()
            return
        if status == 200:
            ok += 1
            if body.get("provisioned") and body.get("device_key"):
                key = body["device_key"]
                save_key(args.device, key)
                print(f"[KEY] 设备已登记，key = {key}")
                print(f"      （已保存到 {KEY_FILE}，以后不用再带 --key）")
            evs = body.get("events") or []
            if evs:
                print(f"  events={evs} floor={body.get('current_floor')} "
                      f"steps={body.get('steps')} climbed={body.get('climbed_floors')}")
            last = body
        else:
            fail += 1
            print(f"[ERR] HTTP {status} {body}")
        batch.clear()

    try:
        idle = 0
        while True:
            try:
                raw = src.readline()
            except Exception as e:
                # 串口被拔掉 / 驱动报错：关掉重连，不要退出
                if ser is None:
                    raise
                print(f"[WARN] 串口读失败：{e}，2 秒后重连……")
                try:
                    ser.close()
                except Exception:
                    pass
                time.sleep(2.0)
                try:
                    ser = serial.Serial()
                    ser.port = args.port
                    ser.baudrate = args.baud
                    ser.timeout = 1
                    ser.dtr = False
                    ser.rts = False
                    ser.open()
                    src = ser
                    idle = 0
                    print("[OK] 串口已重连")
                except Exception as e2:
                    print(f"[ERR] 重连失败：{e2}（2 秒后继续重试）")
                    time.sleep(2.0)
                continue

            if not raw:
                # 关键：readline() 超时返回 b''，这不是 EOF！
                # 以前用 for 迭代 Serial 会把它当 EOF 直接退出整个采集（踩过）。
                if ser is None:
                    break          # stdin 读完，正常退出
                idle += 1
                if idle % 30 == 0:
                    print(f"[WAIT] 串口已经 {idle} 秒没有数据，继续等待……")
                continue
            idle = 0
            lines += 1
            s = parse_line(raw, args.fields)
            if s is None:
                skipped += 1
                continue
            if args.dry_run:
                if len(batch) < 3:
                    print("  ", s)
                batch.append(s)
                if len(batch) >= args.batch:
                    print(f"  ... 第 {lines} 行，累计一批 {len(batch)} 个样本（dry-run 不发送）")
                    batch.clear()
                continue

            batch.append(s)
            if len(batch) >= args.batch:
                flush()
                if args.realtime and args.speed > 0:
                    target = t0 + (lines / args.fs) / args.speed
                    wait = target - time.time()
                    if wait > 0:
                        time.sleep(wait)
    except KeyboardInterrupt:
        print("\n[用户中断]")
    finally:
        if ser is not None:
            ser.close()

    if not args.dry_run:
        flush()

    print("-" * 62)
    print(f"读到 {lines} 行，解析成功 {lines - skipped}，跳过 {skipped}")
    if not args.dry_run:
        print(f"发送 {ok} 批成功 / {fail} 批失败")
        if last:
            print(f"最终：已爬 {last.get('climbed_floors')} 层 / 当前 {last.get('current_floor')} 层"
                  f" / {last.get('steps')} 级台阶 / 中间平台 {last.get('mid_platforms')}")
    return 0 if fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
