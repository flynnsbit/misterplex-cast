#!/usr/bin/env python3
"""DAEMON-FPS-SPEC-1002 T2: replay resolvePlayTarget at a base ref and at this tree
against the same fake PMS over the full env matrix, and check the rate rules.

  run_fps_replay.py [--base-ref f6526d96] [--out DIR] [--quick] [--remove-base-worktree]

Builds tests/unit/test_resolve_fps_replay.cpp twice with identical flags: against a
clean detached worktree of --base-ref and against this tree. Each case runs both
binaries with a scrubbed env (PATH + LC_ALL + case vars only). The stdout JSON, the
stderr and the fake-PMS request-log segment are compared after session ids are
renumbered by first appearance (mplex-S1, ...) and the token is redacted.

Pass (exit 0) needs all of:
  (a) every 480p case is byte-identical to the base (JSON, stderr, request log);
  (b) 240p/480i x {23.976, 24, 29.97, 30/1}: noproxy requests the NTSC content
      clock (24/1→24000/1001, 30/1→30000/1001); proxy still &fps=src with the
      source rate as the daemon's expected rate (60-family halved);
  (c) 240p/480i may remap 24/1 and 30/1 to NTSC clocks (Goal D cadence); PAL
      still refuses as not 24/30/60-family;
  (e) the player's build-ID gate (fpga_build_ids.hpp, harness --caps-build-id): with
      prod's CAPS ID 0x4800F36A and the RC IDs 0x48004D40 / 0x48005D40 (and the N5
      clk_dec steps 0x48005D45 / 0x48005D50) every 480p case is still byte-identical to
      the base;
      0x2400F36A (240p raster) and 0x4801F40A (true 480i) change nothing on their
      modes; unrecognised IDs are refused (exit 3, "ERROR FPGA H.264: unrecognised
      FPGA build ...; refusing to play, no ARM-present fallback"), and the retired
      0x4801F36A with "retired FPGA build ..." naming 42da8d23/ad047c7a/cc7b6e66;
  (g) 240p glass requests (--glass-ref, default dd0bcb08, the daemon of the 5554d013
      glass): on the staged 240p conf, under CAPS 0x2400F36A, Myth (rk 102,
      30000/1001), TNG (rk 101, 24000/1001) and bbb30 (rk 103, 30/1) make the same
      fake-PMS requests and the same ladder URL as the glass daemon, except that
      &fps=<numeric> becomes &fps=src, and the expected rate stays that numeric rate;
  plus the per-family rules:
  * every 240p/480i proxy URL carries &fps=src (DAEMON-FPS-SRC-ALWAYS-1003): the proxy
    probes and keeps the source rate, and a numeric metadata rate stays the daemon's
    expected rate (60-family halved), so nothing on the ladder is remapped;
  * 24-family 240p/480i cases are byte-identical, except on the new raster confs
    below and where the base already took the ladder (WAN, 480i pad ladder): there
    only the &fps= value changes; fps-less titles go to the ladder with &fps=src when
    a proxy is set (rate 0/0, never the 24000/1001 guess; the proxy probes) and are
    refused without one;
  * true 480i (MPX_FPGA_MODE=480i + FPGA_480P_CODED_W=640): proxy ladders are coded
    640x480; without a proxy it refuses (never a 624-wide picture);
  * 240p on the 480p ABI (MPX_FPGA_MODE=240p on a 480 conf ladder): every ladder is
    the 320x240 240crf16 rung (hq, no allidr);
  * 30/60-family with a proxy: the ladder with &fps=src, expecting the source (60
    halved) rate. 240p uses the 320x240 rung; 480i differs from the base only in the rate;
  * 24/1 and 30/60-family without a proxy: PMS universal at 24000/1001 or
    30000/1001 (Goal D NTSC clocks). Unchanged apart from the rate are the
    base's own remote fallbacks: the direct Part, and the not-H.264 refusal;
  * 25/other rates: an explicit refusal (no remap);
  * every MiSTerPlex-FPGA-* profile named exists (fleet film set or repo src30 XML),
    and PMS universal names the case's exact prototype/raster/filter/rate profile;
  * staged confs (assets/confs/staged/*.overlay.conf via scripts/stage_raster_conf.py
    on a 933a4104 stand-in): main.cpp's conf->env rules give MPX_FPGA_MODE and no
    MPX_PRESENT_480I; every source is laddered at the coded size (480i 640x480, 240p
    320x240, no allidr) with &fps=src and the source rate expected; the bank is the FPGA 640x480 store at
    0x300FF000; forbidden keys are refused; the bare stand-in is identical to 8d8faaf1.
Never contacts the proxy address (TEST-NET-1) or any non-loopback host.
"""

import argparse
import glob
import hashlib
import itertools
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
HARNESS = os.path.join(ROOT, "tests", "unit", "test_resolve_fps_replay.cpp")
FAKE_PMS = os.path.join(HERE, "fake_pms.py")
TOKEN = "FAKE-TOKEN-0123456789"
PROXY = "http://192.0.2.1:18765"  # RFC 5737 TEST-NET-1: never the live ladder proxy
CXXFLAGS = ["-std=c++17", "-O2", "-Wall", "-Wextra"]

# mode -> (env, weak profile, NTSC raster?)
MODES = {
    "480p": ({}, "480p", False),
    "480p-env": ({"MPX_FPGA_MODE": "480p"}, "480p", False),
    "240p": ({}, "240p", True),
    "240p-env480": ({"MPX_FPGA_MODE": "240p"}, "480p", True),
    "480i": ({"MPX_FPGA_MODE": "480i"}, "480p", True),
    "480i-present": ({"MPX_PRESENT_480I": "1"}, "480p", True),
    # True 480i core (PLEX_DECODE_CODED_640): the staged conf's TRUE640 opt-in. Only an
    # exact 640 limit plays; "480i" (key missing) and "480i-624" must refuse.
    "480i-640": ({"MPX_FPGA_MODE": "480i", "MPX_FPGA_480P_CODED_W": "640"}, "480p", True),
    "480i-624": ({"MPX_FPGA_MODE": "480i", "MPX_FPGA_480P_CODED_W": "624"}, "480p", True),
}
# main.cpp doPlay AU-safe clamp for weakForPlay (start / seek), per tier.
WEAK_ARGS = {
    ("480p", False): ["--br", "2500", "--q", "40", "--decode", "640x480"],
    ("480p", True): ["--br", "1000", "--q", "30", "--decode", "640x480"],
    ("240p", False): ["--br", "1000", "--q", "40", "--decode", "320x240"],
    ("240p", True): ["--br", "900", "--q", "28", "--decode", "320x240"],
}
PROXIES = {"none": {}, "mpx": {"MPX_REMOTE_LADDER_PROXY": PROXY},
           "alt": {"REMOTE_LADDER_PROXY": PROXY}}
PROTOTYPES = ["ip", "idr"]
FILTERS = ["unset", "off"]
OFFSETS = [0, 480000]
NETS = ["lan", "wan"]
B_SOURCES = ("24000/1001", "24/1", "30000/1001", "30/1")
# (e) Player build-ID gate (host/libmisterplex/fpga_build_ids.hpp), applied by the
# patch harness with --caps-build-id: prod's CAPS ID and the RC IDs (48004D40, N5's
# 48005D40 and its clk_dec 45/50 MHz steps 48005D45/48005D50) on every 480p case, the
# 240p/480i IDs on their modes (240p: pre-OSD 2400F36A and the OSD line 2400F40B),
# and unrecognised IDs that must be refused (an unknown ID, the Plex.qsf default,
# the old 480i ID, one-bit neighbours, 4801F40B on hold).
GATE_KNOWN = {"480p": ("0x4800F36A", "0x48004D40", "0x48005D40", "0x48005D45", "0x48005D50"),
              "480p-env": ("0x4800F36A", "0x48004D40", "0x48005D40", "0x48005D45", "0x48005D50"),
              "240p-env480": ("0x2400F36A", "0x2400F40B"), "480i-640": ("0x4801F40A",)}
GATE_UNKNOWN = ["0xDEADBEEF", "0xE41B0437", "0x48015C03", "0x4800F36B", "0x4801F40B", "0x48005D55"]
# Retired IDs: refused with their own reason (4801F36A: two status layouts behind one ID).
GATE_RETIRED = {"0x4801F36A": ("42da8d23", "ad047c7a", "cc7b6e66")}

FPS_LINE = re.compile(r"misterplexd: FPGA (remap source fps|\S+ raster fps (passthrough|halve)|"
                      r"\S+ raster fps=src to the ladder proxy)")
REMAP_LINE = "misterplexd: FPGA remap source fps"
GUESS_LINE = "misterplexd: FPGA fps meta missing; default 24000/1001"
SESSION = re.compile(r"mplex-[0-9a-f]{1,16}")
FPGA_PROFILE = re.compile(r"MiSTerPlex-FPGA-[A-Za-z0-9-]*[A-Za-z0-9]")
PROFILE_HEADER = re.compile(r"X-Plex-Client-Profile-Name: (\S+)")
LADDER_GEOM = re.compile(r"&w=(\d+)&h=(\d+)&br=(\d+)")
# Film-rate FPGA profiles installed on the lab PMS (fleet generate_profiles.py set).
FLEET_FILM_PROFILES = {f"MiSTerPlex-FPGA-{p}-{t}-{r}-filter-{f}"
                       for p in ("IP", "IDR") for t in ("240p", "480")
                       for r in ("24", "23976") for f in ("on", "off")}
REPO_SRC30_PROFILES = {os.path.basename(x)[:-4] for x in glob.glob(
    os.path.join(ROOT, "assets", "plex-profiles", "MiSTerPlex-FPGA-*-src30-*.xml"))}


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout.strip()


def fps_family(fps):
    if fps in ("24000/1001", "24/1"):
        return "24"
    if fps in ("30000/1001", "30/1"):
        return "30"
    if fps in ("60000/1001", "60/1"):
        return "60"
    return fps or "missing"


def halved(src):
    return {"60000/1001": "30000/1001", "60/1": "30000/1001"}.get(src, src)


def ntsc_content_clock(src):
    return {"24/1": "24000/1001", "30/1": "30000/1001",
            "60/1": "30000/1001", "60000/1001": "30000/1001"}.get(src, src)


def expected_profile(case):
    """The PMS client profile the case must name: none (""). NO-REQUIRED-PROFILE (owner 1011 20:59 CT,
    docs/plans/NO-REQUIRED-PROFILE.md): by default no X-Plex-Client-Profile-Name is sent to any PMS; the
    Profile-Extra carries the limits. The per-variant name (expected_variant) is opt-in
    (SEND_PROFILE_NAME=variant) and covered by test_profile_name_fixed."""
    return ""


def expected_variant(case):
    """The internal stream-contract variant: src30 for 30-family, else the film one."""
    proto = "IDR" if case["prototype"] == "idr" else "IP"
    filt = "off" if case["filter"] == "off" else "on"
    is240 = case["mode"].startswith("240p")
    if fps_family(case["src_fps"]) in ("30", "60"):
        return f"MiSTerPlex-FPGA-{proto}-{'240p' if is240 else '480i'}-src30-filter-{filt}"
    rate = {"24000/1001": "23976", "24/1": "24"}.get(case["src_fps"], "")
    return f"MiSTerPlex-FPGA-{proto}-{'240p' if is240 else '480'}-{rate}-filter-{filt}"


def decision_profiles(req_log):
    return set(FPGA_PROFILE.findall(" ".join(
        l for l in req_log.splitlines() if "/universal/decision?" in l)))


def url_rate(fps):
    num, den = fps.split("/")
    return num if den == "1" else f"{num}%2F{den}"


def route_of(j):
    if not j.get("ok"):
        return "fail"
    p = j.get("playable", "")
    if "/ladder.ts?" in p:
        return "proxy"
    if "/transcode/universal/start" in p:
        return "pms-universal"
    if "/library/parts/" in p:
        return "direct-part"
    return "other"


def ladder_geom(playable):
    m = LADDER_GEOM.search(playable) if "/ladder.ts?" in playable else None
    return f"{m[1]}x{m[2]}@{m[3]}{' hq' if '&hq=1' in playable else ''}" if m else ""


def ladder_240(playable):
    """The 240p rung: 320x240, hq=1, br within the 240p cap."""
    m = LADDER_GEOM.search(playable)
    return bool(m) and (m[1], m[2]) == ("320", "240") and int(m[3]) <= 3000 and \
        "&hq=1" in playable


def mask_fps(s):
    s = re.sub(r"([?&]fps=)[^&\"\s|]*", r"\1FPS", s)
    return re.sub(r"(videoFrameRate=)[^&\"\s|]*", r"\1FPS", s)


def same_except_rate(b, p, profiles=False):
    """Base and patch outputs are equal once rates (and optionally profile names) are masked."""
    def norm(s):
        return mask_fps(FPGA_PROFILE.sub("PROFILE", s) if profiles else s)
    bj, pj = json.loads(b[1]), json.loads(p[1])
    for j in (bj, pj):
        j.pop("fpsNum")
        j.pop("fpsDen")
    bj = {k: norm(v) if isinstance(v, str) else v for k, v in bj.items()}
    pj = {k: norm(v) if isinstance(v, str) else v for k, v in pj.items()}
    keep = lambda e: [l for l in e.splitlines() if not FPS_LINE.match(l)]
    return bj == pj and keep(b[2]) == keep(p[2]) and norm(b[0]) == norm(p[0])


def explicit_refusal(pj, stderr):
    return not pj["ok"] and ("refusing (no remap)" in pj["detail"] or
                             "refusing (no 624 fallback)" in pj["detail"]) and \
        "misterplexd: ERROR " in stderr


def ladder_wh(playable):
    m = LADDER_GEOM.search(playable)
    return (m[1], m[2]) if m else None


def expect_ntsc(case):
    """Expected patch outcome on a 240p/480i case: (kind, rate) with kind in
    identical / refuse / proxy / fallback (noproxy NTSC content clock)."""
    fam = fps_family(case["src_fps"])
    proxy = case["proxy"] != "none"
    mode = case["mode"]
    if mode in ("480i", "480i-624"):
        return ("refuse-width", None)  # MPX_FPGA_MODE=480i without a 640 limit
    if fam == "missing":
        return ("proxy", "src") if proxy else ("refuse", None)
    if fam not in ("24", "30", "60") or (mode == "480i-640" and not proxy):
        return ("refuse", None)
    if proxy:
        return ("proxy", ntsc_content_clock(case["src_fps"]))
    if fam == "24" and case["src_fps"] == "24000/1001":
        return ("identical", case["src_fps"])
    return ("fallback", ntsc_content_clock(case["src_fps"]))


def check_case(case, b, p):
    """Rule check for one case. Returns a list of violations (empty = pass)."""
    bj, pj = json.loads(b[1]), json.loads(p[1])
    if not case["ntsc"]:
        return [] if b == p else ["480p not byte-identical"]
    reasons = []
    if REMAP_LINE in p[2] and fps_family(case["src_fps"]) not in ("24", "30", "60"):
        reasons.append("(c) remap line on an NTSC raster outside 24/30/60")
    if GUESS_LINE in p[2]:
        reasons.append("24000/1001 guess on an NTSC raster")
    unknown = set(FPGA_PROFILE.findall(p[0] + pj["httpHeaders"])) - FLEET_FILM_PROFILES - \
        REPO_SRC30_PROFILES
    if unknown:
        reasons.append("unknown PMS profile " + ",".join(sorted(unknown)))
    meta = lambda r: [l for l in r.splitlines() if "/transcode/universal/" not in l]
    if meta(b[0]) != meta(p[0]):
        reasons.append("metadata requests differ")
    univ = [l for l in p[0].splitlines() if "/transcode/universal/" in l]
    rb, rp = route_of(bj), route_of(pj)
    kind, want = expect_ntsc(case)
    mode = case["mode"]
    if kind == "identical":
        # Unchanged, except where the base already took the ladder proxy (WAN, the 480i
        # pad ladder): every 240p/480i proxy URL now asks for &fps=src, and the
        # metadata rate stays the expected rate (DAEMON-FPS-SRC-ALWAYS-1003).
        if rb == "proxy":
            if rp != "proxy" or "&fps=src&" not in pj["playable"] + "&":
                reasons.append(f"24-family proxy route {rp} without &fps=src")
            if f"{pj['fpsNum']}/{pj['fpsDen']}" != f"{bj['fpsNum']}/{bj['fpsDen']}":
                reasons.append("24-family proxy expected rate changed")
            if not same_except_rate(b, p):
                reasons.append("24-family proxy route differs beyond the &fps= value")
        elif b != p:
            reasons.append("24-family not byte-identical")
    elif kind == "refuse-width":
        if not explicit_refusal(pj, p[2]) or ", not 640 (set FPGA_480P_CODED_W=640)" not in pj["detail"]:
            reasons.append("480i without a 640 coded limit not refused by the width guard")
        if univ:
            reasons.append("refused case still called PMS universal")
    elif kind == "refuse":
        if not explicit_refusal(pj, p[2]):
            reasons.append(f"{case['src_fps'] or 'fps-less'} not refused with an explicit error")
        if univ:
            reasons.append("refused case still called PMS universal")
    elif kind == "proxy":
        got = f"{pj['fpsNum']}/{pj['fpsDen']}"
        if got != ("0/0" if want == "src" else want):
            reasons.append(f"fps {got} != {want}")
        if rp != "proxy" or "&fps=src&" not in pj["playable"] + "&":
            reasons.append(f"proxy route {rp} without &fps=src")
        if univ:
            reasons.append("proxy route still called PMS universal")
        wh = ladder_wh(pj["playable"])
        if mode.startswith("240p"):
            if rp == "proxy" and not ladder_240(pj["playable"]):
                reasons.append(f"240p ladder {wh} is not the 320x240 rung")
        elif mode == "480i-640":
            if wh != ("640", "480"):
                reasons.append(f"true-480i ladder {wh} is not coded 640x480")
        elif wh != ("624", "480"):
            reasons.append(f"480i pad ladder {wh} is not 624x480")
        elif want != "src" and rb == "proxy" and not same_except_rate(b, p):
            reasons.append("480i ladder differs beyond the rate")
    else:  # 30-family, no proxy: src30 PMS fallback, or the base's own fallback
        if f"{pj['fpsNum']}/{pj['fpsDen']}" != want and rp != "fail":
            reasons.append(f"fps {pj['fpsNum']}/{pj['fpsDen']} != {want}")
        if rp in ("pms-universal", "direct-part"):
            prof = PROFILE_HEADER.findall(pj["httpHeaders"])
            if rp == "pms-universal" and (
                    prof[:1] != ([expected_profile(case)] if expected_profile(case) else []) or
                    decision_profiles(p[0]) - {expected_profile(case)}):
                reasons.append(f"PMS fallback profile {prof} / decision "
                               f"{sorted(decision_profiles(p[0]))} != {expected_profile(case)}")
            if rp == "pms-universal" and case["net"] == "lan" and not (
                    f"videoFrameRate={url_rate(want)}" in pj["playable"] and
                    any(f"videoFrameRate={url_rate(want)} " in l + " " or
                        f"videoFrameRate={url_rate(want)}&" in l
                        for l in univ if "/decision?" in l)):
                reasons.append("LAN PMS fallback without videoFrameRate=source")
            # direct-part: the pre-existing remote fallback (source file, source rate).
            if rb != rp or not same_except_rate(b, p, profiles=True):
                reasons.append(f"{rp} fallback differs beyond rate/profile")
        elif rp == "fail":
            # Explicit refusal, or the base's own refusal unchanged apart from the rate.
            if not (explicit_refusal(pj, p[2]) or
                    (rb == "fail" and same_except_rate(b, p, profiles=True))):
                reasons.append("refusal without an explicit error")
            if rb != "fail":
                reasons.append(f"refused where the base played ({rb})")
        else:
            reasons.append(f"unexpected 30-family route {rp}")
    return reasons


def source_rate_requested(case, p):
    """(b): True/False when the case played; None when it was refused."""
    pj = json.loads(p[1])
    if not pj["ok"]:
        return None
    src = case["src_fps"]
    want = ntsc_content_clock(src)
    if f"{pj['fpsNum']}/{pj['fpsDen']}" != want:
        return False
    rp = route_of(pj)
    if rp == "proxy":
        # The proxy keeps the probed source rate; the daemon expects the source rate.
        return "&fps=src&" in pj["playable"] + "&"
    if rp == "direct-part":
        return True
    if rp != "pms-universal":
        return False
    prof = (PROFILE_HEADER.findall(pj["httpHeaders"]) or [""])[0]
    # The exact prototype/raster/filter/rate profile, in the headers and the decision.
    prof_ok = prof == expected_profile(case) and \
        not (decision_profiles(p[0]) - {expected_profile(case)})
    if case["net"] == "wan":
        return prof_ok
    dec = [l for l in p[0].splitlines() if "/universal/decision?" in l]
    tag = f"videoFrameRate={url_rate(want)}"
    return prof_ok and (tag + "&" in pj["playable"] + "&") and \
        any(tag + "&" in l.split(" ")[1] + "&" for l in dec)


def conf_env_keys():
    """Keys main.cpp copies from the conf into the environment (conf wins)."""
    with open(os.path.join(ROOT, "arm", "misterplexd", "main.cpp"), encoding="utf-8") as fh:
        m = re.search(r'for \(const char\* key : \{("MPX_VIDEO_BACKEND".*?)\}\)', fh.read(), re.S)
    return re.findall(r'"([A-Z0-9_]+)"', m.group(1)) if m else []


STAGE_SCRIPT = os.path.join(ROOT, "scripts", "stage_raster_conf.py")
OVERLAYS = {"480i": os.path.join(ROOT, "assets", "confs", "staged",
                                 "misterplex-480i-true.overlay.conf"),
            "240p": os.path.join(ROOT, "assets", "confs", "staged",
                                 "misterplex-240p.overlay.conf")}
# source rating key -> rate the ladder must carry (59.94 halves; fps-less probes).
CONF_SOURCES = {"101": "24000/1001", "102": "30000/1001", "103": "30/1",
                "105": "30000/1001", "109": "src"}
# main.cpp lines the conf->env mirror below depends on (static guard).
MAIN_CONF_RULES = ('loadConf(confPath, "FPGA_480P_CODED_W")',
                   'setenv("MPX_FPGA_480P_CODED_W", v.c_str(), 0)',
                   'loadConf(confPath, "DISPLAY_RES")', 'loadConf(confPath, "CONTENT_RES")',
                   'if (dlab == "480i" || clab == "480i")',
                   'loadConf(confPath, "REMOTE_LADDER_PROXY")')


def prod_standin(pms_lan):
    """Stand-in for prod 933a4104, whose bytes are not available offline: the tail
    AV50-PROD-1002 shows (token there redacted, fake here) plus the FPGA backend."""
    return "\n".join([
        "# stand-in for prod /media/fat/linux/misterplex.conf 933a4104 (not its bytes)",
        "MPX_VIDEO_BACKEND=fpga-h264", "MPX_H264_PROTOTYPE=ip", "DECODE=640x480",
        f"PLEX_BASE={pms_lan}", f"PLEX_TOKEN={TOKEN}",
        f"MPX_REMOTE_LADDER_PROXY={PROXY}", "AUDIO_DELAY_MS=50"]) + "\n"


def daemon_env_from_conf(text, env_keys):
    """main.cpp's conf->env derivation for a conf file (first KEY= line wins)."""
    lines = text.splitlines()

    def load(k):
        for l in lines:
            if l and not l.startswith("#") and l.startswith(k + "="):
                return l[len(k) + 1:]
        return ""
    env = {k: load(k) for k in env_keys if load(k)}
    if load("FPGA_480P_CODED_W"):
        env["MPX_FPGA_480P_CODED_W"] = load("FPGA_480P_CODED_W")
    proxy = load("REMOTE_LADDER_PROXY") or load("MPX_REMOTE_LADDER_PROXY")
    if proxy:
        env["MPX_REMOTE_LADDER_PROXY"] = proxy
    if "MPX_PRESENT_480I" not in env and "480i" in (load("DISPLAY_RES"), load("CONTENT_RES")):
        env["MPX_PRESENT_480I"] = "1"
    return env


def run_conf_case(binary, pms, env, rk, extra=None):
    full = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LC_ALL": "C"}
    full.update(env)
    off = pms.size()
    p = subprocess.run([binary, "--base", pms.lan, "--token", TOKEN,
                        "--key", f"/library/metadata/{rk}", "--offset-ms", "0",
                        "--weak", "480p", "--bank", "640x480"] + WEAK_ARGS[("480p", False)] +
                       (extra or []),
                       env=full, capture_output=True, text=True, timeout=120)
    if p.returncode != 0:
        raise RuntimeError(f"{binary} rc={p.returncode} conf case {rk}: {p.stderr}")
    return normalize(pms.read_from(off), p.stdout, p.stderr)


# (g) The 5554d013 + dd0bcb08 240p glass titles, by fake-PMS source rate.
GLASS240 = (("102", "Myth", "30000/1001"), ("101", "TNG", "24000/1001"),
            ("103", "bbb30", "30/1"))
GLASS240_ID = "0x2400F36A"


def ladder_fps_q(playable):
    return (re.search(r"/ladder\.ts\?.*?[?&]fps=([^&]*)", playable) or [None, ""])[1]


def mask_ladder_fps(playable):
    return re.sub(r"([?&]fps=)[^&]*", r"\1<fps>", playable)


def check_glass240(glass_bin, patch_bin, pms, env_keys):
    """(g) The glass daemon's 240p requests, replayed: staged 240p conf on the prod
    stand-in, the glass binary as built at --glass-ref (no build gate) against this
    tree under CAPS 0x2400F36A. Only the ladder's &fps= value may differ: the glass
    daemon sent the numeric title rate, this tree sends src and keeps that rate as the
    expected rate."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("stage_raster_conf", STAGE_SCRIPT)
    stage_mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(stage_mod)
    with open(OVERLAYS["240p"], encoding="utf-8") as fh:
        text, _ = stage_mod.stage(prod_standin(pms.lan), fh.read())
    env = daemon_env_from_conf(text, env_keys)
    out, problems = {}, []
    for rk, title, rate in GLASS240:
        g_req, g_so, g_se = run_conf_case(glass_bin, pms, env, rk)
        p_req, p_so, p_se = run_conf_case(patch_bin, pms, env, rk, ["--caps-build-id", GLASS240_ID])
        nogate = run_conf_case(patch_bin, pms, env, rk)
        gj, pj = json.loads(g_so), json.loads(p_so)
        rec = {"title": title, "rk": rk, "rate": rate,
               "glass_url": gj["playable"].replace(TOKEN, "REDACTED"),
               "patch_url": pj["playable"].replace(TOKEN, "REDACTED"),
               "glass_fps_q": ladder_fps_q(gj["playable"]), "patch_fps_q": ladder_fps_q(pj["playable"]),
               "glass_rate": f"{gj['fpsNum']}/{gj['fpsDen']}",
               "patch_rate": f"{pj['fpsNum']}/{pj['fpsDen']}",
               "requests_identical": g_req == p_req,
               "gate_changes_nothing": (p_req, p_so, p_se) == nogate}
        gm, pm = dict(gj), dict(pj)
        gm["playable"], pm["playable"] = mask_ladder_fps(gj["playable"]), mask_ladder_fps(pj["playable"])
        rec["json_identical_except_fps"] = gm == pm
        bad = []
        if route_of(gj) != "proxy" or route_of(pj) != "proxy" or not ladder_240(pj["playable"]):
            bad.append(f"route glass {route_of(gj)} patch {route_of(pj)} (want the 320x240 ladder)")
        if rec["glass_fps_q"] != rate or rec["glass_rate"] != rate:
            bad.append(f"glass daemon &fps={rec['glass_fps_q']} rate {rec['glass_rate']} != {rate}")
        if rec["patch_fps_q"] != "src" or rec["patch_rate"] != rate:
            bad.append(f"patch &fps={rec['patch_fps_q']} rate {rec['patch_rate']} != &fps=src rate {rate}")
        if not rec["requests_identical"]:
            bad.append("fake-PMS requests differ from the glass daemon")
        if not rec["json_identical_except_fps"]:
            bad.append("resolve result differs from the glass daemon beyond &fps=")
        if not rec["gate_changes_nothing"]:
            bad.append(f"CAPS {GLASS240_ID} changed the output")
        if REMAP_LINE in p_se or GUESS_LINE in p_se:
            bad.append("remap/guess line")
        rec["ok"] = not bad
        out[title] = rec
        problems += [f"(g) {title} rk {rk}: {b}" for b in bad]
    return out, problems


def check_staged_confs(base_bin, patch_bin, pms, env_keys):
    """Both staged overlays on the prod stand-in: coded geometry, rate policy, FPGA
    store (not ARM-present), forbidden keys. The bare stand-in (480p) stays identical."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("stage_raster_conf", STAGE_SCRIPT)
    stage_mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(stage_mod)
    out, problems = {}, []
    with open(os.path.join(ROOT, "arm", "misterplexd", "main.cpp"), encoding="utf-8") as fh:
        main_src = fh.read()
    for rule in MAIN_CONF_RULES:
        if rule not in main_src:
            problems.append(f"main.cpp conf rule changed: {rule}")
    base = prod_standin(pms.lan)
    forbidden = {"480i": ["MPX_PRESENT_480I=1", "MPX_PRESENT_480I=0", "DISPLAY_RES=480i",
                          "CONTENT_RES=480i"],
                 "240p": ["DISPLAY_RES=480p", "CONTENT_RES=240p", "MPX_PRESENT_480I=0",
                          "MPX_PRESENT_240P=1"]}
    for raster, overlay_path in OVERLAYS.items():
        with open(overlay_path, encoding="utf-8") as fh:
            overlay = fh.read()
        rec = {"overlay": os.path.relpath(overlay_path, ROOT),
               "overlay_md5": hashlib.md5(overlay.encode()).hexdigest(), "sources": {}}
        text, got = stage_mod.stage(base, overlay)
        refused = 0
        for bad in forbidden[raster]:
            try:
                stage_mod.stage(base + bad + "\n", overlay)
            except ValueError:
                refused += 1
        rec["forbidden_refused"] = f"{refused}/{len(forbidden[raster])}"
        if refused != len(forbidden[raster]) or got != raster:
            problems.append(f"{raster}: forbidden-key refusals {rec['forbidden_refused']}")
        keys = [l.split("=", 1)[0] for l in text.splitlines() if l and not l.startswith("#")]
        if raster == "480i" and any(k == "MPX_PRESENT_480I" for k in keys):
            problems.append("480i conf carries MPX_PRESENT_480I")
        if raster == "240p" and any(k in ("DISPLAY_RES", "CONTENT_RES") or
                                    k.startswith("MPX_PRESENT_") for k in keys):
            problems.append("240p conf carries legacy/ARM-present keys")
        env = daemon_env_from_conf(text, env_keys)
        rec["env"] = {k: v for k, v in env.items() if k.startswith("MPX_FPGA")}
        if env.get("MPX_FPGA_MODE") != raster or "MPX_PRESENT_480I" in env:
            problems.append(f"{raster}: daemon env {env}")
        want_wh = ("640", "480") if raster == "480i" else ("320", "240")
        for rk, want in CONF_SOURCES.items():
            req, so, se = run_conf_case(patch_bin, pms, env, rk)
            j = json.loads(so)
            bank = j.get("bank", {})
            res = {"route": route_of(j), "ladder": ladder_geom(j["playable"]),
                   "fps_q": (re.search(r"[?&]fps=([^&]*)", j["playable"]) or [None, ""])[1],
                   "rate": f"{j['fpsNum']}/{j['fpsDen']}",
                   "bank": f"{bank.get('w')}x{bank.get('h')}@{bank.get('doorbell')}"}
            rec["sources"][rk] = res
            bad = []
            if res["route"] != "proxy" or ladder_wh(j["playable"]) != want_wh:
                bad.append(f"coded {res['ladder'] or res['route']} != {'x'.join(want_wh)}")
            if raster == "240p" and (not ladder_240(j["playable"]) or "allidr" in j["playable"]):
                bad.append("not the 240crf16 rung (320x240 hq, no allidr)")
            # Every source asks for &fps=src; the daemon expects the source rate (0/0 fps-less).
            if res["fps_q"] != "src" or res["rate"] != ("0/0" if want == "src" else want):
                bad.append(f"&fps={res['fps_q']} rate {res['rate']} != &fps=src rate {want}")
            if REMAP_LINE in se or GUESS_LINE in se or "/transcode/universal/" in req:
                bad.append("remap/guess or PMS universal")
            if (bank.get("w"), bank.get("h"), bank.get("label"), bank.get("doorbell"),
                    bank.get("present480i")) != (640, 480, "480p", "0x300FF000", False):
                bad.append(f"bank {bank} is not the FPGA 640x480 store")
            if bad:
                problems.append(f"{raster} rk {rk}: {'; '.join(bad)}")
        out[raster] = rec
    # 480i overlay without its FPGA_480P_CODED_W=640 line (test-only variant): the
    # staging script rejects it, and the daemon refuses it via the width guard.
    with open(OVERLAYS["480i"], encoding="utf-8") as fh:
        no_w = "".join(l for l in fh if not l.startswith("FPGA_480P_CODED_W="))
    try:
        stage_mod.stage(base, no_w)
        problems.append("stage_raster_conf.py staged a 480i conf without FPGA_480P_CODED_W=640")
    except ValueError:
        pass
    env_no_w = daemon_env_from_conf(no_w + base, env_keys)
    _, so, se = run_conf_case(patch_bin, pms, env_no_w, "102")
    j = json.loads(so)
    guard = not j["ok"] and ", not 640 (set FPGA_480P_CODED_W=640)" in j["detail"] and \
        "misterplexd: ERROR " in se
    out["480i_without_640"] = {"refused": guard}
    if not guard:
        problems.append(f"480i conf without FPGA_480P_CODED_W=640 was not refused: {j['detail']}")
    # 480p: the bare stand-in (no overlay) behaves exactly like 8d8faaf1.
    env480 = daemon_env_from_conf(base, env_keys)
    same = 0
    for rk in CONF_SOURCES:
        if run_conf_case(base_bin, pms, env480, rk) == run_conf_case(patch_bin, pms, env480, rk):
            same += 1
        else:
            problems.append(f"480p conf rk {rk}: patch differs from base")
    out["480p"] = {"identical": f"{same}/{len(CONF_SOURCES)}"}
    # CLI: a non-prod base is rejected by md5; an allowed one is written 0600.
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        bpath, opath = os.path.join(td, "base.conf"), os.path.join(td, "out.conf")
        with open(bpath, "w", encoding="utf-8") as fh:
            fh.write(base)
        cmd = [sys.executable, STAGE_SCRIPT, "--base", bpath, "--overlay", OVERLAYS["480i"],
               "--out", opath]
        if subprocess.run(cmd, capture_output=True).returncode == 0:
            problems.append("stage_raster_conf.py accepted a base that is not md5 933a4104")
        r = subprocess.run(cmd + ["--base-md5", ""], capture_output=True, text=True)
        if r.returncode != 0 or (os.stat(opath).st_mode & 0o777) != 0o600:
            problems.append(f"stage_raster_conf.py synthetic stage failed: {r.stderr.strip()}")
    return out, problems


def check_src30_profiles():
    """The LAN fallback profiles: no -r pin, frameRate<=30, name and geometry per raster."""
    import xml.etree.ElementTree as ET
    problems, want = [], {f"MiSTerPlex-FPGA-{p}-{r}-src30-filter-{f}" for p in ("IP", "IDR")
                          for r in ("240p", "480i") for f in ("on", "off")}
    if REPO_SRC30_PROFILES != want:
        problems.append(f"src30 set {sorted(REPO_SRC30_PROFILES ^ want)} mismatch")
    for name in sorted(REPO_SRC30_PROFILES & want):
        root = ET.parse(os.path.join(ROOT, "assets", "plex-profiles", name + ".xml")).getroot()
        flags = root.find(".//Setting[@name='VideoEncodeFlags']").get("value")
        bounds = {u.get("name"): u.get("value") for u in
                  root.findall("./CodecProfiles/VideoCodec[@name='*']/Limitations/UpperBound")}
        geom = ("320", "240") if "-240p-" in name else ("624", "480")
        if root.get("name") != name:
            problems.append(f"{name}: Client name {root.get('name')}")
        if re.search(r"(^|\s)-r\s", flags):
            problems.append(f"{name}: -r pin in VideoEncodeFlags")
        if bounds.get("video.frameRate") != "30":
            problems.append(f"{name}: frameRate bound {bounds.get('video.frameRate')}")
        if (bounds.get("video.width"), bounds.get("video.height")) != geom:
            problems.append(f"{name}: geometry {bounds.get('video.width')}x{bounds.get('video.height')}")
        if ("keyint=1:" in flags) != ("-IDR-" in name):
            problems.append(f"{name}: keyint does not match prototype")
    return problems


def ensure_base(base_ref, base_dir):
    want = sh(["git", "-C", ROOT, "rev-parse", base_ref + "^{commit}"])
    if not os.path.isdir(base_dir):
        os.makedirs(os.path.dirname(base_dir), exist_ok=True)
        sh(["git", "-C", ROOT, "worktree", "add", "--detach", base_dir, want])
    head = sh(["git", "-C", base_dir, "rev-parse", "HEAD"])
    dirty = sh(["git", "-C", base_dir, "status", "--porcelain", "--untracked-files=no"])
    if head != want or dirty:
        sys.exit(f"base worktree {base_dir} is not a clean {want}: HEAD={head} dirty={dirty!r}")
    return want


def build(tree, out):
    cmd = ["g++", *CXXFLAGS, "-I" + os.path.join(tree, "host"),
           "-I" + os.path.join(tree, "arm", "misterplexd"), "-o", out, HARNESS,
           os.path.join(tree, "arm", "misterplexd", "plex_resolve.cpp")]
    subprocess.run(cmd, check=True)
    with open(out, "rb") as fh:
        return {"cmd": " ".join(cmd), "md5": hashlib.md5(fh.read()).hexdigest()}


class FakePms:
    def __init__(self, workdir):
        self.port_file = os.path.join(workdir, "fake_pms.ports.json")
        self.log = os.path.join(workdir, "fake_pms.requests.log")
        for p in (self.port_file, self.log):
            if os.path.exists(p):
                os.remove(p)
        self.proc = subprocess.Popen([sys.executable, FAKE_PMS, "--port-file", self.port_file,
                                      "--log", self.log, "--token", TOKEN])
        for _ in range(200):
            if os.path.exists(self.port_file):
                break
            if self.proc.poll() is not None:
                sys.exit("fake PMS exited early")
            time.sleep(0.05)
        with open(self.port_file, encoding="utf-8") as fh:
            ports = json.load(fh)
        self.lan = f"http://127.0.0.1:{ports['port4']}"
        self.wan = f"http://[::1]:{ports['port6']}" if ports.get("port6") else ""

    def size(self):
        return os.path.getsize(self.log) if os.path.exists(self.log) else 0

    def read_from(self, offset):
        with open(self.log, encoding="utf-8") as fh:
            fh.seek(offset)
            return fh.read()

    def stop(self):
        self.proc.terminate()
        self.proc.wait(timeout=10)


def normalize(req, out, err):
    """Renumber session ids by first appearance (request log, then JSON, then stderr)."""
    seen = {}

    def sub(m):
        return seen.setdefault(m.group(0), f"mplex-S{len(seen) + 1}")

    req = SESSION.sub(sub, req)
    out = SESSION.sub(sub, out)
    err = SESSION.sub(sub, err).replace(TOKEN, "REDACTED")
    return req, out, err


def run_case(binary, pms, case, args=None):
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LC_ALL": "C",
           "MPX_VIDEO_BACKEND": "fpga-h264", "MPX_H264_PROTOTYPE": case["prototype"]}
    env.update(case["env"])
    off = pms.size()
    p = subprocess.run([binary] + (args or case["args"]), env=env, capture_output=True,
                       text=True, timeout=120)
    if p.returncode != 0:
        raise RuntimeError(f"{binary} rc={p.returncode} case={case['id']}: {p.stderr}")
    return normalize(pms.read_from(off), p.stdout, p.stderr)


def run_case_rc(binary, pms, case, args):
    """run_case for a run that may exit non-zero: (returncode, normalized outputs)."""
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LC_ALL": "C",
           "MPX_VIDEO_BACKEND": "fpga-h264", "MPX_H264_PROTOTYPE": case["prototype"]}
    env.update(case["env"])
    off = pms.size()
    p = subprocess.run([binary] + args, env=env, capture_output=True, text=True, timeout=120)
    return p.returncode, normalize(pms.read_from(off), p.stdout, p.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base-ref", default="f6526d96")
    ap.add_argument("--base-dir", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--quick", action="store_true", help="LAN, mpx/none proxy, ip, unset filter")
    ap.add_argument("--remove-base-worktree", action="store_true")
    ap.add_argument("--glass-ref", default="dd0bcb08",
                    help="(g) daemon of the 5554d013 240p glass; '' skips the check")
    args = ap.parse_args()

    out = args.out or os.path.join(ROOT, "build", "fps_replay",
                                   "run-" + time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(os.path.join(out, "cases"), exist_ok=True)
    base_dir = args.base_dir or os.path.join(ROOT, "build", "fps_replay",
                                             "base-" + args.base_ref[:8])
    base_sha = ensure_base(args.base_ref, base_dir)
    meta = {
        "base_ref": args.base_ref, "base_sha": base_sha, "base_dir": base_dir,
        "patch_head": sh(["git", "-C", ROOT, "rev-parse", "HEAD"]),
        "patch_dirty": sh(["git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no"]),
        "harness_md5": hashlib.md5(open(HARNESS, "rb").read()).hexdigest(),
        "cxx": sh(["g++", "--version"]).splitlines()[0],
        "src30_profiles": sorted(REPO_SRC30_PROFILES),
        "src30_profile_problems": check_src30_profiles(),
        "conf_env_keys": conf_env_keys(),
        "base_build": build(base_dir, os.path.join(out, "replay.base")),
        "patch_build": build(ROOT, os.path.join(out, "replay.patch")),
    }
    if args.glass_ref:
        glass_dir = os.path.join(ROOT, "build", "fps_replay", "base-" + args.glass_ref[:8])
        meta["glass_ref"] = args.glass_ref
        meta["glass_sha"] = ensure_base(args.glass_ref, glass_dir)
        meta["glass_build"] = build(glass_dir, os.path.join(out, "replay.glass"))
    items = json.loads(sh([sys.executable, FAKE_PMS, "--dump-items", "--port-file", "-",
                           "--log", "-"]))
    pms = FakePms(out)
    meta["fake_pms"] = {"lan": pms.lan, "wan": pms.wan}
    nets = ["lan"] if args.quick or not pms.wan else NETS
    proxies = ["none", "mpx"] if args.quick else list(PROXIES)
    protos = ["ip"] if args.quick else PROTOTYPES
    filters = ["unset"] if args.quick else FILTERS

    cases = []
    for rk, net, mode, proxy, offset, proto, filt in itertools.product(
            sorted(items), nets, MODES, proxies, OFFSETS, protos, filters):
        menv, weak, ntsc = MODES[mode]
        env = dict(menv)
        env.update(PROXIES[proxy])
        if filt != "unset":
            env["MPX_H264_FILTER"] = filt
        cid = f"{rk}-{net}-{mode}-{proxy}-off{offset}-{proto}-{filt}"
        cargs = ["--base", pms.lan if net == "lan" else pms.wan, "--token", TOKEN,
                 "--key", f"/library/metadata/{rk}", "--offset-ms", str(offset),
                 "--weak", weak] + WEAK_ARGS[(weak, offset > 0)]
        cases.append(dict(id=cid, rk=rk, src_fps=items[rk]["fps"], title=items[rk]["title"],
                          net=net, mode=mode, ntsc=ntsc, proxy=proxy, offset=offset,
                          prototype=proto, filter=filt, env=env, args=cargs))

    results = []
    gate_refusals = []
    t0 = time.time()
    try:
        for i, case in enumerate(cases):
            b = run_case(os.path.join(out, "replay.base"), pms, case)
            p = run_case(os.path.join(out, "replay.patch"), pms, case)
            bj, pj = json.loads(b[1]), json.loads(p[1])
            rec = {k: case[k] for k in ("id", "rk", "src_fps", "net", "mode", "ntsc", "proxy",
                                        "offset", "prototype", "filter", "env")}
            rec.update(identical=b == p,
                       base_fps=f"{bj['fpsNum']}/{bj['fpsDen']}",
                       patch_fps=f"{pj['fpsNum']}/{pj['fpsDen']}",
                       base_route=route_of(bj), patch_route=route_of(pj),
                       base_detail=bj["detail"], patch_detail=pj["detail"],
                       base_ladder=ladder_geom(bj["playable"]),
                       patch_ladder=ladder_geom(pj["playable"]),
                       patch_fps_q=(re.search(r"/ladder\.ts\?.*?[?&]fps=([^&]*)",
                                              pj["playable"]) or [None, ""])[1],
                       base_fps_q=(re.search(r"/ladder\.ts\?.*?[?&]fps=([^&]*)",
                                             bj["playable"]) or [None, ""])[1],
                       patch_profile=(PROFILE_HEADER.findall(pj["httpHeaders"]) or [""])[0],
                       remap_line=REMAP_LINE in p[2],
                       guess_line=GUESS_LINE in p[2], base_guess_line=GUESS_LINE in b[2],
                       violations=check_case(case, b, p),
                       sha256={n: hashlib.sha256((x[1] + x[2] + x[0]).encode()).hexdigest()[:16]
                               for n, x in (("base", b), ("patch", p))})
            if case["ntsc"] and case["src_fps"] in B_SOURCES:
                rec["b_requested"] = source_rate_requested(case, p)
            # (e) Player build-ID gate on a recognised CAPS ID: prod's on 480p must
            # stay byte-identical to the base; the 240p/480i IDs change nothing.
            gate_ids = GATE_KNOWN.get(case["mode"], ())
            if gate_ids:
                rec["gate_id"] = ",".join(gate_ids)
                rec["gate_identical"] = True
                for gate_id in gate_ids:
                    g = run_case(os.path.join(out, "replay.patch"), pms, case,
                                 case["args"] + ["--caps-build-id", gate_id])
                    if g != (p if case["ntsc"] else b):
                        rec["gate_identical"] = False
                        rec["violations"].append(f"(e) build gate {gate_id} changed the output")
            prodlike = (case["mode"] == "480p" and case["proxy"] == "mpx" and
                        case["prototype"] == "ip" and case["filter"] == "unset" and
                        case["net"] == "lan")
            if not rec["identical"] or prodlike or rec["violations"]:
                d = os.path.join(out, "cases", case["id"])
                os.makedirs(d, exist_ok=True)
                for tag, (req, so, se) in (("base", b), ("patch", p)):
                    for ext, text in (("req.log", req), ("json", so), ("stderr", se)):
                        with open(os.path.join(d, f"{tag}.{ext}"), "w", encoding="utf-8") as fh:
                            fh.write(text)
                with open(os.path.join(d, "case.json"), "w", encoding="utf-8") as fh:
                    json.dump(case, fh, indent=1)
            results.append(rec)
            if (i + 1) % 400 == 0:
                print(f"  {i + 1}/{len(cases)} cases, {time.time() - t0:.0f}s", flush=True)
        # (e) Unrecognised CAPS build IDs: the player refuses before the first AU.
        for case in cases:
            if not (case["net"] == "lan" and case["proxy"] == "mpx" and case["offset"] == 0 and
                    case["prototype"] == "ip" and case["filter"] == "unset" and
                    case["mode"] in ("480p", "240p-env480", "480i-640")):
                continue
            for gid in GATE_UNKNOWN + list(GATE_RETIRED):
                rc, (_, _, se) = run_case_rc(os.path.join(out, "replay.patch"), pms, case,
                                             case["args"] + ["--caps-build-id", gid])
                kind = "retired" if gid in GATE_RETIRED else "unrecognised"
                want = f"ERROR FPGA H.264: {kind} FPGA build 0x{int(gid, 16):08X} "
                names = GATE_RETIRED.get(gid, ())
                gate_refusals.append({"id": case["id"], "build_id": gid, "rc": rc, "kind": kind,
                                      "refused": rc == 3 and want in se and
                                      "refusing to play, no ARM-present fallback" in se and
                                      all(n in se for n in names)})
        confs, conf_problems = check_staged_confs(os.path.join(out, "replay.base"),
                                                  os.path.join(out, "replay.patch"), pms,
                                                  meta["conf_env_keys"])
        glass, glass_problems = ({}, [])
        if args.glass_ref:
            glass, glass_problems = check_glass240(os.path.join(out, "replay.glass"),
                                                   os.path.join(out, "replay.patch"), pms,
                                                   meta["conf_env_keys"])
    finally:
        pms.stop()
    meta["staged_confs"] = confs
    meta["staged_conf_problems"] = conf_problems
    meta["glass240"] = glass
    meta["glass240_problems"] = glass_problems
    if args.remove_base_worktree:
        sh(["git", "-C", ROOT, "worktree", "remove", "--force", base_dir])

    meta["elapsed_s"] = round(time.time() - t0, 1)
    meta["cases"] = len(results)
    with open(os.path.join(out, "results.jsonl"), "w", encoding="utf-8") as fh:
        for r in results:
            fh.write(json.dumps(r, sort_keys=True) + "\n")
    with open(os.path.join(out, "meta.json"), "w", encoding="utf-8") as fh:
        json.dump(meta, fh, indent=1)
    with open(os.path.join(out, "build_gate_refusals.jsonl"), "w", encoding="utf-8") as fh:
        for g in gate_refusals:
            fh.write(json.dumps(g, sort_keys=True) + "\n")

    p480 = [r for r in results if not r["ntsc"]]
    gate480 = [r for r in p480 if "gate_id" in r]
    gate_ntsc = [r for r in results if r["ntsc"] and "gate_id" in r]
    ntsc = [r for r in results if r["ntsc"]]
    breq = [r for r in ntsc if "b_requested" in r]
    b_ok = [r for r in breq if r["b_requested"] is True]
    b_bad = [r for r in breq if r["b_requested"] is False]
    b_refused = {}
    for r in breq:
        if r["b_requested"] is None:
            k = f"{r['mode']} {r['net']} proxy={r['proxy']} off={r['offset']}: {r['patch_detail']}"
            b_refused[k] = b_refused.get(k, 0) + 1
    remaps = [r for r in ntsc if r["remap_line"]]
    bad = [r for r in results if r["violations"]]
    # fps-less titles on 240p/480i: &fps=src with a proxy (rate 0/0, no guess), else refused.
    # (MPX_FPGA_MODE=480i without a 640 limit refuses earlier, via the width guard.)
    guarded = ("480i", "480i-624")
    f1 = [r for r in ntsc if not r["src_fps"] and r["proxy"] != "none" and
          r["mode"] not in guarded]
    f1_ok = [r for r in f1 if r["patch_route"] == "proxy" and r["patch_fps_q"] == "src" and
             r["patch_fps"] == "0/0" and not r["guess_line"] and not r["remap_line"]]
    f1_np = [r for r in ntsc if not r["src_fps"] and r["proxy"] == "none" and
             r["mode"] not in guarded]
    f1_np_ok = [r for r in f1_np if r["patch_route"] == "fail" and
                ("refusing (no remap)" in r["patch_detail"] or
                 "refusing (no 624 fallback)" in r["patch_detail"])]
    f1_480 = [r for r in p480 if not r["src_fps"]]
    t640 = [r for r in results if r["mode"] == "480i-640"]
    t640_lad = [r for r in t640 if r["patch_route"] == "proxy"]
    t640_np = [r for r in t640 if r["proxy"] == "none"]
    tw = {m: [r for r in results if r["mode"] == m] for m in ("480i", "480i-624")}
    tw_ok = {m: [r for r in v if r["patch_route"] == "fail" and
                 ", not 640 (set FPGA_480P_CODED_W=640)" in r["patch_detail"]]
             for m, v in tw.items()}
    r240 = [r for r in results if r["mode"] == "240p-env480" and r["patch_route"] == "proxy"]
    # DAEMON-FPS-SRC-ALWAYS-1003: every 240p/480i proxy URL asks for &fps=src; a numeric
    # metadata rate stays the expected rate (60-family halved), so nothing is remapped.
    nproxy = [r for r in ntsc if r["patch_route"] == "proxy"]
    nproxy_src = [r for r in nproxy if r["patch_fps_q"] == "src"]
    nproxy_num = [r for r in nproxy if r["src_fps"]]
    nproxy_kept = [r for r in nproxy_num if r["patch_fps"] == halved(r["src_fps"])]
    nproxy_was_num = [r for r in nproxy if r["base_route"] == "proxy" and
                      r["base_fps_q"] not in ("", "src")]
    conf_mode = "MPX_FPGA_MODE" in meta["conf_env_keys"]
    routes = {}
    for r in ntsc:
        if fps_family(r["src_fps"]) in ("30", "60"):
            k = f"{fps_family(r['src_fps'])}-family {r['net']} proxy={'none' if r['proxy'] == 'none' else 'set'}: " \
                f"{r['base_route']}->{r['patch_route']}"
            routes[k] = routes.get(k, 0) + 1
    lines = [
        "# T2 fake-PMS replay: base %s vs patch %s%s" % (
            base_sha[:8], meta["patch_head"][:8], " (+ worktree edits)" if meta["patch_dirty"] else ""),
        "",
        f"- cases: {len(results)} ({meta['elapsed_s']} s); fake PMS {pms.lan} + {pms.wan or 'no IPv6'}",
        f"- replay md5 base {meta['base_build']['md5'][:8]} patch {meta['patch_build']['md5'][:8]}",
        f"- (a) 480p byte-identical (JSON + stderr + request log): "
        f"{sum(r['identical'] for r in p480)}/{len(p480)}",
        f"- (b) 240p/480i x {{23.976, 24, 29.97, 30/1}}: source rate requested on "
        f"{len(b_ok)}/{len(breq)} paths; wrong rate {len(b_bad)}; "
        f"refused {len(breq) - len(b_ok) - len(b_bad)}",
        f"- (c) remap lines on 240p/480i: {len(remaps)} of {len(ntsc)} cases",
        f"- fps=src always (DAEMON-FPS-SRC-ALWAYS-1003): 240p/480i proxy URLs with &fps=src "
        f"{len(nproxy_src)}/{len(nproxy)} ({len(nproxy_was_num)} of them were a numeric &fps= at "
        f"the base); numeric-rate titles keep the source rate as the expected rate "
        f"{len(nproxy_kept)}/{len(nproxy_num)}",
        f"- fps-less titles on 240p/480i: with a proxy &fps=src (rate 0/0, no 24000/1001 guess, "
        f"no remap) {len(f1_ok)}/{len(f1)}; without a proxy refused {len(f1_np_ok)}/{len(f1_np)}; "
        f"480p fps-less byte-identical {sum(r['identical'] for r in f1_480)}/{len(f1_480)}",
        f"- true 480i (MPX_FPGA_MODE=480i): FPGA_480P_CODED_W=640 -> proxy ladders coded 640x480 "
        f"{sum(r['patch_ladder'].startswith('640x480@') for r in t640_lad)}/{len(t640_lad)}, "
        f"no proxy refused {sum(r['patch_route'] == 'fail' for r in t640_np)}/{len(t640_np)}; "
        f"key missing refused by the width guard {len(tw_ok['480i'])}/{len(tw['480i'])}; "
        f"=624 refused {len(tw_ok['480i-624'])}/{len(tw['480i-624'])} (never 624)",
        f"- 240p on the 480p ABI (MPX_FPGA_MODE=240p, 480 conf ladder): ladders on the 320x240 "
        f"240crf16 rung {sum(r['patch_ladder'].startswith('320x240@') and r['patch_ladder'].endswith(' hq') for r in r240)}/{len(r240)}",
        f"- conf->env: main.cpp exports MPX_FPGA_MODE from the conf: {'yes' if conf_mode else 'NO'}",
        f"- staged confs (overlays on a 933a4104 stand-in): "
        + "; ".join(
            f"{k} {v['overlay']} md5 {v['overlay_md5'][:8]} forbidden refused {v['forbidden_refused']}"
            for k, v in confs.items() if k not in ("480p", "480i_without_640"))
        + f"; 480i overlay minus FPGA_480P_CODED_W refused: "
        + f"{'yes' if confs.get('480i_without_640', {}).get('refused') else 'NO'}"
        + f"; 480p stand-in base==patch {confs.get('480p', {}).get('identical')}; problems: "
        + ("; ".join(conf_problems) or "none"),
        f"- src30 LAN-fallback profiles: {len(meta['src30_profiles'])} XMLs, problems: "
        f"{'; '.join(meta['src30_profile_problems']) or 'none'} "
        "(no -r pin, frameRate<=30, 320x240 / 624x480)",
        f"- rule violations: {len(bad)}",
        f"- (e) build-ID gate (player, fpga_build_ids.hpp): 480p with CAPS "
        f"{' + '.join(GATE_KNOWN['480p'])} byte-identical to the base "
        f"{sum(r['gate_identical'] for r in gate480)}/{len(gate480)}; "
        f"240p-env480 0x2400F36A + 0x2400F40B and 480i-640 0x4801F40A unchanged "
        f"{sum(r['gate_identical'] for r in gate_ntsc)}/{len(gate_ntsc)}; unrecognised "
        f"{' '.join(GATE_UNKNOWN)} refused (exit 3, 'ERROR FPGA H.264: unrecognised FPGA build', "
        f"no ARM-present fallback) "
        f"{sum(g['refused'] for g in gate_refusals if g['kind'] == 'unrecognised')}/"
        f"{sum(g['kind'] == 'unrecognised' for g in gate_refusals)}; retired "
        f"{' '.join(GATE_RETIRED)} refused ('retired FPGA build', naming "
        f"{'/'.join(n for v in GATE_RETIRED.values() for n in v)}) "
        f"{sum(g['refused'] for g in gate_refusals if g['kind'] == 'retired')}/"
        f"{sum(g['kind'] == 'retired' for g in gate_refusals)}",
        (f"- (g) 240p glass requests vs {args.glass_ref} (staged 240p conf, CAPS "
         f"{GLASS240_ID}): "
         + "; ".join(f"{t} {v['rate']}: glass &fps={v['glass_fps_q']} -> &fps={v['patch_fps_q']}, "
                     f"expected {v['patch_rate']}, requests "
                     f"{'same' if v['requests_identical'] else 'DIFFER'}, rest "
                     f"{'same' if v['json_identical_except_fps'] else 'DIFFERS'}"
                     for t, v in glass.items())
         + f"; problems: {'; '.join(glass_problems) or 'none'}") if args.glass_ref else
        "- (g) 240p glass requests: skipped (--glass-ref '')",
        "",
        "## (b) refusals (no path; explicit error, never a remap)",
        "",
    ]
    lines += [f"- x{v} {k}" for k, v in sorted(b_refused.items())] or ["- none"]
    lines += ["", "## 240p/480i 30/60-family routes (base -> patch)", ""]
    lines += [f"- x{v} {k}" for k, v in sorted(routes.items())]
    lines += [
        "",
        "## Per-source rate, route and PMS profile (ip, filter unset, offset 0)",
        "",
        "| source | mode | net | proxy | base fps | patch fps | base route | patch route | patch profile / ladder |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for r in results:
        if (r["prototype"] == "ip" and r["filter"] == "unset" and r["offset"] == 0 and
                r["proxy"] in ("mpx", "none") and (r["net"] == "lan" or r["proxy"] == "none")):
            extra = r["patch_ladder"] if r["patch_route"] == "proxy" else r["patch_profile"]
            if r["patch_route"] == "fail":
                extra = r["patch_detail"][:90]
            lines.append(f"| {r['rk']} {r['src_fps'] or 'missing'} | {r['mode']} | {r['net']} | "
                         f"{r['proxy']} | {r['base_fps']} | {r['patch_fps']} | {r['base_route']} | "
                         f"{r['patch_route']} | {extra} |")
    if bad:
        lines += ["", "## Rule violations", ""]
        lines += [f"- {r['id']}: {'; '.join(r['violations'])}" for r in bad[:200]]
    lines += ["", "## Staged confs (per source: route, ladder, &fps=, bank)", ""]
    for k, v in confs.items():
        if k in ("480p", "480i_without_640"):
            continue
        lines.append(f"- {k}: {v['overlay']} md5 {v['overlay_md5']} env {v['env']}")
        lines += [f"  - rk {rk}: {s['route']} {s['ladder']} fps={s['fps_q']} rate {s['rate']} "
                  f"bank {s['bank']}"
                  for rk, s in v["sources"].items()]
    with open(os.path.join(out, "SUMMARY.md"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")
    print("\n".join(lines[:lines.index("## (b) refusals (no path; explicit error, never a remap)")]).rstrip())
    print(f"evidence: {out}")
    ok = not bad and not remaps and not b_bad and all(r["identical"] for r in p480) and \
        not meta["src30_profile_problems"] and len(f1_ok) == len(f1) and \
        len(f1_np_ok) == len(f1_np) and conf_mode and not conf_problems and \
        all(len(tw_ok[m]) == len(tw[m]) for m in tw) and \
        nproxy and len(nproxy_src) == len(nproxy) and len(nproxy_kept) == len(nproxy_num) and \
        gate480 and all(r["gate_identical"] for r in gate480 + gate_ntsc) and \
        gate_refusals and all(g["refused"] for g in gate_refusals) and \
        not glass_problems and (not args.glass_ref or len(glass) == len(GLASS240))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
