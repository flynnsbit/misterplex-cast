#!/usr/bin/env python3
"""airplay-xcode: helper that turns any cast URL into an H.264/AAC MPEG-TS stream misterplexd can open.
GCAST_XCODE and other GCAST_* environment names are read-compat aliases for AIRPLAY_*.

  GET /health
  GET /probe?src=<url>                 -> text "w=..\nh=..\ndur_ms=..\nfps_n=..\nfps_d=..\naudio=0|1\nvcodec=.."
  GET /admit?src=<url>                -> 200 "copy" or "audio", or 415 "refused" (AirPlay /play)
  GET /ladder.ts?src=<url>&w=&h=&br=[&offset_ms=][&remux=1]
      Host FFmpeg pulls an in-spec source and copies it. An HLS master is parsed first: the
      highest-bandwidth in-spec H.264 variant (Baseline/Main/High, level <= 4.1, <=1280x720,
      <=30 fps) is copied, with its AUDIO group when the master names one. AirPlay video with
      no in-spec option (1080p, 60 fps, HEVC, 10-bit, level above 4.1) is refused: HTTP 415
      and no x264. RAOP (/raop.wav, vcodec none) still builds one spectrum frame with libx264.
      The video x264 ladder stays in build_cmd for that spectrum frame only.
      AIRPLAY_MUT=videofallback (GCAST_MUT is the compat alias) turns the video ladder back on; the unit test must fail then.
      offset_ms= runs -ss before -i (same contract as the lab ladder proxy, so MediaPlayer::seekMs works).
  GET /yt.ts?v=<videoId>[&start_ms=]   YouTube: yt-dlp resolves avc1<=720p30 + m4a, pipes yt-dlp -o - into
      ffmpeg copy-remux (no re-encode). On 403/empty, cache invalidated for next play. Works standalone
      under /media/fat/misterplex/. Set AIRPLAY_XCODE (or the compat alias GCAST_XCODE) to use a helper on a computer instead.
Only http(s) or yt:<11-char id> src is accepted; at most MAX_JOBS ffmpeg run at once (oldest killed).
The on-card helper binds 127.0.0.1:18781. A computer helper uses --bind 0.0.0.0 --port 18780.
"""
import argparse, hmac, ipaddress, json, os, re, signal, socket, ssl, subprocess, sys, tempfile, threading, time, urllib.parse, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

def _env(primary, legacy=None, default=None):
    """AIRPLAY_* wins when set (even empty). GCAST_* is the read-compat alias."""
    if primary in os.environ:
        return os.environ[primary]
    if legacy and legacy in os.environ:
        return os.environ[legacy]
    return default

def _mut(tag):
    return _env("AIRPLAY_MUT", "GCAST_MUT", "") == tag

MAX_JOBS = 2
JOBS = []
JOBS_MU = threading.Lock()
LOG = sys.stderr

# Real Apple receivers identify as AppleCoreMedia. CDN / SSAI edges (Fubo etc.)
# often 403/415 misterplex-airplay and ffmpeg's default UA.
APPLE_UA = "AppleCoreMedia/1.0.0.21G93 (iPhone; U; CPU OS 17_6 like Mac OS X; en_us)"
# Optional ?hdr= on /probe /admit /ladder.ts: CRLF "Name: value" lines, whitelist only.
_FWD_HEADER_NAMES = {
    "user-agent": "User-Agent",
    "referer": "Referer",
    "origin": "Origin",
    "cookie": "Cookie",
    "x-playback-session-id": "X-Playback-Session-Id",
    "x-apple-session-id": "X-Apple-Session-ID",
}

# Access logs used to print src=...X-Plex-Token%3D<live token> on GET /admit.
# urllib.quote of an already-encoded src yields %25253D on the requestline;
# %(?:25)*3D matches = as %3D / %253D / %25253D without unquoting the URL.
_TOKEN_IN_LOG = re.compile(
    r"(?i)(X-Plex-Token|accessToken)((?:=|%(?:25)*3D|:)\s*)[^\s&\"']+"
)

def redact_log_line(msg):
    """Replace token query/header values. Keep the key so the line is still useful."""
    s = str(msg)
    if _mut("noredact"):
        return s
    return _TOKEN_IN_LOG.sub(r"\1\2REDACTED", s)

def log(msg):
    LOG.write(time.strftime("%Y-%m-%d %H:%M:%S %Z ") + redact_log_line(msg) + "\n"); LOG.flush()

def sanitize_fwd_headers(s):
    """Parse optional sender hdr= query. Whitelist only; drop CR/LF in values."""
    if not s:
        return {}
    out = {}
    for line in str(s).replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        line = line.strip()
        if not line or ":" not in line:
            continue
        name, _, value = line.partition(":")
        name = name.strip()
        value = value.strip()
        if not name or "\r" in value or "\n" in value:
            continue
        canon = _FWD_HEADER_NAMES.get(name.lower())
        if not canon:
            continue
        out[canon] = value
    return out

def _headers_blob(hdrs):
    """dict -> ffmpeg/ffprobe -headers value (each line ends with CRLF)."""
    if not hdrs:
        return ""
    return "".join("%s: %s\r\n" % (k, v) for k, v in hdrs.items())

def _hdr_has_ua(hdr):
    if not hdr:
        return False
    if isinstance(hdr, dict):
        return any(str(k).lower() == "user-agent" for k in hdr)
    for line in str(hdr).replace("\r\n", "\n").split("\n"):
        if line.lower().startswith("user-agent:"):
            return True
    return False

def _urllib_headers(extra=None):
    """Default Apple UA, overridable by sanitized forwarded headers."""
    h = {"User-Agent": APPLE_UA}
    if extra:
        h.update(extra)
    return h

YT_ID = re.compile(r"^[A-Za-z0-9_-]{11}$")
# FIELD-1018 F1: 2026 YouTube lists the AAC track as 140-5/140-20 (DRC / language variants), so a bare "+140"
# no longer matches on some videos: avc1 by codec and height, any m4a audio, then progressive. rc3: GCAST_YT_FMT overrides.
YT_FMT = _env("AIRPLAY_YT_FMT", "GCAST_YT_FMT",
    "bv*[vcodec^=avc1][height<=720][fps<=30][protocol=https]+ba[ext=m4a][protocol=https]/"
    "bv*[vcodec^=avc1][height<=720][fps<=30]+ba[ext=m4a]/b[vcodec^=avc1][height<=720]/18")
# yt-dlp tries again with other player clients when the default one returns nothing (bot check, PO token, SABR).
YT_CLIENT_RETRY = ([], ["--extractor-args", "youtube:player_client=mweb,web_safari"],
                   ["--extractor-args", "youtube:player_client=tv,web_embedded"],
                   ["--extractor-args", "youtube:player_client=android"])
YT_CACHE = {}
YT_MU = threading.Lock()
YT_TTL = int(_env("AIRPLAY_YT_TTL", "GCAST_YT_TTL", "600"))

def misterplex_root():
    return _env("AIRPLAY_MISTERPLEX_ROOT", "GCAST_MISTERPLEX_ROOT", "/media/fat/misterplex")

def which_default(env_key, names, fallback, legacy_key=""):
    v = os.environ.get(env_key) or (os.environ.get(legacy_key) if legacy_key else None)
    if v: return v
    root = misterplex_root()
    for n in names:
        for cand in (os.path.join(root, "bin", n), os.path.join(root, n)):
            if os.path.isfile(cand) and os.access(cand, os.X_OK): return cand
    return fallback

def cookies_args():
    """GH#8: Netscape cookies file so yt-dlp can pass the bot check. Missing path is omitted."""
    p = (_env("AIRPLAY_YT_COOKIES", "GCAST_YT_COOKIES", "") or os.environ.get("YT_COOKIES") or "").strip()
    if not p:
        p = os.path.join(misterplex_root(), "youtube.cookies")
    if p and os.path.isfile(p):
        return ["--cookies", p]
    return []

def js_runtime_args():
    explicit = _env("AIRPLAY_YT_JS", "GCAST_YT_JS", "").strip()
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

# Protocols ffmpeg/ffprobe may open for an input (#63). Without this a hostile playlist or
# redirect can make ffmpeg read file:, concat:, subfile:, ftp:, rtmp: and the like.
# net: http(s) inputs and HLS segments/keys. pipe: yt-dlp stdout. file: the fifo/temp file.
PROTO_NET = "http,https,tcp,tls,crypto"
PROTO_PIPE = "pipe"
PROTO_FILE = "file"

def proto_whitelist(url):
    """The -protocol_whitelist value for one ffmpeg/ffprobe input."""
    if url.startswith("http://") or url.startswith("https://"): return PROTO_NET
    if url.startswith("pipe:"): return PROTO_PIPE
    return PROTO_FILE

def src_host(src):
    """Host part of an http(s) src, lowercased, no brackets or port. "" when none."""
    try:
        return (urllib.parse.urlsplit(src).hostname or "").lower()
    except ValueError:
        return ""

def host_is_local(host):
    """True for loopback, link-local (incl. 169.254.169.254 metadata), unspecified and
    multicast literals, and for localhost names. Hostnames are NOT resolved here
    (a name that resolves to a local address is a known residual, see docs)."""
    if not host: return True
    host = host.rstrip(".")
    if host == "localhost" or host.endswith(".localhost"): return True
    try:
        ip = ipaddress.ip_address(host)
    except ValueError:
        # getaddrinfo (curl, ffmpeg) also reads 127.1, 0x7f.1, 0177.0.0.1 and 2130706433.
        try:
            ip = ipaddress.ip_address(socket.inet_ntoa(socket.inet_aton(host)))
        except (OSError, ValueError):
            return False
    if getattr(ip, "ipv4_mapped", None): ip = ip.ipv4_mapped
    return ip.is_loopback or ip.is_link_local or ip.is_unspecified or ip.is_multicast

def peer_is_local(addr):
    try:
        ip = ipaddress.ip_address(addr.split("%")[0])
    except ValueError:
        return False
    if getattr(ip, "ipv4_mapped", None): ip = ip.ipv4_mapped
    return ip.is_loopback

def ok_src(src, peer_local=True):
    """peer_local=False means the HTTP client is not on this machine (helper bound to a
    LAN address): refuse sources that point back at this machine or at link-local space.
    The on-card daemon talks to 127.0.0.1 and keeps its old behaviour."""
    if src.startswith("yt:"): return bool(YT_ID.match(src[3:]))
    if not ((src.startswith("http://") or src.startswith("https://")) and not any(c in src for c in "\r\n ")):
        return False
    if not peer_local and os.environ.get("AIRPLAY_XCODE_ALLOW_LOCAL_SRC") != "1" and os.environ.get("GCAST_XCODE_ALLOW_LOCAL_SRC") != "1":
        if "\\" in src or host_is_local(src_host(src)):
            return False
    return True

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
        cmd = [ytdlp, "-j", "--no-playlist", "--skip-download", "-f", YT_FMT] + js_runtime_args() + cookies_args() + \
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
        fd, path = tempfile.mkstemp(prefix="airplay-selftest-", suffix=".wav")
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

_TS_PACKET = 188
_SDT_PID = 0x11  # DVB SDT. The card's ffprobe (--enable-small) aborts in glibc iconv on this table.


def strip_sdt(data):
    """Drop 188-byte TS packets whose PID is 0x11. A probe of what remains is the
    fallback when ffprobe aborts on the SDT. Sync is 0x47; a short read is kept
    only as whole packets."""
    if not data:
        return b""
    out = bytearray()
    i, n = 0, len(data)
    while i < n and data[i] != 0x47:
        i += 1
    while i + _TS_PACKET <= n:
        if data[i] != 0x47:
            i += 1
            continue
        pid = ((data[i + 1] & 0x1F) << 8) | data[i + 2]
        if pid != _SDT_PID:
            out += data[i:i + _TS_PACKET]
        i += _TS_PACKET
    return bytes(out)


def _http_ts_url(src):
    if not (src.startswith("http://") or src.startswith("https://")):
        return False
    path = urllib.parse.urlparse(src).path.lower()
    return path.endswith(".ts") or path.endswith(".mts")


def _ffprobe_cmd(ffprobe, src, shallow=False, headers=None):
    headers = headers or {}
    ua = headers.get("User-Agent") or APPLE_UA
    cmd = [ffprobe, "-v", "error", "-protocol_whitelist", proto_whitelist(src),
           "-user_agent", ua]
    rest = {k: v for k, v in headers.items() if k.lower() != "user-agent"}
    if rest:
        cmd += ["-headers", _headers_blob(rest)]
    cmd += ["-print_format", "json", "-show_streams", "-show_format", src]
    if src.endswith("/raop.wav"):  # AirPlay RAOP live PCM: header is enough, do not wait 5 s of live audio
        cmd[1:1] = ["-probesize", "32768", "-analyzeduration", "0", "-f", "wav"]
    elif shallow:
        # Skip the deep table walk. An SDT (PID 0x11) aborts this ffprobe in iconv.
        cmd[1:1] = ["-probesize", "32768", "-analyzeduration", "0"]
    return cmd


def _accept_probe(out):
    """out is a probe dict (tests), ffprobe stdout, or None. Never raises."""
    if out is None:
        log("probe failed")
        return None
    if isinstance(out, dict):
        return out
    try:
        j = json.loads(out or b"{}")
    except Exception as e:  # noqa: BLE001
        log("probe failed (%s)" % type(e).__name__)
        return None
    return parse_probe_json(j)


def _run_ffprobe(cmd, timeout=25):
    """ffprobe in its own session. An iconv abort kills that process, not the helper."""
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=timeout, env=ca_env(), start_new_session=True)
    except Exception as e:  # noqa: BLE001
        log("probe failed (%s)" % type(e).__name__)
        return None
    if proc.returncode != 0:
        log("probe failed (rc=%s)" % proc.returncode)
        return None
    return _accept_probe(proc.stdout)


def fetch_prefix(url, limit=512 * 1024, headers=None):
    """First bytes of an http(s) segment, for a local SDT-stripped probe."""
    ctx = ssl.create_default_context()
    ca = ca_env().get("SSL_CERT_FILE")
    if ca:
        try:
            ctx.load_verify_locations(ca)
        except Exception:  # noqa: BLE001
            pass
    try:
        req = urllib.request.Request(url, headers=_urllib_headers(headers))
        with urllib.request.urlopen(req, timeout=15, context=ctx) as resp:
            return resp.read(limit)
    except Exception as e:  # noqa: BLE001
        log("probe prefix fetch failed: %s" % type(e).__name__)
        return None


def probe(src, ffprobe="ffprobe", runner=None, budget_s=25, headers=None):
    """Probe src. A crash, timeout, or bad JSON is a failure (None), never a dead helper.

    runner(cmd), when set, stands in for ffprobe and must not touch the network: it
    returns a probe dict, stdout bytes, or None. Production retries once with a short
    analyzeduration, then a longer timeout on http(s), then (http .ts only) a prefix
    with the SDT packets removed. RAOP stays the single short wav probe."""
    headers = headers or {}
    def once(shallow, timeout=None):
        cmd = _ffprobe_cmd(ffprobe, src, shallow=shallow, headers=headers)
        if runner is None:
            return _run_ffprobe(cmd, timeout=budget_s if timeout is None else timeout)
        try:
            return _accept_probe(runner(cmd))
        except Exception as e:  # noqa: BLE001
            log("probe failed (%s)" % type(e).__name__)
            return None

    info = once(False)
    if info or src.endswith("/raop.wav"):
        return info
    info = once(True)
    if info:
        return info
    # Production http(s): one longer attempt before giving up (Fubo SSAI / slow CDN).
    if runner is None and (src.startswith("http://") or src.startswith("https://")):
        log("probe retry long")
        info = once(False, timeout=max(budget_s * 2, 30))
        if info:
            return info
    if runner is not None or not _http_ts_url(src):
        return info
    blob = fetch_prefix(src, headers=headers)
    if not blob:
        return None
    stripped = strip_sdt(blob)
    if not stripped or stripped == blob:
        return None
    fd, path = tempfile.mkstemp(prefix="airplay-sdt-", suffix=".ts")
    try:
        os.write(fd, stripped)
        os.close(fd)
        fd = None
        return _run_ffprobe(_ffprobe_cmd(ffprobe, path, shallow=True, headers=headers))
    except Exception as e:  # noqa: BLE001
        log("probe failed (%s)" % type(e).__name__)
        return None
    finally:
        if fd is not None:
            try:
                os.close(fd)
            except OSError:
                pass
        try:
            os.unlink(path)
        except OSError:
            pass


def playlist_probe_fallback(plan):
    """Master-playlist size when every ffprobe died. vcodec stays empty so the
    ladder re-encodes. /probe can answer 200, and the cast is not refused."""
    if not plan:
        return None
    v = plan.get("fallback")
    if not v:
        ins = plan.get("in_spec") or []
        v = ins[0] if ins else None
    if not v:
        return None
    try:
        w, h = int(v.get("w") or 0), int(v.get("h") or 0)
    except (TypeError, ValueError):
        return None
    if w <= 0 or h <= 0:
        return None
    return {"w": w, "h": h, "fps_n": 30000, "fps_d": 1001, "audio": 1,
            "vcodec": "", "acodec": "", "probe_failed": 1}

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
    if _mut("profname"):
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
    if not _mut("hlslevel"):
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
    # X/Twitter (and some CDNs) omit CODECS on EXT-X-MEDIA. The URI is still the
    # soundtrack; treat a blank codec as AAC so we map the rendition (#236).
    unknown = [m for m in group_m if not (m.get("codecs") or "").strip()]
    pool = aac or unknown or group_m
    defaults = [m for m in pool if m.get("default")]
    chosen = (defaults or pool)[0]
    codecs = chosen.get("codecs") or ""
    copy_aac = _is_aac(codecs) or not codecs.strip()
    return chosen, copy_aac

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
    return {"in_spec": in_spec, "fallback": fallback, "variants": variants}

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

NOTICE_RESOLUTION = "Video too high resolution for MiSTer (max 720p)"
NOTICE_FRAMERATE = "Max 30 fps on MiSTer"
NOTICE_FORMAT = "format not supported"
NOTICE_DRM = "DRM-protected stream not supported"

_REFUSE_KINDS = ("resolution", "framerate", "format", "drm")

def notice_for(kind):
    if kind == "resolution":
        return NOTICE_RESOLUTION
    if kind == "framerate":
        return NOTICE_FRAMERATE
    if kind == "drm":
        return NOTICE_DRM
    return NOTICE_FORMAT

def detect_drm(playlist_text):
    """True when the playlist uses FairPlay / SAMPLE-AES style DRM. AES-128 is not DRM."""
    if not playlist_text:
        return False
    for raw in playlist_text.splitlines():
        s = raw.strip()
        if not (s.startswith("#EXT-X-KEY:") or s.startswith("#EXT-X-SESSION-KEY:")):
            continue
        attrs = parse_attr_list(s.split(":", 1)[1])
        method = (attrs.get("METHOD") or "").upper()
        if method in ("SAMPLE-AES", "SAMPLE-AES-CTR"):
            return True
        keyformat = attrs.get("KEYFORMAT") or ""
        if keyformat == "com.apple.streamingkeydelivery":
            return True
        uri = attrs.get("URI") or ""
        if uri.startswith("skd://"):
            return True
    return False

def video_fallback_on():
    """GCAST_MUT=videofallback puts out-of-spec video back on the x264 ladder.
    The unit test must fail while that mutant is set."""
    return _mut("videofallback")

def _over_size_cap(w, h):
    return w > 1280 or h > 720

def variant_refuse_kind(v):
    """Playlist-only reason. resolution: size or level cap; framerate: rate cap only.
    format: not H.264, or an H.264 profile this core does not copy (10-bit and the rest)."""
    codec = v.get("codec") or ""
    try:
        w, h = int(v.get("w") or 0), int(v.get("h") or 0)
    except (TypeError, ValueError):
        w, h = 0, 0
    fps = v.get("fps")
    parsed = parse_avc1(codec)
    if parsed:
        profile, level = parsed
        if profile not in _AVC1_PROFILES:
            return "format"
        if level <= 0 or level > 41 or _over_size_cap(w, h):
            return "resolution"
        if fps is not None and fps > 30.01:
            return "framerate"
        return "format"
    if _over_size_cap(w, h):
        return "resolution"
    if fps is not None and fps > 30.01:
        return "framerate"
    return "format"

def hls_plan_refuse(plan):
    """No in-spec variant. Prefer framerate, then resolution, then format.
    framerate means a rendition fits the size and level caps and only the rate is over,
    so that is the blocker to name (X portrait masters: 4K..320x568, all 60 fps)."""
    variants = (plan or {}).get("variants") or []
    if not variants and plan and plan.get("fallback"):
        variants = [plan["fallback"]]
    kinds = [variant_refuse_kind(v) for v in variants] or ["format"]
    for kind in ("framerate", "resolution"):
        if kind in kinds:
            return kind
    return "format"

def refuse_kind(info):
    """Probe dict that is not copyable. None when stream_copy_ok."""
    if stream_copy_ok(info):
        return None
    if not info:
        return "format"
    if str(info.get("vcodec") or "").lower() != "h264":
        return "format"
    try:
        w, h = int(info.get("w") or 0), int(info.get("h") or 0)
        fn, fd = int(info.get("fps_n") or 0), int(info.get("fps_d") or 0)
        level = float(info.get("level") or 0)
    except (TypeError, ValueError):
        return "format"
    fps = (fn / float(fd)) if fn > 0 and fd > 0 else None
    over_level = level > 41 or (0 < level < 10 and level > 4.1)
    if _over_size_cap(w, h) or over_level:
        return "resolution"
    # 10-bit, a non-4:2:0 pix_fmt or a profile the core does not copy is format,
    # even when the rate is also over (GBE #627 P3-3).
    try:
        bits = int(info.get("bits") or 0)
    except (TypeError, ValueError):
        bits = -1
    if bits not in (0, 8) or not _pix_copy_ok(info.get("pix_fmt")) or not _profile_copy_ok(info.get("profile")):
        return "format"
    if fps is not None and fps > 30.01:
        return "framerate"
    return "format"

def _refused(kind):
    if kind not in _REFUSE_KINDS:
        kind = "format"
    return {"refused": 1, "reason": kind, "notice": notice_for(kind), "copy": False}

def _audio_only_src(src, info):
    if (src or "").endswith("/raop.wav"):
        return True
    return bool(info) and info.get("vcodec") == "none"

def playback_decision(src, info, hls, plan):
    """copy and audio play. A failed probe attempts x264. Probed out-of-spec video is 415.
    GCAST_MUT=videofallback returns action x264 instead of refuse."""
    if _audio_only_src(src, info):
        return {"action": "audio", "status": 200, "reason": "", "notice": ""}
    hinfo = (hls or {}).get("info") or info
    if hls and hls.get("copy") and stream_copy_ok(hinfo):
        return {"action": "copy", "status": 200, "reason": "", "notice": ""}
    if not hls and stream_copy_ok(info):
        return {"action": "copy", "status": 200, "reason": "", "notice": ""}
    if video_fallback_on():
        return {"action": "x264", "status": 200, "reason": "", "notice": ""}
    if hls and hls.get("refused"):
        reason = hls.get("reason") or "format"
        if reason not in _REFUSE_KINDS:
            reason = "format"
        return {"action": "refuse", "status": 415, "reason": reason,
                "notice": hls.get("notice") or notice_for(reason)}
    # Explicit playlist refuse (master has no in-spec variant) before probe-miss handling.
    if plan and not (plan.get("in_spec") or []):
        kind = hls_plan_refuse(plan)
        return {"action": "refuse", "status": 415, "reason": kind, "notice": notice_for(kind)}
    # Probe failure is not a refusal: attempt playback (Fubo SSAI ffprobe miss, etc.).
    if info is None or info.get("probe_failed"):
        return {"action": "x264", "status": 200, "reason": "", "notice": ""}
    kind = refuse_kind(info) or "format"
    return {"action": "refuse", "status": 415, "reason": kind, "notice": notice_for(kind)}

def notice_event(decision):
    """The on-screen event that goes with a 415. None when the cast is accepted."""
    if not decision or decision.get("action") != "refuse":
        return None
    return {"type": "notice", "text": decision.get("notice") or NOTICE_FORMAT,
            "status": int(decision.get("status") or 415)}

def _fallback_choice(plan, probe_fn):
    """Old closest-to-720p30 x264 pick. Only GCAST_MUT=videofallback calls this."""
    fb = dict(plan["fallback"])
    fb["copy"] = False
    fb["copy_audio"] = False
    info = probe_fn(fb["variant_url"]) if probe_fn else None
    if not info:
        return None
    fb["info"] = info
    if info.get("fps_d"):
        fb["fps"] = info["fps_n"] / float(info["fps_d"])
        if not fb.get("w"):
            fb["w"] = int(info.get("w") or 0)
            fb["h"] = int(info.get("h") or 0)
    return fb

def info_from_hls_variant(v):
    """Synthetic probe dict from master #EXT-X-STREAM-INF tags (no ffprobe)."""
    parsed = parse_avc1(v.get("codec") or "")
    if not parsed:
        return None
    profile_idc, level_idc = parsed
    profile = {0x42: "Baseline", 0x4D: "Main", 0x64: "High"}.get(profile_idc, "")
    level = level_idc / 10.0 if level_idc >= 10 else float(level_idc)
    try:
        w, h = int(v.get("w") or 0), int(v.get("h") or 0)
    except (TypeError, ValueError):
        return None
    fps = v.get("fps")
    if fps is None:
        return None
    try:
        fps = float(fps)
    except (TypeError, ValueError):
        return None
    if abs(fps - round(fps)) < 0.001:
        fn, fd = int(round(fps)), 1
    else:
        fn, fd = int(round(fps * 1000)), 1000
    return {"w": w, "h": h, "fps_n": fn, "fps_d": fd, "audio": 1, "vcodec": "h264",
            "acodec": "aac", "profile": profile, "level": level,
            "pix_fmt": "yuv420p", "bits": 8, "playlist_trust": 1}


def choose_hls_playlist_trust(plan):
    """Pick highest in-spec master variant when ffprobe cannot run (on-card HLS). /admit only."""
    for src in (plan or {}).get("in_spec") or []:
        info = info_from_hls_variant(src)
        if not info or not stream_copy_ok(info):
            continue
        v = dict(src)
        v["copy"] = True
        v["info"] = info
        v["copy_audio"] = bool(src.get("copy_audio"))
        return v
    return None


def choose_hls(plan, probe_fn):
    """Best in-spec variant that probe_fn confirms. None in-spec: refuse, do not probe the rest.
    A probe crash of an in-spec variant (None) is not a fallback. GCAST_MUT=videofallback
    restores the closest-to-720p30 x264 pick."""
    if not plan:
        return None
    if not plan.get("in_spec") and not video_fallback_on():
        return _refused(hls_plan_refuse(plan))
    probed_out = None
    for src in plan.get("in_spec") or []:
        info = probe_fn(src["variant_url"]) if probe_fn else None
        if not info:
            continue
        if not stream_copy_ok(info):
            probed_out = info
            continue
        v = dict(src)
        ainfo = probe_fn(v["audio_url"]) if probe_fn and v.get("audio_url") else None
        v["copy"] = True
        v["info"] = info
        v["copy_audio"] = _audio_copy_ok(v, info, ainfo)
        if v.get("fps") is None and info.get("fps_d"):
            v["fps"] = info["fps_n"] / float(info["fps_d"])
        return v
    if video_fallback_on():
        return _fallback_choice(plan, probe_fn)
    if probed_out is not None:
        return _refused(refuse_kind(probed_out))
    return None

def log_src(src):
    """Request path for the xcode line, with the query stripped so a cast token is not logged."""
    if src.startswith("http://") or src.startswith("https://"):
        p = urllib.parse.urlsplit(src)
        return urllib.parse.urlunsplit((p.scheme, p.netloc, p.path, "", ""))[:120]
    return src[:120]

def hls_playlist_url(src):
    path = urllib.parse.urlparse(src).path.lower()
    return path.endswith(".m3u8") or path.endswith(".m3u")

def fetch_playlist(url, limit=2 * 1024 * 1024, timeout_s=15, headers=None):
    ctx = ssl.create_default_context()
    ca = ca_env().get("SSL_CERT_FILE")
    if ca:
        try:
            ctx.load_verify_locations(ca)
        except Exception:  # noqa: BLE001
            pass
    try:
        req = urllib.request.Request(url, headers=_urllib_headers(headers))
        with urllib.request.urlopen(req, timeout=timeout_s, context=ctx) as resp:
            raw = resp.read(limit + 1)
            final = resp.geturl() or url
        # GCAST_MUT=hlsredir resolves relative variants against the pre-redirect URL.
        if _mut("hlsredir"):
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

def _add_http_input_opts(cmd, url, hdr):
    """Apple UA + optional forwarded -headers on every http(s) ffmpeg input, before -i."""
    if not (url.startswith("http://") or url.startswith("https://")):
        if hdr:
            cmd += ["-headers", hdr if isinstance(hdr, str) else _headers_blob(hdr)]
        return
    blob = hdr if isinstance(hdr, str) else _headers_blob(hdr or {})
    if not _hdr_has_ua(hdr):
        cmd += ["-user_agent", APPLE_UA]
    if blob:
        cmd += ["-headers", blob]

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
        _add_http_input_opts(cmd, src, "")
        cmd += ["-protocol_whitelist", proto_whitelist(src), "-i", src, "-filter_complex",
                ("[0:a:0]asplit=2[ao][av];[av]showspectrum=s=%dx%d:slide=replace:color=intensity:scale=log,"
                 "fps=1,format=yuv420p,setsar=1[vo]") % (w, h),
                "-map", "[vo]", "-map", "[ao]", "-sn", "-dn",
                "-c:v", "libx264", "-preset", "ultrafast", "-tune", "stillimage",
                "-profile:v", "baseline", "-level", "3.0", "-bf", "0",
                "-g", "1", "-keyint_min", "1", "-r", "1", "-pix_fmt", "yuv420p",
                "-b:v", "32k", "-maxrate", "32k", "-bufsize", "32k"]
        remux, copy_audio = False, False
    inputs = inputs or [(src, "")]
    # A failed probe (SDT / iconv abort, or no info at all) still has to play.
    # A short analyze keeps ffmpeg off the table that killed ffprobe. A probe
    # that succeeded does not get these flags: the remux command stays as it was.
    shallow = (not audio_only) and ((not info) or bool(info.get("probe_failed")))
    for url, hdr in ([] if audio_only else inputs):
        if shallow:
            cmd += ["-probesize", "32768", "-analyzeduration", "0"]
        if url.startswith("http") and ".m3u8" not in url:
            cmd += ["-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "5"]
        _add_http_input_opts(cmd, url, hdr)
        if off_ms > 0:
            cmd += ["-ss", "%.3f" % (off_ms / 1000.0)]
        cmd += ["-protocol_whitelist", proto_whitelist(url), "-i", url]
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
        ycmd = [ytdlp, "-f", itag, "-o", "-", "--no-playlist", "--no-progress", "--no-part"] + js_runtime_args() + cookies_args()
        ycmd += ["https://www.youtube.com/watch?v=" + vid]
        parts.append(" ".join(_sq(a) for a in ycmd))
    ff = _sq(ffmpeg)
    ss = ("-ss %.3f" % (off_ms / 1000.0)) if off_ms > 0 else ""
    if len(itags) == 1:
        script = ("%s | %s -nostdin -hide_banner -loglevel error %s -protocol_whitelist pipe -i pipe:0 "
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
                  "%s -nostdin -hide_banner -loglevel error %s -protocol_whitelist file -i \"$d/v\" "
                  "-protocol_whitelist file -i \"$d/a\" "
                  "-map 0:v:0 -map 1:a:0 -sn -dn -c:v copy -bsf:v h264_mp4toannexb -c:a copy "
                  "-f mpegts -muxdelay 0 pipe:1; ec=$?; rm -rf \"$d\"; exit $ec") % (
                      parts[0], parts[1], ff, ss)
        if _mut("nopipenull"):
            script = script.replace(" >/dev/null &", " &")
    return ["/bin/bash", "-c", script], env


def kill(proc):
    try: os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError: return
    try: proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        try: os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError: pass

# LAN_AUTH (#62/#63, supersedes #140). Default off: nothing is checked.
# On (env MPX_LAN_AUTH / LAN_AUTH, else LAN_AUTH= in misterplex.conf): a request
# from a non-loopback peer must send X-MPX-Auth with the shared secret
# (env MPX_LAN_AUTH_SECRET, else conf LAN_AUTH_SECRET, else LAN_AUTH_SECRET_FILE,
# default /media/fat/misterplex/lan_auth.secret, generated by misterplexd).
# Missing header 401, wrong header 403. The secret is never logged.
LAN_AUTH_HEADER = "X-MPX-Auth"
_TOKEN_RE = re.compile(r"^[A-Za-z0-9._~-]{16,128}$")

def _conf_value(key):
    path = os.environ.get("MISTERPLEX_CONF") or "/media/fat/misterplex/misterplex.conf"
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith(key + "="):
                    return line[len(key) + 1:].strip()
    except OSError:
        pass
    return ""

def lan_auth_on():
    if os.environ.get("LAN_AUTH_MUT") == "defaulton":
        return True
    v = os.environ.get("MPX_LAN_AUTH") or os.environ.get("LAN_AUTH") or _conf_value("LAN_AUTH")
    return v.strip().lower() in ("1", "on", "yes", "true")

def lan_auth_secret():
    for v in (os.environ.get("MPX_LAN_AUTH_SECRET", ""), _conf_value("LAN_AUTH_SECRET")):
        if _TOKEN_RE.match(v or ""):
            return v
    path = os.environ.get("LAN_AUTH_SECRET_FILE") or _conf_value("LAN_AUTH_SECRET_FILE") \
        or "/media/fat/misterplex/lan_auth.secret"
    try:
        with open(path, encoding="utf-8") as f:
            v = f.readline().strip()
        return v if _TOKEN_RE.match(v) else ""
    except OSError:
        return ""

def lan_auth_verdict(peer, header):
    """0 allowed, 401 missing, 403 wrong. Loopback peers (the on-card daemon) are exempt."""
    mut = os.environ.get("LAN_AUTH_MUT", "")
    if mut == "ignored" or not lan_auth_on():
        return 0
    if mut != "noloopexempt" and peer_is_local(peer):
        return 0
    if not header:
        return 401
    sec = lan_auth_secret()
    if not sec or not hmac.compare_digest(header.encode(), sec.encode()):
        return 403
    return 0

class H(BaseHTTPRequestHandler):
    server_version = "airplay-xcode/2-standalone"
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
    def _probe_url(self, url, budget_s, headers=None):
        prober = getattr(self.server, "prober", None)
        if prober:
            return prober(url)
        return probe(url, self.server.ffprobe, budget_s=budget_s, headers=headers)
    def _refuse(self, kind):
        if kind not in _REFUSE_KINDS:
            kind = "format"
        log("airplay video refused reason=%s" % kind)
        return self.text(415, "refused\nreason=%s\nnotice=%s\n" % (kind, notice_for(kind)))
    def do_GET(self):  # noqa: N802
        u = urllib.parse.urlparse(self.path); qs = urllib.parse.parse_qs(u.query)
        g = lambda k, d="": (qs.get(k) or [d])[0]
        auth = lan_auth_verdict(self.client_address[0], self.headers.get(LAN_AUTH_HEADER, ""))
        if auth:
            log("LAN_AUTH refused %s %s (auth REDACTED)" % (self.client_address[0], u.path))
            return self.text(auth, "auth required\n" if auth == 401 else "auth refused\n")
        if u.path in ("/", "/health"):
            mode = "standalone" if os.path.isdir(misterplex_root()) else "helper"
            return self.text(200, "ok airplay-xcode mode=%s\n" % mode)
        if u.path == "/probe" and g("selftest") == "1":
            info = probe_selftest(self.server.ffprobe)
            if not info or not info.get("audio"):
                return self.text(502, "probe failed\n")
            return self.text(200, "".join("%s=%s\n" % kv for kv in info.items()))
        src = g("src")
        if u.path == "/yt.ts":
            src = "yt:" + g("v")
        elif u.path not in ("/probe", "/ladder.ts", "/admit"):
            return self.text(404, "use /probe, /ladder.ts, /admit or /yt.ts\n")
        if not ok_src(src, peer_is_local(self.client_address[0])):
            return self.text(400, "src must be http(s) or yt:<videoId>, and not a local address\n")
        fwd = sanitize_fwd_headers(g("hdr")) if u.path in ("/probe", "/admit", "/ladder.ts") else {}
        if fwd:
            log("fwd headers names=%s" % ",".join(fwd.keys()))
        fwd_blob = _headers_blob(fwd)
        yt = yt_resolve(src[3:], self.server.ytdlp) if src.startswith("yt:") else None
        if src.startswith("yt:") and not yt: return self.text(502, "youtube resolve failed\n")
        hls = None
        plan = None
        admit = u.path == "/admit"
        budget = 6 if admit else 25
        if not yt and hls_playlist_url(src):
            got = fetch_playlist(src, timeout_s=4 if admit else 15, headers=fwd)
            if got and detect_drm(got[0]):
                return self._refuse("drm")
            plan = plan_hls_master(got[0], got[1]) if got else None
            if plan:
                hls = choose_hls(plan, lambda url: self._probe_url(url, budget, headers=fwd))
                if admit and (not hls or (not hls.get("copy") and not hls.get("refused"))):
                    trusted = choose_hls_playlist_trust(plan)
                    if trusted:
                        hls = trusted
            if hls and hls.get("refused"):
                pass
            elif not hls or not hls.get("info"):
                hls = None
        if yt:
            info = yt_probe(src[3:])
        elif hls and hls.get("info"):
            info = hls["info"]
        elif hls and hls.get("refused"):
            info = None
        else:
            info = self._probe_url(src, budget, headers=fwd)
        # A probe crash is still a failure of that probe. /probe may answer 200 from
        # the playlist size. /ladder.ts and /admit refuse video that is not a copy.
        if not info:
            info = playlist_probe_fallback(plan)
        if u.path == "/probe":
            if not info: return self.text(502, "probe failed\n")
            return self.text(200, "".join("%s=%s\n" % kv for kv in info.items()))
        if u.path != "/yt.ts" and not (src.startswith("yt:") and yt):
            dec = playback_decision(src, info, hls, plan)
            if u.path == "/admit":
                if dec["action"] == "refuse":
                    return self._refuse(dec["reason"])
                if dec["action"] == "audio":
                    return self.text(200, "audio\n")
                if dec["action"] == "copy":
                    return self.text(200, "copy\n")
                return self.text(200, "x264\n")
            if dec["action"] == "refuse":
                return self._refuse(dec["reason"])
            # A RAOP probe that returned nothing is still the spectrum frame, not the video ladder.
            if dec["action"] == "audio" and not (info and info.get("vcodec") == "none"):
                info = {"vcodec": "none", "audio": 1, "w": 1280, "h": 720, "fps_n": 1, "fps_d": 1}
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
            # Out-of-spec video already returned 415. This x264 branch is the
            # GCAST_MUT=videofallback path, RAOP spectrum, and probe-miss x264 attempt.
            if hls:
                play = hls["variant_url"]
                inputs = [(play, fwd_blob)]
                if hls.get("audio_url"):
                    inputs.append((hls["audio_url"], fwd_blob))
                remux = bool(hls.get("copy")) and stream_copy_ok(info)
                copy_audio = bool(remux and hls.get("copy_audio"))
                cmd = build_cmd(self.server.ffmpeg, play, w, h, br, off, remux, info, inputs, copy_audio=copy_audio)
                shown = dict(hls)
                shown["copy"] = remux
                log(hls_log_line(shown))
            else:
                remux = stream_copy_ok(info)
                copy_audio = remux and (info or {}).get("acodec") == "aac"
                inputs = [(src, fwd_blob)] if fwd_blob else None
                cmd = build_cmd(self.server.ffmpeg, src, w, h, br, off, remux, info, inputs, copy_audio=copy_audio)
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
    ap.add_argument("--bind", default=_env("AIRPLAY_XCODE_BIND", "GCAST_XCODE_BIND", "127.0.0.1"))
    ap.add_argument("--port", type=int, default=int(_env("AIRPLAY_XCODE_PORT", "GCAST_XCODE_PORT", "18781")))
    ap.add_argument("--ffmpeg", default=which_default("AIRPLAY_FFMPEG", ["ffmpeg"], "ffmpeg", "GCAST_FFMPEG"))
    ap.add_argument("--ffprobe", default=which_default("AIRPLAY_FFPROBE", ["ffprobe"], "ffprobe", "GCAST_FFPROBE"))
    ap.add_argument("--ytdlp", default=which_default("AIRPLAY_YTDLP", ["yt-dlp", "yt-dlp_linux_armv7l"], "yt-dlp", "GCAST_YTDLP"))
    a = ap.parse_args()
    srv = ThreadingHTTPServer((a.bind, a.port), H); srv.daemon_threads = True
    srv.ffmpeg, srv.ffprobe, srv.ytdlp = a.ffmpeg, a.ffprobe, a.ytdlp
    log("airplay-xcode on %s:%d ytdlp=%s js=%s cookies=%s" %
        (a.bind, a.port, a.ytdlp, js_runtime_args() or "none", cookies_args()[1] if cookies_args() else "none"))
    if not peer_is_local(a.bind):
        log("WARNING: bound to %s, not loopback: any host that can reach this port can make it fetch URLs. "
            "Use only on a trusted LAN; local/link-local sources are refused for remote clients." % a.bind)
    srv.serve_forever()

if __name__ == "__main__":
    main()
