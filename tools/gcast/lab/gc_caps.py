#!/usr/bin/env python3
# GCAST P6 sender-cap probes against a receiver: slowloris DIAL, DIAL launch/pairing flood, TCP connect flood.
# usage: gc_caps.py HOST [DIAL_PORT] [AIRPLAY_PORT]
import socket, sys, time, threading
H = sys.argv[1]; DP = int(sys.argv[2]) if len(sys.argv) > 2 else 8008; AP = int(sys.argv[3]) if len(sys.argv) > 3 else 7000
def get_dd(timeout=10):
    t0 = time.time()
    try:
        s = socket.create_connection((H, DP), timeout=timeout)
        s.sendall(b"GET /dd.xml HTTP/1.1\r\nHost: x\r\n\r\n"); d = b""
        while True:
            b = s.recv(4096)
            if not b: break
            d += b
        s.close(); return time.time() - t0, d.split(b"\r\n", 1)[0].decode(errors="replace")
    except Exception as e:
        return time.time() - t0, "ERR " + repr(e)
print("baseline dd.xml: %.2fs %s" % get_dd())
# 1 slowloris: one byte per second; the server must drop it near 3 s, and a normal GET must not wait behind it for long
s = socket.create_connection((H, DP), timeout=30); t0 = time.time(); closed_at = None
res = {}
def normal():
    time.sleep(0.5); res["dd"] = get_dd()
th = threading.Thread(target=normal); th.start()
for ch in b"GET /dd.xml HTTP/1.1\r\nHost: slow\r\n":
    try:
        s.send(bytes([ch])); time.sleep(1.0)
        s.settimeout(0.01)
        try:
            if s.recv(1) == b"": closed_at = time.time() - t0; break
        except socket.timeout: pass
        s.settimeout(30)
    except (BrokenPipeError, ConnectionResetError):
        closed_at = time.time() - t0; break
th.join(); s.close()
print("slowloris: server closed at %s s (cap 3 s)" % (("%.1f" % closed_at) if closed_at else "NEVER (sent all)"))
print("normal GET during slowloris: %.2fs %s" % res["dd"])
# 2 DIAL launch + pairing flood: 40 POSTs with distinct pairing codes, no video id
t0 = time.time(); ok = 0
for i in range(40):
    body = ("pairingCode=flood-%04d&theme=cl" % i).encode()
    try:
        c = socket.create_connection((H, DP), timeout=5)
        c.sendall(b"POST /apps/YouTube HTTP/1.1\r\nHost: x\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
        if c.recv(64).startswith(b"HTTP/1.1 201"): ok += 1
        c.close()
    except Exception: pass
print("pairing flood: %d/40 accepted (201) in %.1fs" % (ok, time.time() - t0))
# 3 TCP connect flood on DIAL and AirPlay: 100 connect/close each
for port in (DP, AP):
    n = 0
    for i in range(100):
        try: socket.create_connection((H, port), timeout=3).close(); n += 1
        except Exception: pass
    print("connect flood :%d %d/100" % (port, n))
print("after floods dd.xml: %.2fs %s" % get_dd())
