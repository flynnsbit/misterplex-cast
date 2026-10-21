#!/usr/bin/env bash
# Observe ARM bitstream producer vs FPGA consumer + PARSE desync sticky.
#
# ONE command closes the triage loop:
#   - ring write ptr advancing (producer @ PLXB CTRL 0x30140000)
#   - read ptr following (FPGA consume @ PLXR READ 0x30140008)
#   - NAL liveness: nalu_count, last_nal_type classed as SPS/PPS/IDR/P
#   - sticky PARSE desync flag, cause, last-good/break MB from UIO status_telem
#     raw[15] / raw[10..11] (via push_frame --status / --raw)
#   - frame-store mailboxes PLXS/PLXD at PLX_PAGE+0x100/+0x128 (liveness only)
#
# status_telem (Plex.sv → UIO_GET_STATUS 0x29 → 16 raw bytes LE):
#   raw[0..1]  status[15:0] OSD/kick        raw[2] flags  raw[3] last_nal
#   raw[4..5]  nalu_count                   raw[6] mb0    raw[7] slice_type
#   raw[8] residual pack  raw[9] {ddr_busy,swap,qp}
#   raw[10..11] sps_mb_w/h OR desync_mb LE when PARSE sticky
#   raw[12] res_dc  raw[13] res_csum  raw[14] recon_sig
#   raw[15] recon_dbg / PARSE overlay:
#           bit0=early bit1=long [7:4]=cause (kDesyncCause*)
# PLXS DDR mailbox is ONLY OSD[15:0]+seq — full telem is SPI UIO only.
#
# Triage:
#   write static            → producer dormant / STREAM=0 / no demux
#   write↑ read stuck       → FPGA consumer wedged
#   both↑ + desync=1        → PARSE walk broke (cause/mb)
#   both↑ desync=0 bad pic  → genuine decode bug
#
# Run ON MiSTer (root) or from build host with MISTER_HOST (+ sshpass).
# Does not load cores or kill processes.
# PLX_BUILD=0xID: FPGA_VIDEO_BUILD_ID of the loaded RBF (daemon log "media: FPGA build
#   0x..."); its fpga_build_ids.hpp row gives the PLX page (PLX_ROW_PAGES below). An ID
#   without a row gets no PLX reads.
# PLX_PAGE=0x...: an explicit page, printed as an override; if PLX_BUILD is also set the
#   two must agree, else no PLX reads. Neither set: PLXS/PLXD are not read. The ring
#   lines (0x3014xxxx, absolute) are read in every case.
# PROBE_MEM: a memory image instead of /dev/mem (offline tests).
set -euo pipefail

STATUS_FILE="${BITSTREAM_STATUS_FILE:-/media/fat/misterplex/bitstream_ring.status}"
PUSH_FRAME="${PUSH_FRAME:-/media/fat/misterplex/bin/push_frame}"
SAMPLE_SLEEP_S="${SAMPLE_SLEEP_S:-0.35}"
PLX_BUILD="${PLX_BUILD:-}"
PLX_PAGE="${PLX_PAGE:-}"
PROBE_MEM="${PROBE_MEM:-/dev/mem}"

# Recognised FPGA builds -> PLX page: host/libmisterplex/fpga_build_ids.hpp rows through
# plx_page.hpp. tests/unit/test_plx_page_scripts.sh keeps this equal to that table.
PLX_ROW_PAGES="0x4800F36A:0x300FF000 0x48004D57:0x300FF000 0x48004D40:0x300FF000 0x48005D40:0x300FF000 0x48005D45:0x300FF000 0x48005D50:0x300FF000 0x48005D52:0x300FF000 0x4800A1FB:0x300FF000 0x4800A152:0x300FF000 0x48009E36:0x300FF000 0x48002731:0x300FF000 0x4800C88F:0x300FF000 0x48003ECB:0x300FF000 0x480058BC:0x300FF000 0x480094DD:0x300FF000 0x48018AE0:0x300FF000 0x48FF65F2:0x300FF000 0x487A1BD1:0x300FF000 0x48C5385C:0x300FF000 0x48C51FF1:0x300FF000 0x48CFEE31:0x300FF000 0x48A64AED:0x300FF000 0x48110015:0x300FF000 0x48032F84:0x300FF000 0x48038D9A:0x300FF000 0x4800F40A:0x300FF000 0x48006A7E:0x300FF000 0x2400F36A:0x300FF000 0x2400F40B:0x300FF000 0x24003997:0x300FF000 0x24F1A2C4:0x300FF000 0x24A10026:0x300FF000 0x24C07821:0x300FF000 0x4801F40A:0x300FF000 0x48F14A0B:0x300FF000 0x48C078E2:0x300FF000"

PLX_NOTE=""
resolve_plx_page() {
  local num='^(0[xX][0-9A-Fa-f]{1,8}|0|[1-9][0-9]{0,9})$' id e want=""
  if [[ -n "$PLX_PAGE" && ! "$PLX_PAGE" =~ $num ]]; then
    PLX_NOTE="PLX_PAGE=$PLX_PAGE is not a number: no PLX reads"
    PLX_PAGE=""
    return
  fi
  if [[ -n "$PLX_BUILD" ]]; then
    if [[ ! "$PLX_BUILD" =~ $num ]]; then
      PLX_NOTE="PLX_BUILD=$PLX_BUILD is not a number: no PLX reads"
      PLX_PAGE=""
      return
    fi
    id=$(printf '0x%08X' "$((PLX_BUILD))")
    for e in $PLX_ROW_PAGES; do
      [[ "${e%%:*}" == "$id" ]] && want="${e#*:}"
    done
    if [[ -z "$want" ]]; then
      PLX_NOTE="build $id has no fpga_build_ids.hpp row: no PLX page, no PLX reads"
      PLX_PAGE=""
    elif [[ -n "$PLX_PAGE" ]] && (( PLX_PAGE != want )); then
      PLX_NOTE="PLX_PAGE=$PLX_PAGE disagrees with build $id row page $want: no PLX reads"
      PLX_PAGE=""
    else
      PLX_PAGE="$want"
      PLX_NOTE="page $want from build $id row"
    fi
  elif [[ -n "$PLX_PAGE" ]]; then
    PLX_NOTE="page $PLX_PAGE from PLX_PAGE (override, not from a build row)"
  fi
}
resolve_plx_page

remote_probe() {
  STATUS_FILE="$STATUS_FILE" PUSH_FRAME="$PUSH_FRAME" SAMPLE_SLEEP_S="$SAMPLE_SLEEP_S" \
  PLX_PAGE="$PLX_PAGE" PLX_NOTE="$PLX_NOTE" PROBE_MEM="$PROBE_MEM" \
  python3 - <<'PY'
import os, struct, subprocess, time, mmap

STATUS_FILE = os.environ.get("STATUS_FILE", "/media/fat/misterplex/bitstream_ring.status")
PUSH_FRAME = os.environ.get("PUSH_FRAME", "/media/fat/misterplex/bin/push_frame")
SLEEP = float(os.environ.get("SAMPLE_SLEEP_S", "0.35"))
MEM = os.environ.get("PROBE_MEM", "/dev/mem") or "/dev/mem"

# Bitstream ring (ddr_bitstream_ring.hpp)
CTRL_PHYS = 0x30140000
READ_PHYS = 0x30140008
ERR_PHYS  = 0x30140010
STAT0_PHYS = 0x30140018
STAT5_PHYS = 0x30140040
STAT6_PHYS = 0x30140048
# Frame-store mailboxes: mailbox_abi_spec.hpp offsets on the build's PLX page
# (resolved from PLX_BUILD's row by the bash wrapper). Liveness only, not full
# telem. No page, no reads.
PLX_PAGE = int(os.environ.get("PLX_PAGE", "") or "0", 0)
PLX_NOTE = os.environ.get("PLX_NOTE", "")
PLXS_OFF = 0x100  # +0x104 hi: OSD + heartbeat seq
PLXD_OFF = 0x128  # +0x12C hi

def u32(fd, phys):
    page = phys & ~0xFFF
    off = phys - page
    mm = mmap.mmap(fd, 0x1000, offset=page, access=mmap.ACCESS_READ)
    try:
        return struct.unpack_from("<I", mm, off)[0]
    finally:
        mm.close()

def u64(fd, phys):
    page = phys & ~0xFFF
    off = phys - page
    mm = mmap.mmap(fd, 0x1000, offset=page, access=mmap.ACCESS_READ)
    try:
        return struct.unpack_from("<Q", mm, off)[0]
    finally:
        mm.close()

def mag_s(m):
    bs = bytes([(m >> (8 * i)) & 0xFF for i in range(4)])
    if all(32 <= b < 127 for b in bs):
        return bs.decode("ascii")
    return "????"

def sample_ring(fd):
    ctrl = u64(fd, CTRL_PHYS)
    rd = u64(fd, READ_PHYS)
    err = u64(fd, ERR_PHYS)
    st0 = u64(fd, STAT0_PHYS)
    st5 = u64(fd, STAT5_PHYS)
    st6 = u64(fd, STAT6_PHYS)
    plxs = u64(fd, PLX_PAGE + PLXS_OFF) if PLX_PAGE else 0
    plxd = u64(fd, PLX_PAGE + PLXD_OFF) if PLX_PAGE else 0
    cm = ctrl & 0xFFFFFFFF
    prod = (ctrl >> 32) & 0x7FFFFFFF
    epoch = (ctrl >> 63) & 1
    rm = rd & 0xFFFFFFFF
    cons = (rd >> 32) & 0xFFFFFFFF
    em = err & 0xFFFFFFFF
    s0m = st0 & 0xFFFFFFFF
    ring_level = (st0 >> 32) & 0xFFFFFFFF if s0m == 0x504C5854 else None
    s6m = st6 & 0xFFFFFFFF
    xport_desync = xport_fatal = None
    xport_desync_count = None
    if s6m == 0x504C5851:  # PLXQ
        ds = (st6 >> 32) & 0xFFFFFFFF
        flags = ds & 0xFFFF
        xport_desync_count = (ds >> 16) & 0xFFFF
        xport_desync = bool((flags >> 10) & 1)
        xport_fatal = bool((flags >> 11) & 1)
    return {
        "cm": cm, "prod": prod, "epoch": epoch,
        "rm": rm, "cons": cons, "em": em,
        "ring_level": ring_level,
        "xport_desync": xport_desync,
        "xport_fatal": xport_fatal,
        "xport_desync_count": xport_desync_count,
        "plxs": plxs, "plxd": plxd,
        "st5": st5, "err": err,
    }

def classify_nal(last_nal, slice_type, has_idr, sps_valid, pps_valid):
    # H.264 nal_unit_type in last_nal (low 5 bits typically)
    nt = last_nal & 0x1F
    label = {
        1: "nonIDR_slice",
        5: "IDR",
        6: "SEI",
        7: "SPS",
        8: "PPS",
        9: "AUD",
    }.get(nt, f"type{nt}")
    # slice_type: 0/5=P, 2/7=I (H.264 Table 7-6); status may hold raw ue value
    st = slice_type
    if nt == 1:
        if st in (0, 5):
            label = "P"
        elif st in (2, 7):
            label = "I_slice"
        else:
            label = f"slice(st={st})"
    seen = []
    if sps_valid:
        seen.append("SPS")
    if pps_valid:
        seen.append("PPS")
    if has_idr:
        seen.append("IDR")
    if nt == 1 and st in (0, 5):
        seen.append("P")
    return label, seen

def parse_status_line(line):
    # push_frame --status key=value dump
    out = {}
    for tok in line.split():
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        out[k] = v
    return out

def parse_raw_line(line):
    # "raw[N]: aa bb ... lo=0x...."
    if ":" not in line:
        return None
    body = line.split(":", 1)[1]
    hexes = []
    for tok in body.split():
        if tok.startswith("lo="):
            break
        try:
            hexes.append(int(tok, 16))
        except ValueError:
            continue
    if len(hexes) < 16:
        return None
    return hexes[:16]

def decode_telem(raw):
    # status_telemetry.hpp / Plex.sv layout
    flags = raw[2]
    last_nal = raw[3]
    nalu = raw[4] | (raw[5] << 8)
    mb0 = raw[6]
    slice_type = raw[7]
    recon_dbg = raw[15]
    early = recon_dbg & 1
    longb = (recon_dbg >> 1) & 1
    cause = (recon_dbg >> 4) & 0xF
    desync = bool(early or longb or cause)
    mb = raw[10] | (raw[11] << 8) if desync else 0
    return {
        "flags": flags, "last_nal": last_nal, "nalu": nalu, "mb0": mb0,
        "slice_type": slice_type, "recon_dbg": recon_dbg,
        "early": early, "long": longb, "cause": cause, "desync": int(desync),
        "desync_mb": mb,
        "has_idr": (flags >> 4) & 1,
        "sps_valid": (flags >> 6) & 1,
        "pps_valid": (flags >> 7) & 1,
        "res_csum": raw[13], "recon_sig": raw[14],
    }

def run_push(args):
    if not os.path.isfile(PUSH_FRAME) or not os.access(PUSH_FRAME, os.X_OK):
        return None, f"(no executable {PUSH_FRAME})"
    try:
        p = subprocess.run(
            [PUSH_FRAME] + args,
            capture_output=True, text=True, timeout=8,
        )
        text = (p.stdout or "") + (p.stderr or "")
        return p.returncode, text.strip()
    except Exception as e:
        return None, f"(push_frame failed: {e})"

fd = os.open(MEM, os.O_RDONLY | os.O_SYNC)
try:
    a = sample_ring(fd)
    time.sleep(SLEEP)
    b = sample_ring(fd)
finally:
    os.close(fd)

d_prod = (b["prod"] - a["prod"]) & 0x7FFFFFFF
# consumer is full 32-bit
d_cons = (b["cons"] - a["cons"]) & 0xFFFFFFFF

print("=== DDR bitstream ring (two samples) ===")
print(f"CTRL@0x30140000 magic={mag_s(a['cm'])}/→{mag_s(b['cm'])} "
      f"producer_bytes t0={a['prod']} t1={b['prod']} Δ={d_prod} epoch={b['epoch']}")
print(f"READ@0x30140008 magic={mag_s(a['rm'])}/→{mag_s(b['rm'])} "
      f"consumer_bytes t0={a['cons']} t1={b['cons']} Δ={d_cons}")
print(f"ERR @0x30140010 magic={mag_s(b['em'])}(0x{b['em']:08x})")
if b["ring_level"] is not None:
    print(f"STAT0 ring_level={b['ring_level']}")
if b["xport_desync"] is not None:
    print(f"STAT6/PLXQ transport desync={int(b['xport_desync'])} "
          f"fatal={int(b['xport_fatal'])} desync_count={b['xport_desync_count']}")

# Frame-store liveness (not PARSE telem)
plxs_lo = b["plxs"] & 0xFFFFFFFF
plxs_mid = (b["plxs"] >> 32) & 0xFFFF
plxs_seq = (b["plxs"] >> 48) & 0xFFFF
plxd_lo = b["plxd"] & 0xFFFFFFFF
plxd_hi = (b["plxd"] >> 32) & 0xFFFFFFFF
print()
print("=== Frame-store mailboxes (DDR, not full status_telem) ===")
if PLX_NOTE:
    print(f"PLX page: {PLX_NOTE}")
if not PLX_PAGE:
    if not PLX_NOTE:
        print("PLX page unset: PLXS/PLXD not read (set PLX_BUILD=0xID of the loaded build; "
              "its fpga_build_ids.hpp row gives the page)")
else:
    hb = "live" if (plxs_lo == 0x504C5853 and (a["plxs"] >> 32) != (b["plxs"] >> 32)) \
        else "NOT MOVING (not the loaded build's page, or no core)"
    print(f"PLXS@0x{PLX_PAGE + PLXS_OFF:08X} magic={mag_s(plxs_lo)}(0x{plxs_lo:08x}) "
          f"osd=0x{plxs_mid:04x} seq={plxs_seq}  (hi word @0x{PLX_PAGE + PLXS_OFF + 4:08X}) "
          f"heartbeat={hb}")
    print(f"PLXD@0x{PLX_PAGE + PLXD_OFF:08X} magic={mag_s(plxd_lo)}(0x{plxd_lo:08x}) "
          f"hi@0x{PLX_PAGE + PLXD_OFF + 4:08X}=0x{plxd_hi:08x}  (bank-release ACK)")

# Ring triage
cm, prod, rm, cons = b["cm"], b["prod"], b["rm"], b["cons"]
if cm == 0x504C5844:
    triage = "STARVED_producer_dormant_PLXD"
elif cm != 0x504C5842:
    triage = "STARVED_no_PLXB_ctrl"
elif prod == 0:
    triage = "STARVED_no_writes"
elif d_prod == 0 and d_cons == 0 and prod > 0:
    triage = "IDLE_ptrs_static_after_feed"
elif d_prod > 0 and d_cons == 0:
    triage = "BROKEN_consumer_stuck"
elif rm != 0x504C5852:
    triage = "BROKEN_no_PLXR_read"
elif cons == 0 and prod > 0:
    triage = "BROKEN_consumer_never_started"
elif cons + 4096 < prod:
    triage = "CONSUMER_LAGGING"
elif d_prod > 0 and d_cons > 0:
    triage = "FEED_LIVE_both_advancing"
else:
    triage = "FEED_OK_check_decode"
print(f"ring_triage={triage}")

print()
print("=== UIO_GET_STATUS status_telem (PARSE desync + NAL liveness) ===")
print("# path: core status_in 128b → Main shadow → push_frame SPI UIO 0x29")
print("# desync: raw[15] early/long/cause; raw[10..11]=desync_mb when sticky")
rc_raw, raw_txt = run_push(["--raw"])
telem = None
if rc_raw == 0 and raw_txt:
    lines = [ln for ln in raw_txt.splitlines() if ln.startswith("raw[")]
    raw_line = lines[-1] if lines else (raw_txt.splitlines()[-1] if raw_txt else "")
    print(raw_line)
    raw_bytes = parse_raw_line(raw_line)
    if raw_bytes:
        telem = decode_telem(raw_bytes)
else:
    print(f"raw: {raw_txt}")

rc_st, st_txt = run_push(["--status"])
kv = {}
if rc_st == 0 and st_txt:
    st_line = None
    for ln in st_txt.splitlines():
        if "nalu=" in ln or ln.startswith("status "):
            st_line = ln
            break
    if st_line is None:
        st_line = st_txt.splitlines()[-1]
    print(st_line)
    kv = parse_status_line(st_line)
else:
    print(f"status: {st_txt}")

try:
    # Prefer raw[15]/raw[10..11] decode (works even if deployed push_frame lacks desync= keys)
    if telem is not None:
        last_nal = telem["last_nal"]
        slice_type = telem["slice_type"]
        has_idr = telem["has_idr"]
        sps_v = telem["sps_valid"]
        pps_v = telem["pps_valid"]
        nalu = telem["nalu"]
        desync = telem["desync"]
        cause = telem["cause"]
        mb = telem["desync_mb"]
        recon_dbg = telem["recon_dbg"]
        early = telem["early"]
        longb = telem["long"]
    else:
        last_nal = int(kv.get("last_nal", "0"), 0)
        slice_type = int(kv.get("slice_type", "0"), 0)
        has_idr = int(kv.get("has_idr", "0"))
        sps_v = int(kv.get("sps_valid", "0"))
        pps_v = int(kv.get("pps_valid", "0"))
        nalu = int(kv.get("nalu", "0"))
        recon_dbg = int(kv.get("recon_dbg", "0"), 0)
        early = recon_dbg & 1
        longb = (recon_dbg >> 1) & 1
        cause = int(kv.get("cause", str((recon_dbg >> 4) & 0xF)))
        desync = int(kv.get("desync", str(int(bool(early or longb or cause)))))
        mb = int(kv.get("mb", "0"))
    label, seen = classify_nal(last_nal, slice_type, has_idr, sps_v, pps_v)
    print(f"NAL: nalu_count={nalu} last={label}(0x{last_nal:02x}) "
          f"flags_seen=[{','.join(seen) if seen else 'none'}] "
          f"(ABI: no SPS/PPS/IDR/P running counters in UIO; use flags+last_nal)")
    cause_name = {
        0: "none", 1: "early", 2: "long", 3: "skip_overrun",
        4: "cavlc", 5: "rbsp_overrun", 6: "syntax",
    }.get(cause, f"code{cause}")
    print(f"PARSE_desync sticky={desync} early={early} long={longb} "
          f"cause={cause}({cause_name}) desync_mb={mb} "
          f"raw15=0x{recon_dbg:02x} (raw[10..11]={'desync_mb' if desync else 'sps_mb_w/h'})")
    print("ARM_readable=YES via UIO_GET_STATUS / push_frame --raw|--status "
          "(raw[15] overlay + raw[10..11] desync_mb); PLXS (PLX page +0x100) is OSD-only")
except Exception as e:
    print(f"(parse telem failed: {e})")
    if not kv and telem is None:
        print("ARM_readable=CONDITIONAL — need push_frame for UIO; ring+PLXS/PLXD via /dev/mem")

print()
if os.path.isfile(STATUS_FILE):
    print(f"=== daemon snapshot ({STATUS_FILE}) ===")
    with open(STATUS_FILE, "r", encoding="utf-8", errors="replace") as f:
        print(f.read().rstrip())
else:
    print(f"(no {STATUS_FILE} yet — start misterplexd / play once)")

print()
print("Rule: static write→producer; write↑ read stuck→consumer; "
      "both↑ + desync→PARSE; both↑ clean bad pic→decode")
PY
}

# Prefer SSH when MISTER_HOST is set (build hosts often have unusable /dev/mem).
# Force local with FORCE_LOCAL=1 even if MISTER_HOST is set.
use_ssh=0
if [[ -n "${MISTER_HOST:-}" && "${FORCE_LOCAL:-0}" != "1" ]]; then
  use_ssh=1
elif [[ ! -e /dev/mem ]]; then
  use_ssh=0
fi

if [[ "$use_ssh" == "1" ]]; then
  sshpass -p "${MISTER_PASS:?set MISTER_PASS}" ssh -o StrictHostKeyChecking=no \
    -o ConnectTimeout=8 \
    "${MISTER_USER:-root}@${MISTER_HOST}" \
    "STATUS_FILE=$(printf %q "$STATUS_FILE") PUSH_FRAME=$(printf %q "$PUSH_FRAME") SAMPLE_SLEEP_S=$(printf %q "$SAMPLE_SLEEP_S") PLX_PAGE=$(printf %q "$PLX_PAGE") PLX_NOTE=$(printf %q "$PLX_NOTE") bash -s" <<'EOS'
set -euo pipefail
STATUS_FILE="${STATUS_FILE:-/media/fat/misterplex/bitstream_ring.status}"
PUSH_FRAME="${PUSH_FRAME:-/media/fat/misterplex/bin/push_frame}"
SAMPLE_SLEEP_S="${SAMPLE_SLEEP_S:-0.35}"
# shellcheck disable=SC1091
# Inline same probe body (remote has no repo copy required)
python3 - <<'PY'
import os, struct, subprocess, time, mmap

STATUS_FILE = os.environ.get("STATUS_FILE", "/media/fat/misterplex/bitstream_ring.status")
PUSH_FRAME = os.environ.get("PUSH_FRAME", "/media/fat/misterplex/bin/push_frame")
SLEEP = float(os.environ.get("SAMPLE_SLEEP_S", "0.35"))

CTRL_PHYS = 0x30140000
READ_PHYS = 0x30140008
ERR_PHYS  = 0x30140010
STAT0_PHYS = 0x30140018
STAT6_PHYS = 0x30140048
# mailbox_abi_spec.hpp offsets on the build's PLX page (resolved from PLX_BUILD's row
# by the bash wrapper); no page, no PLX reads.
PLX_PAGE = int(os.environ.get("PLX_PAGE", "") or "0", 0)
PLX_NOTE = os.environ.get("PLX_NOTE", "")
PLXS_OFF = 0x100
PLXD_OFF = 0x128

def u64(fd, phys):
    page = phys & ~0xFFF
    off = phys - page
    mm = mmap.mmap(fd, 0x1000, offset=page, access=mmap.ACCESS_READ)
    try:
        return struct.unpack_from("<Q", mm, off)[0]
    finally:
        mm.close()

def mag_s(m):
    bs = bytes([(m >> (8 * i)) & 0xFF for i in range(4)])
    if all(32 <= b < 127 for b in bs):
        return bs.decode("ascii")
    return "????"

def sample_ring(fd):
    ctrl = u64(fd, CTRL_PHYS)
    rd = u64(fd, READ_PHYS)
    err = u64(fd, ERR_PHYS)
    st0 = u64(fd, STAT0_PHYS)
    st6 = u64(fd, STAT6_PHYS)
    plxs = u64(fd, PLX_PAGE + PLXS_OFF) if PLX_PAGE else 0
    plxd = u64(fd, PLX_PAGE + PLXD_OFF) if PLX_PAGE else 0
    return {
        "cm": ctrl & 0xFFFFFFFF,
        "prod": (ctrl >> 32) & 0x7FFFFFFF,
        "epoch": (ctrl >> 63) & 1,
        "rm": rd & 0xFFFFFFFF,
        "cons": (rd >> 32) & 0xFFFFFFFF,
        "em": err & 0xFFFFFFFF,
        "ring_level": ((st0 >> 32) & 0xFFFFFFFF) if (st0 & 0xFFFFFFFF) == 0x504C5854 else None,
        "st6": st6,
        "plxs": plxs,
        "plxd": plxd,
    }

def classify_nal(last_nal, slice_type, has_idr, sps_valid, pps_valid):
    nt = last_nal & 0x1F
    label = {1: "nonIDR_slice", 5: "IDR", 6: "SEI", 7: "SPS", 8: "PPS", 9: "AUD"}.get(nt, f"type{nt}")
    st = slice_type
    if nt == 1:
        if st in (0, 5):
            label = "P"
        elif st in (2, 7):
            label = "I_slice"
        else:
            label = f"slice(st={st})"
    seen = []
    if sps_valid: seen.append("SPS")
    if pps_valid: seen.append("PPS")
    if has_idr: seen.append("IDR")
    if nt == 1 and st in (0, 5): seen.append("P")
    return label, seen

def parse_status_line(line):
    out = {}
    for tok in line.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out

def parse_raw_line(line):
    # "raw[N]: aa bb ... lo=0x...."
    if ":" not in line:
        return None
    body = line.split(":", 1)[1]
    hexes = []
    for tok in body.split():
        if tok.startswith("lo="):
            break
        try:
            hexes.append(int(tok, 16))
        except ValueError:
            continue
    if len(hexes) < 16:
        return None
    return hexes[:16]

def decode_telem(raw):
    # status_telemetry.hpp / Plex.sv layout
    flags = raw[2]
    last_nal = raw[3]
    nalu = raw[4] | (raw[5] << 8)
    mb0 = raw[6]
    slice_type = raw[7]
    recon_dbg = raw[15]
    early = recon_dbg & 1
    longb = (recon_dbg >> 1) & 1
    cause = (recon_dbg >> 4) & 0xF
    desync = bool(early or longb or cause)
    mb = raw[10] | (raw[11] << 8) if desync else 0
    return {
        "flags": flags, "last_nal": last_nal, "nalu": nalu, "mb0": mb0,
        "slice_type": slice_type, "recon_dbg": recon_dbg,
        "early": early, "long": longb, "cause": cause, "desync": int(desync),
        "desync_mb": mb,
        "has_idr": (flags >> 4) & 1,
        "sps_valid": (flags >> 6) & 1,
        "pps_valid": (flags >> 7) & 1,
        "res_csum": raw[13], "recon_sig": raw[14],
    }

def run_push(args):
    if not os.path.isfile(PUSH_FRAME) or not os.access(PUSH_FRAME, os.X_OK):
        return None, f"(no executable {PUSH_FRAME})"
    try:
        p = subprocess.run([PUSH_FRAME] + args, capture_output=True, text=True, timeout=8)
        return p.returncode, ((p.stdout or "") + (p.stderr or "")).strip()
    except Exception as e:
        return None, f"(push_frame failed: {e})"

fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
try:
    a = sample_ring(fd)
    time.sleep(SLEEP)
    b = sample_ring(fd)
finally:
    os.close(fd)

d_prod = (b["prod"] - a["prod"]) & 0x7FFFFFFF
d_cons = (b["cons"] - a["cons"]) & 0xFFFFFFFF
print("=== DDR bitstream ring (two samples) ===")
print(f"CTRL@0x30140000 magic={mag_s(a['cm'])}/→{mag_s(b['cm'])} producer_bytes t0={a['prod']} t1={b['prod']} Δ={d_prod} epoch={b['epoch']}")
print(f"READ@0x30140008 magic={mag_s(a['rm'])}/→{mag_s(b['rm'])} consumer_bytes t0={a['cons']} t1={b['cons']} Δ={d_cons}")
print(f"ERR @0x30140010 magic={mag_s(b['em'])}(0x{b['em']:08x})")
if b["ring_level"] is not None:
    print(f"STAT0 ring_level={b['ring_level']}")
st6 = b["st6"]
if (st6 & 0xFFFFFFFF) == 0x504C5851:
    ds = (st6 >> 32) & 0xFFFFFFFF
    flags = ds & 0xFFFF
    print(f"STAT6/PLXQ transport desync={ (flags>>10)&1 } fatal={ (flags>>11)&1 } desync_count={ (ds>>16)&0xFFFF }")

plxs_lo = b["plxs"] & 0xFFFFFFFF
plxs_mid = (b["plxs"] >> 32) & 0xFFFF
plxs_seq = (b["plxs"] >> 48) & 0xFFFF
plxd_lo = b["plxd"] & 0xFFFFFFFF
plxd_hi = (b["plxd"] >> 32) & 0xFFFFFFFF
print()
print("=== Frame-store mailboxes (DDR, not full status_telem) ===")
if PLX_NOTE:
    print(f"PLX page: {PLX_NOTE}")
if not PLX_PAGE:
    if not PLX_NOTE:
        print("PLX page unset: PLXS/PLXD not read (set PLX_BUILD=0xID of the loaded build; its fpga_build_ids.hpp row gives the page)")
else:
    hb = "live" if (plxs_lo == 0x504C5853 and (a["plxs"] >> 32) != (b["plxs"] >> 32)) else "NOT MOVING (not the loaded build's page, or no core)"
    print(f"PLXS@0x{PLX_PAGE + PLXS_OFF:08X} magic={mag_s(plxs_lo)}(0x{plxs_lo:08x}) osd=0x{plxs_mid:04x} seq={plxs_seq}  (hi word @0x{PLX_PAGE + PLXS_OFF + 4:08X}) heartbeat={hb}")
    print(f"PLXD@0x{PLX_PAGE + PLXD_OFF:08X} magic={mag_s(plxd_lo)}(0x{plxd_lo:08x}) hi@0x{PLX_PAGE + PLXD_OFF + 4:08X}=0x{plxd_hi:08x}  (bank-release ACK)")

cm, prod, rm, cons = b["cm"], b["prod"], b["rm"], b["cons"]
if cm == 0x504C5844:
    triage = "STARVED_producer_dormant_PLXD"
elif cm != 0x504C5842:
    triage = "STARVED_no_PLXB_ctrl"
elif prod == 0:
    triage = "STARVED_no_writes"
elif d_prod == 0 and d_cons == 0 and prod > 0:
    triage = "IDLE_ptrs_static_after_feed"
elif d_prod > 0 and d_cons == 0:
    triage = "BROKEN_consumer_stuck"
elif rm != 0x504C5852:
    triage = "BROKEN_no_PLXR_read"
elif cons == 0 and prod > 0:
    triage = "BROKEN_consumer_never_started"
elif cons + 4096 < prod:
    triage = "CONSUMER_LAGGING"
elif d_prod > 0 and d_cons > 0:
    triage = "FEED_LIVE_both_advancing"
else:
    triage = "FEED_OK_check_decode"
print(f"ring_triage={triage}")

print()
print("=== UIO_GET_STATUS status_telem (PARSE desync + NAL liveness) ===")
print("# path: core status_in 128b → Main shadow → push_frame SPI UIO 0x29")
print("# desync: raw[15] early/long/cause; raw[10..11]=desync_mb when sticky")
rc_raw, raw_txt = run_push(["--raw"])
telem = None
if rc_raw == 0 and raw_txt:
    lines = [ln for ln in raw_txt.splitlines() if ln.startswith("raw[")]
    raw_line = lines[-1] if lines else raw_txt.splitlines()[-1]
    print(raw_line)
    raw_bytes = parse_raw_line(raw_line)
    if raw_bytes:
        telem = decode_telem(raw_bytes)
else:
    print(f"raw: {raw_txt}")

rc_st, st_txt = run_push(["--status"])
kv = {}
if rc_st == 0 and st_txt:
    st_line = None
    for ln in st_txt.splitlines():
        if "nalu=" in ln or ln.startswith("status "):
            st_line = ln
            break
    if st_line is None:
        st_line = st_txt.splitlines()[-1]
    print(st_line)
    kv = parse_status_line(st_line)
else:
    print(f"status: {st_txt}")

try:
    if telem is not None:
        last_nal = telem["last_nal"]; slice_type = telem["slice_type"]
        has_idr = telem["has_idr"]; sps_v = telem["sps_valid"]; pps_v = telem["pps_valid"]
        nalu = telem["nalu"]; desync = telem["desync"]; cause = telem["cause"]
        mb = telem["desync_mb"]; recon_dbg = telem["recon_dbg"]
        early = telem["early"]; longb = telem["long"]
    else:
        last_nal = int(kv.get("last_nal", "0"), 0)
        slice_type = int(kv.get("slice_type", "0"), 0)
        has_idr = int(kv.get("has_idr", "0"))
        sps_v = int(kv.get("sps_valid", "0")); pps_v = int(kv.get("pps_valid", "0"))
        nalu = int(kv.get("nalu", "0"))
        recon_dbg = int(kv.get("recon_dbg", "0"), 0)
        early = recon_dbg & 1; longb = (recon_dbg >> 1) & 1
        cause = int(kv.get("cause", str((recon_dbg >> 4) & 0xF)))
        desync = int(kv.get("desync", str(int(bool(early or longb or cause)))))
        mb = int(kv.get("mb", "0"))
    label, seen = classify_nal(last_nal, slice_type, has_idr, sps_v, pps_v)
    print(f"NAL: nalu_count={nalu} last={label}(0x{last_nal:02x}) flags_seen=[{','.join(seen) if seen else 'none'}] (ABI: no SPS/PPS/IDR/P running counters in UIO; use flags+last_nal)")
    cause_name = {0: "none", 1: "early", 2: "long", 3: "skip_overrun", 4: "cavlc", 5: "rbsp_overrun", 6: "syntax"}.get(cause, f"code{cause}")
    print(f"PARSE_desync sticky={desync} early={early} long={longb} cause={cause}({cause_name}) desync_mb={mb} raw15=0x{recon_dbg:02x}")
    print("ARM_readable=YES via UIO_GET_STATUS / push_frame --raw|--status (raw[15]+raw[10..11]); PLXS (PLX page +0x100) OSD-only")
except Exception as e:
    print(f"(parse telem failed: {e})")
    if not kv and telem is None:
        print("ARM_readable=CONDITIONAL — need push_frame for UIO; ring+PLXS/PLXD via /dev/mem")

print()
if os.path.isfile(STATUS_FILE):
    print(f"=== daemon snapshot ({STATUS_FILE}) ===")
    print(open(STATUS_FILE, "r", encoding="utf-8", errors="replace").read().rstrip())
else:
    print(f"(no {STATUS_FILE} yet — start misterplexd / play once)")
print()
print("Rule: static write→producer; write↑ read stuck→consumer; both↑ + desync→PARSE; both↑ clean bad pic→decode")
PY
EOS
else
  remote_probe
fi
