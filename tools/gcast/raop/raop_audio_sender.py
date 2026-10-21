#!/usr/bin/env python3
"""AirPlay RAOP audio test sender (lab). Streams real encrypted RTP audio the way an iPhone does for
music (ALAC ct=2, AAC-LC ct=4; ct=8 AAC-ELD if given a raw ELD packet file) to a UxPlay-based receiver.

usage: raop_audio_sender.py HOST PORT CODEC SECONDS [--src lavfi|file] [--vol "t:dB,..."] [--pfkey PATH]
  CODEC alac|aac  (eld: --eld-packets FILE with 4-byte BE length prefixed raw AAC-ELD AUs)
Flow: fp-setup x2 (keymsg chosen here) -> SETUP (ekey/eiv, NTP timingPort) -> SETUP stream type 96
 -> RECORD -> RTP to dataPort (AES-128-CBC full blocks, IV reset per packet, key = playfair(keymsg, ekey)
 computed by pfkey, no pair-verify so no ecdh hash) + 1 s sync packets to controlPort + NTP replies
 -> SET_PARAMETER volume at --vol times -> TEARDOWN.
"""
import argparse, os, plistlib, socket, struct, subprocess, sys, threading, time
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

DEVID = "FA:13:17:AA:00:01"
BP = "application/x-apple-binary-plist"
def log(*a): print(time.strftime("%H:%M:%S"), *a, flush=True)

class Rtsp:
    def __init__(self, host, port):
        self.host = host; self.s = socket.create_connection((host, port), timeout=10); self.cseq = 0
    def req(self, method, url, body=b"", ctype=None, extra=None):
        h = [f"{method} {url} RTSP/1.0", f"CSeq: {self.cseq}", "X-Apple-ProtocolVersion: 1",
             f"Content-Length: {len(body)}", "User-Agent: AirPlay/960.13.1",
             "DACP-ID: 14413BE4996FEA4D", "Active-Remote: 2543110914"]
        if ctype: h.append(f"Content-Type: {ctype}")
        for k, v in (extra or {}).items(): h.append(f"{k}: {v}")
        self.cseq += 1
        self.s.sendall(("\r\n".join(h) + "\r\n\r\n").encode() + body)
        data = b""
        while b"\r\n\r\n" not in data:
            c = self.s.recv(65536)
            if not c: raise RuntimeError("receiver closed on " + method)
            data += c
        head, rest = data.split(b"\r\n\r\n", 1)
        cl = 0
        for line in head.split(b"\r\n")[1:]:
            k, _, v = line.partition(b":")
            if k.strip().lower() == b"content-length": cl = int(v)
        while len(rest) < cl:
            rest += self.s.recv(65536)
        st = head.split(b"\r\n")[0].decode(errors="replace")
        log("<<", method, url.split("/")[-1][:20], st, f"{cl}B")
        return st, rest

def ntp_now():
    t = time.time() + 2208988800
    return struct.pack(">II", int(t), int((t % 1) * 2**32))

def ntp_responder(sock, stop):
    sock.settimeout(0.3)
    while not stop.is_set():
        try: req, addr = sock.recvfrom(128)
        except socket.timeout: continue
        if len(req) >= 32 and req[1] & 0x7f == 0x52:
            sock.sendto(b"\x80\xd3\x00\x07\x00\x00\x00\x00" + req[24:32] + ntp_now() + ntp_now(), addr)

def pcm_source(args, secs):
    """44.1k s16le stereo: L 440 Hz, R 660 Hz (amplitude 0.5 FS), or a file."""
    if args.src == "lavfi":
        inp = ["-f", "lavfi", "-i", f"sine=f=440:r=44100:d={secs}", "-f", "lavfi", "-i", f"sine=f=660:r=44100:d={secs}",
               "-filter_complex", "[0:a][1:a]join=inputs=2:channel_layout=stereo,volume=4"]
    else:
        inp = ["-t", str(secs), "-i", args.src]
    return inp

def aac_packets(args, secs):
    cmd = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error"] + pcm_source(args, secs) + \
          ["-ar", "44100", "-ac", "2", "-c:a", "aac", "-b:a", "128k", "-f", "adts", "pipe:1"]
    b = subprocess.run(cmd, capture_output=True, check=True).stdout
    out, i = [], 0
    while i + 7 <= len(b):
        if b[i] != 0xFF or (b[i + 1] & 0xF0) != 0xF0: raise RuntimeError("adts sync lost")
        hl = 7 if b[i + 1] & 1 else 9
        fl = ((b[i + 3] & 3) << 11) | (b[i + 4] << 3) | (b[i + 5] >> 5)
        out.append(b[i + hl:i + fl]); i += fl
    return out, 1024

def alac_packets(args, secs):
    """ALAC escape (uncompressed) CPE frames, 352 samples: valid ALAC that any decoder must accept."""
    cmd = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error"] + pcm_source(args, secs) + \
          ["-ar", "44100", "-ac", "2", "-f", "s16be", "pipe:1"]
    pcm = subprocess.run(cmd, capture_output=True, check=True).stdout
    out, fb = [], 352 * 4
    for o in range(0, len(pcm) - fb + 1, fb):
        # tag CPE(3b)=1 inst(4b)=0 unused(12b)=0 has_size(1b)=0 shift(2b)=0 not_compressed(1b)=1 -> 23 bits
        frame = bytearray(); acc, nb = (1 << 20) | 1, 23
        for k in range(o, o + fb, 2):
            acc = (acc << 16) | (pcm[k] << 8) | pcm[k + 1]; nb += 16
            while nb >= 8: nb -= 8; frame.append((acc >> nb) & 0xFF); acc &= (1 << nb) - 1
        acc = (acc << 3) | 7; nb += 3                 # ID_END
        while nb >= 8: nb -= 8; frame.append((acc >> nb) & 0xFF); acc &= (1 << nb) - 1
        if nb: frame.append((acc << (8 - nb)) & 0xFF)
        out.append(bytes(frame))
    return out, 352

def eld_packets(path):
    b, out, i = open(path, "rb").read(), [], 0
    while i + 4 <= len(b):
        n = struct.unpack(">I", b[i:i + 4])[0]; out.append(b[i + 4:i + 4 + n]); i += 4 + n
    return out, 480

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host"); ap.add_argument("port", type=int); ap.add_argument("codec", choices=["alac", "aac", "eld"])
    ap.add_argument("seconds", type=float); ap.add_argument("--src", default="lavfi")
    ap.add_argument("--vol", default=""); ap.add_argument("--eld-packets", default="")
    ap.add_argument("--pfkey", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "pfkey"))
    a = ap.parse_args()
    ct = {"alac": 2, "aac": 4, "eld": 8}[a.codec]
    pkts, spf = (alac_packets(a, a.seconds) if ct == 2 else aac_packets(a, a.seconds) if ct == 4
                 else eld_packets(a.eld_packets))
    log(f"codec {a.codec} ct={ct} packets={len(pkts)} spf={spf} ({len(pkts) * spf / 44100:.1f} s)")
    keymsg = b"FPLY\x03\x01\x03\x00\x00\x00\x00\x98\x02" + os.urandom(151)   # [12] = mode 2 (0..3)
    ekey = b"FPLY\x01\x02\x01\x00\x00\x00\x00\x3c\x00\x00\x00\x00" + os.urandom(56)
    eiv = os.urandom(16)
    key = bytes.fromhex(subprocess.run([a.pfkey, keymsg.hex(), ekey.hex()], capture_output=True, check=True,
                                       text=True).stdout.strip())
    ntp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); ntp.bind(("0.0.0.0", 0))
    ctl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); ctl.bind(("0.0.0.0", 0))
    stop = threading.Event(); threading.Thread(target=ntp_responder, args=(ntp, stop), daemon=True).start()
    c = Rtsp(a.host, a.port); url = "rtsp://%s/%s" % (a.host, "1234567890")
    c.req("POST", "/fp-setup", b"FPLY\x03\x01\x01\x00\x00\x00\x00\x04\x02\x00\x02\xbb", "application/octet-stream")
    c.req("POST", "/fp-setup", keymsg, "application/octet-stream")
    s1 = {"deviceID": DEVID, "name": "raop-test", "model": "iPhone16,1", "sourceVersion": "960.13.1",
          "ekey": ekey, "eiv": eiv, "timingProtocol": "NTP", "timingPort": ntp.getsockname()[1],
          "isScreenMirroringSession": False, "sessionUUID": "8E3F0A6B-0000-4000-8000-00000000A0D1"}
    c.req("SETUP", url, plistlib.dumps(s1, fmt=plistlib.FMT_BINARY), BP)
    fmt = {2: 0x40000, 4: 0x400000, 8: 0x1000000}[ct]
    s2 = {"streams": [{"type": 96, "ct": ct, "spf": spf, "audioFormat": fmt, "controlPort": ctl.getsockname()[1],
                       "isMedia": True, "usingScreen": False, "latencyMin": 11025, "latencyMax": 88200,
                       "streamConnectionID": 4242}]}
    st, body = c.req("SETUP", url, plistlib.dumps(s2, fmt=plistlib.FMT_BINARY), BP)
    r = plistlib.loads(body)["streams"][0]
    dport, cport = r["dataPort"], r.get("controlPort", 0)
    log(f"dataPort {dport} controlPort {cport}")
    c.req("RECORD", url, extra={"RTP-Info": "seq=0;rtptime=0", "Range": "npt=0-"})
    c.req("SET_PARAMETER", url, b"volume: 0.000000\r\n", "text/parameters")
    stream(a, c, url, pkts, spf, key, eiv, ctl, dport, cport)
    c.req("TEARDOWN", url, plistlib.dumps({"streams": [{"type": 96}]}, fmt=plistlib.FMT_BINARY), BP)
    c.req("TEARDOWN", url)
    stop.set(); c.s.close()

def stream(a, c, url, pkts, spf, key, eiv, ctl, dport, cport):
    vols = sorted((float(t), float(d)) for t, d in (x.split(":") for x in a.vol.split(",") if x))
    data = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ssrc, seq, rtp, t0 = 0x1A2B3C4D, 1, 0, time.time()
    for i, p in enumerate(pkts):
        due = t0 + rtp / 44100.0
        while time.time() < due: time.sleep(min(0.005, max(0, due - time.time())))
        if i % 125 == 0 and cport:   # sync: rtp now <-> ntp now, next rtp
            ctl.sendto(struct.pack(">BBH", 0x90 if i == 0 else 0x80, 0xd4, 4) + struct.pack(">I", rtp) + ntp_now() +
                       struct.pack(">I", rtp + spf), (a.host, cport))
        n = len(p) // 16 * 16
        enc = Cipher(algorithms.AES(key), modes.CBC(eiv)).encryptor().update(p[:n]) if n else b""
        data.sendto(struct.pack(">BBHII", 0x80, 0x60 | (0x80 if i == 0 else 0), seq & 0xFFFF, rtp, ssrc) + enc + p[n:],
                    (a.host, dport))
        seq += 1; rtp += spf
        el = time.time() - t0
        while vols and el >= vols[0][0]:
            db = vols.pop(0)[1]; log(f"t={el:.1f}s volume {db} dB")
            c.req("SET_PARAMETER", url, b"volume: %f\r\n" % db, "text/parameters")
    log(f"sent {seq - 1} packets in {time.time() - t0:.1f} s")

if __name__ == "__main__":
    main()
