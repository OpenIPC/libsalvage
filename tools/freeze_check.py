#!/usr/bin/env python3
"""
Detect a decoder freeze latch: when MPP stops submitting frames to hardware it
returns stale buffers from its pool, so the output repeats bit-for-bit with the
pool's period. Real decoded output never does that.

    ./freeze_check.py out.yuv -w 1280 -H 720 --from 21 --to 60
"""

import argparse

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("yuv")
    ap.add_argument("-w", type=int, default=1280)
    ap.add_argument("-H", type=int, default=720)
    ap.add_argument("--from", dest="lo", type=int, default=0)
    ap.add_argument("--to", dest="hi", type=int, default=60)
    ap.add_argument("--max-period", type=int, default=24)
    args = ap.parse_args()

    fsz = args.w * args.H * 3 // 2
    ysz = args.w * args.H
    a = np.memmap(args.yuv, dtype=np.uint8, mode="r")
    n = len(a) // fsz
    hi = min(args.hi, n - 1)

    def y(i):
        return np.asarray(a[i * fsz:i * fsz + ysz], dtype=np.int16)

    print(f"{n} frames in {args.yuv}; scanning {args.lo}..{hi}")

    found = False
    for period in range(1, args.max_period + 1):
        hits = [i for i in range(args.lo, hi - period)
                if not np.any(y(i) - y(i + period))]
        if len(hits) >= 3:
            print(f"  FREEZE LATCH: {len(hits)} frame pairs are bit-identical at "
                  f"period {period} (e.g. {hits[0]}=={hits[0]+period}, "
                  f"{hits[1]}=={hits[1]+period})")
            print(f"  -> MPP stopped decoding and is recycling {period} stale pool buffers.")
            found = True
            break

    if not found:
        print("  no latch: consecutive frames all differ, decoder kept running")

    print("\n  frame-to-frame max luma delta:")
    for i in range(args.lo, min(args.lo + 12, hi)):
        d = int(np.abs(y(i + 1) - y(i)).max())
        print(f"    {i:3d}->{i+1:3d}  {d:4d}")


if __name__ == "__main__":
    main()
