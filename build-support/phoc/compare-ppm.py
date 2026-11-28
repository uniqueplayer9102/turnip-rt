#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Compare two binary PPM captures produced by grim -t ppm."""

import argparse
import statistics


def read_ppm(path):
    with open(path, "rb") as f:
        header = []
        while len(header) < 4:
            token = b""
            while True:
                byte = f.read(1)
                if not byte:
                    raise ValueError(f"{path}: truncated header")
                if byte.isspace():
                    if token:
                        break
                    continue
                if byte == b"#":
                    f.readline()
                    continue
                token += byte
            header.append(token)
        if header[0] != b"P6":
            raise ValueError(f"{path}: expected P6, got {header[0]!r}")
        width, height, maximum = map(int, header[1:])
        if maximum != 255:
            raise ValueError(f"{path}: unsupported max value {maximum}")
        pixels = f.read()
        expected = width * height * 3
        if len(pixels) != expected:
            raise ValueError(f"{path}: got {len(pixels)} bytes, expected {expected}")
    return width, height, pixels


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("reference")
    parser.add_argument("candidate")
    parser.add_argument("--report", type=int, default=12)
    args = parser.parse_args()
    w1, h1, ref = read_ppm(args.reference)
    w2, h2, got = read_ppm(args.candidate)
    if (w1, h1) != (w2, h2):
        raise SystemExit(f"different sizes: {w1}x{h1} vs {w2}x{h2}")

    diffs = []
    signs = []
    for i in range(0, len(ref), 3):
        for c in range(3):
            delta = got[i + c] - ref[i + c]
            diffs.append(abs(delta))
            signs.append(delta)
    changed = sum(d != 0 for d in diffs)
    print(
        f"{w1}x{h1}: mean_abs={statistics.mean(diffs):.2f} "
        f"p95_abs={sorted(diffs)[len(diffs) * 95 // 100]} "
        f"max_abs={max(diffs)} changed={changed}/{len(diffs)}"
    )
    samples = [0, 3 * (w1 // 2 + 4 * w1), 3 * (w1 // 2 + 100 * w1)]
    samples += list(range(3 * (w1 // 2 + 20 * w1), 3 * (w1 // 2 + 20 * w1) + 30, 3))
    for index in samples[: args.report]:
        if index + 2 < len(ref):
            pixel = index // 3 % w1 - w1 // 2, index // 3 // w1
            print(f"  pixel={pixel} ref={tuple(ref[index:index+3])} got={tuple(got[index:index+3])}")


if __name__ == "__main__":
    main()
