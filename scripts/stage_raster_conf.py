#!/usr/bin/env python3
"""Build a staged 240p / true-480i misterplexd conf from a byte copy of prod conf
933a4104 plus a token-free overlay (assets/confs/staged/*.overlay.conf).

  stage_raster_conf.py --base PROD_CONF --overlay OVERLAY --out OUT
                       [--base-md5 933a41044fb6605cf629ead07d827ff3]

The base must match --base-md5 (pass --base-md5 '' only for synthetic test bases).
The overlay keys go first (main.cpp loadConf takes the first KEY= line); base lines
for the same keys are dropped. The script refuses keys that would turn on the
ARM-present path:
  480i: MPX_PRESENT_480I (any value), DISPLAY_RES=480i, CONTENT_RES=480i
  240p: any DISPLAY_RES, CONTENT_RES or MPX_PRESENT_* key
It also requires AUDIO_DELAY_MS=50, and FPGA_480P_CODED_W=640 for 480i. OUT holds
the prod PLEX_TOKEN: it is written 0600 and must never be committed. Prints the md5.
Offline only; it never touches a MiSTer.
"""

import argparse
import hashlib
import os
import sys

PROD_MD5 = "933a41044fb6605cf629ead07d827ff3"


def conf_lines(text):
    return text.splitlines()


def keyed(line):
    """KEY for a KEY=VALUE line, as loadConf sees it ('' for blank/comment lines)."""
    if not line or line.startswith("#") or "=" not in line:
        return ""
    return line.split("=", 1)[0]


def load_conf(lines, key):
    """main.cpp loadConf: the value of the first non-comment line starting KEY=."""
    p = key + "="
    for line in lines:
        if line and not line.startswith("#") and line.startswith(p):
            return line[len(p):]
    return None


def forbidden(lines, raster):
    bad = []
    for line in lines:
        k = keyed(line)
        if not k:
            continue
        v = line.split("=", 1)[1]
        if raster == "480i" and (k == "MPX_PRESENT_480I" or
                                 (k in ("DISPLAY_RES", "CONTENT_RES") and v == "480i")):
            bad.append(line)
        if raster == "240p" and (k in ("DISPLAY_RES", "CONTENT_RES") or
                                 k.startswith("MPX_PRESENT_")):
            bad.append(line)
    return bad


def stage(base_text, overlay_text):
    """Return (conf_text, raster). Raises ValueError on any policy violation."""
    overlay = [l for l in conf_lines(overlay_text) if keyed(l)]
    raster = load_conf(overlay, "MPX_FPGA_MODE")
    if raster not in ("240p", "480i"):
        raise ValueError(f"overlay MPX_FPGA_MODE must be 240p or 480i, not {raster!r}")
    base = conf_lines(base_text)
    bad = forbidden(base + overlay, raster)
    if bad:
        raise ValueError(f"{raster}: forbidden ARM-present/legacy keys: {bad}")
    keys = {keyed(l) for l in overlay}
    out = overlay + [l for l in base if keyed(l) not in keys]
    checks = {"MPX_FPGA_MODE": raster, "AUDIO_DELAY_MS": "50"}
    if raster == "480i":
        checks["FPGA_480P_CODED_W"] = "640"
    for k, v in checks.items():
        if load_conf(out, k) != v:
            raise ValueError(f"{raster}: effective {k}={load_conf(out, k)!r}, want {v}")
    if raster == "240p" and load_conf(out, "FPGA_480P_CODED_W") == "640":
        raise ValueError("240p: FPGA_480P_CODED_W=640 is a 480-tier switch")
    return "\n".join(out) + "\n", raster


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", required=True)
    ap.add_argument("--overlay", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--base-md5", default=PROD_MD5)
    args = ap.parse_args()
    with open(args.base, "rb") as fh:
        raw = fh.read()
    md5 = hashlib.md5(raw).hexdigest()
    if args.base_md5 and md5 != args.base_md5:
        sys.exit(f"base md5 {md5} != {args.base_md5}: not the prod conf")
    with open(args.overlay, encoding="utf-8") as fh:
        overlay = fh.read()
    try:
        text, raster = stage(raw.decode("utf-8"), overlay)
    except ValueError as e:
        sys.exit(f"refusing to stage: {e}")
    fd = os.open(args.out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        fh.write(text)
    print(f"{hashlib.md5(text.encode()).hexdigest()}  {args.out}  "
          f"({raster}; base {md5[:8]}; overlay {os.path.basename(args.overlay)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
