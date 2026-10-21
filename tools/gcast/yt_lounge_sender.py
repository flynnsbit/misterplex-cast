#!/usr/bin/env python3
"""Simulated YouTube-app sender for the misterplexd YouTube (DIAL + Lounge) receiver.

Does what the phone app does: SSDP M-SEARCH for DIAL -> GET LOCATION (Application-URL)
-> GET <app-url>/YouTube (screenId) -> lounge token -> bind as REMOTE_CONTROL ->
setPlaylist / pause / play / seekTo / stopVideo through YouTube's Lounge servers.
Uses pyytlounge (pip, in a venv) for the remote side of the Lounge protocol.

usage: yt_lounge_sender.py HOST VIDEO_ID [step ...]
  steps: wait:N  pause  play  seek:S  stop  (default: wait:20 pause wait:6 play wait:6 seek:120 wait:15 stop)
  env DIAL_PORT (default 8008); NO_SSDP=1 to skip the multicast probe.
"""
import asyncio, os, re, socket, sys, time, urllib.request
from pyytlounge import YtLoungeApi, EventListener

T0 = time.time()
def log(*a):
    print("[%7.2f]" % (time.time() - T0), *a, flush=True)

def ssdp(host, timeout=3.0):
    msg = ("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
           "MX: 1\r\nST: urn:dial-multiscreen-org:service:dial:1\r\n\r\n").encode()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(msg, ("239.255.255.250", 1900))
    s.sendto(msg, (host, 1900))
    end = time.time() + timeout
    while time.time() < end:
        try:
            data, addr = s.recvfrom(4096)
        except socket.timeout:
            break
        if addr[0] != host:
            continue
        m = re.search(rb"(?im)^LOCATION:\s*(\S+)", data)
        if m:
            return m.group(1).decode()
    return None

def dial_screen_id(host):
    loc = None if os.environ.get("NO_SSDP") else ssdp(host)
    log("ssdp LOCATION:", loc)
    if not loc:
        loc = "http://%s:%s/dd.xml" % (host, os.environ.get("DIAL_PORT", "8008"))
    with urllib.request.urlopen(loc, timeout=5) as r:
        app_url = r.headers.get("Application-URL")
        dd = r.read().decode(errors="replace")
    fn = re.search(r"<friendlyName>([^<]*)</friendlyName>", dd)
    log("dd.xml friendlyName=%s Application-URL=%s" % (fn.group(1) if fn else "?", app_url))
    with urllib.request.urlopen(app_url.rstrip("/") + "/YouTube", timeout=5) as r:
        xml = r.read().decode(errors="replace")
    sid = re.search(r"<screenId>([^<]+)</screenId>", xml)
    st = re.search(r"<state>([^<]+)</state>", xml)
    log("DIAL YouTube state=%s screenId=%s..." % (st.group(1) if st else "?", sid.group(1)[:6] if sid else None))
    return (sid.group(1) if sid else None), (fn.group(1) if fn else None)

class L(EventListener):
    async def playback_state_changed(self, e):
        log("screen state:", getattr(e, "state", None), "t=%s dur=%s" % (getattr(e, "current_time", None), getattr(e, "duration", None)))
    async def now_playing_changed(self, e):
        log("screen nowPlaying:", getattr(e, "video_id", None), getattr(e, "state", None))
    async def disconnected(self, e):
        log("screen disconnected")

async def main():
    import traceback
    try:
        return await _main()
    except Exception:
        log("FATAL\n" + traceback.format_exc())
        return 4

async def _main():
    host, vid = sys.argv[1], sys.argv[2]
    steps = sys.argv[3:] or "wait:20 pause wait:6 play wait:6 seek:120 wait:15 stop".split()
    screen_id, name = dial_screen_id(host)
    if not screen_id:
        log("FAIL no screenId from DIAL"); return 2
    async with YtLoungeApi("gcast-sim-sender", L()) as api:
        ok = await api.pair_with_screen_id(screen_id, name)
        log("paired:", ok, "screen_name:", api.screen_name)
        if not ok or not await api.connect():
            log("FAIL connect"); return 3
        log("connected; screen available:", await api.is_available())
        sub = asyncio.create_task(api.subscribe())
        await asyncio.sleep(1)
        log("-> setPlaylist", vid); log("   ok:", await api.play_video(vid))
        for st in steps:
          try:
            if st.startswith("wait:"):
                await asyncio.sleep(float(st[5:]))
            elif st == "pause":
                log("-> pause", await api.pause())
            elif st == "play":
                log("-> play", await api.play())
            elif st.startswith("seek:"):
                log("-> seekTo", st[5:], await api.seek_to(float(st[5:])))
            elif st == "stop":
                log("-> stopVideo", await api._command("stopVideo"))
          except Exception as ex:  # keep the script going; the log is the proof
            log("step %s raised %r (connected=%s)" % (st, ex, api.connected()))
        await asyncio.sleep(2)
        sub.cancel()
        await api.disconnect()
    log("done")
    return 0

if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
