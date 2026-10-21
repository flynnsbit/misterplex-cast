#!/usr/bin/env python3
"""Fake mpx-menu helper and fake PMS for the browse P1 tests (loopback only).

menu mode (default): Plex-shaped MediaContainer XML in the live mpx-menu shapes
(rel-mpxmenu-contract-1004.md §8-9) plus P0's resolve row:
  /menu                          roots: CONTINUE WATCHING, MOVIES, SERVERS >
  /menu/servers                  MENU1 OWNED LOCAL, SHARED A REMOTE, SHARED B OFFLINE (mpxKind)
  /menu/hub/local/continue       one local item
  /menu/section/local/1/all?page=P   45 items, 20 a page, PREV / NEXT rows (type and mpxKind
                                 page), MediaContainer@mpxHeader "MOVIES 21-40/45"
  /menu/item/local/<rk>          info rows (mpxKind=info), then action rows RESUME FROM mm:ss
                                 (mpxOffsetMs = RESUME[rk]) and PLAY FROM START (0), or PLAY,
                                 keyed /menu/ticket/local/<rk>/o<ms>
  /menu/s/sremote1               a remote list: a ticket row, then P0's resolve row
                                 (mpxSource=ladder)
  /menu/ticket/<srv>/<rk>/o<ms>  an MpxPlay ticket: local = the helper's own 127.0.0.1 view,
                                 mpxLadderInput=part (rk 2001: mpxPlayable=no); remote = the
                                 --remote-base server, mpxLadderInput=server-transcode
  /menu/resolve/sremote1/<rk>    mpxLadderSrc = an owner-transcode URL on --remote-base (token
                                 --remote-token), mpxOffsetMs from the query
  /status/<code>, /slow/<ms>     for the curl tests
pms mode (--pms): tests/fixtures/fake_pms/fake_pms.py's handler (metadata, decision, start)
plus /library/sections and /library/sections/1/all, Video@viewOffset from --view-offset
RK=MS (a PMS resume point the browse rows must override), and --start-needs-headers-file
(start.mp4 answers 400 to a request without X-Plex headers while that file exists).

Every request is logged (--log), tokens redacted. --port-file gets the port (menu) or
"port4 port6" (pms).
"""
import argparse
import importlib.util
import os
import re
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote, urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
FAKE_PMS = os.path.join(HERE, "..", "fixtures", "fake_pms", "fake_pms.py")

# rk -> (title, menu resume offset ms). 103 and 101 resolve on the fake PMS.
ITEMS = [("103", "BBB THIRTY", 61000), ("101", "TNG TWENTY FOUR", 0)] + [
    (str(2000 + i), "MOVIE %d" % (i + 3), 0) for i in range(43)
]
RESUME = {rk: off for rk, _, off in ITEMS}


def mmss(ms):
    s = ms // 1000
    return "%02d:%02d" % (s // 60, s % 60)


def container(rows, header=""):
    h = ' mpxHeader="%s"' % header if header else ""
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<MediaContainer size="%d"%s>\n%s'
            "</MediaContainer>\n" % (len(rows), h, "".join(r + "\n" for r in rows)))


def menu_answer(path, q, args, port=0):
    if path == "/menu":
        return container([
            '<Directory title="CONTINUE WATCHING" key="/menu/hub/local/continue" type="mpxhub"/>',
            '<Directory title="MOVIES" key="/menu/section/local/1/all?page=0" type="mpxsection"/>',
            '<Directory title="SERVERS &gt;" key="/menu/servers" type="mpxservers"/>',
        ], "PLEX")
    if path == "/menu/servers":
        return container([
            '<Directory title="MENU1  OWNED LOCAL" key="/menu" mpxServer="local" mpxKind="server"/>',
            '<Directory title="SHARED A  SHARED REMOTE" key="/menu/s/sremote1" '
            'mpxServer="sremote1" mpxKind="server"/>',
            '<Directory title="SHARED B  SHARED OFFLINE" mpxServer="soffln1" mpxKind="offline"/>',
        ], "SERVERS")
    if path == "/menu/hub/local/continue":
        return container(['<Directory title="BBB THIRTY  37PCT" key="/menu/item/local/103" '
                          'type="mpxitem"/>'], "CONTINUE WATCHING")
    if path == "/menu/section/local/1/all":
        page = int(q.get("page", ["0"])[0])
        lo, hi = page * 20, min(len(ITEMS), page * 20 + 20)
        rows = []
        if page > 0:
            rows.append('<Directory title="PREV" key="/menu/section/local/1/all?page=%d" '
                        'type="page" mpxKind="page"/>' % (page - 1))
        for rk, title, _ in ITEMS[lo:hi]:
            rows.append('<Directory title="%s" key="/menu/item/local/%s" type="mpxitem"/>'
                        % (title, rk))
        if hi < len(ITEMS):
            rows.append('<Directory title="NEXT" key="/menu/section/local/1/all?page=%d" '
                        'type="page" mpxKind="page"/>' % (page + 1))
        return container(rows, "MOVIES %d-%d/%d" % (lo + 1, hi, len(ITEMS)))
    m = re.fullmatch(r"/menu/item/local/(\w+)", path)
    if m:
        rk = m.group(1)
        title = next((t for k, t, _ in ITEMS if k == rk), "ITEM " + rk)
        off = RESUME.get(rk, 0)
        rows = ['<Directory title="%s" mpxKind="info" type="mpxinfo"/>' % title,
                '<Directory title="2008  10M" mpxKind="info" type="mpxinfo"/>']
        if off > 0:
            rows.append('<Video title="RESUME FROM %s" type="movie" ratingKey="%s" '
                        'mpxServer="local" mpxKind="action" mpxOffsetMs="%d" '
                        'key="/menu/ticket/local/%s/o%d"/>' % (mmss(off), rk, off, rk, off))
            rows.append('<Video title="PLAY FROM START" type="movie" ratingKey="%s" '
                        'mpxServer="local" mpxKind="action" mpxOffsetMs="0" '
                        'key="/menu/ticket/local/%s/o0"/>' % (rk, rk))
        else:
            rows.append('<Video title="PLAY" type="movie" ratingKey="%s" mpxServer="local" '
                        'mpxKind="action" mpxOffsetMs="0" key="/menu/ticket/local/%s/o0"/>'
                        % (rk, rk))
        return container(rows, title)
    if path == "/menu/s/sremote1":
        return container([
            '<Video title="S01E03 THE NAKED NOW  RESUME 01:35" type="episode" ratingKey="101" '
            'mpxServer="sremote1" mpxKind="action" mpxOffsetMs="95000" '
            'key="/menu/ticket/sremote1/101/o95000"/>',
            '<Video title="S01E04 CODE OF HONOR  RESUME 01:35" type="episode" ratingKey="101" '
            'mpxServer="sremote1" mpxSource="ladder" mpxOffsetMs="95000" '
            'key="/menu/resolve/sremote1/101?offsetMs=95000"/>',
        ], "SHARED A")
    m = re.fullmatch(r"/menu/ticket/(\w+)/(\d+)/o(\d+)", path)
    if m:
        server, rk, off = m.group(1), m.group(2), int(m.group(3))
        if server == "local":  # the helper's own view of its PMS: lab-pc's 127.0.0.1
            attrs = ('address="127.0.0.1" port="32400" protocol="http" token="HELPERLOCALTOKEN" '
                     'local="1" mpxLadderInput="part" mpxPartKbps="2619"')
            playable = "no" if rk == "2001" else "yes"
        else:  # a friend's server: 25 Mbps original, so Q4 asks for its own transcode
            host, port_ = args.remote_base.split("//")[1].rsplit(":", 1)
            attrs = ('address="%s" port="%s" protocol="http" token="%s" local="0" '
                     'mpxLadderInput="server-transcode" mpxPartKbps="25148"'
                     % (host, port_, args.remote_token))
            # mpx-menu 59044aa: the relay on the helper (the ladder on lab-pc fetches it at
            # 127.0.0.1), its start in its path.
            if args.tsrc_file and os.path.exists(args.tsrc_file):
                attrs += ' mpxLadderSrc="%s"' % quote(
                    "http://127.0.0.1:%d/menu/tsrc/%s/%s/o%d" % (port, server, rk, off), safe="")
            playable = "yes"
        reason = ' mpxReason="NOT PLAYABLE HERE"' if playable == "no" else ""
        return container(['<MpxPlay title="TICKET" ratingKey="%s" key="/library/metadata/%s" '
                          'machineIdentifier="mid%s" %s mpxOffsetMs="%d" mpxPlayable="%s"%s/>'
                          % (rk, rk, server, attrs, off, playable, reason)])
    m = re.fullmatch(r"/menu/resolve/sremote1/(\d+)", path)
    if m:
        rk = m.group(1)
        off = int(q.get("offsetMs", ["0"])[0])
        # lead Q4: above ~8 Mbps the helper hands over the owner's own transcode, not the
        # original Part; the daemon must use exactly this as the ladder input.
        src = "%s/video/:/transcode/universal/start.ts?path=%%2Flibrary%%2Fmetadata%%2F%s" \
              "&mpxq4=owner&X-Plex-Token=%s" % (args.remote_base, rk, args.remote_token)
        return container(['<Video title="S01E03 THE NAKED NOW" type="episode" '
                          'mpxLadderSrc="%s" mpxW="640" mpxH="480" mpxBr="2000" mpxFps="src" '
                          'mpxOffsetMs="%d" mpxDurationMs="120000"/>' % (quote(src, safe=""), off)])
    return None


class Log:
    def __init__(self, path):
        self.lock = threading.Lock()
        self.fh = open(path, "a", encoding="utf-8") if path else None

    def write(self, line):
        if not self.fh:
            return
        line = re.sub(r"(X-Plex-Token(=|%3D|%3d))[^&\s%]*", r"\1REDACTED", line)
        with self.lock:
            self.fh.write(line + "\n")
            self.fh.flush()


class MenuHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body, ctype="application/xml"):
        data = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        u = urlsplit(self.path)
        self.server.log.write("GET " + self.path)
        m = re.fullmatch(r"/status/(\d+)", u.path)
        if m:
            self._send(int(m.group(1)), "status\n", "text/plain")
            return
        m = re.fullmatch(r"/slow/(\d+)", u.path)
        if m:
            time.sleep(int(m.group(1)) / 1000.0)
            self._send(200, "<MediaContainer/>\n")
            return
        delay = self.server.args.delay_ms
        if delay:
            time.sleep(delay / 1000.0)
        body = menu_answer(u.path, parse_qs(u.query), self.server.args,
                           self.server.server_address[1])
        if body is None:
            self._send(404, "not found\n", "text/plain")
        else:
            self._send(200, body)


class Server4(ThreadingHTTPServer):
    daemon_threads = True


class Server6(ThreadingHTTPServer):
    daemon_threads = True
    address_family = socket.AF_INET6


def write_port_file(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write(text + "\n")
    os.replace(tmp, path)


def run_menu(args):
    s = Server4(("127.0.0.1", args.port), MenuHandler)
    s.log = Log(args.log)
    s.args = args
    write_port_file(args.port_file, str(s.server_address[1]))
    s.serve_forever()


def run_pms(args):
    spec = importlib.util.spec_from_file_location("fake_pms", FAKE_PMS)
    fp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fp)
    offsets = {}
    for kv in args.view_offset or []:
        rk, ms = kv.split("=")
        offsets[rk] = int(ms)

    class PmsHandler(fp.Handler):
        def do_GET(self):
            path = urlsplit(self.path).path
            if path == "/library/sections":
                self._log_request()
                self._send(200, '<MediaContainer size="2" title1="Plex Library">'
                                '<Directory key="1" type="movie" title="Movies &amp; Docs"/>'
                                '<Directory key="2" type="show" title="TV Shows"/>'
                                "</MediaContainer>\n", "application/xml")
                return
            if path == "/library/sections/1/all":
                self._log_request()
                rows = "".join('<Video ratingKey="%s" key="/library/metadata/%s" type="movie" '
                               'title="%s" viewOffset="%d"/>' % (rk, rk, fp.ITEMS[rk]["title"],
                                                                 offsets.get(rk, 0))
                               for rk in ("103", "101"))
                self._send(200, '<MediaContainer size="2" title1="Movies">%s</MediaContainer>\n'
                           % rows, "application/xml")
                return
            m = re.fullmatch(r"/library/metadata/(\d+)", path)
            if m and m.group(1) in offsets and m.group(1) in fp.ITEMS:
                self._log_request()
                body = fp.metadata_xml(m.group(1), fp.ITEMS[m.group(1)]).replace(
                    'viewOffset="0"', 'viewOffset="%d"' % offsets[m.group(1)], 1)
                self._send(200, body, "application/xml")
                return
            # wT's 13:06 finding, as a switch: a transcode start that needs the client
            # profile as headers (what the ladder's ffmpeg cannot send).
            if (path == "/video/:/transcode/universal/start.mp4" and args.start_needs_headers_file
                    and os.path.exists(args.start_needs_headers_file)
                    and "X-Plex-Client-Profile-Name" not in self.headers):
                self._log_request()
                self._send(400, "<html><body>Bad Request</body></html>\n", "text/html")
                return
            super().do_GET()

    log = fp.Log(args.log, args.token)
    s4 = fp.Server4(("127.0.0.1", 0), PmsHandler)
    s6 = fp.Server6(("::1", 0), PmsHandler)
    servers = [s4, s6]
    ports = [s4.server_address[1], s6.server_address[1]]
    if args.remote_host:  # a "remote" server address (the test adds it to its own netns lo)
        sr = fp.Server4((args.remote_host, 0), PmsHandler)
        servers.append(sr)
        ports.append(sr.server_address[1])
    for s in servers:
        s.log = log
        threading.Thread(target=s.serve_forever, daemon=True).start()
    write_port_file(args.port_file, " ".join(str(p) for p in ports))
    while True:
        time.sleep(3600)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port-file", required=True)
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--log", default="")
    ap.add_argument("--menu-only", action="store_true", help="menu mode (default)")
    ap.add_argument("--pms", action="store_true")
    ap.add_argument("--token", default="")
    ap.add_argument("--view-offset", action="append")
    ap.add_argument("--remote-base", default="http://[::1]:9")
    ap.add_argument("--remote-token", default="REMOTETOKEN")
    ap.add_argument("--remote-host", default="", help="pms: one more IPv4 listener")
    ap.add_argument("--start-needs-headers-file", default="",
                    help="pms: while this file exists, start.mp4 needs X-Plex headers")
    ap.add_argument("--tsrc-file", default="",
                    help="menu: while this file exists, remote tickets carry the relay mpxLadderSrc")
    ap.add_argument("--delay-ms", type=int, default=0)
    args = ap.parse_args()
    if args.pms:
        run_pms(args)
    else:
        run_menu(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
