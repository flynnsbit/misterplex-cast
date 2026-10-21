#!/usr/bin/env python3
"""Host-side 720p24 transcode proxy for MiSTer.

MiSTer still requests PMS universal start.mp4 (videoResolution=1280x720).
This process sits in front of the remote PMS, passes metadata through, and
replaces the video transcode with an x86 libx264 baseline 1280x720 @ 24p
~1500 kbps stream that dual-A9 can present in realtime.

Remote PMS HEVC 1080→720 is not realtime (~13 unique). Isolated ARM decode
of 1280x720 baseline is ~32 fps.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

UPSTREAM_DEFAULT = "http://198.51.100.153:32400"


def _ffmpeg_720p(src_url: str, start_sec: float, headers: dict[str, str]) -> subprocess.Popen:
    del headers  # original Part fetch uses token in the URL; MiSTer client headers break it
    args = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostdin",
        "-fflags",
        "+genpts",
    ]
    if start_sec > 0:
        args += ["-ss", f"{start_sec:.3f}"]
    args += [
        "-i",
        src_url,
        "-map",
        "0:v:0",
        "-map",
        "0:a:0?",
        "-vf",
        "scale=1280:720:flags=fast_bilinear,fps=24000/1001",
        "-c:v",
        "libx264",
        "-preset",
        "ultrafast",
        "-tune",
        "zerolatency",
        "-profile:v",
        "baseline",
        "-level",
        "3.1",
        "-pix_fmt",
        "yuv420p",
        "-g",
        "48",
        "-b:v",
        "900k",
        "-maxrate",
        "1100k",
        "-bufsize",
        "1800k",
        "-c:a",
        "aac",
        "-ar",
        "48000",
        "-ac",
        "2",
        "-b:a",
        "128k",
        "-f",
        "mpegts",
        "pipe:1",
    ]
    return subprocess.Popen(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        bufsize=0,
    )


STUB_40868 = (
    b'<?xml version="1.0" encoding="UTF-8"?>'
    b'<MediaContainer size="1">'
    b'<Video ratingKey="40868" key="/library/metadata/40868" title="Encounter at Farpoint"'
    b' type="episode" duration="5484416">'
    b'<Media duration="5484416" videoResolution="1080" width="1440" height="1080"'
    b' aspectRatio="1.333" videoFrameRate="24p" videoCodec="hevc" audioCodec="aac"'
    b' container="mkv">'
    b'<Part key="/library/parts/448398/file.mkv" duration="5484416">'
    b'<Stream streamType="1" codec="hevc" width="1440" height="1080" frameRate="23.976"/>'
    b'<Stream streamType="2" codec="aac" channels="2"/>'
    b"</Part></Media></Video></MediaContainer>"
)


class Proxy(BaseHTTPRequestHandler):
    upstream_base = UPSTREAM_DEFAULT

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def do_GET(self) -> None:
        path = self.path
        if "/video/:/transcode/universal/start" in path:
            self._transcode()
            return
        self._passthrough()

    def _passthrough(self) -> None:
        if "/library/metadata/40868" in self.path.split("?")[0]:
            self.send_response(200)
            self.send_header("Content-Type", "application/xml")
            self.send_header("Content-Length", str(len(STUB_40868)))
            self.end_headers()
            self.wfile.write(STUB_40868)
            return
        if "/transcode/universal/decision" in self.path:
            body = b'<?xml version="1.0"?><MediaContainer transcodeDecisionCode="1000"/>'
            self.send_response(200)
            self.send_header("Content-Type", "application/xml")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        url = self.upstream_base + self.path
        headers = {k: v for k, v in self.headers.items() if k.lower() != "host"}
        req = urllib.request.Request(url, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=8) as resp:
                body = resp.read()
                self.send_response(resp.status)
                ctype = resp.headers.get("Content-Type", "application/octet-stream")
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        except Exception:
            if "/identity" in self.path:
                body = b'<?xml version="1.0"?><MediaContainer size="0"/>'
                self.send_response(200)
                self.send_header("Content-Type", "application/xml")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            self.send_error(502, "upstream")

    def _transcode(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
        token = (q.get("X-Plex-Token") or [self.headers.get("X-Plex-Token", "")])[0]
        meta_path = (q.get("path") or [""])[0]
        try:
            start_sec = float((q.get("offset") or ["0"])[0] or 0)
        except ValueError:
            start_sec = 0.0
        src = self.upstream_base + parsed.path + "?" + parsed.query
        if meta_path:
            try:
                meta_url = self.upstream_base + meta_path
                if token:
                    meta_url += ("&" if "?" in meta_url else "?") + "X-Plex-Token=" + urllib.parse.quote(
                        token
                    )
                req = urllib.request.Request(meta_url, headers={"Accept": "application/xml"})
                with urllib.request.urlopen(req, timeout=12) as resp:
                    xml = resp.read().decode("utf-8", "replace")
                m = re.search(r"<Part[^>]*\skey=\"([^\"]+)\"", xml)
                if m:
                    part = m.group(1)
                    if not part.startswith("/"):
                        part = "/" + part
                    src = self.upstream_base + part
                    if token:
                        src += ("&" if "?" in src else "?") + "X-Plex-Token=" + urllib.parse.quote(
                            token
                        )
            except Exception as exc:
                sys.stderr.write("meta/part fallback: %s\n" % exc)
        hdrs = {k: v for k, v in self.headers.items() if k.lower() != "host"}
        proc = _ffmpeg_720p(src, start_sec, hdrs)
        try:
            self.send_response(200)
            self.send_header("Content-Type", "video/mp2t")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            assert proc.stdout is not None
            while True:
                chunk = proc.stdout.read(64 * 1024)
                if not chunk:
                    break
                self.wfile.write(chunk)
        except BrokenPipeError:
            pass
        finally:
            proc.kill()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=32401)
    ap.add_argument("--upstream", default=os.environ.get("PMS_UPSTREAM", UPSTREAM_DEFAULT))
    args = ap.parse_args()
    Proxy.upstream_base = args.upstream.rstrip("/")
    httpd = ThreadingHTTPServer((args.bind, args.port), Proxy)
    print(f"pms_720p_proxy {args.bind}:{args.port} -> {Proxy.upstream_base}", flush=True)
    httpd.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
