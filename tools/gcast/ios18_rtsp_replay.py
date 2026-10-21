#!/usr/bin/env python3
# iOS 18 shaped AirPlay RTSP replay. Replays what an iPhone (AirPlay/960.13.1) sends.
# usage: ios18_rtsp_replay.py HOST [PORT] [SCENARIO]
# SCENARIO: full|info|pair|fp|setup1|mirror|audio|ptp|rst|badplist|all
# Exit 0 = receiver still answers GET /info after; 3 = gone.
import os, plistlib, socket, struct, sys, time
try:
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    HAVE_CRYPTO = True
except ImportError:
    HAVE_CRYPTO = False

HOST = sys.argv[1]
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 7000
SCEN = sys.argv[3] if len(sys.argv) > 3 else "full"
UA = "AirPlay/960.13.1"
DEVID = "00:11:22:33:44:55"

def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)

class Rtsp:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=8)
        self.cseq = 0
    def req(self, method, url, body=b"", ctype=None, extra=None, expect=True):
        h = [f"{method} {url} RTSP/1.0", f"CSeq: {self.cseq}",
             "X-Apple-ProtocolVersion: 1", f"Content-Length: {len(body)}",
             f"User-Agent: {UA}", "DACP-ID: 14413BE4996FEA4D",
             "Active-Remote: 2543110914"]
        if ctype:
            h.append(f"Content-Type: {ctype}")
        for k, v in (extra or {}).items():
            h.append(f"{k}: {v}")
        self.cseq += 1
        self.s.sendall(("\r\n".join(h) + "\r\n\r\n").encode() + body)
        log(">>", method, url, f"{len(body)}B")
        if not expect:
            return None, b""
        data = b""
        while b"\r\n\r\n" not in data:
            c = self.s.recv(65536)
            if not c:
                log("<<", "connection closed by receiver")
                return None, b""
            data += c
        head, rest = data.split(b"\r\n\r\n", 1)
        cl = 0
        for line in head.split(b"\r\n")[1:]:
            k, _, v = line.partition(b":")
            if k.strip().lower() == b"content-length":
                cl = int(v)
        while len(rest) < cl:
            c = self.s.recv(65536)
            if not c:
                break
            rest += c
        st = head.split(b"\r\n")[0].decode(errors="replace")
        log("<<", st, f"body {cl}B")
        return st, rest
    def rst(self):
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        self.s.close()
    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

BP = "application/x-apple-binary-plist"
def bpl(d):
    return plistlib.dumps(d, fmt=plistlib.FMT_BINARY)

def info(c, qualifier=True):
    if qualifier:
        return c.req("GET", "/info", bpl({"qualifier": ["txtAirPlay"]}), BP)
    return c.req("GET", "/info")

def pair(c):
    if not HAVE_CRYPTO:
        c.req("POST", "/pair-setup", os.urandom(32), "application/octet-stream")
        c.req("POST", "/pair-verify", b"\x01\0\0\0" + os.urandom(64), "application/octet-stream")
        return
    ek = Ed25519PrivateKey.generate()
    raw = lambda k: k.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    c.req("POST", "/pair-setup", raw(ek.public_key()), "application/octet-stream")
    xk = X25519PrivateKey.generate()
    st, body = c.req("POST", "/pair-verify",
                     b"\x01\0\0\0" + raw(xk.public_key()) + raw(ek.public_key()),
                     "application/octet-stream")
    if not body or len(body) < 96:
        log("  pair-verify 1 short reply"); return
    secret = xk.exchange(X25519PublicKey.from_public_bytes(body[:32]))
    def derive(salt):
        h = hashes.Hash(hashes.SHA512()); h.update(salt); h.update(secret); return h.finalize()[:16]
    key, iv = derive(b"Pair-Verify-AES-Key"), derive(b"Pair-Verify-AES-IV")
    enc = Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()
    # Fake round then encrypt zeros so finish() fails cleanly (no exit).
    fake = enc.update(b"\0" * 64)
    c.req("POST", "/pair-verify", b"\0\0\0\0" + fake, "application/octet-stream")

def fp(c):
    chal = bytes([0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x01, 0x00] + [0] * 8)
    c.req("POST", "/fp-setup", chal, "application/octet-stream")
    c.req("POST", "/fp-setup", b"\x46\x50\x4c\x59" + os.urandom(160), "application/octet-stream")

def setup1(c, timing="NTP", mirror_session=False, remote_ctrl=False):
    d = {"deviceID": DEVID, "name": "Owner", "model": "iPhone16,1",
         "osBuildVersion": "22A3354", "sourceVersion": "960.13.1",
         "eiv": os.urandom(16), "ekey": os.urandom(72),
         "timingProtocol": timing, "timingPort": 5353,
         "isScreenMirroringSession": mirror_session}
    if remote_ctrl:
        d["isRemoteControlOnly"] = True
    return c.req("SETUP", "rtsp://%s/%s" % (HOST, DEVID.replace(":","")), bpl(d), BP)

def setup_stream(c, typ):
    d = {"streams": [{"type": typ, "streamConnectionID": 1,
                      "controlPort": 6001, "ct": 1, "spf": 352,
                      "audioFormat": 0x400000, "latencyMin": 11025, "latencyMax": 44100}]}
    return c.req("SETUP", "rtsp://%s/%s" % (HOST, DEVID.replace(":","")), bpl(d), BP)

def alive():
    try:
        c = Rtsp(); st, _ = info(c, False); c.close(); return st is not None
    except OSError as e:
        log("alive? no:", e); return False

def run(scen):
    log("SCENARIO", scen)
    c = Rtsp()
    try:
        if scen in ("info", "full", "all"):
            info(c, True); info(c, False)
        if scen in ("pair", "full", "all"):
            pair(c)
        if scen in ("fp", "full", "all"):
            fp(c)
        if scen in ("setup1", "full", "all", "mirror", "audio", "ptp"):
            tp = "PTP" if scen == "ptp" else "NTP"
            setup1(c, timing=tp, mirror_session=(scen == "mirror"))
        if scen in ("mirror", "full", "all"):
            setup_stream(c, 110)
        if scen in ("audio", "full", "all"):
            setup_stream(c, 96)
        if scen in ("full", "all", "audio", "mirror"):
            c.req("GET_PARAMETER", "rtsp://%s/volume" % HOST, b"volume\r\n", "text/parameters")
            c.req("RECORD", "rtsp://%s/%s" % (HOST, DEVID.replace(":","")))
            c.req("SET_PARAMETER", "rtsp://%s/volume" % HOST, b"volume: -20.000000\r\n", "text/parameters")
            c.req("POST", "/feedback")
        if scen == "badplist":
            c.req("SETUP", "rtsp://%s/x" % HOST, b"not-a-plist", BP)
        if scen == "rst":
            info(c, False)
            c.req("POST", "/feedback", expect=False)
            c.rst(); return
        if scen in ("full", "all", "mirror", "audio"):
            c.req("TEARDOWN", "rtsp://%s/%s" % (HOST, DEVID.replace(":","")))
    except OSError as e:
        log("IO", e)
    finally:
        try: c.close()
        except Exception: pass

if SCEN == "all":
    for s in ("info", "pair", "fp", "setup1", "ptp", "mirror", "audio", "badplist", "rst", "full"):
        run(s); time.sleep(0.2)
else:
    run(SCEN)
time.sleep(0.3)
ok = alive()
log("ALIVE" if ok else "DEAD")
sys.exit(0 if ok else 3)
