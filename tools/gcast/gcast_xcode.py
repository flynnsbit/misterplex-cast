#!/usr/bin/env python3
"""gcast-xcode: helper that turns any cast URL into an H.264/AAC MPEG-TS stream misterplexd can open.

  GET /health
  GET /probe?src=<url>                 -> text "w=..\nh=..\ndur_ms=..\nfps_n=..\nfps_d=..\naudio=0|1\nvcodec=.."
  GET /ladder.ts?src=<url>&w=&h=&br=[&offset_ms=][&remux=1]
      Host FFmpeg pulls src (http/https, HLS, DASH-less mp4/mkv/ts, HEVC...), letterboxes into w x h
      An HLS master playlist is parsed first: the highest-bandwidth in-spec H.264 variant
      (Baseline/Main/High, level <= 4.1, <=1280x720, <=30 fps) is copied, with its AUDIO group
      when the master names one. Anything else is the x264 ladder on the variant closest to 720p30.
      (SAR 1, limited BT.601), keeps the source rate (60-family halved, unknown -> 30000/1001),
      libx264 main, no B-frames, GOP 2 s, CBR-ish br kbps, AAC stereo 48 kHz, streams MPEG-TS.
      offset_ms= runs -ss before -i (same contract as the lab ladder proxy, so MediaPlayer::seekMs works).
      remux=1 copies an already-H.264 source (no re-encode).
  GET /yt.ts?v=<videoId>[&start_ms=]   YouTube: yt-dlp resolves avc1<=720p30 + m4a, pipes yt-dlp -o - into
      ffmpeg copy-remux (no re-encode). On 403/empty, cache invalidated for next play. Works standalone
      under /media/fat/misterplex/. Set GCAST_XCODE to use a helper on a computer instead.
Only http(s) or yt:<11-char id> src is accepted; at most MAX_JOBS ffmpeg run at once (oldest killed).
The on-card helper binds 127.0.0.1:18781. A computer helper uses --bind 0.0.0.0 --port 18780.
"""
import argparse, json, os, re, signal, ssl, subprocess, sys, threading, time, urllib.parse, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAX_JOBS = 2
JOBS = []
JOBS_MU = threading.Lock()
LOG = sys.stderr

def log(msg):
    LOG.write(time.strftime("%Y-%m-%d %H:%M:%S %Z ") + msg + "\n"); LOG.flush()

YT_ID = re.compile(r"^[A-Za-z0-9_-]{11}$")
# FIELD-1018 F1: 2026 YouTube lists the AAC track as 140-5/140-20 (DRC / language variants), so a bare "+140"
# no longer matches on some videos: avc1 by codec and height, any m4a audio, then progressive. rc3: GCAST_YT_FMT overrides.
YT_FMT = os.environ.get("GCAST_YT_FMT",
    "bv*[vcodec^=avc1][height<=720][fps<=30][protocol=https]+ba[ext=m4a][protocol=https]/"
    "bv*[vcodec^=avc1][height<=720][fps<=30]+ba[ext=m4a]/b[vcodec^=avc1][height<=720]/18")
# yt-dlp tries again with other player clients when the default one returns nothing (bot check, PO token, SABR).
YT_CLIENT_RETRY = ([], ["--extractor-args", "youtube:player_client=mweb,web_safari"],
                   ["--extractor-args", "youtube:player_client=tv,web_embedded"])
YT_CACHE = {}
YT_MU = threading.Lock()
YT_TTL = int(os.environ.get("GCAST_YT_TTL", "600"))

def misterplex_root():
    return os.environ.get("GCAST_MISTERPLEX_ROOT", "/media/fat/misterplex")

def which_default(env_key, names, fallback):
    v = os.environ.get(env_key)
    if v: return v
    root = misterplex_root()
    for n in names:
        for cand in (os.path.join(root, "bin", n), os.path.join(root, n)):
            if os.path.isfile(cand) and os.access(cand, os.X_OK): return cand
    return fallback

def js_runtime_args():
    explicit = os.environ.get("GCAST_YT_JS", "").strip()
    if explicit:
        kind = "node" if "node" in os.path.basename(explicit).lower() else "quickjs"
        return ["--js-runtimes", "%s:%s" % (kind, explicit)]
    root = misterplex_root()
    for label, paths in (
        ("node", [os.path.join(root, "bin", "node"), os.path.join(root, "node", "bin", "node")]),
        ("quickjs", [os.path.join(root, "bin", "qjs"), os.path.join(root, "bin", "qjsng")]),
    ):
        for cand in paths:
            if os.path.isfile(cand) and os.access(cand, os.X_OK):
                return ["--js-runtimes", "%s:%s" % (label, cand)]
    return []

def ca_env():
    ca = os.environ.get("MPX_CA_BUNDLE") or os.environ.get("SSL_CERT_FILE")
    if not ca:
        cand = os.path.join(misterplex_root(), "ca", "misterplex-cacert.pem")
        if os.path.isfile(cand): ca = cand
    env = os.environ.copy()
    if ca:
        env["SSL_CERT_FILE"] = ca; env["REQUESTS_CA_BUNDLE"] = ca; env["CURL_CA_BUNDLE"] = ca
    return env

def yt_invalidate(vid):   # kept for callers; F1 yt_evict logs
    yt_evict(vid)

def ok_src(src):
    if src.startswith("yt:"): return bool(YT_ID.match(src[3:]))
    return (src.startswith("http://") or src.startswith("https://")) and not any(c in src for c in "\r\n ")

SANE_WALL_EPOCH = 1577836800  # 2020-01-01 UTC; same floor as misterplexd wall_clock.hpp
SANE_WALL_CEILING = 4102444800  # 2100-01-01 UTC; same ceiling as misterplexd wall_clock.hpp

def wall_clock_sane():
    t = time.time()
    return t >= SANE_WALL_EPOCH and t < SANE_WALL_CEILING

def wait_plausible_wall_clock(seconds=20):
    """The daemon steps the clock via SNTP before this request. Wait in case it is still moving."""
    if wall_clock_sane():
        return True
    log("yt: wall clock is implausible (boot with no NTP); waiting up to %ds" % seconds)
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if wall_clock_sane():
            return True
        time.sleep(0.5)
    log("yt: wall clock still implausible; not calling yt-dlp (TLS certs and signed URLs would fail)")
    return False

def yt_resolve(vid, ytdlp="yt-dlp"):
    """videoId -> {inputs:[(url, headers)], w, h, fps, dur_ms, title}; cached YT_TTL s (URLs expire in ~6 h)."""
    if not YT_ID.match(vid): return None
    if not wait_plausible_wall_clock(): return None
    force = False
    with YT_MU:
        hit = YT_CACHE.get(vid)
        if not force and hit and time.time() - hit[0] < YT_TTL: return hit[1]
    j, why = {}, ""
    for extra in YT_CLIENT_RETRY:
        # rc3: one youtube extractor-args string (skip list + optional player_client), JS runtime and CA env
        # from the standalone install, 90 s per attempt (ARM yt-dlp is slow on a cold start).
        ea = "youtube:skip=translated_subs,hls" + (";" + extra[1].split(":", 1)[1] if extra else "")
        cmd = [ytdlp, "-j", "--no-playlist", "--skip-download", "-f", YT_FMT] + js_runtime_args() + \
              ["--extractor-args", ea, "https://www.youtube.com/watch?v=" + vid]
        try:
            p = subprocess.run(cmd, capture_output=True, timeout=90, env=ca_env())
            j = json.loads(p.stdout or b"{}")
            why = (p.stderr or b"")[-300:].decode("utf-8", "replace").strip().replace("\n", " | ")
        except Exception as e:  # noqa: BLE001
            j, why = {}, str(e)
        if j.get("requested_formats") or j.get("url"): break
        log("yt-dlp: no formats for %s (client %s): %s" % (vid, " ".join(extra[1:]) or "default", why))
    fmts = j.get("requested_formats") or ([j] if j.get("url") else [])
    if not fmts: return None
    hdr = lambda f: "".join("%s: %s\r\n" % kv for kv in (f.get("http_headers") or {}).items())
    v = next((f for f in fmts if f.get("vcodec") not in (None, "none")), fmts[0])
    itag_list = [str(f.get("format_id") or "") for f in fmts]
    r = {"inputs": [(f["url"], hdr(f)) for f in fmts], "w": int(v.get("width") or 0), "h": int(v.get("height") or 0),
         "fps": float(v.get("fps") or 30), "dur_ms": int(float(j.get("duration") or 0) * 1000),
         "title": (j.get("title") or "")[:120], "itags": itag_list,
         "itags_s": "+".join(itag_list)}
    with YT_MU: YT_CACHE[vid] = (time.time(), r)
    log("yt %s itags=%s %dx%d dur=%d" % (vid, r["itags_s"], r["w"], r["h"], r["dur_ms"]))
    return r

def yt_evict(vid):
    with YT_MU:
        if YT_CACHE.pop(vid, None) is not None: log("yt %s: cache dropped (403 / empty stream)" % vid)

def yt_probe(vid):
    r = yt_resolve(vid)
    if not r: return None
    fps = int(round(r["fps"]))
    return {"w": r["w"], "h": r["h"], "dur_ms": r["dur_ms"], "fps_n": fps, "fps_d": 1, "audio": 1,
            "vcodec": "h264", "title": r["title"].replace("\n", " ")}

def probe_selftest(ffprobe="ffprobe"):
    """ffprobe a generated 16-bit stereo WAV. No network. Ready-card gate uses this."""
    import struct, tempfile
    data = b"\x00" * 8
    hdr = struct.pack("<4sI4s4sIHHIIHH4sI",
                      b"RIFF", 36 + len(data), b"WAVE", b"fmt ", 16,
                      1, 2, 48000, 48000 * 4, 4, 16, b"data", len(data))
    path = None
    try:
        fd, path = tempfile.mkstemp(prefix="gcast-selftest-", suffix=".wav")
        os.write(fd, hdr + data)
        os.close(fd)
        return probe(path, ffprobe)
    except Exception as e:  # noqa: BLE001
        log("selftest failed: %s" % e)
        return None
    finally:
        if path:
            try:
                os.remove(path)
            except OSError:
                pass

def _parse_rate(value):
    """ffprobe frame rate. This --enable-small build prints "30/1". Also accept a bare number (30, "30", "30.0")."""
    if isinstance(value, bool) or value is None:
        return 0, 0
    if isinstance(value, (int, float)):
        if value <= 0:
            return 0, 0
        if isinstance(value, float) and not value.is_integer():
            return int(round(float(value) * 1000.0)), 1000
        return int(value), 1
    text = str(value).strip()
    if not text or text.upper() == "N/A":
        return 0, 0
    if "/" in text:
        num, _, den = text.partition("/")
        try:
            return int(num), int(den)
        except ValueError:
            return 0, 0
    if "." in text:
        try:
            return _parse_rate(float(text))
        except ValueError:
            return 0, 0
    try:
        n = int(text)
    except ValueError:
        return 0, 0
    return (n, 1) if n > 0 else (0, 0)

def _frame_rate(stream):
    """Prefer avg_frame_rate. "0/0" is not a rate, so fall through to r_frame_rate."""
    for key in ("avg_frame_rate", "r_frame_rate"):
        fn, fd = _parse_rate(stream.get(key))
        if fn > 0 and fd > 0:
            return fn, fd
    return 0, 0

def _parse_bits(stream):
    """bits_per_raw_sample from ffprobe e49cfc7c is the string "8", not an int."""
    for key in ("bits_per_raw_sample", "bits_per_sample"):
        bits = stream.get(key)
        if bits in (None, "", "N/A"):
            continue
        try:
            return int(bits)
        except (TypeError, ValueError):
            return -1
    return 0

def parse_probe_json(j):
    """Map one ffprobe -show_streams -show_format JSON document to the probe dict."""
    if not isinstance(j, dict):
        return None
    streams = j.get("streams") or []
    v = next((s for s in streams if s.get("codec_type") == "video"), None)
    astream = next((s for s in streams if s.get("codec_type") == "audio"), None)
    a = astream is not None
    acodec = (astream or {}).get("codec_name") or ""
    if not v:
        if not a:
            return None
        dur = 0.0
        try:
            dur = float((j.get("format") or {}).get("duration") or 0)
        except (TypeError, ValueError):
            pass
        return {"w": 640, "h": 480, "dur_ms": int(dur * 1000), "fps_n": 30000, "fps_d": 1001, "audio": 1,
                "vcodec": "none", "acodec": acodec}
    fn, fd = _frame_rate(v)
    dur = 0.0
    try:
        dur = float((j.get("format") or {}).get("duration") or 0)
    except (TypeError, ValueError):
        pass
    return {"w": int(v.get("width") or 0), "h": int(v.get("height") or 0), "dur_ms": int(dur * 1000),
            "fps_n": fn, "fps_d": fd, "audio": 1 if a else 0, "vcodec": v.get("codec_name") or "",
            "acodec": acodec, "profile": v.get("profile") if v.get("profile") is not None else "",
            "level": v.get("level") or 0, "pix_fmt": v.get("pix_fmt") if v.get("pix_fmt") is not None else "",
            "bits": _parse_bits(v)}

def probe(src, ffprobe="ffprobe"):
    cmd = [ffprobe, "-v", "error", "-print_format", "json", "-show_streams", "-show_format", src]
    if src.endswith("/raop.wav"):  # AirPlay RAOP live PCM: header is enough, do not wait 5 s of live audio
        cmd[1:1] = ["-probesize", "32768", "-analyzeduration", "0", "-f", "wav"]
    try:
        out = subprocess.run(cmd, capture_output=True, timeout=25, env=ca_env()).stdout
        j = json.loads(out or b"{}")
        return parse_probe_json(j)
    except Exception as e:  # noqa: BLE001
        log("probe failed (%s)" % type(e).__name__); return None

# PlexOB plays H.264 Baseline/Main/High, 8-bit yuv420p, up to level 4.1, at most 1280x720.
# fps<=30 matches the yt-dlp selector. Anything else is re-encoded.
# AVPixelFormat ABI: YUV420P is 0, YUVJ420P is 12. This ffprobe prints the name.
_PIX_420_NAMES = ("yuv420p", "yuvj420p")
_PIX_420_IDS = (0, 12)
# H.264 profile_idc as ffprobe --enable-small prints it (no name table):
# 66 Baseline, 578 Constrained Baseline (66|512), 77 Main, 100 High.
# 110 High 10, 122 High 4:2:2, 244 High 4:4:4 Predictive stay on the x264 ladder.
_PROFILE_NAMES = ("baseline", "constrained baseline", "main", "high")
_PROFILE_IDS = (66, 578, 77, 100)

def _pix_copy_ok(pix):
    if isinstance(pix, bool):
        return False
    if isinstance(pix, int):
        return pix in _PIX_420_IDS
    token = str(pix or "").strip().lower()
    if token in _PIX_420_NAMES:
        return True
    if token.isdigit():
        return int(token) in _PIX_420_IDS
    return False

def _profile_copy_ok(profile):
    """Names, or the numeric ids from ffprobe e49cfc7c. GCAST_MUT=profname keeps names only."""
    if isinstance(profile, bool):
        return False
    if isinstance(profile, int):
        token = str(profile)
    else:
        token = str(profile or "").strip().lower()
    if os.environ.get("GCAST_MUT") == "profname":
        return token in _PROFILE_NAMES
    if token in _PROFILE_NAMES:
        return True
    if token.isdigit():
        return int(token) in _PROFILE_IDS
    return False

def stream_copy_ok(info):
    if not info or info.get("vcodec") != "h264":
        return False
    if not _pix_copy_ok(info.get("pix_fmt")):
        return False
    try:
        bits = int(info.get("bits") or 0)
    except (TypeError, ValueError):
        return False
    if bits not in (0, 8):
        return False
    if not _profile_copy_ok(info.get("profile")):
        return False
    try:
        level = float(info.get("level") or 0)
    except (TypeError, ValueError):
        return False
    if level <= 0 or (level < 10 and level > 4.1) or (level >= 10 and level > 41):
        return False
    try:
        w, h = int(info.get("w") or 0), int(info.get("h") or 0)
        fn, fd = int(info.get("fps_n") or 0), int(info.get("fps_d") or 0)
    except (TypeError, ValueError):
        return False
    if w <= 0 or h <= 0 or w > 1280 or h > 720 or fn <= 0 or fd <= 0:
        return False
    return (fn / float(fd)) <= 30.01

# avc1.PPCCLL: PP is profile_idc, LL is level_idc. Level 4.1 is level_idc 41 (hex 29).
_AVC1_PROFILES = (0x42, 0x4D, 0x64)  # Baseline, Main, High

def parse_attr_list(body):
    """HLS attribute list. Quoted values may contain commas."""
    attrs = {}
    i, n = 0, len(body)
    while i < n:
        while i < n and body[i] in ", \t":
            i += 1
        if i >= n:
            break
        eq = body.find("=", i)
        if eq < 0:
            break
        key = body[i:eq].strip().upper()
        i = eq + 1
        if i < n and body[i] == '"':
            i += 1
            j = body.find('"', i)
            if j < 0:
                val, i = body[i:], n
            else:
                val, i = body[i:j], j + 1
        else:
            j = body.find(",", i)
            if j < 0:
                val, i = body[i:].strip(), n
            else:
                val, i = body[i:j].strip(), j + 1
        if key:
            attrs[key] = val
    return attrs

def parse_avc1(codec):
    """Return (profile_idc, level_idc) for an avc1.PPCCLL token, else None."""
    c = (codec or "").strip()
    if not c.lower().startswith("avc1."):
        return None
    hexpart = c.split(".", 1)[1].strip()
    if len(hexpart) < 6:
        return None
    try:
        profile = int(hexpart[0:2], 16)
        level = int(hexpart[4:6], 16)
    except ValueError:
        return None
    return profile, level

def _codec_items(s):
    return [c.strip() for c in (s or "").split(",") if c.strip()]

def _video_codec(codecs):
    for c in codecs:
        cl = c.lower()
        if cl.startswith(("avc1", "hvc1", "hev1", "dvh1", "av01")):
            return c
    return ""

def _is_aac(codec):
    return (codec or "").strip().lower().startswith("mp4a.40")

def _parse_res(s):
    if not s or "x" not in s.lower():
        return 0, 0
    a, b = s.lower().split("x", 1)
    try:
        return int(float(a)), int(float(b))
    except ValueError:
        return 0, 0

def _parse_fps(s):
    if s is None or str(s).strip() == "":
        return None
    try:
        return float(s)
    except ValueError:
        return None

def _parse_bw(s):
    try:
        return int(float(s))
    except (TypeError, ValueError):
        return 0

def variant_in_spec(profile, level, w, h, fps):
    """Playlist in-spec test. Missing fps is allowed; ffprobe confirms it later.
    GCAST_MUT=hlslevel drops the level ceiling so a too-high level is accepted."""
    if profile not in _AVC1_PROFILES:
        return False
    if os.environ.get("GCAST_MUT") != "hlslevel":
        if level <= 0 or level > 41:
            return False
    if w <= 0 or h <= 0 or w > 1280 or h > 720:
        return False
    if fps is not None and fps > 30.01:
        return False
    return True

def _pick_audio(medias, group):
    """AUDIO group: prefer mp4a.40.*, then DEFAULT=YES. Returns (media or None, copy_aac)."""
    group_m = [m for m in medias if m.get("group") == group and m.get("type") == "AUDIO"]
    if not group_m:
        return None, False
    aac = [m for m in group_m if _is_aac(m.get("codecs") or "")]
    pool = aac or group_m
    defaults = [m for m in pool if m.get("default")]
    chosen = (defaults or pool)[0]
    return chosen, _is_aac(chosen.get("codecs") or "")

def plan_hls_master(text, master_url):
    """Parse an HLS master. None when the body is not a master playlist.
    in_spec is highest BANDWIDTH first. fallback is the variant closest to 720p30."""
    if not text or "#EXT-X-STREAM-INF" not in text:
        return None
    medias = []
    raw_variants = []
    pending = None
    for raw in text.splitlines():
        s = raw.strip()
        if not s:
            continue
        if s.startswith("#EXT-X-MEDIA:"):
            a = parse_attr_list(s.split(":", 1)[1])
            medias.append({
                "type": (a.get("TYPE") or "").upper(),
                "group": a.get("GROUP-ID") or "",
                "codecs": a.get("CODECS") or "",
                "default": (a.get("DEFAULT") or "").upper() == "YES",
                "uri": a.get("URI") or "",
            })
            continue
        if s.startswith("#EXT-X-STREAM-INF:"):
            pending = parse_attr_list(s.split(":", 1)[1])
            continue
        if s.startswith("#"):
            continue
        if pending is None:
            continue
        raw_variants.append((pending, s))
        pending = None
    variants = []
    for attrs, uri in raw_variants:
        codecs = _codec_items(attrs.get("CODECS") or "")
        vcodec = _video_codec(codecs)
        parsed = parse_avc1(vcodec)
        w, h = _parse_res(attrs.get("RESOLUTION") or "")
        fps = _parse_fps(attrs.get("FRAME-RATE"))
        bw = _parse_bw(attrs.get("BANDWIDTH"))
        profile, level = parsed if parsed else (0, 0)
        audio_codecs = [c for c in codecs if c != vcodec and not c.lower().startswith(("avc1", "hvc1", "hev1", "dvh1", "av01"))]
        group = attrs.get("AUDIO") or ""
        media, media_aac = _pick_audio(medias, group) if group else (None, False)
        if media is not None:
            playlist_aac = media_aac
            audio_url = urllib.parse.urljoin(master_url, media["uri"]) if media.get("uri") else ""
        else:
            playlist_aac = any(_is_aac(c) for c in audio_codecs)
            audio_url = ""
        variants.append({
            "variant_url": urllib.parse.urljoin(master_url, uri),
            "audio_url": audio_url or None,
            "playlist_aac": playlist_aac,
            "copy_audio": playlist_aac,
            "w": w, "h": h, "fps": fps, "bw": bw,
            "codec": vcodec,
            "copy": variant_in_spec(profile, level, w, h, fps),
        })
    if not variants:
        return None
    in_spec = [v for v in variants if v["copy"]]
    in_spec.sort(key=lambda v: v["bw"], reverse=True)

    def closeness(v):
        fps = 30.0 if v["fps"] is None else v["fps"]
        return (abs(v["w"] - 1280) + abs(v["h"] - 720), abs(fps - 30.0), v["bw"])

    fallback = dict(min(variants, key=closeness))
    fallback["copy"] = False
    fallback["copy_audio"] = False
    return {"in_spec": in_spec, "fallback": fallback}

def _fmt_fps(fps):
    if fps is None:
        return "missing"
    if abs(fps - round(fps)) < 0.001:
        return str(int(round(fps)))
    return ("%.3f" % fps).rstrip("0").rstrip(".")

def hls_log_line(v):
    """One choice line. No URL and no query string."""
    mode = "copy" if v.get("copy") else "x264"
    codec = v.get("codec") or "missing"
    return "hls variant %dx%d fps=%s codec=%s bw=%d %s" % (
        int(v.get("w") or 0), int(v.get("h") or 0), _fmt_fps(v.get("fps")), codec, int(v.get("bw") or 0), mode)

def direct_log_line(info, copy):
    mode = "copy" if copy else "x264"
    if not info:
        return "direct 0x0 fps=missing codec=missing %s" % mode
    fps = "%s/%s" % (info.get("fps_n") or 0, info.get("fps_d") or 0)
    return "direct %dx%d fps=%s codec=%s %s" % (
        int(info.get("w") or 0), int(info.get("h") or 0), fps, info.get("vcodec") or "missing", mode)

def _audio_copy_ok(variant, vinfo, ainfo):
    if not variant.get("playlist_aac"):
        return False
    if variant.get("audio_url"):
        if ainfo is None:
            return True
        return ainfo.get("acodec") == "aac"
    if vinfo is None:
        return True
    return bool(vinfo.get("audio")) and vinfo.get("acodec") == "aac"

def choose_hls(plan, probe_fn):
    """Confirm playlist in-spec variants with probe_fn (ffprobe). Else the 720p30-closest x264 fallback."""
    if not plan:
        return None
    for src in plan["in_spec"]:
        info = probe_fn(src["variant_url"]) if probe_fn else None
        if not stream_copy_ok(info):
            continue
        v = dict(src)
        ainfo = probe_fn(v["audio_url"]) if probe_fn and v.get("audio_url") else None
        v["copy"] = True
        v["info"] = info
        v["copy_audio"] = _audio_copy_ok(v, info, ainfo)
        if v.get("fps") is None and info.get("fps_d"):
            v["fps"] = info["fps_n"] / float(info["fps_d"])
        return v
    fb = dict(plan["fallback"])
    fb["copy"] = False
    fb["copy_audio"] = False
    info = probe_fn(fb["variant_url"]) if probe_fn else None
    # Every candidate 404'd or otherwise did not probe. Leave hls unset so /probe
    # and the ladder use the original src (rc4 x264) instead of a 502.
    if not info:
        return None
    fb["info"] = info
    if info.get("fps_d"):
        fb["fps"] = info["fps_n"] / float(info["fps_d"])
        if not fb.get("w"):
            fb["w"] = int(info.get("w") or 0)
            fb["h"] = int(info.get("h") or 0)
    return fb

def log_src(src):
    """Request path for the xcode line, with the query stripped so a cast token is not logged."""
    if src.startswith("http://") or src.startswith("https://"):
        p = urllib.parse.urlsplit(src)
        return urllib.parse.urlunsplit((p.scheme, p.netloc, p.path, "", ""))[:120]
    return src[:120]

def hls_playlist_url(src):
    path = urllib.parse.urlparse(src).path.lower()
    return path.endswith(".m3u8") or path.endswith(".m3u")

def fetch_playlist(url, limit=2 * 1024 * 1024):
    ctx = ssl.create_default_context()
    ca = ca_env().get("SSL_CERT_FILE")
    if ca:
        try:
            ctx.load_verify_locations(ca)
        except Exception:  # noqa: BLE001
            pass
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "misterplex-gcast"})
        with urllib.request.urlopen(req, timeout=15, context=ctx) as resp:
            raw = resp.read(limit + 1)
            final = resp.geturl() or url
        # GCAST_MUT=hlsredir resolves relative variants against the pre-redirect URL.
        if os.environ.get("GCAST_MUT") == "hlsredir":
            final = url
        if len(raw) > limit:
            log("hls playlist too large")
            return None
        return raw.decode("utf-8", "replace"), final
    except Exception as e:  # noqa: BLE001
        log("hls playlist fetch failed: %s" % type(e).__name__)
        return None

def out_fps(fn, fd):
    """Source rate snapped to a broadcast rate; 50/60-family halved; unknown -> 30000/1001."""
    if fn <= 0 or fd <= 0: return "30000/1001"
    r = fn / fd
    if r > 31: r /= 2
    for name, val in (("24000/1001", 24000 / 1001), ("24/1", 24.0), ("25/1", 25.0),
                      ("30000/1001", 30000 / 1001), ("30/1", 30.0)):
        if abs(r - val) / val < 0.0005: return name
    if r < 20: return "30000/1001"
    best = min((("24000/1001", 23.976), ("25/1", 25.0), ("30000/1001", 29.97)), key=lambda c: abs(r - c[1]))
    return best[0]

def build_cmd(ffmpeg, src, w, h, br, off_ms, remux, info, inputs=None, copy_audio=False):
    cmd = [ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error"]
    audio_only = bool(info) and info.get("vcodec") == "none"
    if audio_only:
        # RAOP / music: a 30 fps spectrum through x264 saturated an ARM core and dropped
        # audio, and the decoder kept the last mp4 frame. The shipped helper ffmpeg has
        # showspectrum, fps, and libx264, and no lavfi demuxer or color source, so the
        # picture is one spectrum frame per second at the ladder size. That paints over
        # the latched frame. The graph ends when the audio ends.
        if src.endswith("/raop.wav"):
            cmd += ["-probesize", "32768", "-analyzeduration", "0", "-f", "wav"]
        elif off_ms > 0:
            cmd += ["-ss", "%.3f" % (off_ms / 1000.0)]
        cmd += ["-i", src, "-filter_complex",
                ("[0:a:0]asplit=2[ao][av];[av]showspectrum=s=%dx%d:slide=replace:color=intensity:scale=log,"
                 "fps=1,format=yuv420p,setsar=1[vo]") % (w, h),
                "-map", "[vo]", "-map", "[ao]", "-sn", "-dn",
                "-c:v", "libx264", "-preset", "ultrafast", "-tune", "stillimage",
                "-profile:v", "baseline", "-level", "3.0", "-bf", "0",
                "-g", "1", "-keyint_min", "1", "-r", "1", "-pix_fmt", "yuv420p",
                "-b:v", "32k", "-maxrate", "32k", "-bufsize", "32k"]
        remux, copy_audio = False, False
    inputs = inputs or [(src, "")]
    for url, hdr in ([] if audio_only else inputs):
        if url.startswith("http") and ".m3u8" not in url:
            cmd += ["-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "5"]
        if hdr:
            cmd += ["-headers", hdr]
        if off_ms > 0:
            cmd += ["-ss", "%.3f" % (off_ms / 1000.0)]
        cmd += ["-i", url]
    if not audio_only:
        cmd += ["-map", "0:v:0", "-map", "1:a:0" if len(inputs) > 1 else "0:a:0?", "-sn", "-dn"]
    if remux:
        cmd += ["-c:v", "copy", "-bsf:v", "h264_mp4toannexb"]
    elif not audio_only:
        fps = out_fps(info.get("fps_n", 0), info.get("fps_d", 0)) if info else "30000/1001"
        fn, fd = (int(x) for x in fps.split("/"))
        gop = max(12, int(round(2 * fn / fd)))
        vf = ("scale=%d:%d:force_original_aspect_ratio=decrease:out_color_matrix=bt601:out_range=tv,"
              "pad=%d:%d:(ow-iw)/2:(oh-ih)/2,setsar=1,fps=%s,format=yuv420p") % (w, h, w, h, fps)
        cmd += ([] if audio_only else ["-vf", vf]) + ["-c:v", "libx264", "-preset", "veryfast", "-profile:v", "main", "-level", "4.0",
                "-bf", "0", "-g", str(gop), "-keyint_min", str(gop), "-sc_threshold", "0",
                "-b:v", "%dk" % br, "-maxrate", "%dk" % br, "-bufsize", "%dk" % (br * 2),
                "-colorspace", "smpte170m", "-color_primaries", "smpte170m", "-color_trc", "smpte170m",
                "-color_range", "tv"]
    if copy_audio:
        cmd += ["-c:a", "copy"]
    else:
        cmd += ["-c:a", "aac", "-ac", "2", "-ar", "48000", "-b:a", "128k"]
    cmd += ["-f", "mpegts", "-muxdelay", "0", "pipe:1"]
    return cmd

def _sq(a):
    return "'" + a.replace("'", "'\\''") + "'"

def build_yt_pipe_cmd(ffmpeg, ytdlp, vid, itags, off_ms):
    """yt-dlp -f itag -o - | ffmpeg copy -> MPEG-TS (avoids googlevideo 403)."""
    env = ca_env()
    parts = []
    for itag in itags:
        ycmd = [ytdlp, "-f", itag, "-o", "-", "--no-playlist", "--no-progress", "--no-part"] + js_runtime_args()
        ycmd += ["https://www.youtube.com/watch?v=" + vid]
        parts.append(" ".join(_sq(a) for a in ycmd))
    ff = _sq(ffmpeg)
    ss = ("-ss %.3f" % (off_ms / 1000.0)) if off_ms > 0 else ""
    if len(itags) == 1:
        script = ("%s | %s -nostdin -hide_banner -loglevel error %s -i pipe:0 "
                  "-map 0:v:0? -map 0:a:0? -sn -dn -c:v copy -bsf:v h264_mp4toannexb "
                  "-c:a copy -f mpegts -muxdelay 0 pipe:1") % (parts[0], ff, ss)
    else:
        # The subshell's own stdout is /dev/null so a background yt-dlp does not
        # keep this script's pipe open after ffmpeg exits. fd 7 is a dup of
        # that stdout, taken before the fifo redirect: with >/dev/null it is
        # /dev/null, so a child that outlives the 403 cannot hold the response
        # pipe. The redirect inside the subshell still feeds the fifo. A 403
        # on the first itag can then fall through instead of sitting until
        # the 17.5s deadline.
        script = ("set -e; d=$(mktemp -d); mkfifo \"$d/v\" \"$d/a\"; "
                  "(exec 7>&1; %s > \"$d/v\") >/dev/null & "
                  "(exec 7>&1; %s > \"$d/a\") >/dev/null & "
                  "%s -nostdin -hide_banner -loglevel error %s -i \"$d/v\" -i \"$d/a\" "
                  "-map 0:v:0 -map 1:a:0 -sn -dn -c:v copy -bsf:v h264_mp4toannexb -c:a copy "
                  "-f mpegts -muxdelay 0 pipe:1; ec=$?; rm -rf \"$d\"; exit $ec") % (
                      parts[0], parts[1], ff, ss)
        if os.environ.get("GCAST_MUT") == "nopipenull":
            script = script.replace(" >/dev/null &", " &")
    return ["/bin/bash", "-c", script], env


def kill(proc):
    try: os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError: return
    try: proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        try: os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError: pass

class H(BaseHTTPRequestHandler):
    server_version = "gcast-xcode/2-standalone"
    def log_message(self, fmt, *a): log("%s %s" % (self.client_address[0], fmt % a))
    def text(self, code, body):
        b = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(b)))
        # Headers and body in one write. A second write loses the body when
        # the Ready probe reads under load. Do not set wbufsize: /ladder.ts
        # uses this same handler.
        if self.request_version != "HTTP/0.9":
            self._headers_buffer.append(b"\r\n" + b)
            self.flush_headers()
        else:
            self.wfile.write(b)
    def do_GET(self):  # noqa: N802
        u = urllib.parse.urlparse(self.path); qs = urllib.parse.parse_qs(u.query)
        g = lambda k, d="": (qs.get(k) or [d])[0]
        if u.path in ("/", "/health"):
            mode = "standalone" if os.path.isdir(misterplex_root()) else "helper"
            return self.text(200, "ok gcast-xcode mode=%s\n" % mode)
        if u.path == "/probe" and g("selftest") == "1":
            info = probe_selftest(self.server.ffprobe)
            if not info or not info.get("audio"):
                return self.text(502, "probe failed\n")
            return self.text(200, "".join("%s=%s\n" % kv for kv in info.items()))
        src = g("src")
        if u.path == "/yt.ts":
            src = "yt:" + g("v")
        elif u.path not in ("/probe", "/ladder.ts"): return self.text(404, "use /probe, /ladder.ts or /yt.ts\n")
        if not ok_src(src): return self.text(400, "src must be http(s) or yt:<videoId>\n")
        yt = yt_resolve(src[3:], self.server.ytdlp) if src.startswith("yt:") else None
        if src.startswith("yt:") and not yt: return self.text(502, "youtube resolve failed\n")
        hls = None
        if not yt and hls_playlist_url(src):
            got = fetch_playlist(src)
            plan = plan_hls_master(got[0], got[1]) if got else None
            if plan:
                hls = choose_hls(plan, lambda url: probe(url, self.server.ffprobe))
            if not hls or not hls.get("info"):
                hls = None
        if yt:
            info = yt_probe(src[3:])
        elif hls and hls.get("info"):
            info = hls["info"]
        elif hls:
            info = None
        else:
            info = probe(src, self.server.ffprobe)
        if u.path == "/probe":
            if not info: return self.text(502, "probe failed\n")
            return self.text(200, "".join("%s=%s\n" % kv for kv in info.items()))
        try:
            w = max(64, min(1920, int(g("w", "1280")))); h = max(64, min(1080, int(g("h", "720"))))
            br = max(200, min(8000, int(g("br", "3000"))))
            off = max(0, int(g("start_ms" if u.path == "/yt.ts" else "offset_ms", "0")))
        except ValueError:
            return self.text(400, "bad w/h/br/offset_ms\n")
        pipe_env = ca_env()
        if u.path == "/yt.ts" or (src.startswith("yt:") and yt):
            itags = yt.get("itags") or ["best"]
            if isinstance(itags, str): itags = [x for x in itags.split("+") if x]
            cmd, pipe_env = build_yt_pipe_cmd(self.server.ffmpeg, self.server.ytdlp, src[3:], itags, off)
            remux = True
            log("xcode /yt.ts pipe itags=%s off=%d" % ("+".join(itags), off))
        else:
            # In-spec H.264 (including an HLS variant chosen from the master) is remuxed.
            # remux=1 cannot force a copy of an out-of-spec stream; that still re-encodes.
            if hls:
                play = hls["variant_url"]
                inputs = [(play, "")]
                if hls.get("audio_url"):
                    inputs.append((hls["audio_url"], ""))
                remux = bool(hls.get("copy")) and stream_copy_ok(info)
                copy_audio = bool(remux and hls.get("copy_audio"))
                cmd = build_cmd(self.server.ffmpeg, play, w, h, br, off, remux, info, inputs, copy_audio=copy_audio)
                shown = dict(hls)
                shown["copy"] = remux
                log(hls_log_line(shown))
            else:
                remux = stream_copy_ok(info)
                copy_audio = remux and (info or {}).get("acodec") == "aac"
                cmd = build_cmd(self.server.ffmpeg, src, w, h, br, off, remux, info, None, copy_audio=copy_audio)
                log(direct_log_line(info, remux))
            log("xcode %s %dx%d %dk off=%d copy=%d src=%s" % (u.path, w, h, br, off, int(remux), log_src(src)))
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, preexec_fn=os.setsid, env=pipe_env)
        with JOBS_MU:
            JOBS.append(proc)
            while len(JOBS) > MAX_JOBS:
                old = JOBS.pop(0); log("job cap: killing pid %d" % old.pid); kill(old)
        self.send_response(200); self.send_header("Content-Type", "video/mp2t")
        self.send_header("Cache-Control", "no-store"); self.send_header("Connection", "close"); self.end_headers()
        sent = 0
        try:
            while True:
                chunk = proc.stdout.read1(64 * 1024)  # what is there now: low-rate live streams
                if not chunk: break
                self.wfile.write(chunk); sent += len(chunk)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            kill(proc)
            with JOBS_MU:
                if proc in JOBS: JOBS.remove(proc)
            err_b = (proc.stderr.read() or b"")[-400:]
            err = err_b.decode("utf-8", "replace").strip()
            log("xcode done bytes=%d rc=%s %s" % (sent, proc.returncode, err))
            if src.startswith("yt:") and (sent == 0 or b"403" in err_b or "403" in err):   # F1: cached googlevideo URLs went 403
                yt_evict(src[3:])

def main():
    ap = argparse.ArgumentParser()
    # On-card helper: loopback, port 18781. A computer overrides with
    # --bind 0.0.0.0 --port 18780, or GCAST_XCODE_BIND / GCAST_XCODE_PORT.
    ap.add_argument("--bind", default=os.environ.get("GCAST_XCODE_BIND", "127.0.0.1"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("GCAST_XCODE_PORT", "18781")))
    ap.add_argument("--ffmpeg", default=which_default("GCAST_FFMPEG", ["ffmpeg"], "ffmpeg"))
    ap.add_argument("--ffprobe", default=which_default("GCAST_FFPROBE", ["ffprobe"], "ffprobe"))
    ap.add_argument("--ytdlp", default=which_default("GCAST_YTDLP", ["yt-dlp", "yt-dlp_linux_armv7l"], "yt-dlp"))
    a = ap.parse_args()
    srv = ThreadingHTTPServer((a.bind, a.port), H); srv.daemon_threads = True
    srv.ffmpeg, srv.ffprobe, srv.ytdlp = a.ffmpeg, a.ffprobe, a.ytdlp
    log("gcast-xcode on %s:%d ytdlp=%s js=%s" % (a.bind, a.port, a.ytdlp, js_runtime_args() or "none"))
    srv.serve_forever()

if __name__ == "__main__":
    main()
