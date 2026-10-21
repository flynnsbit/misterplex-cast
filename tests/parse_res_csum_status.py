#!/usr/bin/env python3
"""Host-only residual_csum RCA helper (post-R-csum1 re-gate).

No SPI / no lab thrash. Decodes push_frame --status / --raw lines or raw hex
for status bytes [12]=res_dc, [13]=res_csum. On P3-3l2+ raw[14] is
recon_sig and raw[15] is recon RCA debug flags; pre-3.3l-2 used raw[14:15] for stream.

Host golden (locked in host/libmisterplex/h264_residual_gold.hpp):
  res_dc   = -24 = 0xE8 (sat8(coeff[0]))
  res_csum =  20 = 0x14 (XOR sat8(full coeff[16]) — NOT arith sum -20 / 0xEC)

Usage:
  python3 tests/parse_res_csum_status.py              # print goldens + map
  python3 tests/parse_res_csum_status.py --self-test  # offline checks
  python3 tests/parse_res_csum_status.py e8 14 3b 53  # raw[12..15] or 16B
  echo 'status ... res_dc=-24 res_csum=20 ...' | python3 tests/parse_res_csum_status.py -
  echo 'raw[0]: .. e8 ef 2a 00  lo=...' | python3 tests/parse_res_csum_status.py -
"""
from __future__ import annotations

import re
import sys
from typing import List, Optional, Sequence, Tuple

# Locked Baseline first-residual goldens (residual_gold::kDc / kCsum8).
GOLD_RES_DC = -24
GOLD_RES_DC_U8 = 0xE8
GOLD_RES_CSUM = 20
GOLD_RES_CSUM_HEX = 0x14
GOLD_RECON_SIG = 0x3B
STALE_ARITH_SUM = -20
STALE_SUM_U8 = 0xEC
# Full scan for documentation / self-check (matches residual_gold::kCoeffScan).
GOLD_COEFF_SCAN = (-24, 4, 4, 0, -4, 0, -1, 0, 0, -1, 1, 0, 1, 0, 0, 0)


def sat_s8(v: int) -> int:
    if v > 127:
        return 127
    if v < -128:
        return -128
    return v


def residual_csum8(coeff: Sequence[int]) -> int:
    c = 0
    for v in coeff:
        c ^= sat_s8(v) & 0xFF
    return c & 0xFF


def s8_from_u8(b: int) -> int:
    b &= 0xFF
    return b - 256 if b >= 128 else b


def print_goldens() -> None:
    csum = residual_csum8(GOLD_COEFF_SCAN)
    arith = sum(sat_s8(v) for v in GOLD_COEFF_SCAN)
    print("=== HOST residual csum goldens (locked unit) ===")
    print(f"  algo:        residual_csum = XOR sat_s8(coeff[0..15])  as uint8")
    print(f"  coeff_scan:  {' '.join(str(x) for x in GOLD_COEFF_SCAN)}")
    print(f"  res_dc:      {GOLD_RES_DC}  (u8=0x{GOLD_RES_DC_U8:02x})  = sat8(coeff[0])")
    print(f"  res_csum:    {GOLD_RES_CSUM}  (u8=0x{GOLD_RES_CSUM_HEX:02x})  = XOR sat8 full-16")
    print(f"  compute:     residual_csum8(gold) = 0x{csum:02x} ({csum})")
    print(f"  anti-golden: arith sum sat8 = {arith} → u8 0x{arith & 0xFF:02x}  (DO NOT USE)")
    print("  status map (post-3.3l-1 pack):")
    print("    raw[12]     residual_dc      status[103:96]   expect 0xe8 (-24)")
    print("    raw[13]     residual_csum8   status[111:104]  expect 0x14 (20)")
    print("    raw[14]     recon_sig8      status[119:112]  expect 0x3b after 3.3l-2")
    print("    raw[15]     recon_dbg flags status[127:120]  P3 RCA; AR may mask bits [2:1]")
    print("  hard gate:    res_dc=-24 AND res_csum=20 (raw[13]=0x14)")
    print("  soft-skip:    test_f3_residual EXIT=0 on csum miss is NOT hard PASS")
    print("  SoT: host/libmisterplex/h264_residual_gold.hpp + tests/unit/test_idct_quant.cpp")
    print("  ARM: push_frame --status (res_csum=) / --raw (hex dump); parseCoreStatus raw[13]")
    print("  re-gate RCA:  host expected always 0x14; lab actual from --status/--raw only")
    print("                (no residual push storm; R-csum1 fit stays sole owner of RBF)")


def classify(dc_u8: int, csum_u8: int, stream16: int) -> str:
    dc = s8_from_u8(dc_u8)
    notes: List[str] = []
    dc_ok = dc == GOLD_RES_DC and dc_u8 == GOLD_RES_DC_U8
    csum_ok = csum_u8 == GOLD_RES_CSUM_HEX

    if csum_u8 == STALE_SUM_U8:
        notes.append("STALE_ARITH_SUM_FOLD (0xEC) — RTL/host must XOR not sum")
    # Pre-3.3l-1 alias: residual_csum was stream_bytes[7:0]
    if csum_u8 == (stream16 & 0xFF) and not csum_ok:
        notes.append("possible STREAM_BYTES_ALIAS (raw[13]==stream_bytes[7:0])")
    # Idle / no residual path (common without F3 push)
    if dc_u8 == 0 and csum_u8 != GOLD_RES_CSUM_HEX:
        notes.append("res_dc=0 — no residual latch (idle or pre-F3)")
    if dc_ok and csum_ok:
        verdict = "HARD_PASS"
    elif dc_ok and not csum_ok:
        verdict = "CSUM_FAIL_DC_OK"
        notes.append("live pack likely (dc stable) but XOR wrong/unstable — R-csum1 class")
    elif not dc_ok and csum_ok:
        verdict = "DC_FAIL_CSUM_OK"
        notes.append("unexpected: csum green but res_dc regression")
    else:
        verdict = "DC_AND_CSUM_FAIL"
        notes.append("check F3 push / core loaded / status map")

    note_s = "; ".join(notes) if notes else "ok"
    return f"{verdict}  ({note_s})"


def print_expected_vs_actual(dc_u8: int, csum_u8: int, stream16: Optional[int] = None) -> None:
    """One-line expected vs actual for post-re-gate RCA (no lab)."""
    dc = s8_from_u8(dc_u8)
    dc_ok = "PASS" if dc == GOLD_RES_DC else "FAIL"
    cs_ok = "PASS" if csum_u8 == GOLD_RES_CSUM_HEX else "FAIL"
    print("  EXPECTED:  res_dc=-24 (0xe8)  res_csum=20 (0x14)  [host residual_gold]")
    print(f"  ACTUAL:    res_dc={dc:+d} (0x{dc_u8:02x})  res_csum={csum_u8} (0x{csum_u8:02x})"
          + (f"  stream16=0x{stream16:04x}" if stream16 is not None else ""))
    print(f"  GATE:      res_dc={dc_ok}  res_csum={cs_ok}  hard="
          f"{'PASS' if dc_ok == 'PASS' and cs_ok == 'PASS' else 'FAIL'}")


def decode_bytes(raw12_15: Sequence[int], label: str = "") -> None:
    if len(raw12_15) < 4:
        print(f"ERROR: need at least 4 bytes (raw[12..15]), got {len(raw12_15)}", file=sys.stderr)
        return
    b = [x & 0xFF for x in raw12_15[:4]]
    dc_u8, csum_u8 = b[0], b[1]
    stream16 = b[2] | (b[3] << 8)
    dc = s8_from_u8(dc_u8)
    tag = f" [{label}]" if label else ""
    print(f"--- decode raw[12..15]{tag} ---")
    print(f"  raw[12] res_dc    = 0x{dc_u8:02x}  signed={dc:+d}   "
          f"expect 0x{GOLD_RES_DC_U8:02x} ({GOLD_RES_DC})")
    print(f"  raw[13] res_csum  = 0x{csum_u8:02x}  unsigned={csum_u8}   "
          f"expect 0x{GOLD_RES_CSUM_HEX:02x} ({GOLD_RES_CSUM})")
    print(f"  raw[14] recon_sig = 0x{b[2]:02x}  unsigned={b[2]}   "
          f"expect 0x{GOLD_RECON_SIG:02x} ({GOLD_RECON_SIG}) after 3.3l-2")
    print(f"  raw[15] recon_dbg = 0x{b[3]:02x}  ({b[3]})  P3 RCA flags (AR may mask bits [2:1])")
    print_expected_vs_actual(dc_u8, csum_u8, stream16)
    print(f"  class:      {classify(dc_u8, csum_u8, stream16)}")


def parse_hex_tokens(tokens: Sequence[str]) -> Optional[List[int]]:
    vals: List[int] = []
    for t in tokens:
        t = t.strip().lower().rstrip(",")
        if not t:
            continue
        if t.startswith("0x"):
            t = t[2:]
        if re.fullmatch(r"[0-9a-f]{1,2}", t):
            vals.append(int(t, 16))
            continue
        # glued hex dumps e.g. e8142a00
        if re.fullmatch(r"[0-9a-f]+", t) and len(t) % 2 == 0 and len(t) >= 2:
            for i in range(0, len(t), 2):
                vals.append(int(t[i : i + 2], 16))
            continue
        return None
    return vals if vals else None


def extract_from_status_line(line: str) -> Optional[Tuple[int, int, Optional[int]]]:
    """Return (res_dc_signed, res_csum_u, stream_liveness|None) from push_frame --status."""
    m_dc = re.search(r"res_dc=(-?\d+)", line)
    m_cs = re.search(r"res_csum=(\d+)", line)
    if not m_dc or not m_cs:
        return None
    dc = int(m_dc.group(1))
    csum = int(m_cs.group(1)) & 0xFF
    m_b = re.search(r"bytes_in=(\d+)", line)
    stream = int(m_b.group(1)) if m_b else None
    return dc, csum, stream


def extract_from_raw_line(line: str) -> Optional[List[int]]:
    """Parse push_frame --raw dumps; prefer explicit raw[N]: 16-hex form.

    Avoid false positives from status lines that contain qp=25 etc. as free hex.
    """
    # Explicit labeled dump: raw[0]: xx xx ... (16 bytes) or "raw: xx xx ..."
    m = re.search(
        r"(?:raw\[\d+\]|raw):\s*((?:[0-9a-fA-F]{2}(?:\s+|$)){4,16})",
        line,
        re.IGNORECASE,
    )
    if m:
        hx = re.findall(r"[0-9a-fA-F]{2}", m.group(1))
        return [int(x, 16) for x in hx]

    # Only accept free-form hex if line looks like a pure dump (no key=value telem).
    if re.search(r"\b(?:res_dc|res_csum|has_frame|bytes_in|stream_nalus)=", line):
        return None
    hx = re.findall(r"\b([0-9a-fA-F]{2})\b", line)
    if len(hx) >= 16:
        return [int(x, 16) for x in hx[:16]]
    if len(hx) == 4:
        return [int(x, 16) for x in hx]
    return None


def handle_text(text: str) -> int:
    found = 0
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        # Prefer structured --status fields over opportunistic hex scraping.
        st = extract_from_status_line(line)
        if st is not None:
            dc, csum, stream = st
            dc_u8 = dc & 0xFF
            stream16 = (stream & 0xFFFF) if stream is not None else None
            b2 = (stream16 & 0xFF) if stream16 is not None else 0
            b3 = ((stream16 >> 8) & 0xFF) if stream16 is not None else 0
            print("--- decode push_frame --status ---")
            print(f"  res_dc={dc} (u8=0x{dc_u8:02x})  res_csum={csum} (0x{csum:02x})  "
                  f"stream_liveness={stream if stream is not None else '?'}")
            if stream16 is not None:
                print(f"  mapped raw[12..15] ≈ {dc_u8:02x} {csum:02x} {b2:02x} {b3:02x}"
                      " (pre-3.3l-2 stream mapping; P3-3l2 raw[14] is recon_sig)")
            else:
                print(f"  mapped raw[12..13] ≈ {dc_u8:02x} {csum:02x}  (stream unknown)")
            print_expected_vs_actual(dc_u8, csum, stream16)
            print(f"  class:      {classify(dc_u8, csum, stream16 or 0)}")
            found += 1
            continue

        raw = extract_from_raw_line(line)
        if raw is not None and len(raw) >= 16:
            decode_bytes(raw[12:16], label="from --raw line")
            found += 1
            continue
        if raw is not None and len(raw) == 4:
            decode_bytes(raw, label="4-byte raw[12..15]")
            found += 1
            continue
    if found == 0:
        print("No status/raw residual fields found in input.", file=sys.stderr)
        return 2
    return 0


def self_test() -> int:
    print_goldens()
    print()
    ok = True
    csum = residual_csum8(GOLD_COEFF_SCAN)
    if csum != GOLD_RES_CSUM_HEX:
        print(f"SELF-TEST FAIL: csum=0x{csum:02x} want 0x14")
        ok = False
    if residual_csum8(GOLD_COEFF_SCAN) == STALE_SUM_U8:
        print("SELF-TEST FAIL: csum equals stale sum")
        ok = False

    # PASS vector (P3-3l2 ABI: raw[14]=recon_sig 0x3b, raw[15]=recon_dbg 0xf9)
    print("self-test PASS vector e8 14 3b f9:")
    decode_bytes([0xE8, 0x14, 0x3B, 0xF9], label="PASS")
    # aa146c17-class stream alias
    print("self-test stream-alias vector e8 53 53 02:")
    decode_bytes([0xE8, 0x53, 0x53, 0x02], label="alias")
    # live wrong / unstable raw[13] with stable dc (820484a6 re-gate class)
    print("self-test live-wrong vector e8 e8 2a 00:")
    decode_bytes([0xE8, 0xE8, 0x2A, 0x00], label="live-wrong")
    # unstable sample class (context: raw[13] thrash)
    print("self-test unstable-csum vector e8 ef 2a 00:")
    decode_bytes([0xE8, 0xEF, 0x2A, 0x00], label="unstable")
    # stale sum
    print("self-test stale-sum vector e8 ec 00 00:")
    decode_bytes([0xE8, 0xEC, 0x00, 0x00], label="stale-sum")

    # status line — must prefer res_dc=/res_csum= over qp=25 hex scrapes
    line = ("status has_frame=1 has_audio=0 has_stream=1 underrun=0 has_idr=1 stub_busy=0 "
            "sps_valid=1 pps_valid=1 nalu=4 last_nal=0x05 slice_type=7 mb0=0 qp=25 "
            "res_ok=1 res_tc=8 res_t1=3 res_dc=-24 res_csum=20 ddr_busy=0 sps=320x240 stream_nalus=4")
    print("self-test status line:")
    # Capture that status path yields HARD_PASS
    st = extract_from_status_line(line)
    if st is None or st[0] != -24 or st[1] != 20 or st[2] is not None:
        print(f"SELF-TEST FAIL: status parse got {st} want (-24, 20, None)")
        ok = False
    # Ensure free-hex scrape does NOT steal status line
    if extract_from_raw_line(line) is not None:
        print("SELF-TEST FAIL: status line wrongly matched as raw hex dump")
        ok = False
    rc = handle_text(line)
    if rc != 0:
        print(f"SELF-TEST FAIL: handle_text status rc={rc}")
        ok = False

    # --raw form must still work
    raw_line = "raw[0]: 20 22 15 01 80 00 00 00 00 00 00 00 e8 14 3b f9"
    print("self-test raw line:")
    raw = extract_from_raw_line(raw_line)
    if raw is None or len(raw) < 16 or raw[12] != 0xE8 or raw[13] != 0x14:
        print(f"SELF-TEST FAIL: raw line parse {raw}")
        ok = False
    handle_text(raw_line)

    # FAIL status (820484a6 class example: dc ok, csum wrong)
    fail_line = ("status has_frame=1 has_stream=1 res_ok=1 res_tc=8 res_t1=3 "
                 "res_dc=-24 res_csum=239 stream_nalus=4")
    print("self-test FAIL status (csum!=20):")
    stf = extract_from_status_line(fail_line)
    if stf is None or stf[0] != -24 or stf[1] != 239:
        print(f"SELF-TEST FAIL: fail status parse {stf}")
        ok = False
    handle_text(fail_line)

    if ok:
        print("parse_res_csum_status: SELF-TEST OK (host golden 0x14 locked)")
        return 0
    print("parse_res_csum_status: SELF-TEST FAILED", file=sys.stderr)
    return 1


def main(argv: List[str]) -> int:
    if len(argv) == 1:
        print_goldens()
        print()
        print("Tip: pass hex raw[12..15] or full 16B, or pipe push_frame --status/--raw.")
        print("     python3 tests/parse_res_csum_status.py --self-test")
        return 0
    if argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0
    if argv[1] == "--self-test":
        return self_test()
    if argv[1] == "-":
        return handle_text(sys.stdin.read())
    if argv[1] == "--goldens":
        print_goldens()
        return 0

    # CLI hex tokens
    vals = parse_hex_tokens(argv[1:])
    if vals is None:
        # treat as free text
        return handle_text(" ".join(argv[1:]))
    if len(vals) >= 16:
        decode_bytes(vals[12:16], label="16B status raw[12..15]")
        return 0
    if len(vals) >= 4:
        decode_bytes(vals[:4], label="CLI raw[12..15]")
        return 0
    if len(vals) == 1:
        # single res_csum byte
        c = vals[0]
        print(f"res_csum byte 0x{c:02x} ({c})  expect 0x{GOLD_RES_CSUM_HEX:02x} ({GOLD_RES_CSUM})  "
              f"{'PASS' if c == GOLD_RES_CSUM_HEX else 'FAIL'}")
        print(f"  EXPECTED res_csum=20 (0x14)  ACTUAL={c} (0x{c:02x})  "
              f"{'PASS' if c == GOLD_RES_CSUM_HEX else 'FAIL'}")
        if c == STALE_SUM_U8:
            print("  note: matches STALE arith-sum fold — wrong algorithm")
        return 0 if c == GOLD_RES_CSUM_HEX else 1
    print(f"ERROR: need 1, 4, or 16 hex bytes; got {len(vals)}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
