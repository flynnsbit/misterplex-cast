#!/usr/bin/env python3
"""Offline fake Plex Media Server for resolvePlayTarget replays (DAEMON-FPS-SPEC-1002 T2).

Serves canned library metadata, a universal-transcode decision and a tiny start.mp4
body on loopback only. Every request is logged (method, URL, X-Plex-* / Range /
Accept headers) with the Plex token redacted, one line per request, flushed before
the response is sent so a caller that has exited sees all of its requests.

  fake_pms.py --port-file PATH --log PATH [--token TOKEN] [--host 127.0.0.1] [--host6 ::1]

--host6 '' disables the IPv6 listener. http://[::1]:PORT is not a private-LAN host to
plex_resolve's plexLocationForBase(), so it drives the resolver's remote ("wan") branches.
"""

import argparse
import json
import os
import re
import socket
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

# rating key -> canned source. Rates use the strings PMS emits (Media@videoFrameRate
# token, Stream@frameRate decimal). "fps" is the exact source rate for reports only.
ITEMS = {
    "12": dict(title="PQ First Bind Short", fps="24/1", vfr="24p", fr="24.000",
               codec="h264", profile="main", w=640, h=480, ar="1.33", container="mp4"),
    "123": dict(title="PQ First Bind Exact", fps="24/1", vfr="24p", fr="24.000",
                codec="h264", profile="main", w=640, h=480, ar="1.33", container="mp4"),
    "101": dict(title="FPS 23.976 H.264 1080p", fps="24000/1001", vfr="24p", fr="23.976",
                codec="h264", profile="high", w=1920, h=1080, ar="1.78", container="mkv"),
    "102": dict(title="FPS 29.97 HEVC 1440x1080", fps="30000/1001", vfr="NTSC", fr="29.970",
                codec="hevc", profile="main", w=1440, h=1080, ar="1.78", container="mkv",
                anamorphic="1", par="4:3"),
    "103": dict(title="FPS 30 H.264 640x480", fps="30/1", vfr="NTSC", fr="30.000",
                codec="h264", profile="high", w=640, h=480, ar="1.33", container="mp4"),
    "104": dict(title="FPS 30 HEVC 1080p", fps="30/1", vfr="NTSC", fr="30.000",
                codec="hevc", profile="main", w=1920, h=1080, ar="1.78", container="mkv"),
    "105": dict(title="FPS 59.94 H.264 720p", fps="60000/1001", vfr="60p", fr="59.940",
                codec="h264", profile="high", w=1280, h=720, ar="1.78", container="mkv"),
    "106": dict(title="FPS 60 H.264 720p", fps="60/1", vfr="60p", fr="60.000",
                codec="h264", profile="main", w=1280, h=720, ar="1.78", container="mp4"),
    "107": dict(title="FPS 25 H.264 PAL 576", fps="25/1", vfr="PAL", fr="25.000",
                codec="h264", profile="main", w=720, h=576, ar="1.78", container="mkv",
                anamorphic="1", par="64:45"),
    "108": dict(title="FPS 24 H.264 1080p", fps="24/1", vfr="24p", fr="24.000",
                codec="h264", profile="high", w=1920, h=1080, ar="1.78", container="mkv"),
    "109": dict(title="FPS missing H.264 480p", fps="", vfr="", fr="",
                codec="h264", profile="main", w=640, h=480, ar="1.33", container="mp4"),
    "110": dict(title="FPS missing HEVC 1080p", fps="", vfr="", fr="",
                codec="hevc", profile="main", w=1920, h=1080, ar="1.78", container="mkv"),
}

DECISION_XML = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<MediaContainer size="1" allowSync="1" directPlayDecisionCode="3000" '
    'directPlayDecisionText="App cannot direct play this item. Direct play is disabled." '
    'generalDecisionCode="1001" generalDecisionText="Direct play not available; Conversion OK." '
    'transcodeDecisionCode="1001" transcodeDecisionText="Direct play not available; '
    'Conversion OK.">\n<Video ratingKey="0" type="movie"/>\n</MediaContainer>\n'
)

NOT_FOUND_HTML = "<html><head><title>Not Found</title></head><body><h1>404 Not Found</h1></body></html>\n"

LOGGED_HEADERS = ("accept", "range")


def metadata_xml(rk, it):
    media_fps = f' videoFrameRate="{it["vfr"]}"' if it["vfr"] else ""
    stream_fps = f' frameRate="{it["fr"]}"' if it["fr"] else ""
    ana = ""
    if it.get("anamorphic"):
        ana = f' anamorphic="{it["anamorphic"]}" pixelAspectRatio="{it["par"]}"'
    part_key = f"/library/parts/{int(rk) + 1000}/1700000000/file.{it['container']}"
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<MediaContainer size="1" allowSync="1" identifier="com.plexapp.plugins.library" '
        'librarySectionID="1" librarySectionTitle="Movies">\n'
        f'<Video ratingKey="{rk}" key="/library/metadata/{rk}" type="movie" '
        f'title="{it["title"]}" duration="120000" viewOffset="0">\n'
        f'<Media id="{int(rk) + 2000}" duration="120000" bitrate="4000" width="{it["w"]}" '
        f'height="{it["h"]}" aspectRatio="{it["ar"]}" audioChannels="2" audioCodec="aac" '
        f'videoCodec="{it["codec"]}" videoProfile="{it["profile"]}" '
        f'container="{it["container"]}"{media_fps}>\n'
        f'<Part id="{int(rk) + 1000}" key="{part_key}" duration="120000" '
        f'file="/media/movies/fps{rk}.{it["container"]}" size="60000000" '
        f'container="{it["container"]}">\n'
        f'<Stream id="{int(rk) + 3000}" streamType="1" default="1" codec="{it["codec"]}" '
        f'index="0" bitrate="3800" width="{it["w"]}" height="{it["h"]}"{stream_fps} '
        f'profile="{it["profile"]}" scanType="progressive"{ana} />\n'
        f'<Stream id="{int(rk) + 4000}" streamType="2" selected="1" default="1" codec="aac" '
        'index="1" channels="2" bitrate="128" samplingRate="48000" />\n'
        '</Part>\n</Media>\n</Video>\n</MediaContainer>\n'
    )


def play_queue_xml(qid):
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<MediaContainer size="2" playQueueID="{qid}" playQueueVersion="17">\n'
        '<Video ratingKey="12" key="/library/metadata/12" type="movie" '
        'title="PQ First Bind Short" duration="120000" playQueueItemID="7001" />\n'
        '<Video ratingKey="123" key="/library/metadata/123" type="movie" '
        'title="PQ First Bind Exact" duration="120000" playQueueItemID="7002" />\n'
        '</MediaContainer>\n'
    )


class Log:
    def __init__(self, path, token):
        self.lock = threading.Lock()
        self.fh = open(path, "a", encoding="utf-8")
        self.token = token

    def redact(self, s):
        s = re.sub(r"(X-Plex-Token=)[^&\s]*", r"\1REDACTED", s, flags=re.I)
        if self.token:
            s = s.replace(self.token, "REDACTED")
        return s

    def write(self, line):
        with self.lock:
            self.fh.write(self.redact(line) + "\n")
            self.fh.flush()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "FakePMS/1"
    sys_version = ""

    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body, ctype):
        data = body if isinstance(body, bytes) else body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def _log_request(self):
        hdrs = []
        for name, value in self.headers.items():
            lname = name.lower()
            if lname == "x-plex-token":
                value = "REDACTED"
            if lname.startswith("x-plex-") or lname in LOGGED_HEADERS:
                hdrs.append(f"{name}: {value}")
        # Stable sort: duplicate headers (e.g. two Profile-Name values) keep wire order.
        hdrs.sort(key=lambda h: h.split(":", 1)[0].lower())
        self.server.log.write(f"{self.command} {self.path} | " + " | ".join(hdrs))

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/__ping":
            self._send(200, "ok\n", "text/plain")
            return
        self._log_request()
        m = re.fullmatch(r"/library/metadata/(\d+)", path)
        if m and m.group(1) in ITEMS:
            self._send(200, metadata_xml(m.group(1), ITEMS[m.group(1)]), "application/xml")
        elif re.fullmatch(r"/playQueues/(\d+)", path):
            qid = path.rsplit("/", 1)[1]
            self._send(200, play_queue_xml(qid), "application/xml")
        elif path == "/video/:/transcode/universal/decision":
            self._send(200, DECISION_XML, "application/xml")
        elif path == "/video/:/transcode/universal/start.mp4":
            # Printable stand-in for TS bytes: >=64 bytes, no NUL (resolver reads it as text).
            self._send(200, b"G" * 4096, "video/mp2t")
        else:
            self._send(404, NOT_FOUND_HTML, "text/html")


class Server4(ThreadingHTTPServer):
    daemon_threads = True


class Server6(ThreadingHTTPServer):
    daemon_threads = True
    address_family = socket.AF_INET6


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--host6", default="::1")
    ap.add_argument("--port6", type=int, default=0)
    ap.add_argument("--port-file", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--token", default="")
    ap.add_argument("--dump-items", action="store_true", help="print ITEMS as JSON and exit")
    args = ap.parse_args()
    if args.dump_items:
        json.dump(ITEMS, sys.stdout, indent=1, sort_keys=True)
        return 0
    for h in (args.host, args.host6):
        if h and h not in ("127.0.0.1", "::1", "localhost"):
            ap.error("fake PMS binds loopback only")
    log = Log(args.log, args.token)
    servers = []
    s4 = Server4((args.host, args.port), Handler)
    s4.log = log
    servers.append(s4)
    ports = {"port4": s4.server_address[1], "port6": 0}
    if args.host6:
        s6 = Server6((args.host6, args.port6), Handler)
        s6.log = log
        servers.append(s6)
        ports["port6"] = s6.server_address[1]
    for s in servers:
        threading.Thread(target=s.serve_forever, daemon=True).start()
    tmp = args.port_file + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(ports, fh)
    os.replace(tmp, args.port_file)
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
