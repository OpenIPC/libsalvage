#!/usr/bin/env python3
"""Compare two NV12/I420-luma YUV sequences frame by frame and localise damage."""

import argparse
import sys

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ref")
    ap.add_argument("test")
    ap.add_argument("-w", type=int, default=1280)
    ap.add_argument("-H", type=int, default=720)
    ap.add_argument("--mb", type=int, default=16, help="row granularity for damage map")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    w, h = args.w, args.H
    fsz = w * h * 3 // 2
    ysz = w * h

    a = np.memmap(args.ref, dtype=np.uint8, mode="r")
    b = np.memmap(args.test, dtype=np.uint8, mode="r")
    na, nb = len(a) // fsz, len(b) // fsz
    n = min(na, nb)
    print(f"ref frames={na} test frames={nb} comparing={n}")

    bad = []
    for i in range(n):
        ya = np.asarray(a[i * fsz: i * fsz + ysz], dtype=np.int16).reshape(h, w)
        yb = np.asarray(b[i * fsz: i * fsz + ysz], dtype=np.int16).reshape(h, w)
        d = np.abs(ya - yb)
        if not d.any():
            continue
        mse = float((d.astype(np.float64) ** 2).mean())
        psnr = 10 * np.log10(255.0 ** 2 / mse) if mse > 0 else 99.0
        rows = d.max(axis=1)
        dirty = np.nonzero(rows)[0]
        mbrows = sorted(set(int(r) // args.mb for r in dirty))
        # compress mb-row list into ranges
        rngs, s = [], mbrows[0]
        for j in range(1, len(mbrows) + 1):
            if j == len(mbrows) or mbrows[j] != mbrows[j - 1] + 1:
                rngs.append(f"{s}" if s == mbrows[j - 1] else f"{s}-{mbrows[j-1]}")
                if j < len(mbrows):
                    s = mbrows[j]
        pct = 100.0 * np.count_nonzero(d) / d.size
        bad.append((i, psnr, pct, ",".join(rngs), int(d.max())))

    if not bad:
        print("IDENTICAL")
        return

    print(f"{len(bad)}/{n} frames differ")
    if not args.quiet:
        print(f"{'frm':>4} {'psnr':>7} {'pix%':>6} {'maxdiff':>7}  dirty MB-rows")
        for i, psnr, pct, rngs, mx in bad:
            print(f"{i:4d} {psnr:7.2f} {pct:6.2f} {mx:7d}  {rngs}")


if __name__ == "__main__":
    main()
