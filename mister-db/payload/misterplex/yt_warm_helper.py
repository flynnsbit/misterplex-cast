#!/usr/bin/env python3
"""Resident, stdlib-only Android Innertube resolver (Python 3.9+)."""

import argparse
import errno
import json
import os
import re
import signal
import socket
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


CLIENT_VERSION = "21.26.364"
USER_AGENT = "com.google.android.youtube/" + CLIENT_VERSION + " (Linux; U; Android 11) gzip"
MAX_MESSAGE = 65536


def https_url(value):
    if not isinstance(value, str) or any(c.isspace() for c in value):
        return False
    parsed = urllib.parse.urlsplit(value)
    return parsed.scheme == "https" and bool(parsed.hostname)


def _format_choice(url, audio_url, fmt, adaptive):
    height = int(fmt.get("height", 0) or 0)
    itag = int(fmt.get("itag", 0) or 0)
    capped = height < 720 and _android_sabr_capped(height, adaptive)
    return {
        "url": url,
        "audioUrl": audio_url or "",
        "height": height,
        "itag": itag,
        "capped": capped,
    }


def _android_sabr_capped(chosen_height, adaptive):
    for fmt in adaptive:
        mime = fmt.get("mimeType", "")
        if not (mime.startswith("video/mp4") and "avc1" in mime):
            continue
        h = fmt.get("height", 0)
        fps = fmt.get("fps", 0)
        if h > chosen_height and 0 < h <= 720 and 0 < fps <= 30 and not https_url(fmt.get("url")):
            return True
    return False


def select_formats(player):
    if os.environ.get("MUT_YT_WARM_PROGRESSIVE_FIRST") == "1":
        return _select_formats_progressive_first(player)
    status = player.get("playabilityStatus", {})
    if status.get("status") != "OK":
        raise ValueError("Android player unavailable: " + str(status.get("status", "missing status")))
    if player.get("videoDetails", {}).get("isLiveContent"):
        raise ValueError("live content requires one-shot resolver")
    streams = player.get("streamingData", {})
    adaptive = streams.get("adaptiveFormats", [])

    def avc_video(fmt):
        mime = fmt.get("mimeType", "")
        return (mime.startswith("video/mp4") and "avc1" in mime
                and 0 < fmt.get("height", 0) <= 720
                and 0 < fmt.get("fps", 0) <= 30
                and https_url(fmt.get("url")))

    def avc_progressive(fmt):
        mime = fmt.get("mimeType", "")
        return avc_video(fmt) and "mp4a" in mime

    def rank(fmt):
        return fmt.get("height", 0), fmt.get("bitrate", 0)

    progressive = [f for f in streams.get("formats", []) if avc_progressive(f)]
    videos = [f for f in adaptive if avc_video(f)]
    audio = [f for f in adaptive
             if f.get("mimeType", "").startswith("audio/mp4")
             and "mp4a" in f.get("mimeType", "") and https_url(f.get("url"))]
    best_prog = max(progressive, key=rank) if progressive else None
    if videos and audio:
        best_vid = max(videos, key=rank)
        best_aud = max(audio, key=lambda f: f.get("bitrate", 0))
        if not best_prog or best_vid.get("height", 0) > best_prog.get("height", 0):
            return _format_choice(best_vid["url"], best_aud["url"], best_vid, adaptive)
    if best_prog:
        return _format_choice(best_prog["url"], "", best_prog, adaptive)
    if not videos or not audio:
        raise ValueError("Android player has no direct AVC/AAC <=720p30 HTTPS formats")
    best_vid = max(videos, key=rank)
    best_aud = max(audio, key=lambda f: f.get("bitrate", 0))
    return _format_choice(best_vid["url"], best_aud["url"], best_vid, adaptive)


def _select_formats_progressive_first(player):
    """Test mutant: old progressive-before-adaptive ordering."""
    status = player.get("playabilityStatus", {})
    if status.get("status") != "OK":
        raise ValueError("Android player unavailable: " + str(status.get("status", "missing status")))
    if player.get("videoDetails", {}).get("isLiveContent"):
        raise ValueError("live content requires one-shot resolver")
    streams = player.get("streamingData", {})

    def avc(fmt):
        mime = fmt.get("mimeType", "")
        return (mime.startswith("video/mp4") and "avc1" in mime
                and 0 < fmt.get("height", 0) <= 720
                and 0 < fmt.get("fps", 0) <= 30
                and https_url(fmt.get("url")))

    def rank(fmt):
        return fmt.get("height", 0), fmt.get("bitrate", 0)

    progressive = [f for f in streams.get("formats", [])
                   if avc(f) and "mp4a" in f.get("mimeType", "")]
    if progressive:
        best = max(progressive, key=rank)
        return _format_choice(best["url"], "", best, streams.get("adaptiveFormats", []))
    adaptive = streams.get("adaptiveFormats", [])
    videos = [f for f in adaptive if avc(f)]
    audio = [f for f in adaptive
             if f.get("mimeType", "").startswith("audio/mp4")
             and "mp4a" in f.get("mimeType", "") and https_url(f.get("url"))]
    if not videos or not audio:
        raise ValueError("Android player has no direct AVC/AAC <=720p30 HTTPS formats")
    best_vid = max(videos, key=rank)
    best_aud = max(audio, key=lambda f: f.get("bitrate", 0))
    return _format_choice(best_vid["url"], best_aud["url"], best_vid, adaptive)


class Worker:
    def __init__(self):
        ca = os.environ.get("MPX_CA_BUNDLE")
        context = ssl.create_default_context(cafile=ca if ca else None)
        self.http = urllib.request.build_opener(urllib.request.HTTPSHandler(context=context))

    def resolve(self, request):
        video_id = request.get("videoId", "")
        if not isinstance(video_id, str) or not re.fullmatch(r"[A-Za-z0-9_-]{11}", video_id):
            raise ValueError("invalid videoId")
        if request.get("format"):
            raise ValueError("custom format requires one-shot resolver")
        timeout_ms = request.get("timeoutMs", 5000)
        if isinstance(timeout_ms, bool) or not isinstance(timeout_ms, int) or timeout_ms <= 0:
            raise ValueError("invalid timeoutMs")
        deadline = time.monotonic() + min(timeout_ms, 5000) / 1000
        payload = {
            "context": {"client": {
                "clientName": "ANDROID", "clientVersion": CLIENT_VERSION,
                "androidSdkVersion": 30, "hl": "en", "gl": "US",
                "userAgent": USER_AGENT, "osName": "Android", "osVersion": "11",
            }},
            "videoId": video_id, "contentCheckOk": True, "racyCheckOk": True,
        }
        req = urllib.request.Request(
            "https://www.youtube.com/youtubei/v1/player?prettyPrint=false",
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json", "User-Agent": USER_AGENT,
                     "X-Youtube-Client-Name": "3", "X-Youtube-Client-Version": CLIENT_VERSION})
        with self.http.open(req, timeout=max(0.001, deadline - time.monotonic())) as response:
            # Bound both memory and the total read, not only individual socket operations.
            raw = bytearray()
            while True:
                if time.monotonic() >= deadline:
                    raise TimeoutError("Android player timed out")
                chunk = response.read(65536)
                if not chunk:
                    break
                raw.extend(chunk)
                if len(raw) > 2 * 1024 * 1024:
                    raise ValueError("Android player response too large")
        player = json.loads(raw)
        choice = select_formats(player)
        return {
            "ok": True,
            "url": choice["url"],
            "audioUrl": choice["audioUrl"],
            "height": choice["height"],
            "itag": choice["itag"],
            "capped": choice["capped"],
            "title": player.get("videoDetails", {}).get("title", ""),
        }


def bind_socket(server, path):
    try:
        server.bind(path)
    except OSError as exc:
        if exc.errno not in (errno.EPERM, errno.EOPNOTSUPP):
            raise
        # MiSTer's vfat SD cannot hold socket inodes. Keep the logical endpoint
        # on SD and use a Linux abstract AF_UNIX address with that same name.
        try:
            server.bind("\0" + path)
        except OSError as abstract_exc:
            # Orphaned helper after misterplexd kill -9 keeps the abstract name
            # (P2-a). Supervisor kills stale workers before spawn; still surface
            # EADDRINUSE clearly for logs / glass.
            if abstract_exc.errno == errno.EADDRINUSE:
                raise OSError(errno.EADDRINUSE, "yt-warm abstract endpoint in use: " + path)
            raise
        with open(path, "x", encoding="ascii") as marker:
            marker.write("abstract\n")
        print("yt-warm: SD filesystem uses abstract AF_UNIX endpoint", file=sys.stderr, flush=True)


def serve(args, worker):
    os.makedirs(args.cache_dir, exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(args.socket)), exist_ok=True)
    os.umask(0o077)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    bound = False
    try:
        bind_socket(server, args.socket)
        bound = True
        server.listen(4)
        server.settimeout(1.0)
        print("ready", flush=True)
        while True:
            if args.parent_pid is not None and os.getppid() != args.parent_pid:
                print("yt-warm: parent pid changed; exiting", file=sys.stderr, flush=True)
                raise SystemExit(0)
            try:
                conn, _ = server.accept()
            except socket.timeout:
                continue
            with conn:
                conn.settimeout(6)
                try:
                    with conn.makefile("rb") as stream:
                        while True:
                            line = stream.readline(MAX_MESSAGE + 1)
                            if not line:
                                break
                            if len(line) > MAX_MESSAGE or not line.endswith(b"\n"):
                                raise ValueError("request too large or unterminated")
                            request = json.loads(line)
                            if not isinstance(request, dict):
                                raise ValueError("request must be an object")
                            if request.get("op") == "ping":
                                result = {"ok": True}
                            elif request.get("op") == "resolve":
                                result = worker.resolve(request)
                            else:
                                raise ValueError("unknown operation")
                            conn.sendall(json.dumps(result, ensure_ascii=True).encode("ascii") + b"\n")
                except (ValueError, KeyError, TypeError, OSError, urllib.error.URLError):
                    # Do not log signed media URLs or remote response bodies.
                    try:
                        conn.sendall(b'{"ok":false,"error":"warm resolve failed"}\n')
                    except OSError as exc:
                        print("yt-warm: response delivery failed: " + type(exc).__name__,
                              file=sys.stderr, flush=True)
    finally:
        server.close()
        if bound:
            os.unlink(args.socket)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", default=os.environ.get(
        "MPX_YT_WARM_SOCK", os.environ.get("YT_WARM_SOCK",
                                         "/media/fat/misterplex/yt-warm.sock")))
    parser.add_argument("--cache-dir", default="/media/fat/misterplex/yt-cache")
    parser.add_argument("--ytdlp-bin", default="")  # Reserved for a future portable CPython worker.
    parser.add_argument("--parent-pid", type=int, default=None,
                        help="Exit when os.getppid() differs (survives kill -9 of misterplexd).")
    args = parser.parse_args()
    os.nice(10)

    def terminate(_sig, _frame):
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, terminate)
    signal.signal(signal.SIGINT, terminate)
    serve(args, Worker())


if __name__ == "__main__":
    main()
