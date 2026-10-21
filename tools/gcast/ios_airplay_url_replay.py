#!/usr/bin/env python3
"""FIELD-1018 F2: replay what iOS 18 (AirPlay/960.13.1) does for an AirPlay URL session (Photos/Safari/Plex HLS).
Sequence from a lab MiSTer log and the AirPlay video protocol: GET /server-info; POST /reverse (Upgrade: PTTH/1.0,
X-Apple-Purpose: event) -> 101; the receiver talks on the reverse socket (POST /event, FCUP) and the iPhone ANSWERS
on it with "HTTP/1.1 200 OK"; POST /play with a binary plist (Content-Location + Start-Position); the /rate,
/playback-info, /scrub polling; POST /stop.
usage: [HOLD_S=n] ios_airplay_url_replay.py HOST PORT [URL]   (HOLD_S: keep playing n s, polling, before /stop)   exit 0 = reverse socket and session survived."""
import os, plistlib, socket, sys, time, uuid
host, port = sys.argv[1], int(sys.argv[2])
url = sys.argv[3] if len(sys.argv) > 3 else "http://127.0.0.1:9/test.mp4"
sid = str(uuid.uuid4()).upper()
UA = "AirPlay/960.13.1"
fails = []
def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok: fails.append(what)

def read_msg(sock, timeout=3.0):
    sock.settimeout(timeout); buf = b""
    try:
        while b"\r\n\r\n" not in buf:
            d = sock.recv(4096)
            if not d: return None
            buf += d
        head, rest = buf.split(b"\r\n\r\n", 1)
        n = 0
        for line in head.split(b"\r\n")[1:]:
            k, _, v = line.partition(b":")
            if k.strip().lower() == b"content-length": n = int(v)
        while len(rest) < n:
            d = sock.recv(4096)
            if not d: break
            rest += d
        return head.split(b"\r\n")[0].decode(errors="replace"), head, rest
    except socket.timeout:
        return ("TIMEOUT", b"", b"")
    except OSError:
        return None

def req(sock, method, path, body=b"", ctype=None, extra=""):
    h = "%s %s HTTP/1.1\r\nUser-Agent: %s\r\nX-Apple-Session-ID: %s\r\nX-Apple-Device-ID: 0xfa1317fb63d7\r\n" % (method, path, UA, sid)
    if ctype: h += "Content-Type: %s\r\n" % ctype
    h += extra + "Content-Length: %d\r\n\r\n" % len(body)
    try: sock.sendall(h.encode() + body)
    except OSError: return None
    return read_msg(sock)

def alive(sock, wait=1.0):
    sock.settimeout(wait)
    try:
        return sock.recv(4096, socket.MSG_PEEK) != b""   # an event waiting is alive too
    except socket.timeout:
        return True
    except OSError:
        return False

def iphone_answer(sock):   # the iPhone's answer to a receiver POST /event: the exact bytes a lab MiSTer choked on
    try: sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")
    except OSError: pass
    time.sleep(0.4)

ok200 = lambda r: r is not None and r[0].startswith("HTTP/1.1 200")
main = socket.create_connection((host, port), 5)
r = req(main, "GET", "/server-info")
check(ok200(r), "GET /server-info 200 (%s)" % (r and r[0]))
rev = socket.create_connection((host, port), 5)
r = req(rev, "POST", "/reverse", extra="Upgrade: PTTH/1.0\r\nConnection: Upgrade\r\nX-Apple-Purpose: event\r\n")
check(r is not None and " 101" in r[0], "POST /reverse -> 101 Switching Protocols (%s)" % (r and r[0]))
for i in range(2):
    iphone_answer(rev)
    check(alive(rev), "reverse socket alive after iPhone response %d" % (i + 1))
body = plistlib.dumps({"Content-Location": url, "Start-Position": 0.0, "Start-Position-Seconds": 0.0,
                       "uuid": str(uuid.uuid4()).upper(), "clientProcName": "MobileSafari",
                       "playbackRestrictions": 0, "rate": 1.0}, fmt=plistlib.FMT_BINARY)
r = req(main, "POST", "/play", body, "application/x-apple-binary-plist")
check(ok200(r), "POST /play bplist 200 (%s)" % (r and r[0]))
for path, m in (("/rate?value=1.000000", "POST"), ("/playback-info", "GET"), ("/scrub", "GET"),
                ("/playback-info", "GET"), ("/scrub?position=5.000000", "POST")):
    r = req(main, m, path)
    check(ok200(r), "%s %s 200 (%s)" % (m, path, r and r[0]))
    time.sleep(0.2)
end = time.time() + float(os.environ.get("HOLD_S", "0"))
while time.time() < end:   # iOS keeps polling /playback-info about once a second while it plays
    r = req(main, "GET", "/playback-info")
    if not ok200(r): check(False, "GET /playback-info while holding (%s)" % (r and r[0])); break
    time.sleep(1.0)
iphone_answer(rev)
check(alive(rev), "reverse socket alive after polling")
r = req(main, "POST", "/stop")
check(ok200(r), "POST /stop 200 (%s)" % (r and r[0]))
print("ios_airplay_url_replay: %s" % ("OK" if not fails else "FAIL %d" % len(fails)))
sys.exit(1 if fails else 0)
