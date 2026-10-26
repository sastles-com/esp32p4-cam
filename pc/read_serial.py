#!/usr/bin/env python3
"""USB シリアルから boot log を読んで Wi-Fi 接続と IP を確認する。

使い方:
  pc/.venv/bin/python pc/read_serial.py [--port /dev/cu.usbmodem5B910635451] [--seconds 12]
"""
import argparse
import time

import serial


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbmodem5B910635451")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--no-reset", action="store_true", help="リセットせず現在の出力だけ読む")
    args = ap.parse_args()

    p = serial.Serial(args.port, args.baud, timeout=1)

    if not args.no_reset:
        # RTS パルスで EN リセット (RST 線が無い構成でも効くことがある)
        p.dtr = False
        p.rts = True
        time.sleep(0.2)
        p.rts = False

    end = time.time() + args.seconds
    while time.time() < end:
        d = p.read(200)
        if d:
            print(d.decode("utf-8", "replace"), end="", flush=True)
    print()
    p.close()


if __name__ == "__main__":
    main()