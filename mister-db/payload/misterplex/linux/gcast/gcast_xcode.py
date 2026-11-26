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
  GET /refresh?src=<url>[&hdr=]        A held play of the same stream (stream_identity) now has this signed URL. Stored only,
      no fetch. The trust of /ladder.ts (LAN_AUTH when on). Body "ok refresh" when the running ffmpeg reloads under it
      (live); "vod ttl=N" / "cookie ttl=N" when it cannot (N: seconds left on what it reads, or none); "none ttl=none"
      for a stream with no live entry (no /admit or /ladder.ts of it): never creates one.
  GET /hls/<key>/media.m3u8|audio.m3u8 Loopback only (the helper's own ffmpeg). An HLS remux reads its media playlist here,
      fetched from the origin under the newest signature with every URI made absolute (see live_register). A source that
      sets a cookie is read directly instead.
Only http(s) or yt:<11-char id> src is accepted; at most MAX_JOBS ffmpeg run at once (oldest killed).
Time to first byte: /admit, /probe and /ladder.ts of one source share one HLS plan + probe (PLAN_TTL_S cache);
a repeat /ladder.ts of the SAME stream (token path/query ignored, see stream_identity) adopts the running ffmpeg
of an open its client dropped before the first byte (ADOPT_GRACE_S) instead of starting a second one.
The on-card helper binds 127.0.0.1:18781. A computer helper uses --bind 0.0.0.0 --port 18780.
"""
import argparse, base64, calendar, copy, hashlib, hmac, ipaddress, json, os, re, select, signal, socket, ssl, subprocess, sys, tempfile, threading, time, urllib.error, urllib.parse, urllib.request
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


_URL_IN_TEXT = re.compile(r"(?i)\b(https?)://([^/\s'\"?#]+)[^\s'\"]*")
_LONG_TOKEN = re.compile(r"[A-Za-z0-9_%=+/~.-]{40,}")

def redact_tool_stderr(raw, limit=200, nlines=2):
    """Short, one-line, token-free tail of ffprobe/ffmpeg stderr for the log. A URL keeps only its scheme and
    host (cast URLs carry signed tokens in the path and query); any other long token-like run is dropped.
    AIRPLAY_MUT=rawstderr keeps the text as is (the unit test must fail)."""
    if not raw:
        return ""
    s = raw.decode("utf-8", "replace") if isinstance(raw, (bytes, bytearray)) else str(raw)
    if _mut("rawstderr"):
        return s.strip()[-limit:]
    lines = [ln.strip() for ln in s.replace("\r", "\n").split("\n") if ln.strip()]
    s = " | ".join(lines[-nlines:])
    s = _URL_IN_TEXT.sub(lambda m: "%s://%s/..." % (m.group(1).lower(), m.group(2)), s)
    s = _LONG_TOKEN.sub("<redacted>", s)
    s = "".join(c if 32 <= ord(c) < 127 else "?" for c in s)
    return s[-limit:]


def _run_ffprobe(cmd, timeout=25):
    """ffprobe in its own session. An iconv abort kills that process, not the helper."""
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=timeout, env=ca_env(), start_new_session=True)
    except Exception as e:  # noqa: BLE001
        log("probe failed (%s)" % type(e).__name__)
        return None
    if proc.returncode != 0:
        tail = "" if _mut("noprobestderr") else redact_tool_stderr(proc.stderr)
        log("probe failed (rc=%s)%s" % (proc.returncode, (" stderr: " + tail) if tail else ""))
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
    # even when the rate is also over (browse-lane #627 P3-3).
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

def _tls_ctx():
    ctx = ssl.create_default_context()
    ca = ca_env().get("SSL_CERT_FILE")
    if ca:
        try:
            ctx.load_verify_locations(ca)
        except Exception:  # noqa: BLE001
            pass
    return ctx

def _sets_cookie(resp):
    return bool(resp.headers.get_all("Set-Cookie") or resp.headers.get_all("Set-Cookie2"))

def fetch_playlist(url, limit=2 * 1024 * 1024, timeout_s=15, headers=None, meta=None):
    """(text, final URL) or None. meta (a dict) gets cookie=True when the response sets one."""
    ctx = _tls_ctx()
    try:
        req = urllib.request.Request(url, headers=_urllib_headers(headers))
        with urllib.request.urlopen(req, timeout=timeout_s, context=ctx) as resp:
            raw = resp.read(limit + 1)
            final = resp.geturl() or url
            if meta is not None and _sets_cookie(resp):
                meta["cookie"] = True
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

LIVE_HLS_COPY_INPUT_OPTS = ["-fflags", "+nobuffer", "-probesize", "1500000", "-analyzeduration", "1500000"]

def build_cmd(ffmpeg, src, w, h, br, off_ms, remux, info, inputs=None, copy_audio=False, fast=True):
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
        elif remux and fast and hls_playlist_url(url) and not _mut("slowstart"):
            # The variant's codecs are already known (probe or playlist), so ffmpeg does not need the default
            # 5 s analyze: a TS with a stream that never carries parameters (SCTE-35 / timed metadata on SSAI
            # live) held the first byte for that whole window. VOD copy gets the same short analyze; it only
            # reads the PAT/PMT and the first GOP.
            cmd += LIVE_HLS_COPY_INPUT_OPTS
            # 2026-11-18 .110 (EIO): a source whose audio starts more than ~1.5 s after its first video packet
            # (DAI / SSAI live) never gets its AAC sample rate inside this window; the copy muxer then dies with
            # "sample rate not set" before the first byte. do_GET retries the SAME stream copy once with the full
            # default probe (fast=False) and remembers the stream (_slowstart_*). Never a transcode.
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

def fast_probe_used(cmd):
    """True when cmd carries the short live-HLS copy probe (LIVE_HLS_COPY_INPUT_OPTS)."""
    o = LIVE_HLS_COPY_INPUT_OPTS
    return any(cmd[i:i + len(o)] == o for i in range(len(cmd)))

SLOWSTART_TTL_S = 900.0
_SLOWSTART = {}
def _slowstart_known(ident):
    if _mut("noslowmemo"):
        return False
    with _PLAN_MU:
        t = _SLOWSTART.get(ident)
        if t is not None and t > _mono():
            return True
        _SLOWSTART.pop(ident, None)
    return False
def _slowstart_remember(ident):
    with _PLAN_MU:
        now = _mono()
        for k in [k for k, t in _SLOWSTART.items() if t <= now]:
            del _SLOWSTART[k]
        while len(_SLOWSTART) >= PLAN_MAX:
            _SLOWSTART.pop(next(iter(_SLOWSTART)))
        _SLOWSTART[ident] = now + SLOWSTART_TTL_S

# ---- Time to first byte (AirPlay video from a live HLS source) -------------------------------------------
# 2026-11-18 .110: /admit -> /probe -> /ladder.ts each re-fetched the master and re-ran ffprobe (~4 s), ffmpeg then
# needed another ~6-7 s for its first byte, and the phone re-sent /play every 2-5 s; every new play dropped the open
# in flight. The daemon now coalesces a repeat play of the same stream; this side (1) shares the plan + probe
# between /admit, /probe and /ladder.ts and (2) lets a repeat /ladder.ts adopt the ffmpeg whose client went away
# before the first byte, so the work already done is not thrown away.
PLAN_TTL_S = 90.0
PLAN_MAX = 32
ADOPT_GRACE_S = 20.0      # an orphaned (client gone, no byte delivered) ffmpeg waits this long for a repeat open
ADOPT_WAIT_S = 1.0        # a repeat open waits this long for the previous client of the same stream to be seen gone
_VOLATILE_QS = frozenset((
    "hdnts", "traceid", "hmac", "exp", "expires", "acl", "token", "sig", "signature", "sid", "session",
    "sessionid", "wsid", "x-plex-token", "accesstoken", "auth", "policy", "key-pair-id", "nonce", "ts", "timestamp"))

def _mono():
    return time.monotonic()

def _token_segment(seg):
    """A path segment that is a signed token (base64-ish, 40+ chars, upper case and digits), e.g. the Fox
    ZXhwPTE3OTUx... segment: it changes on every cast of the same channel."""
    return (len(seg) >= 40 and re.fullmatch(r"[A-Za-z0-9_%=+.-]+", seg) is not None
            and any(c.isdigit() for c in seg) and any(c.isupper() for c in seg))

def stream_identity(url):
    """The same stream under a different signature: scheme, host, path without token segments, query without
    the volatile signing keys. Everything else (channel, rendition, item id) stays part of the identity.
    AIRPLAY_MUT=exactid makes it the exact URL (the unit test must fail)."""
    if _mut("exactid") or not (url.startswith("http://") or url.startswith("https://")):
        return url
    p = urllib.parse.urlsplit(url)
    segs = [x for x in p.path.split("/") if not _token_segment(x)]
    q = sorted((k, v) for k, v in urllib.parse.parse_qsl(p.query, keep_blank_values=True)
               if k.lower() not in _VOLATILE_QS)
    return "%s://%s%s?%s" % (p.scheme.lower(), p.netloc.lower(), "/".join(segs), urllib.parse.urlencode(q))

_PLAN = {}
_PLAN_MU = threading.Lock()
_PLAN_KEYLOCKS = {}

def plan_key(src, fwd_blob):
    return hashlib.sha256(("%s\n%s" % (src, fwd_blob)).encode()).hexdigest()

def plan_cache_get(key):
    if _mut("noplancache"):
        return None
    with _PLAN_MU:
        e = _PLAN.get(key)
        if e and (e[0] > _mono() or _mut("nottl")):
            return e[1]
        _PLAN.pop(key, None)
    return None

# v1.0.1 glass (device, 2026-11-23, FPGA Bug N4 on 990b3122): the plan is keyed by stream identity (signature stripped)
# for PLAN_TTL_S, but its variant/audio URLs carry the FIRST play's signature. A Fox token that expired at 75 s gave
# "plan cache hit /ladder.ts" -> ffmpeg "403 Forbidden" -> "open: Input/output error" x9 (78.7-88.2 s), even after a
# fresh-URL replay at 70.1 s. Two guards: (1) a plan never outlives the earliest signed-URL expiry it carries
# (exp/expires query keys, hdnts/hdntl "exp=", base64 Fox path tokens "exp=...", X-Amz-Date + X-Amz-Expires) minus
# PLAN_EXP_MARGIN_S; (2) an upstream 403/410 before the first byte drops the plan and re-resolves ONCE from the
# request's own URL, stream copy only (never x264: Shawn's AirPlay rule). AIRPLAY_MUT=noexpcap / noreplan / noflag / no4xx.
PLAN_EXP_MARGIN_S = float(os.environ.get("AIRPLAY_PLAN_EXP_MARGIN_S", "10"))
_EXP_QS_KEYS = frozenset(("exp", "expires", "expiry", "expiration", "x-expires", "e"))
_EXP_IN_TEXT = re.compile(r"(?:^|[~&;,:/ ])exp(?:ires)?=(\d{9,11})(?:\D|$)")
_WALL_FLOOR = 1000000000   # 2001: smaller numbers are durations or ids, not epoch seconds

def _epoch_ok(v):
    return _WALL_FLOOR < v < SANE_WALL_CEILING

def _exp_from_text(text):
    out = []
    for m in _EXP_IN_TEXT.finditer(text):
        v = int(m.group(1))
        if _epoch_ok(v):
            out.append(v)
    return out

def signed_url_expiry(url):
    """Earliest expiry (epoch seconds) a signed URL carries, or None when it names none."""
    if not url or not (url.startswith("http://") or url.startswith("https://")):
        return None
    try:
        p = urllib.parse.urlsplit(url)
        qs = urllib.parse.parse_qsl(p.query, keep_blank_values=True)
    except ValueError:
        return None
    found = []
    amz_date = amz_exp = None
    for k, v in qs:
        kl = k.lower()
        if kl in _EXP_QS_KEYS and v.isdigit() and _epoch_ok(int(v)):
            found.append(int(v))
        elif kl == "x-amz-date":
            amz_date = v
        elif kl == "x-amz-expires" and v.isdigit():
            amz_exp = int(v)
        else:
            found += _exp_from_text(urllib.parse.unquote(v))
    if amz_date and amz_exp is not None:
        try:
            t = time.strptime(amz_date, "%Y%m%dT%H%M%SZ")
            found.append(calendar.timegm(t) + amz_exp)
        except ValueError:
            pass
    for seg in p.path.split("/"):
        if not _token_segment(seg):
            continue
        raw = urllib.parse.unquote(seg)
        try:
            dec = base64.urlsafe_b64decode(raw + "=" * (-len(raw) % 4)).decode("latin-1")
        except (ValueError, TypeError):
            continue
        found += _exp_from_text(dec)
    return min(found) if found else None

def log_src_tokenless(src):
    """log_src with every signed path segment (Fox base64 / opaque token) replaced: for the expiry/re-resolve lines."""
    if src.startswith("http://") or src.startswith("https://"):
        p = urllib.parse.urlsplit(src)
        path = "/".join("<tok>" if _token_segment(x) else x for x in p.path.split("/"))
        return urllib.parse.urlunsplit((p.scheme, p.netloc, path, "", ""))[:120]
    return log_src(src)

def plan_expiry(src, hls):
    """Earliest signed-URL expiry over the source and the URLs the plan will hand to ffmpeg."""
    urls = [src]
    if hls:
        urls += [hls.get("variant_url"), hls.get("audio_url")]
    exps = [e for e in (signed_url_expiry(u) for u in urls if u) if e]
    return min(exps) if exps else None

def plan_ttl_s(exp_wall, now_wall=None):
    """PLAN_TTL_S, capped so the plan is gone PLAN_EXP_MARGIN_S before its first signed URL expires (<= 0: don't cache)."""
    if exp_wall is None or _mut("noexpcap"):
        return PLAN_TTL_S
    now_wall = time.time() if now_wall is None else now_wall
    return min(PLAN_TTL_S, exp_wall - now_wall - PLAN_EXP_MARGIN_S)

def plan_cache_drop(key):
    with _PLAN_MU:
        return _PLAN.pop(key, None) is not None

def plan_cache_put(key, entry, ttl_s=None):
    ttl_s = PLAN_TTL_S if ttl_s is None else ttl_s
    if ttl_s <= 0:
        return False
    with _PLAN_MU:
        now = _mono()
        for k in [k for k, v in _PLAN.items() if v[0] <= now]:
            del _PLAN[k]
        while len(_PLAN) >= PLAN_MAX:
            _PLAN.pop(next(iter(_PLAN)))
        _PLAN[key] = (now + ttl_s, entry)
    return True

_UPSTREAM_DENIED = re.compile(r"(?:Server returned|HTTP error) (403|410)\b|\b(403 Forbidden|410 Gone)\b")
# The shipped device ffmpeg (8.1.2, md5 80baac9d) at the helper's -loglevel error reports a 410 only as
# "Server returned 4XX Client Error, but not one of 40{0,1,3,4}" (its "HTTP error 410 Gone" line is warning
# level), so a 410 must match that generic 4XX line too. Broad on purpose: the re-resolve is once per open.
# AIRPLAY_MUT=no4xx drops it (the unit test must fail).
_UPSTREAM_DENIED_4XX = re.compile(r"Server returned 4XX Client Error")

def upstream_denied(err):
    """An ffmpeg stderr tail that says the upstream refused the (signed) URL: 403 Forbidden / 410 Gone, including
    the device ffmpeg's generic "Server returned 4XX Client Error" line for a 410."""
    if isinstance(err, bytes):
        err = err.decode("utf-8", "replace")
    err = err or ""
    if _UPSTREAM_DENIED.search(err):
        return True
    return not _mut("no4xx") and bool(_UPSTREAM_DENIED_4XX.search(err))

class _PlanLock:
    """One computation per source at a time: a second request for the same source waits and then hits the cache."""
    def __init__(self, key): self.key = key
    def __enter__(self):
        with _PLAN_MU:
            ent = _PLAN_KEYLOCKS.setdefault(self.key, [threading.Lock(), 0])
            ent[1] += 1
        self.lock = ent[0]
        self.lock.acquire()
    def __exit__(self, *exc):
        self.lock.release()
        with _PLAN_MU:
            ent = _PLAN_KEYLOCKS.get(self.key)
            if ent:
                ent[1] -= 1
                if ent[1] <= 0:
                    del _PLAN_KEYLOCKS[self.key]

class LadderJob:
    """A running /ladder.ts ffmpeg. claimed: a client handler owns it. sent: bytes delivered to any client."""
    def __init__(self, key, proc):
        self.key, self.proc = key, proc
        self.claimed, self.sent, self.head, self.timer = True, 0, b"", None
        self.retried = False   # the fast-probe start already failed once and was redone with the full probe
        self.replanned = False # an upstream 403/410 before the first byte already dropped the plan and re-resolved
        self.live_pin = None   # orphaned: the live key its ffmpeg pins (live_register), dropped when it is reaped

LADDER = {}
LADDER_MU = threading.Lock()

def ladder_job_key(src, w, h, br, off_ms, remux):
    return hashlib.sha256(("%s|%d|%d|%d|%d|%d" % (stream_identity(src), w, h, br, off_ms, int(bool(remux)))).encode()).hexdigest()

def ladder_register(job):
    with LADDER_MU:
        LADDER[job.key] = job

def ladder_forget(job):
    with LADDER_MU:
        if LADDER.get(job.key) is job:
            del LADDER[job.key]
        if job.timer:
            job.timer.cancel()

def _ladder_expire(job):
    with LADDER_MU:
        if job.claimed or LADDER.get(job.key) is not job:
            return
        del LADDER[job.key]
    log("xcode orphan expired after %ds, no repeat open: killing pid %d" % (ADOPT_GRACE_S, job.proc.pid))
    _reap_orphan(job)

def ladder_orphan(job, head=b""):
    """The client dropped before the first byte: keep ffmpeg (and the chunk that failed to write) for a repeat open."""
    with LADDER_MU:
        job.head, job.claimed = head, False
        t = threading.Timer(ADOPT_GRACE_S, _ladder_expire, args=(job,))
        t.daemon = True
        job.timer = t
        t.start()

def ladder_adopt(key, wait_s=ADOPT_WAIT_S):
    """The orphaned job of the same stream, claimed for the caller, or None. Another stream's orphans are killed:
    a different stream still preempts. A job whose client is still attached waits up to wait_s to be seen gone."""
    if _mut("noadopt"):
        return None
    deadline = _mono() + wait_s
    while True:
        dead = []
        with LADDER_MU:
            job = LADDER.get(key)
            for k, j in list(LADDER.items()):
                if k != key and not j.claimed and not _mut("nopreempt"):
                    del LADDER[k]
                    dead.append(j)
            if job is not None and (not job.claimed or _mut("steal")):
                if job.proc.poll() not in (None, 0) and not job.head:
                    del LADDER[key]
                    dead.append(job)
                    job = None
                else:
                    job.claimed = True
                    if job.timer:
                        job.timer.cancel()
                    for j in dead:
                        _ladder_kill_later(j)
                    return job
            waiting = job is not None and job.sent == 0
        for j in dead:
            _ladder_kill_later(j)
        if not waiting or _mono() >= deadline:
            return None
        time.sleep(0.05)

def _ladder_kill_later(job):
    if job.timer:
        job.timer.cancel()
    log("xcode orphan of another stream preempted: killing pid %d" % job.proc.pid)
    _reap_orphan(job)

def _reap_orphan(job):
    _reap_proc(job.proc)
    if job.live_pin:
        live_unpin(job.live_pin)
        job.live_pin = None

def _reap_proc(proc):
    kill(proc)
    with JOBS_MU:
        if proc in JOBS:
            JOBS.remove(proc)
    for f in (proc.stdout, proc.stderr):
        try:
            f.close()
        except Exception:  # noqa: BLE001
            pass

# ---- A held play under a new signature (v1.0.3 .229 glass, 2026-11-24) ---------------------------------------------
# misterplexd holds a re-sent /play of the stream already playing, so the re-send's fresh signed URL never reaches a
# new /ladder.ts. ffmpeg kept refreshing the FIRST signed media playlist until it expired at 75 s: 2 x 403,
# Input/output error, PLAYBACK ERROR. A running ffmpeg's -i cannot change, so an HLS remux reads a stable helper
# playlist (/hls/<key>/media.m3u8, audio.m3u8) instead of the origin URL. <key> hashes stream_identity: no token in
# argv. Each playlist GET fetches the live entry's media playlist from the origin and makes every URI absolute, so
# ffmpeg fetches its segments under the newest signature. /refresh (misterplexd, on a held re-send) only stores the new
# master; the next playlist GET re-fetches it and keeps the same rendition. A 403/410 is passed through unchanged, so
# the v1.0.2 re-resolve still sees it. AIRPLAY_MUT=norefresh | nolivehls | norewrite | liveorder | liveanypeer.
# FPGA Bug on cda4ae41 (device ffmpeg 8.1.2 under qemu): (F1) a source whose master or media playlist sets a cookie
# (Akamai hdntl) needs that cookie on its segments; this proxy drops it, so 403 from t=0. Such a source keeps the old
# direct -i variant_url input. (F2) a VOD playlist (#EXT-X-ENDLIST) is read once and never reloaded, so the proxy
# cannot re-sign it. The media playlist is fetched once before ffmpeg starts to tell both apart (ffmpeg's first
# reload gets that body), and /refresh answers "ok refresh" only for a live, cookie-free entry. Otherwise it answers
# "vod" / "cookie" with the seconds left on the URL ffmpeg reads, and misterplexd reopens the held play at its
# playhead. AIRPLAY_MUT=nocookiefall | vodok | refreshloop.
# FPGA Bug N6 on 2d49edab: /refresh created a missing entry and evicted the oldest at LIVE_MAX, so 32 /refresh of
# unknown streams from a LAN peer ended a running live play with no origin fetch. /refresh now only updates; /admit
# creates the entry. AIRPLAY_MUT=refreshcreate.
# FPGA Bug R1 on 3e3910b3: 32 /admit of distinct streams from a LAN peer evicted a running VOD / cookie play's entry
# (touched every LIVE_TOUCH_S only), so its next /refresh answered none and the play ended at token expiry. An entry a
# running /ladder.ts ffmpeg reads is pinned: never reaped or evicted; a full table of pinned entries creates nothing.
# AIRPLAY_MUT=evictjob.
LIVE_MAX = 32
LIVE_IDLE_S = 600.0
LIVE_PREFETCH_REUSE_S = 3.0   # ffmpeg's first playlist GET reuses the pre-start fetch when it is this fresh
LIVE_TOUCH_S = 30.0           # a /ladder.ts that streams an entry keeps it from LIVE_IDLE_S eviction (VOD never reloads)
_LIVE = {}
_LIVE_MU = threading.Lock()
_URL_SEEN = {}
_URL_SEEN_MAX = 512
_URL_SEEN_N = [0]

class LiveSource:
    """What an HLS remux ffmpeg of one stream reads now. Fields change under _LIVE_MU only."""
    def __init__(self, key):
        self.key = key
        self.master, self.master_seq = None, -1
        self.refreshed_seq = -1   # url_seen order of the newest /refresh
        self.hdrs = {}
        self.variant = self.audio = None
        self.rung = None
        self.stale = False   # the master is newer than variant/audio: re-fetch it before the next playlist
        self.kind = None     # "live" / "vod" (#EXT-X-ENDLIST) once a media playlist was seen
        self.cookie = False  # the origin sets a cookie: ffmpeg reads it directly (no /hls/)
        self.read_exp = {}   # "media"/"audio" -> earliest signed expiry of what a VOD / direct ffmpeg reads, or None
        self.pre = {}        # "media"/"audio" -> (url, text, final URL, mono) of the pre-start fetch
        self.touched = _mono()
        self.pins = 0        # /ladder.ts requests or orphaned jobs whose ffmpeg reads this entry and still runs
        self.fetch_mu = threading.Lock()

def live_key(src):
    return hashlib.sha256(stream_identity(src).encode()).hexdigest()[:24]

def url_seen(src):
    """Order in which this exact URL first reached the helper. Every /play's /admit comes before its /refresh, so a
    held re-send is always newer than the /ladder.ts of the open it is held behind, whatever order those two arrive."""
    with _LIVE_MU:
        return _url_seen_locked(src)

def _url_seen_locked(src):
    """url_seen. Caller holds _LIVE_MU."""
    h = hashlib.sha256(src.encode()).digest()
    n = _URL_SEEN.get(h)
    if n is None:
        _URL_SEEN_N[0] += 1
        n = _URL_SEEN[h] = _URL_SEEN_N[0]
        while len(_URL_SEEN) > _URL_SEEN_MAX:
            _URL_SEEN.pop(next(iter(_URL_SEEN)))
    return n

def _live_entry(key):
    """The entry for key, created when missing, or None when all LIVE_MAX entries are pinned. Only unpinned entries
    are reaped (LIVE_IDLE_S) or evicted (oldest first). Caller holds _LIVE_MU."""
    now = _mono()
    e = _LIVE.get(key)
    if e is None:
        pinned = lambda v: v.pins > 0 and not _mut("evictjob")
        for k in [k for k, v in _LIVE.items() if not pinned(v) and now - v.touched > LIVE_IDLE_S]:
            del _LIVE[k]
        free = sorted((k for k, v in _LIVE.items() if not pinned(v)), key=lambda k: _LIVE[k].touched)
        while len(_LIVE) >= LIVE_MAX:
            if not free:
                return None
            del _LIVE[free.pop(0)]
        e = _LIVE[key] = LiveSource(key)
    e.touched = now
    return e

def live_unpin(key):
    with _LIVE_MU:
        e = _LIVE.get(key)
        if e is not None and e.pins > 0:
            e.pins -= 1

def _rung(v):
    return {"ident": stream_identity(v.get("variant_url") or ""), "w": v.get("w"), "h": v.get("h"),
            "bw": v.get("bw"), "codec": v.get("codec")}

def same_rung(plan, rung):
    """The in-spec variant of a re-fetched master that is the rendition ffmpeg already copies. Never out of spec."""
    ins = (plan or {}).get("in_spec") or []
    if not ins:
        return None
    rung = rung or {}
    for match in (lambda v: stream_identity(v["variant_url"]) == rung.get("ident"),
                  lambda v: (v.get("w"), v.get("h"), v.get("bw"), v.get("codec")) ==
                            (rung.get("w"), rung.get("h"), rung.get("bw"), rung.get("codec")),
                  lambda v: (v.get("w"), v.get("h")) == (rung.get("w"), rung.get("h"))):
        for v in ins:
            if match(v):
                return v
    return ins[0]

def _playlist_kind(text):
    return "vod" if "#EXT-X-ENDLIST" in text else "live"

def _read_expiry(urls, bodies):
    """Earliest signed expiry over the playlist URLs ffmpeg reads and the first / last segment URI of each body."""
    cand = [u for u in urls if u]
    for text, final in bodies:
        segs = [s.strip() for s in text.splitlines() if s.strip() and not s.strip().startswith("#")]
        cand += [urllib.parse.urljoin(final, s) for s in segs[:1] + segs[-1:]]
    exps = [x for x in (signed_url_expiry(u) for u in cand) if x]
    return min(exps) if exps else None

def live_register(src, fwd, hls, plan_seq, pin=False):
    """/ladder.ts (or its re-resolve) starts an HLS remux of src from hls, a plan resolved from a URL first seen at
    plan_seq: that plan is what ffmpeg reads now. A /refresh newer than src stays the master; a /refresh newer than
    the plan (a cached plan outlived by a held play's new signature) marks the plan's rendition stale.
    pin: the caller's ffmpeg holds the entry until live_unpin. The key, or None when the table is full of pins."""
    seq = url_seen(src)
    key = live_key(src)
    with _LIVE_MU:
        e = _live_entry(key)
        if e is None:
            return None
        if pin:
            e.pins += 1
        e.variant, e.audio, e.rung, e.hdrs = hls["variant_url"], hls.get("audio_url"), _rung(hls), dict(fwd or {})
        if seq >= e.master_seq or _mut("liveorder"):
            e.master, e.master_seq, e.stale = src, seq, plan_seq < e.refreshed_seq
        else:
            e.stale = True
        e.cookie = e.cookie or bool(hls.get("cookie"))
    return key

def live_prepare(key):
    """Before ffmpeg starts: the entry's rendition (re-signed first when a /refresh is newer than the plan) and its
    media / audio playlist fetched once: kind (#EXT-X-ENDLIST), Set-Cookie, and the body ffmpeg's first GET reuses.
    A failed fetch (e.g. a 403 of an expired plan) records nothing: ffmpeg's own GET sees it. (variant, audio, cookie)."""
    with _LIVE_MU:
        e = _LIVE.get(key)
    if e is None:
        return None
    with e.fetch_mu:
        with _LIVE_MU:
            need = e.master is not None and e.stale
        if need:
            _live_remaster(e)
        with _LIVE_MU:
            variant, audio, hdrs = e.variant, e.audio, dict(e.hdrs)
        cookie, kind, bodies = False, None, {}
        for name, url in (("media", variant), ("audio", audio)):
            if not url:
                continue
            meta = {}
            st, text, final = origin_get(url, hdrs, timeout_s=6, meta=meta)
            cookie = cookie or bool(meta.get("cookie"))
            if st != 200:
                continue
            bodies[name] = (url, text, final, _mono())
            if name == "media":
                kind = _playlist_kind(text)
        with _LIVE_MU:
            e.cookie = e.cookie or cookie
            if kind:
                e.kind = kind
            e.pre = bodies
            e.read_exp = dict((n, _read_expiry([b[0]], [(b[1], b[2])])) for n, b in bodies.items())
            return variant, audio, e.cookie

def refresh_answer(e):
    """/refresh body for e (caller holds _LIVE_MU). "ok refresh": the running ffmpeg reloads its playlist (live, or
    not seen yet). "vod" / "cookie": it cannot; ttl= the seconds left on what it reads (none: no expiry in it)."""
    if e.cookie:
        kind = "cookie"
    elif e.kind == "vod" and not _mut("vodok"):
        kind = "vod"
    else:
        return "ok refresh"
    exps = [x for x in e.read_exp.values() if x]
    return "%s ttl=%s" % (kind, max(0, int(min(exps) - time.time())) if exps else "none")

def live_admit(src):
    """/admit of an HLS stream copy (every /play asks it first): the stream's entry exists from here, so a held
    re-send's /refresh that lands before the open's /ladder.ts is stored. Never at the cost of a pinned entry."""
    with _LIVE_MU:
        _live_entry(live_key(src))

def live_refresh(src, fwd):
    """/refresh: src is the newest URL of its stream. Stored only, and only in an existing entry: a /refresh never
    creates one, so it cannot evict a running play's (LIVE_MAX). "none ttl=none" for a stream the helper does not
    serve through /hls/ (misterplexd keeps the play). That answer takes no url_seen order either: an earlier order
    would let the open's later /ladder.ts of its first URL replace the /refresh stored after /admit.
    (True when it replaced the master, the answer). AIRPLAY_MUT=refreshcreate: the old create-and-evict."""
    if _mut("norefresh"):
        return False, "ok refresh"
    with _LIVE_MU:
        key = live_key(src)
        e = _live_entry(key) if _mut("refreshcreate") else _LIVE.get(key)
        if e is None:
            return False, "none ttl=none"
        e.touched = _mono()
        seq = _url_seen_locked(src)
        if seq <= e.master_seq:
            return False, refresh_answer(e)
        e.master, e.master_seq, e.refreshed_seq, e.stale = src, seq, seq, True
        if fwd:
            e.hdrs = dict(fwd)
        return True, refresh_answer(e)

def live_touch(key):
    with _LIVE_MU:
        e = _LIVE.get(key)
        if e is not None:
            e.touched = _mono()

def live_input_urls(port, key, audio):
    base = "http://127.0.0.1:%d/hls/%s/" % (port, key)
    return base + "media.m3u8", (base + "audio.m3u8") if audio else None

def live_peer_ok(addr):
    """/hls/ serves the helper's own ffmpeg only."""
    return peer_is_local(addr) or _mut("liveanypeer")

def refresh_peer_ok(addr, header=""):
    """/refresh has the trust of /ladder.ts: any peer lan_auth_verdict lets through (all of them with LAN_AUTH off,
    the default). A computer helper (--bind 0.0.0.0) gets it from the card. It only re-signs a stream_identity
    that peer could already open through /ladder.ts. AIRPLAY_MUT=refreshloop: the old loopback-only gate."""
    if _mut("refreshloop"):
        return live_peer_ok(addr) or lan_auth_on()
    return lan_auth_verdict(addr, header) == 0

def origin_get(url, headers=None, timeout_s=8, limit=2 * 1024 * 1024, meta=None):
    """(status, text or None, final URL). An HTTP error keeps its status; any other failure is 502.
    meta (a dict) gets cookie=True when a 200 sets one."""
    try:
        req = urllib.request.Request(url, headers=_urllib_headers(headers))
        with urllib.request.urlopen(req, timeout=timeout_s, context=_tls_ctx()) as resp:
            raw = resp.read(limit + 1)
            final = resp.geturl() or url
            if meta is not None and _sets_cookie(resp):
                meta["cookie"] = True
    except urllib.error.HTTPError as e:
        e.close()
        return e.code, None, url
    except Exception:  # noqa: BLE001
        return 502, None, url
    if len(raw) > limit:
        return 502, None, url
    return 200, raw.decode("utf-8", "replace"), final

_URI_ATTR = re.compile(r'URI="([^"]*)"')

def rewrite_playlist(text, base):
    """Every segment line and URI="..." attribute (EXT-X-KEY, EXT-X-MAP, ...) made absolute against base."""
    if _mut("norewrite"):
        return text
    join = lambda u: urllib.parse.urljoin(base, u)
    out = []
    for raw in text.splitlines():
        s = raw.strip()
        if not s:
            continue
        if s.startswith("#"):
            out.append(_URI_ATTR.sub(lambda m: 'URI="%s"' % join(m.group(1)), s) if 'URI="' in s else s)
        else:
            out.append(join(s))
    return "\n".join(out) + "\n"

def _expiring(url):
    exp = signed_url_expiry(url)
    return exp is not None and exp - time.time() < PLAN_EXP_MARGIN_S

def _live_remaster(e):
    """Re-fetch e's master and take the same rendition from it. Caller holds e.fetch_mu, not _LIVE_MU."""
    with _LIVE_MU:
        master, mseq, hdrs, rung, was_stale, old = e.master, e.master_seq, dict(e.hdrs), e.rung, e.stale, e.variant
    meta = {}
    st, text, final = origin_get(master, hdrs, timeout_s=6, meta=meta)
    plan = plan_hls_master(text, final) if st == 200 else None
    v = same_rung(plan, rung) if plan else None
    with _LIVE_MU:
        if meta.get("cookie"):
            e.cookie = True
        if e.master_seq != mseq:
            return   # a newer /refresh landed meanwhile: the next GET re-fetches that one
        if v is None:
            if st == 200 or st in (403, 410):
                e.stale = False   # that master is dead or has no in-spec rendition: keep the current one
            log("held play: master re-fetch %s for %s; keeping the current rendition" % (
                "found no in-spec variant" if plan is not None else "status %d" % st, log_src_tokenless(master)))
            return
        e.variant, e.rung, e.stale = v["variant_url"], _rung(v), False
        if v.get("audio_url"):
            e.audio = v["audio_url"]
    if was_stale or v["variant_url"] != old:
        log("held play: rendition re-signed from %s (%s)" % (log_src_tokenless(master), hls_log_line(dict(v, copy=True))))

def access_line(requestline):
    """An access-log request line with src= cut to log_src_tokenless and the hdr= value dropped: a cast src carries
    signed path tokens and hdnts/hmac queries, hdr= can carry a Cookie. AIRPLAY_MUT=rawaccess keeps it as is."""
    parts = str(requestline).split(" ")
    if len(parts) != 3 or "?" not in parts[1] or _mut("rawaccess"):
        return requestline
    path, _, query = parts[1].partition("?")
    qs = [(k, log_src_tokenless(v) if k == "src" else ("REDACTED" if k == "hdr" else v))
          for k, v in urllib.parse.parse_qsl(query, keep_blank_values=True)]
    return "%s %s?%s %s" % (parts[0], path, urllib.parse.urlencode(qs), parts[2])


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

class _NullCtx:
    def __enter__(self): return self
    def __exit__(self, *exc): return False

class H(BaseHTTPRequestHandler):
    server_version = "airplay-xcode/2-standalone"
    def log_message(self, fmt, *a): log("%s %s" % (self.client_address[0], fmt % a))
    def log_request(self, code="-", size="-"):
        code = getattr(code, "value", code)
        if self.path.startswith("/hls/") and str(code).startswith("2"):
            return   # ffmpeg reloads its live playlist every target duration
        self.log_message('"%s" %s %s', access_line(self.requestline), str(code), str(size))
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
    def _plan_and_probe(self, src, fwd, yt, admit, budget):
        """(hls, plan, info, drm, trusted_used): the HLS master plan and the probe of the source."""
        hls = None
        plan = None
        trusted_used = False
        if not yt and hls_playlist_url(src):
            meta = {}
            got = fetch_playlist(src, timeout_s=4 if admit else 15, headers=fwd, meta=meta)
            if got and detect_drm(got[0]):
                return None, None, None, True, False
            plan = plan_hls_master(got[0], got[1]) if got else None
            if plan:
                hls = choose_hls(plan, lambda url: self._probe_url(url, budget, headers=fwd))
                if hls and meta.get("cookie"):
                    hls["cookie"] = True
                if admit and (not hls or (not hls.get("copy") and not hls.get("refused"))):
                    trusted = choose_hls_playlist_trust(plan)
                    if trusted:
                        hls = trusted
                        trusted_used = True
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
        return hls, plan, info, False, trusted_used
    def _replan_copy_cmd(self, src, fwd, fwd_blob, cache_key, w, h, br, off):
        """v1.0.1 403: a fresh plan + probe from the request's own URL and its stream-copy ffmpeg command, or None.
        Never an x264 command: a source that is not stream-copyable now ends the open (Shawn's AirPlay rule)."""
        hls, plan, info, drm, trusted_used = self._plan_and_probe(src, fwd, None, False, 25)
        if drm or not hls or not info or hls.get("refused"):
            return None
        remux = bool(hls.get("copy")) and stream_copy_ok(info)
        if not remux:
            return None
        seq = url_seen(src)
        if cache_key and not trusted_used and not info.get("probe_failed"):
            plan_cache_put(cache_key, {"hls": hls, "plan": plan, "info": info, "seq": seq},
                           plan_ttl_s(plan_expiry(src, hls)))
        play, audio = self._hls_inputs(src, fwd, hls, seq)
        inputs = [(play, fwd_blob)] + ([(audio, fwd_blob)] if audio else [])
        log("xcode re-resolved %s: %s" % (log_src_tokenless(src), hls_log_line(dict(hls, copy=True))))
        return build_cmd(self.server.ffmpeg, play, w, h, br, off, True, info, inputs,
                         copy_audio=bool(hls.get("copy_audio")), fast=False)
    def _hls_inputs(self, src, fwd, hls, plan_seq):
        """ffmpeg inputs of an HLS stream copy: the helper's own live playlists (see live_register), or the origin
        rendition itself when the origin sets a cookie (its segments need it; the proxy playlist would drop it)."""
        if _mut("nolivehls"):
            return hls["variant_url"], hls.get("audio_url")
        key = live_register(src, fwd, hls, plan_seq, pin=self._live_pin is None)
        if key is None:
            log("hls live table full of running plays: %s reads the origin directly, a held play of it is kept "
                "as is" % log_src_tokenless(src))
            return hls["variant_url"], hls.get("audio_url")
        self._live_key = self._live_pin = key
        got = live_prepare(key)
        if got and got[2] and not _mut("nocookiefall"):
            log("hls source sets a cookie (%s): ffmpeg reads the origin playlist directly; a held play of it is "
                "reopened, not re-signed in place" % log_src_tokenless(src))
            return got[0], got[1]
        return live_input_urls(self.server.server_address[1], key, bool(hls.get("audio_url")))
    def _live_playlist(self, path):
        """GET /hls/<key>/media.m3u8|audio.m3u8: the live entry's media playlist from the origin, URIs absolute."""
        if not live_peer_ok(self.client_address[0]):
            return self.text(403, "loopback only\n")
        parts = path.split("/")
        if len(parts) != 4 or parts[3] not in ("media.m3u8", "audio.m3u8"):
            return self.text(404, "no such playlist\n")
        audio = parts[3] == "audio.m3u8"
        with _LIVE_MU:
            e = _LIVE.get(parts[2])
            if e is not None:
                e.touched = _mono()
        if e is None:
            return self.text(404, "no live stream\n")
        name = parts[3][:-5]
        with e.fetch_mu:
            with _LIVE_MU:
                url = e.audio if audio else e.variant
                need = e.master is not None and (e.stale or bool(url and _expiring(url)))
                pre = e.pre.pop(name, None)
            if need:
                _live_remaster(e)
            with _LIVE_MU:
                url, hdrs = (e.audio if audio else e.variant), dict(e.hdrs)
        if not url:
            return self.text(404, "no rendition\n")
        meta = {}
        if pre and not need and pre[0] == url and _mono() - pre[3] <= LIVE_PREFETCH_REUSE_S:
            st, text, final = 200, pre[1], pre[2]
        else:
            st, text, final = origin_get(url, hdrs, meta=meta)
        if st != 200:
            log("hls %s playlist: origin status %d (%s)" % ("audio" if audio else "media", st, log_src_tokenless(url)))
            return self.text(st if st in (401, 403, 404, 410) else 502, "origin status %d\n" % st)
        with _LIVE_MU:
            if meta.get("cookie") and not e.cookie:
                e.cookie = True
                log("hls %s playlist: the origin set a cookie mid-play (%s): /refresh answers cookie" % (
                    name, log_src_tokenless(url)))
            if not audio:
                e.kind = _playlist_kind(text)
            if e.kind == "vod":
                e.read_exp[name] = _read_expiry([url], [(text, final)])
        body = rewrite_playlist(text, final).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/vnd.apple.mpegurl")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def _refresh(self, src, fwd):
        """GET /refresh?src=: a held play's newest signed URL. Stored only: the next playlist GET fetches it. The body
        says whether the running ffmpeg will (refresh_answer); misterplexd reopens the play when it will not."""
        if not refresh_peer_ok(self.client_address[0], self.headers.get(LAN_AUTH_HEADER, "")):
            return self.text(403, "loopback only\n")
        if not (src.startswith("http://") or src.startswith("https://")):
            return self.text(400, "src must be http(s)\n")
        stored, ans = live_refresh(src, fwd)
        if stored:
            log("held play: refreshed source %s (stored; %s)" % (
                log_src_tokenless(src), "fetched at the next playlist reload" if ans.startswith("ok") else
                "answered %s: its ffmpeg does not reload" % ans))
        return self.text(200, ans + "\n")
    def _orphan(self, job, head=b""):
        """ladder_orphan. The live pin moves to the job first: its ffmpeg runs on, and a repeat open may adopt it."""
        job.live_pin, self._live_pin = self._live_pin, None
        ladder_orphan(job, head=head)
    def _refuse(self, kind):
        if kind not in _REFUSE_KINDS:
            kind = "format"
        log("airplay video refused reason=%s" % kind)
        return self.text(415, "refused\nreason=%s\nnotice=%s\n" % (kind, notice_for(kind)))
    def do_GET(self):  # noqa: N802
        self._live_key = self._live_pin = None
        try:
            self._get()
        finally:
            if self._live_pin:
                live_unpin(self._live_pin)
    def _get(self):
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
        if u.path.startswith("/hls/"):
            return self._live_playlist(u.path)
        src = g("src")
        if u.path == "/yt.ts":
            src = "yt:" + g("v")
        elif u.path not in ("/probe", "/ladder.ts", "/admit", "/refresh"):
            return self.text(404, "use /probe, /ladder.ts, /admit or /yt.ts\n")
        if not ok_src(src, peer_is_local(self.client_address[0])):
            return self.text(400, "src must be http(s) or yt:<videoId>, and not a local address\n")
        fwd = sanitize_fwd_headers(g("hdr")) if u.path in ("/probe", "/admit", "/ladder.ts", "/refresh") else {}
        if fwd:
            log("fwd headers names=%s" % ",".join(fwd.keys()))
        if u.path == "/refresh":
            return self._refresh(src, fwd)
        if not src.startswith("yt:"):
            url_seen(src)
        fwd_blob = _headers_blob(fwd)
        yt = yt_resolve(src[3:], self.server.ytdlp) if src.startswith("yt:") else None
        if src.startswith("yt:") and not yt: return self.text(502, "youtube resolve failed\n")
        admit = u.path == "/admit"
        budget = 6 if admit else 25
        # /admit, /probe and /ladder.ts of one source share one plan + probe (PLAN_TTL_S). A request for a source
        # that is being planned right now waits for it and then hits the cache.
        cache_key = None if (yt or src.startswith("yt:")) else plan_key(stream_identity(src), fwd_blob)
        held = _PlanLock(cache_key) if cache_key else None
        if held: held.__enter__()
        try:  # held until the ffmpeg is started and registered: a repeat open of the same stream queues behind it
            hit = plan_cache_get(cache_key) if cache_key else None
            if hit is not None:
                log("plan cache hit %s (%s)" % (u.path, log_src_tokenless(src)))
                if hit.get("drm"):
                    return self._refuse("drm")
                hls, plan, info = copy.deepcopy((hit["hls"], hit["plan"], hit["info"]))
                plan_seq = hit.get("seq", -1)
            else:
                hls, plan, info, drm, trusted_used = self._plan_and_probe(src, fwd, yt, admit, budget)
                plan_seq = -1 if src.startswith("yt:") else url_seen(src)
                if drm:
                    if cache_key:
                        plan_cache_put(cache_key, {"drm": True})
                    return self._refuse("drm")
                # A trusted-by-playlist /admit answer and a failed probe (playlist fallback) are not a probe result:
                # they are not shared, the next request probes for real.
                if cache_key and (info or (hls and hls.get("refused"))) and \
                        (not trusted_used or _mut("plantrusted")) and not (info or {}).get("probe_failed"):
                    ttl = plan_ttl_s(plan_expiry(src, hls))
                    if not plan_cache_put(cache_key, {"hls": hls, "plan": plan, "info": info, "seq": plan_seq}, ttl):
                        log("plan not cached: its signed URL expires in %.0fs (margin %.0fs)" % (
                            ttl + PLAN_EXP_MARGIN_S, PLAN_EXP_MARGIN_S))
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
                    if hls and dec["action"] in ("copy", "audio"):
                        live_admit(src)
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
            retry_cmd = retry_cmd_saved = None
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
                    remux = bool(hls.get("copy")) and stream_copy_ok(info)
                    play, audio = hls["variant_url"], hls.get("audio_url")
                    if remux:
                        play, audio = self._hls_inputs(src, fwd, hls, plan_seq)
                    inputs = [(play, fwd_blob)] + ([(audio, fwd_blob)] if audio else [])
                    copy_audio = bool(remux and hls.get("copy_audio"))
                    slow_known = _slowstart_known(stream_identity(src))
                    cmd = build_cmd(self.server.ffmpeg, play, w, h, br, off, remux, info, inputs, copy_audio=copy_audio,
                                    fast=not slow_known)
                    if fast_probe_used(cmd) and not _mut("noretry"):
                        # AIRPLAY_MUT=x264retry / retryfast: a transcode / the same fast probe again (the test must fail)
                        retry_cmd = retry_cmd_saved = build_cmd(self.server.ffmpeg, play, w, h, br, off,
                                                                remux and not _mut("x264retry"), info, inputs,
                                                                copy_audio=copy_audio, fast=_mut("retryfast"))
                    shown = dict(hls)
                    shown["copy"] = remux
                    log(hls_log_line(shown))
                else:
                    remux = stream_copy_ok(info)
                    copy_audio = remux and (info or {}).get("acodec") == "aac"
                    inputs = [(src, fwd_blob)] if fwd_blob else None
                    cmd = build_cmd(self.server.ffmpeg, src, w, h, br, off, remux, info, inputs, copy_audio=copy_audio)
                    log(direct_log_line(info, remux))
                log("xcode %s %dx%d %dk off=%d copy=%d src=%s" % (u.path, w, h, br, off, int(remux), log_src_tokenless(src)))
            # A repeat open of the SAME stream adopts the ffmpeg of an open whose client dropped before the first byte.
            adoptable = (u.path == "/ladder.ts" and not src.startswith("yt:")
                         and not (info and info.get("vcodec") == "none"))
            job = ladder_adopt(ladder_job_key(src, w, h, br, off, remux)) if adoptable else None
            if job:
                proc = job.proc
                if job.live_pin:   # this request pins the same stream's entry now (or takes the orphan's pin)
                    if self._live_pin:
                        live_unpin(job.live_pin)
                    else:
                        self._live_pin = job.live_pin
                    job.live_pin = None
                log("xcode adopt /ladder.ts: repeat open of the same stream joins the running ffmpeg pid %d "
                    "(orphan head=%d bytes)" % (proc.pid, len(job.head)))
            else:
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, preexec_fn=os.setsid, env=pipe_env)
                job = LadderJob(ladder_job_key(src, w, h, br, off, remux) if adoptable else "", proc)
                with JOBS_MU:
                    JOBS.append(proc)
                    while len(JOBS) > MAX_JOBS:
                        old = JOBS.pop(0); log("job cap: killing pid %d" % old.pid); kill(old)
                if adoptable:
                    ladder_register(job)
        finally:
            if held: held.__exit__(None, None, None)
        sent = 0
        kept = False
        out_fd, conn = proc.stdout.fileno(), self.connection
        watch_conn = adoptable
        touch_at = _mono() + LIVE_TOUCH_S
        try:
            try:
                self.send_response(200); self.send_header("Content-Type", "video/mp2t")
                self.send_header("Cache-Control", "no-store"); self.send_header("Connection", "close"); self.end_headers()
            except (BrokenPipeError, ConnectionResetError):
                # The client left while the plan/probe/ffmpeg start was running (the daemon dropped the open).
                if adoptable:
                    self._orphan(job)
                    kept = True
                raise
            pending, job.head = job.head, b""
            if pending:
                try:
                    self.wfile.write(pending)
                except (BrokenPipeError, ConnectionResetError):
                    self._orphan(job, head=pending)   # the next repeat open still gets that chunk
                    kept = True
                    raise
                sent += len(pending); job.sent += len(pending)
            while True:
                if watch_conn and job.sent == 0:
                    # Before the first byte: see the client leave (the daemon dropped this open) instead of
                    # finding out at the first write ~10 s later.
                    r, _, _ = select.select([out_fd, conn], [], [], 0.2)
                    if conn in r:
                        try:
                            gone = conn.recv(1, socket.MSG_PEEK) == b""
                        except BlockingIOError:
                            gone = False
                        except OSError:
                            gone = True
                        if gone:
                            self._orphan(job)
                            kept = True
                            break
                        watch_conn = False   # the client sent more bytes: stop watching, rely on write errors
                    if out_fd not in r:
                        continue
                chunk = os.read(out_fd, 64 * 1024)  # what is there now: low-rate live streams
                if not chunk:
                    early_err, early_rc = None, None
                    if sent == 0 and job.sent == 0 and (retry_cmd or (cache_key and not job.replanned)):
                        try:
                            early_rc = proc.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            early_rc = None
                        if early_rc not in (0, None):
                            early_err = (proc.stderr.read() or b"")[-600:]
                    if (early_err is not None and cache_key and not job.replanned and not _mut("noreplan")
                            and upstream_denied(early_err)):
                        # v1.0.1 403: the cached plan's signed variant URL is dead. Drop the plan and re-resolve ONCE
                        # from this request's own URL; same stream copy, never a transcode.
                        job.replanned = not _mut("noflag")   # AIRPLAY_MUT=noflag: no once-guard (test must fail)
                        plan_cache_drop(cache_key)
                        log("xcode upstream refused the cached plan's URL rc=%s before the first byte (%s): plan "
                            "invalidated, re-resolving %s once (stream copy)" % (
                                early_rc, redact_tool_stderr(early_err, 260, 3), log_src_tokenless(src)))
                        new_cmd = self._replan_copy_cmd(src, fwd, fwd_blob, cache_key, w, h, br, off)
                        if new_cmd:
                            retry_cmd = None
                            _reap_proc(proc)
                            proc = subprocess.Popen(new_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                                    preexec_fn=os.setsid, env=pipe_env)
                            job.proc = proc
                            with JOBS_MU:
                                JOBS.append(proc)
                                while len(JOBS) > MAX_JOBS:
                                    o2 = JOBS.pop(0); log("job cap: killing pid %d" % o2.pid); kill(o2)
                            out_fd = proc.stdout.fileno()
                            continue
                        log("xcode re-resolve after the upstream refusal found no stream copy: ending the open")
                        break
                    if retry_cmd and sent == 0 and job.sent == 0 and not job.retried:
                        # The fast copy start died before any byte (e.g. "sample rate not set": late audio).
                        # One retry of the SAME stream copy with the old, full probe; never a transcode.
                        old, retry_cmd = proc, None
                        rc = early_rc
                        if rc not in (0, None):
                            log("xcode fast start failed rc=%s before the first byte (%s): retrying the same stream copy "
                                "with the full probe" % (rc, redact_tool_stderr(early_err or b"", 260, 3)))
                            _slowstart_remember(stream_identity(src))
                            job.retried = True
                            _reap_proc(old)
                            proc = subprocess.Popen(retry_cmd_saved, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                                    preexec_fn=os.setsid, env=pipe_env)
                            job.proc = proc
                            with JOBS_MU:
                                JOBS.append(proc)
                                while len(JOBS) > MAX_JOBS:
                                    o2 = JOBS.pop(0); log("job cap: killing pid %d" % o2.pid); kill(o2)
                            out_fd = proc.stdout.fileno()
                            continue
                    break
                try:
                    self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    if adoptable and job.sent == 0:
                        self._orphan(job, head=chunk)
                        kept = True
                    break
                sent += len(chunk); job.sent += len(chunk)
                if self._live_key and _mono() >= touch_at:
                    live_touch(self._live_key)
                    touch_at = _mono() + LIVE_TOUCH_S
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            if kept:
                log("xcode orphaned bytes=0 pid=%d kept %ds for a repeat open of the same stream" % (proc.pid, ADOPT_GRACE_S))
            else:
                ladder_forget(job)
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
