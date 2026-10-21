#!/usr/bin/env python3
from __future__ import annotations

import json
import sys
from pathlib import Path


def clip1(v: int) -> int:
    return 0 if v < 0 else 255 if v > 255 else v


def avg2(a: int, b: int) -> int:
    return (a + b + 1) >> 1


def hraw(ref: list[int], row: int, col: int) -> int:
    def p(r: int, c: int) -> int:
        return ref[r * 9 + c]

    return p(row, col - 2) - 5 * p(row, col - 1) + 20 * p(row, col) + 20 * p(row, col + 1) - 5 * p(row, col + 2) + p(row, col + 3)


def half_h(ref: list[int], rowoff: int, coloff: int) -> int:
    return clip1((hraw(ref, 4 + rowoff, 4 + coloff) + 16) >> 5)


def half_v(ref: list[int], rowoff: int, coloff: int) -> int:
    col = 4 + coloff

    def p(r: int, c: int) -> int:
        return ref[r * 9 + c]

    return clip1((p(2 + rowoff, col) - 5 * p(3 + rowoff, col) + 20 * p(4 + rowoff, col) + 20 * p(5 + rowoff, col) - 5 * p(6 + rowoff, col) + p(7 + rowoff, col) + 16) >> 5)


def half_c(ref: list[int], rowoff: int, coloff: int) -> int:
    row = 4 + rowoff
    col = 4 + coloff
    s = hraw(ref, row - 2, col) - 5 * hraw(ref, row - 1, col) + 20 * hraw(ref, row, col) + 20 * hraw(ref, row + 1, col) - 5 * hraw(ref, row + 2, col) + hraw(ref, row + 3, col)
    return clip1((s + 512) >> 10)


def qpel(ref: list[int], fx: int, fy: int) -> int:
    p = lambda r, c: ref[r * 9 + c]
    h0 = half_h(ref, 0, 0)
    h1 = half_h(ref, 1, 0)
    v0 = half_v(ref, 0, 0)
    v1 = half_v(ref, 0, 1)
    c = half_c(ref, 0, 0)
    table = {
        (0, 0): p(4, 4),
        (1, 0): avg2(p(4, 4), h0),
        (2, 0): h0,
        (3, 0): avg2(h0, p(4, 5)),
        (0, 1): avg2(p(4, 4), v0),
        (1, 1): avg2(h0, v0),
        (2, 1): avg2(h0, c),
        (3, 1): avg2(h0, v1),
        (0, 2): v0,
        (1, 2): avg2(v0, c),
        (2, 2): c,
        (3, 2): avg2(c, v1),
        (0, 3): avg2(v0, p(5, 4)),
        (1, 3): avg2(h1, v0),
        (2, 3): avg2(c, h1),
        (3, 3): avg2(h1, v1),
    }
    return table[(fx, fy)]


def chroma(p00: int, p10: int, p01: int, p11: int, fx: int, fy: int) -> int:
    return ((8 - fx) * (8 - fy) * p00 + fx * (8 - fy) * p10 + (8 - fx) * fy * p01 + fx * fy * p11 + 32) >> 6


def median(a: int, b: int, c: int) -> int:
    return a + b + c - min(a, b, c) - max(a, b, c)


def mv_case(name: str, a: tuple[int, int] | None, b: tuple[int, int] | None, c: tuple[int, int] | None,
            d: tuple[int, int] | None, mvd: tuple[int, int], skip: bool) -> dict:
    use_c = c if c is not None else d
    avail = [v for v in (a, b, use_c) if v is not None]
    if skip and (a is None or b is None or a == (0, 0) or b == (0, 0)):
        pred = (0, 0)
        skip_zero = True
    else:
        skip_zero = False
        if not avail:
            pred = (0, 0)
        elif len(avail) == 1:
            pred = avail[0]
        else:
            ax = a[0] if a is not None else 0
            ay = a[1] if a is not None else 0
            bx = b[0] if b is not None else 0
            by = b[1] if b is not None else 0
            cx = use_c[0] if use_c is not None else 0
            cy = use_c[1] if use_c is not None else 0
            pred = (median(ax, bx, cx), median(ay, by, cy))
    mv = pred if skip else (pred[0] + mvd[0], pred[1] + mvd[1])
    return {
        "name": name,
        "avail": {"a": a is not None, "b": b is not None, "c": c is not None, "d": d is not None},
        "a": list(a or (0, 0)), "b": list(b or (0, 0)), "c": list(c or (0, 0)), "d": list(d or (0, 0)),
        "mvd": list(mvd), "p_skip": skip, "skip_zero": skip_zero,
        "pred": list(pred), "mv": list(mv),
    }


def part_case(name: str, mode: int, idx: int, a: tuple[int, int] | None, b: tuple[int, int] | None,
              c: tuple[int, int] | None, d: tuple[int, int] | None, mvd: tuple[int, int], skip: bool = False) -> dict:
    base = mv_case(name, a, b, c, d, mvd, skip)
    median_pred = tuple(base["pred"])
    if not base["skip_zero"]:
        if mode == 1 and idx == 0 and b is not None:
            median_pred = b
        elif mode == 1 and idx == 1 and a is not None:
            median_pred = a
        elif mode == 2 and idx == 0 and a is not None:
            median_pred = a
        elif mode == 2 and idx == 1 and c is not None:
            median_pred = c
    mv = median_pred if skip else (median_pred[0] + mvd[0], median_pred[1] + mvd[1])
    base.update({"mode": mode, "idx": idx, "pred": list(median_pred), "mv": list(mv)})
    return base


def main() -> int:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("tests/fixtures/p3_inter_pred/inter_mc_v1.json")
    ref = [((r * 37 + c * 19 + (r * c * 7)) ^ ((r + 3) * 11)) & 0xFF for r in range(9) for c in range(9)]
    data = {
        "format": "misterplex.p3.inter_mc.v1",
        "profile": {
            "source": "PMS XML VideoEncodeFlags / local x264 proxy",
            "coded": [624, 480],
            "display": [618, 480],
            "x264opts": "cabac=0:bframes=0:ref=1:weightp=0:8x8dct=0:partitions=none",
            "partition_measurement": {
                "frames": {"I": 6, "P": 294, "B": 0},
                "mb_P": {"intra": 4.8, "p16x16": 17.6, "sub_mb": 0.0, "skip": 77.7},
                "note": "x264 CLI encode with the exact VideoEncodeFlags; partitions=none leaves P16x16 and P_Skip for inter MBs.",
            },
        },
        "mv_cases": [
            mv_case("median3", (4, -2), (-8, 6), (12, 10), None, (1, -1), False),
            mv_case("c_fallback_d", (20, 4), (-4, 8), None, (6, -12), (-2, 3), False),
            mv_case("top_row_single_a", (7, -5), None, None, None, (0, 0), False),
            mv_case("skip_left_edge", None, (9, 4), (7, 2), None, (5, 5), True),
            mv_case("skip_zero_neighbor", (0, 0), (9, 4), (7, 2), None, (3, -3), True),
            mv_case("skip_predicted_nonzero", (4, 2), (8, 6), (12, 10), None, (9, 9), True),
        ],
        "partition_cases": [
            part_case("p16x16_median", 0, 0, (4, -2), (-8, 6), (12, 10), None, (1, -1)),
            part_case("pskip_predicted_nonzero", 0, 0, (4, 2), (8, 6), (12, 10), None, (9, 9), True),
            part_case("pskip_left_edge_zero", 0, 0, None, (9, 4), (7, 2), None, (5, 5), True),
            part_case("p16x8_top_uses_b", 1, 0, (100, 0), (1, 2), (50, 0), None, (3, 4)),
            part_case("p16x8_bottom_uses_a", 1, 1, (7, -3), (20, 12), (30, 2), None, (-2, 5)),
            part_case("p8x16_left_uses_a", 2, 0, (6, -6), (40, 1), (30, 8), None, (4, -1)),
            part_case("p8x16_right_uses_c", 2, 1, (6, -6), (40, 1), (9, 11), None, (2, 3)),
            part_case("p8x16_right_c_missing_median_d", 2, 1, (2, 20), (8, 4), None, (6, 10), (1, -2)),
            part_case("p8x8_median", 3, 2, (2, 20), (8, 4), None, (6, 10), (-3, 7)),
            part_case("sub_partition_median", 4, 3, (-5, 1), (11, 9), (3, 7), None, (2, -4)),
        ],
        "luma_ref_9x9": ref,
        "luma_cases": [{"frac": [fx, fy], "sample": qpel(ref, fx, fy)} for fy in range(4) for fx in range(4)],
        "chroma_cases": [
            {"p": [23, 101, 77, 209], "frac": [0, 0], "sample": chroma(23, 101, 77, 209, 0, 0)},
            {"p": [23, 101, 77, 209], "frac": [3, 5], "sample": chroma(23, 101, 77, 209, 3, 5)},
            {"p": [250, 7, 33, 199], "frac": [7, 7], "sample": chroma(250, 7, 33, 199, 7, 7)},
        ],
        "clamp_cases": [
            {"xy": [-5, 10], "wh": [624, 480], "clamped": [0, 10]},
            {"xy": [630, -9], "wh": [624, 480], "clamped": [623, 0]},
            {"xy": [623, 479], "wh": [624, 480], "clamped": [623, 479]},
        ],
        "fetch_cases": [
            {"base": [0, 0], "tap": 0, "wh": [624, 480], "xy": [0, 0]},
            {"base": [0, 0], "tap": 40, "wh": [624, 480], "xy": [0, 0]},
            {"base": [623, 479], "tap": 80, "wh": [624, 480], "xy": [623, 479]},
            {"base": [100, 50], "tap": 0, "wh": [624, 480], "xy": [96, 46]},
            {"base": [100, 50], "tap": 80, "wh": [624, 480], "xy": [104, 54]},
        ],
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(data, indent=2) + "\n")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
