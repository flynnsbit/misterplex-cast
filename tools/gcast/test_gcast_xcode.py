#!/usr/bin/env python3
"""Unit test for gcast_xcode helpers. Mutant: GCAST_MUT=nohalve|nossrc|ytany|aonly|hlslevel|hlsredir|profname."""
import hashlib, io, json, os, shutil, subprocess, sys, tempfile, threading, urllib.parse, urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gcast_xcode as g
mut = os.environ.get("GCAST_MUT", "")
if mut == "nohalve":
    _o = g.out_fps
    g.out_fps = lambda fn, fd: "%d/%d" % (fn, fd) if fn and fd and fn / fd > 31 else _o(fn, fd)
if mut == "ytany":
    g.YT_ID = __import__("re").compile(r".+")
fails = 0
def check(c, what):
    global fails
    if not c: fails += 1; print("FAIL", what)
check(g.out_fps(60000, 1001) == "30000/1001", "60-family halved")
check(g.out_fps(50, 1) == "25/1", "50 halved")
check(g.out_fps(30, 1) == "30/1", "30 exact")
check(g.out_fps(24000, 1001) == "24000/1001", "24p")
check(g.out_fps(0, 0) == "30000/1001", "unknown")
check(g.out_fps(359, 12) == "30000/1001", "odd hls rate snaps")
check(g.ok_src("http://a/b") and g.ok_src("https://a") and not g.ok_src("file:///etc/passwd"), "scheme")
check(not g.ok_src("http://a/b\r\nX: y"), "crlf")
info = {"fps_n": 30, "fps_d": 1}
c = g.build_cmd("ffmpeg", "http://s/x.mp4", 1280, 720, 3000, 5000, False, info)
if mut == "nossrc": c = [x for x in c if x != "-ss"]
check(c[c.index("-ss") + 1] == "5.000" if "-ss" in c else False, "offset -> -ss")
check(c.index("-ss") < c.index("-i") if "-ss" in c else False, "-ss before -i")
check("-bf" in c and c[c.index("-bf") + 1] == "0", "no B-frames")
check("pad=1280:720" in c[c.index("-vf") + 1], "letterbox pad")
r = g.build_cmd("ffmpeg", "https://s/master.m3u8", 640, 480, 1500, 0, True, info)
check("copy" in r and "-reconnect" not in r and "-ss" not in r, "remux hls")
check(g.ok_src("yt:dQw4w9WgXcQ") and not g.ok_src("yt:short") and not g.ok_src("yt:dQw4w9WgXcQ;x") and not g.ok_src("yt:../../etc/pa"), "yt id")
check(g.yt_resolve("bad id") is None, "yt_resolve rejects bad id without running yt-dlp")
y = g.build_cmd("ffmpeg", "yt:dQw4w9WgXcQ", 624, 480, 3000, 12000, True, info,
                [("https://v/vid", "User-Agent: x\r\n"), ("https://v/aud", "")], copy_audio=True)
ii = [i for i, x in enumerate(y) if x == "-i"]; ss = [i for i, x in enumerate(y) if x == "-ss"]
check(len(ii) == 2 and len(ss) == 2 and ss[0] < ii[0] < ss[1] < ii[1], "yt: -ss before each of 2 inputs")
check("1:a:0" in y and y[y.index("-c:a") + 1] == "copy" and "-headers" in y, "yt: audio from input 1, copy, headers")
t = g.build_cmd("ffmpeg", "yt:dQw4w9WgXcQ", 624, 480, 3000, 0, False, info, [("https://v/vid", ""), ("https://v/aud", "")])
check("libx264" in t and t[t.index("-c:a") + 1] == "aac", "yt ladder transcodes")
# AirPlay RAOP: one spectrum frame per second at the ladder size, not a 30 fps encode.
# The shipped helper ffmpeg has no lavfi demuxer and no color source.
ainfo = {"w": 640, "h": 480, "fps_n": 30000, "fps_d": 1001, "audio": 1, "vcodec": "none"}
if mut == "aonly": ainfo["vcodec"] = "h264"
a = g.build_cmd("ffmpeg", "http://192.0.2.228:7002/raop.wav", 624, 480, 3000, 0, False, ainfo)
joined = " ".join(a)
check("showspectrum=s=624x480" in joined and "fps=1" in joined and "noise=" not in joined,
      "audio-only: 1 fps spectrum at the ladder size, no grain")
check("fps=30000/1001" not in joined and a[a.index("-r") + 1] == "1" and "-shortest" not in a,
      "audio-only: 1 fps, graph ends with the audio")
check(a[a.index("-c:v") + 1] == "libx264" and a[a.index("-preset") + 1] == "ultrafast",
      "audio-only: still-frame encode, not veryfast 720p")
check("veryfast" not in a and "-vf" not in a and "[vo]" in a and "[ao]" in a,
      "audio-only: the ladder scale is not applied")
check(a[a.index("-c:a") + 1] == "aac" and "48000" in a and "copy" not in a, "audio-only: aac 48k, no copy")
check("-f" in a and "wav" in a, "raop.wav: wav demuxer forced (live, no seek)")
b = g.build_cmd("ffmpeg", "https://pod/x.mp3", 624, 480, 3000, 7000, False, ainfo)
check("-ss" in b and b.index("-ss") < b.index("https://pod/x.mp3"), "audio-only url keeps offset")
ok = {"vcodec": "h264", "pix_fmt": "yuv420p", "bits": 8, "profile": "High", "level": 40,
      "w": 1280, "h": 720, "fps_n": 30000, "fps_d": 1001, "acodec": "aac"}
check(g.stream_copy_ok(ok), "720p30 high is copied")
check(g.stream_copy_ok(dict(ok, profile="Baseline", level=31, w=640, h=480, fps_n=30, fps_d=1)), "480p baseline copied")
check(not g.stream_copy_ok(dict(ok, vcodec="hevc")), "hevc re-encodes")
check(not g.stream_copy_ok(dict(ok, pix_fmt="yuv422p")), "422 re-encodes")
check(not g.stream_copy_ok(dict(ok, bits=10)), "10-bit re-encodes")
check(not g.stream_copy_ok(dict(ok, profile="High 10")), "high10 re-encodes")
check(not g.stream_copy_ok(dict(ok, level=51)), "level 5.1 re-encodes")
check(not g.stream_copy_ok(dict(ok, w=1920, h=1080)), "1080p re-encodes")
check(not g.stream_copy_ok(dict(ok, fps_n=60, fps_d=1)), "60 fps re-encodes")
check(not g.stream_copy_ok(dict(ok, fps_n=0, fps_d=1)), "unknown fps re-encodes")
hls = g.build_cmd("ffmpeg", "https://s/master.m3u8", 1280, 720, 4000, 0, True, ok, copy_audio=True)
check(hls[hls.index("-c:v") + 1] == "copy" and "h264_mp4toannexb" in hls, "hls copy uses annexb bsf")
check(hls[hls.index("-c:a") + 1] == "copy", "aac audio is copied")
bad_audio = g.build_cmd("ffmpeg", "https://s/v.mp4", 1280, 720, 4000, 0, True, dict(ok, acodec="mp3"))
check(bad_audio[bad_audio.index("-c:v") + 1] == "copy" and bad_audio[bad_audio.index("-c:a") + 1] == "aac",
      "non-aac audio is encoded while video is copied")

def fixture(name):
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "hls", name)
    with open(here, "r", encoding="utf-8") as fh:
        return fh.read()

apple = g.plan_hls_master(fixture("apple_multivariant.m3u8"), "https://cdn.example/master.m3u8?token=SECRETMASTER")
check(apple is not None and apple["in_spec"], "apple master has an in-spec variant")
top = apple["in_spec"][0]
check(top["codec"] == "avc1.640029" and top["w"] == 1280 and top["h"] == 720 and top["fps"] == 30.0
      and top["bw"] == 2600000 and top["copy"] and top["variant_url"].endswith("/v720/prog_index.m3u8?token=SECRET720"),
      "apple master picks the highest-bandwidth in-spec variant")
picked = [v["codec"] for v in apple["in_spec"]]
check("avc1.42c01e" in picked and "hvc1.2.4.L123.B0" not in picked and "avc1.640032" not in picked
      and "avc1.6e001f" not in picked and "avc1.640020" not in picked,
      "apple master skips 1080p, HEVC, High 10, 60 fps, and level 5.0")
check(all("v1080" not in v["variant_url"] for v in apple["in_spec"]), "1080p is not a copy candidate")
apple_log = g.hls_log_line(top)
check(apple_log == "hls variant 1280x720 fps=30 codec=avc1.640029 bw=2600000 copy", "apple choice log")
check("http" not in apple_log and "?" not in apple_log and "token" not in apple_log and "SECRET" not in apple_log,
      "choice log has no url or token")

boundary = (
    "#EXTM3U\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=9000000,CODECS=\"avc1.64002a,mp4a.40.2\",RESOLUTION=1280x720,FRAME-RATE=30\n"
    "high.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=1000000,CODECS=\"avc1.640029,mp4a.40.2\",RESOLUTION=1280x720,FRAME-RATE=30\n"
    "ok.m3u8\n")
bound = g.plan_hls_master(boundary, "https://cdn.example/m.m3u8")
check(bound["in_spec"] and bound["in_spec"][0]["codec"] == "avc1.640029",
      "level 4.1 is in spec and level 4.2 is not")

master = "https://cdn.example/live/master.m3u8?token=SECRET"
rel = g.plan_hls_master(fixture("relative.m3u8"), master)
check(rel["in_spec"][0]["variant_url"] == "https://cdn.example/live/720/index.m3u8", "relative URI resolved against the master")
check(rel["in_spec"][1]["variant_url"] == "https://cdn.example/low/index.m3u8", "parent-relative URI resolved")
check("token" not in rel["in_spec"][0]["variant_url"] and rel["in_spec"][0]["fps"] == 29.97, "relative winner is 720p29.97")
check(g.hls_log_line(rel["in_spec"][0]) == "hls variant 1280x720 fps=29.97 codec=avc1.4d401f bw=1800000 copy",
      "fractional fps log")

aud = g.plan_hls_master(fixture("audio_group.m3u8"), master)
av = aud["in_spec"][0]
check(av["audio_url"] == "https://cdn.example/live/audio/en.m3u8", "DEFAULT=YES AAC group, not commentary or AC-3")
check(av["variant_url"] == "https://cdn.example/live/video/720.m3u8" and av["copy_audio"] and av["playlist_aac"],
      "audio group keeps the variant and copies AAC")
check("other.m3u8" not in (av["audio_url"] or ""), "a different AUDIO group is not selected")
ac3 = (
    "#EXTM3U\n"
    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"dd\",NAME=\"DD\",DEFAULT=YES,CODECS=\"ac-3\",URI=\"audio/ac3.m3u8\"\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=1000,CODECS=\"avc1.64001f,ac-3\",RESOLUTION=1280x720,FRAME-RATE=30,AUDIO=\"dd\"\n"
    "v.m3u8\n")
ac3p = g.plan_hls_master(ac3, "https://cdn.example/a/master.m3u8")
ac3v = ac3p["in_spec"][0]
check(ac3v["copy"] and not ac3v["copy_audio"] and ac3v["audio_url"].endswith("/audio/ac3.m3u8"),
      "non-AAC audio group is mapped but not copied")
mapped = g.build_cmd("ffmpeg", av["variant_url"], 1280, 720, 3000, 0, True, ok,
                     [(av["variant_url"], ""), (av["audio_url"], "")], copy_audio=True)
check(mapped.count("-i") == 2 and mapped[mapped.index("-c:v") + 1] == "copy"
      and mapped[mapped.index("-c:a") + 1] == "copy" and "1:a:0" in mapped,
      "separate audio group maps both inputs and copies AAC")
encoded = g.build_cmd("ffmpeg", ac3v["variant_url"], 1280, 720, 3000, 0, True, ok,
                      [(ac3v["variant_url"], ""), (ac3v["audio_url"], "")], copy_audio=False)
check(encoded[encoded.index("-c:v") + 1] == "copy" and encoded[encoded.index("-c:a") + 1] == "aac",
      "non-AAC group audio is encoded to AAC")

noc = g.plan_hls_master(fixture("no_codecs.m3u8"), master)
check(noc["in_spec"] == [] and noc["fallback"]["variant_url"].endswith("/mid/index.m3u8")
      and not noc["fallback"]["copy"] and noc["fallback"]["codec"] == "",
      "no-CODECS master falls back to the 720p30 variant")
check(g.hls_log_line(noc["fallback"]) == "hls variant 1280x720 fps=30 codec=missing bw=1500000 x264",
      "no-CODECS fallback log")

oos = g.plan_hls_master(fixture("all_out_of_spec.m3u8"), master)
check(oos["in_spec"] == [] and oos["fallback"]["variant_url"].endswith("/level50.m3u8")
      and not oos["fallback"]["copy"],
      "all-out-of-spec master falls back to the variant closest to 720p30")
check(g.hls_log_line(oos["fallback"]).endswith("x264") and "1080" not in g.hls_log_line(oos["fallback"]),
      "out-of-spec fallback is x264 at 720p")

missing_fps = (
    "#EXTM3U\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2700000,CODECS=\"avc1.64001f,mp4a.40.2\",RESOLUTION=1280x720\n"
    "nofps.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=1000,CODECS=\"avc1.42c01e,mp4a.40.2\",RESOLUTION=640x360,FRAME-RATE=30\n"
    "low.m3u8\n")
mf = g.plan_hls_master(missing_fps, "https://cdn.example/m.m3u8")
check(mf["in_spec"][0]["variant_url"].endswith("/nofps.m3u8") and mf["in_spec"][0]["fps"] is None,
      "missing FRAME-RATE still counts as in spec")
check(g.hls_log_line(mf["in_spec"][0]) == "hls variant 1280x720 fps=missing codec=avc1.64001f bw=2700000 copy",
      "missing fps log")

def probe_confirm(url):
    if url.endswith("/nofps.m3u8"):
        return dict(ok, w=1280, h=720, fps_n=60, fps_d=1)
    if url.endswith("/low.m3u8"):
        return dict(ok, w=640, h=360, fps_n=30, fps_d=1, profile="Baseline", level=30)
    return None

confirmed = g.choose_hls(mf, probe_confirm)
check(confirmed["copy"] and confirmed["variant_url"].endswith("/low.m3u8"),
      "ffprobe drops a missing-fps variant that is actually 60 fps")

check(g.plan_hls_master("#EXTM3U\n#EXTINF:4,\nseg.ts\n", master) is None, "a media playlist is not a master")
later_audio = (
    "#EXTM3U\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=1000,CODECS=\"avc1.64001f,mp4a.40.2\",RESOLUTION=640x360,FRAME-RATE=30,AUDIO=\"aac\"\n"
    "v.m3u8\n"
    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"en\",DEFAULT=YES,CODECS=\"mp4a.40.2\",URI=\"a.m3u8\"\n")
late = g.plan_hls_master(later_audio, "https://cdn.example/m.m3u8")
check(late["in_spec"][0]["audio_url"] == "https://cdn.example/a.m3u8", "audio group after the variant is still used")

# A redirected master: relative variant and audio URIs resolve against the final URL.
REDIR_MASTER = (
    "#EXTM3U\n"
    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aac\",NAME=\"English\",DEFAULT=YES,"
    "CODECS=\"mp4a.40.2\",URI=\"audio/en.m3u8\"\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=8000000,CODECS=\"avc1.640028\",RESOLUTION=1920x1080,FRAME-RATE=30\n"
    "video/1080.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2500000,CODECS=\"avc1.640029,mp4a.40.2\","
    "RESOLUTION=1280x720,FRAME-RATE=30,AUDIO=\"aac\"\n"
    "video/720.m3u8\n")

class _RedirHandler(BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        path = self.path.split("?", 1)[0]
        if path == "/cast/master.m3u8":
            loc = "http://%s/real/path/master.m3u8" % self.headers.get("Host")
            self.send_response(302)
            self.send_header("Location", loc)
            self.end_headers()
            return
        if path == "/real/path/master.m3u8":
            body = REDIR_MASTER.encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/vnd.apple.mpegurl")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(404)
        self.end_headers()
    def log_message(self, fmt, *args):
        return

redir = HTTPServer(("127.0.0.1", 0), _RedirHandler)
redir_thread = threading.Thread(target=redir.serve_forever, daemon=True)
redir_thread.start()
try:
    redir_src = "http://127.0.0.1:%d/cast/master.m3u8?token=SECRETCAST" % redir.server_address[1]
    got = g.fetch_playlist(redir_src)
    check(got is not None and got[1].endswith("/real/path/master.m3u8") and "token" not in got[1],
          "fetch follows the 302 and returns the final master URL")
    redir_plan = g.plan_hls_master(got[0], got[1]) if got else None
    final_variant = "http://127.0.0.1:%d/real/path/video/720.m3u8" % redir.server_address[1]
    final_audio = "http://127.0.0.1:%d/real/path/audio/en.m3u8" % redir.server_address[1]

    def probe_final(url):
        if url == final_variant:
            return dict(ok)
        if url == final_audio:
            return {"acodec": "aac", "audio": 1}
        return None

    redir_choice = g.choose_hls(redir_plan, probe_final) if redir_plan else None
    check(redir_choice and redir_choice.get("copy") and redir_choice["variant_url"] == final_variant
          and redir_choice["audio_url"] == final_audio and "/cast/" not in redir_choice["variant_url"],
          "redirected master resolves against the final URL and copy is chosen")
finally:
    redir.shutdown()

none_plan = g.plan_hls_master(
    "#EXTM3U\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2500000,CODECS=\"avc1.640029,mp4a.40.2\",RESOLUTION=1280x720,FRAME-RATE=30\n"
    "good.m3u8\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=400000,CODECS=\"avc1.640032\",RESOLUTION=1280x720,FRAME-RATE=30\n"
    "badlevel.m3u8\n",
    "https://cdn.example/m.m3u8")
check(g.choose_hls(none_plan, lambda url: None) is None,
      "no variant probes: fall back to the original src instead of a 502")

def probe_fallback_only(url):
    if url.endswith("/badlevel.m3u8"):
        return dict(ok, level=50, w=1280, h=720)
    return None

fb_only = g.choose_hls(none_plan, probe_fallback_only)
check(fb_only and not fb_only["copy"] and fb_only["info"] and fb_only["variant_url"].endswith("/badlevel.m3u8"),
      "a probed out-of-spec fallback is still x264, not dropped")

# Real stdout of the shipped ffprobe e49cfc7c (--enable-small) under qemu-arm-static.
# profile is the number as a string. pix_fmt stays a name. bits_per_raw_sample is "8".
# rates are "N/D".
_PROBE_EXPECT = {
    "high.json": "100",
    "main.json": "77",
    "baseline.json": "66",
    "constrained-baseline.json": "578",
    "hls-variant.json": "100",
}

def load_probe_raw(name):
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "ffprobe", name)
    with open(here, "rb") as fh:
        return fh.read()

def parse_probe_raw(raw):
    return g.parse_probe_json(json.loads(raw))

high_raw = load_probe_raw("high.json")
high_info = None
for name, prof in _PROBE_EXPECT.items():
    info = parse_probe_raw(load_probe_raw(name))
    if name == "high.json":
        high_info = info
    check(info["vcodec"] == "h264" and info["profile"] == prof and info["level"] == 41
          and info["pix_fmt"] == "yuv420p" and info["bits"] == 8
          and info["w"] == 1280 and info["h"] == 720
          and info["fps_n"] == 30 and info["fps_d"] == 1,
          "shipped ffprobe %s fields" % name)
    label = "shipped ffprobe numeric profile 100 is copied" if name == "high.json" else (
        "shipped ffprobe %s profile %s is copied" % (name, prof))
    check(g.stream_copy_ok(info), label)

check(high_info and g.direct_log_line(high_info, True).endswith(" copy"),
      "direct log of a numeric High probe ends in copy")

def mutated(raw, **fields):
    doc = json.loads(raw)
    video = next(s for s in doc["streams"] if s.get("codec_type") == "video")
    video.update(fields)
    return g.parse_probe_json(doc)

for bad in ("110", "122", "244", 110, 122, 244, "88", "144", "0", "High 10", "High 4:2:2", "High 4:4:4", ""):
    check(not g.stream_copy_ok(mutated(high_raw, profile=bad)), "profile %s rejected" % bad)

for pix in (0, "0", 12, "12", "yuv420p", "yuvj420p"):
    check(g.stream_copy_ok(mutated(high_raw, pix_fmt=pix)), "pix_fmt %s copied" % pix)
for pix in (1, "1", 4, "yuv422p", "yuv420p10le"):
    check(not g.stream_copy_ok(mutated(high_raw, pix_fmt=pix)), "pix_fmt %s rejected" % pix)

for rate in ("30", 30, "30.0", 30.0):
    got = mutated(high_raw, avg_frame_rate=rate, r_frame_rate="0/0")
    check(got["fps_d"] and abs(got["fps_n"] / float(got["fps_d"]) - 30.0) < 0.001
          and g.stream_copy_ok(got), "numeric fps %s is 30" % rate)
zero_avg = mutated(high_raw, avg_frame_rate="0/0", r_frame_rate="30/1")
check(zero_avg["fps_n"] == 30 and zero_avg["fps_d"] == 1 and g.stream_copy_ok(zero_avg),
      "avg 0/0 falls through to r_frame_rate")
sixty = mutated(high_raw, avg_frame_rate="60", r_frame_rate="60/1")
check(sixty["fps_n"] == 60 and not g.stream_copy_ok(sixty), "numeric 60 fps re-encodes")
film = mutated(high_raw, avg_frame_rate="30000/1001", r_frame_rate="30000/1001")
check(film["fps_n"] == 30000 and film["fps_d"] == 1001 and g.stream_copy_ok(film),
      "30000/1001 slash rate still copies")

def qemu_copy_e2e():
    """Shipped ffmpeg+ffprobe under qemu: in-spec HLS and mp4 log lines end in copy."""
    ffprobe = "/tmp/mpx-gcast-ffmpeg/ffprobe"
    ffmpeg = "/tmp/mpx-gcast-ffmpeg/ffmpeg"
    qemu = "/usr/bin/qemu-arm-static"
    if not (os.path.isfile(ffprobe) and os.path.isfile(ffmpeg) and os.path.isfile(qemu) and shutil.which("ffmpeg")):
        print("skip qemu copy e2e: shipped helper or host ffmpeg missing")
        return
    def md5(path):
        h = hashlib.md5()
        with open(path, "rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    check(md5(ffprobe) == "e49cfc7c609edf7e6ba9246f1938f574", "e2e ffprobe is e49cfc7c")
    check(md5(ffmpeg) == "80baac9d0912532c471c94a6f71068bd", "e2e ffmpeg is 80baac9d")
    if fails:
        return
    tmp = tempfile.mkdtemp(prefix="mpx-copy-e2e-")
    try:
        subprocess.check_call([
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
            "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
            "-frames:v", "12", "-shortest",
            "-c:v", "libx264", "-profile:v", "high", "-level", "4.1", "-pix_fmt", "yuv420p", "-bf", "0",
            "-c:a", "aac", "-b:a", "64k", "-movflags", "+faststart",
            os.path.join(tmp, "in720.mp4")], stdout=subprocess.DEVNULL)
        subprocess.check_call([
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
            "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
            "-frames:v", "24", "-shortest",
            "-c:v", "libx264", "-profile:v", "high", "-level", "4.1", "-pix_fmt", "yuv420p", "-bf", "0",
            "-c:a", "aac", "-b:a", "64k", "-f", "mpegts",
            os.path.join(tmp, "with-sdt.ts")], stdout=subprocess.DEVNULL)
        data = open(os.path.join(tmp, "with-sdt.ts"), "rb").read()
        start = data.find(b"\x47")
        data = data[start:]
        out = bytearray()
        i = 0
        while i + 188 <= len(data):
            pkt = data[i:i + 188]
            if pkt[0] != 0x47:
                j = data.find(b"\x47", i + 1)
                if j < 0:
                    break
                i = j
                continue
            pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
            if pid != 0x11:
                out += pkt
            i += 188
        hls = os.path.join(tmp, "hls")
        os.mkdir(hls)
        open(os.path.join(hls, "seg0.ts"), "wb").write(out)
        open(os.path.join(hls, "index.m3u8"), "w").write(
            "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n"
            "#EXTINF:1.000,\nseg0.ts\n#EXT-X-ENDLIST\n")
        open(os.path.join(hls, "master.m3u8"), "w").write(
            "#EXTM3U\n#EXT-X-VERSION:3\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=2500000,CODECS=\"avc1.640029,mp4a.40.2\",RESOLUTION=1280x720,FRAME-RATE=30\n"
            "index.m3u8\n")
        wrap_ffprobe = os.path.join(tmp, "ffprobe")
        wrap_ffmpeg = os.path.join(tmp, "ffmpeg")
        for path, binary in ((wrap_ffprobe, ffprobe), (wrap_ffmpeg, ffmpeg)):
            with open(path, "w") as fh:
                fh.write("#!/bin/sh\nexec %s %s \"$@\"\n" % (qemu, binary))
            os.chmod(path, 0o755)
        from functools import partial
        from http.server import SimpleHTTPRequestHandler
        class _Quiet(SimpleHTTPRequestHandler):
            def log_message(self, fmt, *args):
                return
        media = g.ThreadingHTTPServer(("127.0.0.1", 0), partial(_Quiet, directory=tmp))
        helper = g.ThreadingHTTPServer(("127.0.0.1", 0), g.H)
        helper.ffmpeg, helper.ffprobe, helper.ytdlp = wrap_ffmpeg, wrap_ffprobe, "yt-dlp"
        mt = threading.Thread(target=media.serve_forever, daemon=True)
        ht = threading.Thread(target=helper.serve_forever, daemon=True)
        mt.start(); ht.start()
        try:
            mport = media.server_address[1]
            hport = helper.server_address[1]
            buf = io.StringIO()
            old_log = g.LOG
            g.LOG = buf
            try:
                def play(src):
                    buf.seek(0); buf.truncate(0)
                    q = urllib.parse.urlencode({"src": src, "w": "1280", "h": "720", "br": "4000"})
                    url = "http://127.0.0.1:%d/ladder.ts?%s" % (hport, q)
                    resp = urllib.request.urlopen(url, timeout=90)
                    body = resp.read()
                    resp.close()
                    return buf.getvalue(), body
                mp4_log, mp4_body = play("http://127.0.0.1:%d/in720.mp4" % mport)
                hls_log, hls_body = play("http://127.0.0.1:%d/hls/master.m3u8" % mport)
            finally:
                g.LOG = old_log
        finally:
            helper.shutdown()
            media.shutdown()
        def ends_copy(text, needle):
            lines = [ln for ln in text.splitlines() if needle in ln]
            return bool(lines) and lines[-1].rstrip().endswith(" copy")
        check(ends_copy(mp4_log, "direct ") and "copy=1" in mp4_log and len(mp4_body) > 188,
              "qemu mp4 log ends in copy (%d bytes)" % len(mp4_body))
        if not (ends_copy(mp4_log, "direct ") and "copy=1" in mp4_log):
            print(mp4_log)
        check(ends_copy(hls_log, "hls variant ") and "copy=1" in hls_log and len(hls_body) > 188,
              "qemu hls log ends in copy (%d bytes)" % len(hls_body))
        if not (ends_copy(hls_log, "hls variant ") and "copy=1" in hls_log):
            print(hls_log)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

if not mut:
    child_env = os.environ.copy()
    child_env["GCAST_MUT"] = "hlsredir"
    child = subprocess.run([sys.executable, os.path.abspath(__file__)], env=child_env,
                           capture_output=True, text=True)
    check(child.returncode != 0 and "redirected master resolves against the final URL" in child.stdout,
          "pre-redirect mutant must fail")
    child_env["GCAST_MUT"] = "profname"
    child = subprocess.run([sys.executable, os.path.abspath(__file__)], env=child_env,
                           capture_output=True, text=True)
    check(child.returncode != 0 and "shipped ffprobe numeric profile 100 is copied" in child.stdout,
          "names-only profile mutant must fail")
    qemu_copy_e2e()

print("test_gcast_xcode: %s" % ("FAIL %d" % fails if fails else "OK"))
sys.exit(1 if fails else 0)
