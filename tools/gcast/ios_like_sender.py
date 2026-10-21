#!/usr/bin/env python3
"""Scripted AirPlay URL-mode sender shaped like iOS AVPlayer/Safari (header X-Apple-Session-ID,
binary plist with uuid/Content-Location/Start-Position-Seconds). Logs every request/response.
usage: ios_like_sender.py HOST PORT URL [start_s] [proc_name]"""
import plistlib, socket, sys, time, uuid

host, port, url = sys.argv[1], int(sys.argv[2]), sys.argv[3]
start = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
proc = sys.argv[5] if len(sys.argv) > 5 else "MobileSafari"
mode = sys.argv[6] if len(sys.argv) > 6 else "controls"   # controls | wait:<sec> | hold:<sec>
sid = str(uuid.uuid4()).upper()
s = socket.create_connection((host, port), timeout=10)

def log(tag, msg):
    print(time.strftime("%H:%M:%S"), tag, msg, flush=True)

def req(method, path, body=b"", ctype=None):
    h = [f"{method} {path} HTTP/1.1", f"Host: {host}:{port}", "User-Agent: AppleCoreMedia/1.0.0.21A350 (iPhone; U; CPU OS 17_0 like Mac OS X)",
         f"X-Apple-Session-ID: {sid}", f"Content-Length: {len(body)}"]
    if ctype: h.append(f"Content-Type: {ctype}")
    raw = ("\r\n".join(h) + "\r\n\r\n").encode() + body
    log(">>", raw.split(b"\r\n\r\n")[0].decode().replace("\r\n", " | ") + (f" | <bplist {len(body)}B>" if body else ""))
    s.sendall(raw)
    data = b""
    while b"\r\n\r\n" not in data:
        c = s.recv(65536)
        if not c: log("<<", "connection closed"); return None, b""
        data += c
    head, rest = data.split(b"\r\n\r\n", 1)
    cl = 0
    for line in head.split(b"\r\n")[1:]:
        k, _, v = line.partition(b":")
        if k.strip().lower() == b"content-length": cl = int(v)
    while len(rest) < cl: rest += s.recv(65536)
    status = head.split(b"\r\n")[0].decode()
    log("<<", head.decode().replace("\r\n", " | ") + (f" | body {cl}B" if cl else ""))
    return status, rest

st, body = req("GET", "/server-info")
if body:
    try: log("  ", "server-info " + str({k: v for k, v in plistlib.loads(body).items() if k in ("features", "model", "deviceid", "protovers", "srcvers")}))
    except Exception as e: log("  ", f"server-info parse {e}")
pl = {"Content-Location": url, "Start-Position-Seconds": start, "uuid": str(uuid.uuid4()).upper(),
      "clientProcName": proc, "clientBundleID": "com.apple.mobilesafari", "playbackRestrictions": 0,
      "rate": 1.0, "volume": 1.0, "SenderMACAddress": "02:00:00:00:00:01"}
log("  ", f"/play plist {pl}")
req("POST", "/play", plistlib.dumps(pl, fmt=plistlib.FMT_BINARY), "application/x-apple-binary-plist")

def info():
    st, b = req("GET", "/playback-info")
    if b:
        d = plistlib.loads(b)
        log("  ", "playback-info " + str({k: d.get(k) for k in ("duration", "position", "rate", "readyToPlay")}))

if mode.startswith("wait:") or mode.startswith("hold:"):
    limit = time.time() + float(mode.split(":")[1])
    while time.time() < limit:
        time.sleep(1)
        try:
            st, b = req("GET", "/playback-info")
        except OSError as e:
            log("  ", "connection ended %s" % e); sys.exit(0)
        if st is None: sys.exit(0)
        if b:
            d = plistlib.loads(b); log("  ", "playback-info " + str({k: d.get(k) for k in ("duration", "position", "rate")}))
    if mode.startswith("wait:"): sys.exit(2)
    req("POST", "/stop"); sys.exit(0)
for _ in range(3): time.sleep(1); info()
req("POST", "/rate?value=0.000000"); time.sleep(1); info()
req("POST", "/rate?value=1.000000"); time.sleep(1)
req("POST", "/scrub?position=30.000000"); time.sleep(1); info()
req("POST", "/stop")
s.close()
