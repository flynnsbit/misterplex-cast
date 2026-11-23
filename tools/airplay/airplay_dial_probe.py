#!/usr/bin/env python3
# AirPlay + DIAL start probe for a running misterplexd (host, qemu-arm, or device).
# PASS needs: RTSP/1.0 GET /info -> 200 with a binary plist (what iOS/macOS senders send),
# and DIAL GET /apps/YouTube on :8008 -> YouTube service in an idle-valid state (stopped|running).
# Note: an HTTP/1.1 "curl http://host:7000/info" (Host, no CSeq) is routed to UxPlay's HLS
# side and answers 404 by design on every build; it is printed for reference only and must not
# be used as the AirPlay start check. DIAL <state>stopped</state> is the normal idle state
# until a sender POSTs a launch.
import socket, sys, urllib.request, time
H=sys.argv[1] if len(sys.argv)>1 else "127.0.0.1"
def raw(req, port=7000, t=4):
    try:
        s=socket.create_connection((H,port),timeout=t)
    except OSError as e:
        return ("<connect failed: %s>" % e.strerror).encode()
    s.sendall(req); b=b""
    try:
        while True:
            c=s.recv(65536)
            if not c: break
            b+=c
            if b"\r\n\r\n" in b:
                hd,_,body=b.partition(b"\r\n\r\n"); cl=0
                for l in hd.split(b"\r\n"):
                    if l.lower().startswith(b"content-length:"): cl=int(l.split(b":")[1])
                if len(body)>=cl: break
    except socket.timeout: pass
    s.close(); return b
r=raw(b"GET /info RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: AirPlay/690.7.1\r\nX-Apple-ProtocolVersion: 1\r\n\r\n")
st=r.split(b"\r\n",1)[0].decode(errors="replace"); body=r.partition(b"\r\n\r\n")[2]
ok_rtsp = st.startswith("RTSP/1.0 200") and body.startswith(b"bplist00")
print("airplay_rtsp_info:", st, "body=%dB bplist=%s" % (len(body), body.startswith(b"bplist00")))
r2=raw(b"GET /info HTTP/1.1\r\nHost: %s:7000\r\n\r\n" % H.encode(), t=3)
print("airplay_http11_info (harness form):", r2.split(b"\r\n",1)[0].decode(errors="replace") or "<none>")
u="http://%s:8008/apps/YouTube" % H
try:
    x=urllib.request.urlopen(u,timeout=4).read().decode()
except Exception as e:
    x="<dial failed: %s>" % e
st0 = "running" if "<state>running" in x else ("stopped" if "<state>stopped" in x else "?")
print("dial_youtube_idle_state:", st0)
ok_dial = "<name>YouTube</name>" in x and st0 in ("stopped","running")
print("RESULT", "PASS" if (ok_rtsp and ok_dial) else "FAIL")
sys.exit(0 if ok_rtsp and ok_dial else 1)
