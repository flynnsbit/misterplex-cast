#!/usr/bin/env bash
# Recovery of the old live gate's negative/secret tests, without prompts or pass stamps.
set +x
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
if ! "$ROOT/build/pms_baseline_probe" --probe-contract >/dev/null 2>&1; then
  echo "SKIP: pms_baseline_probe --probe-contract absent on canon (unit-host class a)" >&2
  exit 0
fi
python3 - "$ROOT" <<'PY'
from pathlib import Path
import contextlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from types import SimpleNamespace

root = Path(sys.argv[1])
contract = json.loads(subprocess.check_output([str(root/"build/pms_baseline_probe"), "--probe-contract"]))
script = root / "assets/plex-profiles/probe_pms.py"
token = "MISTERPLEX_SYNTHETIC_LEAK_SENTINEL"
env = os.environ.copy()
for name in ("PLEX_BASE", "PLEX_TOKEN", "PLEX_KEY", "MISTERPLEX_BASELINE_KEY"):
    env.pop(name, None)
env["MISTERPLEX_CONF"] = str(root / "build/nonexistent-profile-test.conf")
result = subprocess.run(["python3", str(script)], env=env, capture_output=True)
assert result.returncode == 77 and b"SKIP-NOT-PASS" in result.stderr
result = subprocess.run(["python3", str(script), "--token", token], env=env, capture_output=True)
assert result.returncode == 2 and token.encode() not in result.stdout + result.stderr
env.update(PLEX_TOKEN=token, PLEX_BASE="http://127.0.0.1:1",
           MISTERPLEX_BASELINE_KEY="/library/metadata/1")
out = root / "build/pms-secret-negative"
out.mkdir(exist_ok=True)
for f in out.iterdir():
    assert f.is_file()
    f.unlink()
result = subprocess.run(["python3", str(script), "--output", "build/pms-secret-negative"],
                        env=env, capture_output=True, timeout=20)
assert result.returncode != 0 and token.encode() not in result.stdout + result.stderr
for f in out.iterdir():
    assert token.encode() not in f.read_bytes()
spec = importlib.util.spec_from_file_location("probe_pms", script)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
assert module.vcl_byte_sizes(b"\x00\x00\x00\x01\x65\x00\x00\x03\x01\x80") == (6, 4, 1)
assert module.vcl_byte_sizes(b"\x00\x00\x01\x65\x00\x00\x03\x03\x80") == (6, 4, 1)
for selected, decision, identifier, expected in (
        ("1", "burn", "290", True), ("0", "copy", "290", False),
        ("1", "burn", "291", False), ("0", "burn", "290", False)):
    xml = module.ET.fromstring(
        f'<Media subtitleDecision="burn"><Stream streamType="3" id="{identifier}" '
        f'selected="{selected}" decision="{decision}"/></Media>')
    assert module.subtitle_decision_summary(xml, 290)["requested_burn_confirmed"] is expected
xml = module.ET.fromstring(
    f'<Media subtitleDecision="{token}"><Stream streamType="3" id="{token}" '
    f'selected="{token}" decision="{token}"/></Media>')
assert token not in json.dumps(module.subtitle_decision_summary(xml, 290))
stale_root = root/"build/pms-stale-harness-test"
(stale_root/"build").mkdir(parents=True, exist_ok=True)
stale_probe = stale_root/"build/pms_baseline_probe"
stale_probe.write_text("#!/bin/sh\nexit 2\n")
stale_probe.chmod(0o755)
original_root, original_argv = module.ROOT, sys.argv
module.ROOT, sys.argv = stale_root, ["probe_pms.py"]
errors = io.StringIO()
try:
    with contextlib.redirect_stderr(errors):
        assert module.main() == 1
finally:
    module.ROOT, sys.argv = original_root, original_argv
assert "harness/probe ABI mismatch" in errors.getvalue()

# A loopback fake endpoint sends a small body prefix and then stalls. No live PMS is contacted.
release_stream = threading.Event()
http_state = {"starts": [], "stops": [], "kind": "partial"}

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *unused):
        pass

    def do_PUT(self):
        path = module.urllib.parse.urlsplit(self.path)
        query = module.urllib.parse.parse_qs(path.query)
        assert path.path == "/library/parts/700" and query["allParts"] == ["0"]
        chosen = int(query["subtitleStreamID"][0])
        http_state["puts"].append(chosen)
        is_restore = len(http_state["puts"]) > 1
        failing = (http_state["kind"] == "lab-restore-error" and is_restore)
        if not failing and http_state["kind"] != "lab-ignore-set":
            http_state["selected"] = chosen
        failing |= http_state["kind"] == "lab-set-error" and not is_restore
        self.send_response(500 if failing else 200)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        path = module.urllib.parse.urlsplit(self.path)
        query = module.urllib.parse.parse_qs(path.query)
        if path.path.endswith(("/decision", "/start.mp4")):
            expected_subtitles = "burn" if http_state["kind"].startswith(("subtitle-", "lab-")) else "none"
            assert query["subtitles"] == [expected_subtitles]
        if path.path.endswith("/start.mp4"):
            http_state["starts"].append(query["session"][0])
            if http_state["kind"] == "bad-status":
                self.wfile.write(b"HTTP/1.1 "+token.encode()+b"\r\n\r\n")
                self.wfile.flush()
                self.close_connection = True
                return
            if http_state["kind"] == "complete":
                self.send_response(200)
                self.send_header("Content-Type", "video/mp2t")
                self.send_header("Content-Length", str(len(http_state["body"])))
                self.end_headers()
                self.wfile.write(http_state["body"])
                self.wfile.flush()
                self.close_connection = True
                return
            self.send_response(200)
            self.send_header("Content-Type", "video/mp2t")
            if http_state["kind"] in ("chunked", "trickle"):
                self.send_header("Transfer-Encoding", "chunked")
            else:
                self.send_header("Content-Length", "32768")
            self.end_headers()
            if http_state["kind"] == "trickle":
                try:
                    while not release_stream.wait(0.1):
                        self.wfile.write(b"f")
                        self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                self.close_connection = True
                return
            if http_state["kind"] == "chunked":
                self.wfile.write(b"8000\r\n")
            if http_state["kind"] != "empty":
                self.wfile.write(b"\x47"+b"\x00"*187)
            self.wfile.flush()
            if http_state["kind"] != "short":
                release_stream.wait(10)
            self.close_connection = True
            return
        if path.path.endswith("/stop"):
            http_state["stops"].append(query["session"][0])
        body = (b'<MediaContainer><Video><Media videoCodec="h264" duration="16000">'
                b'<Part><Stream streamType="1" frameRate="24.000"/></Part>'
                b'</Media></Video></MediaContainer>')
        if http_state["kind"].startswith("subtitle-"):
            selected = http_state["kind"] == "subtitle-confirmed"
            subtitle = (b'<Stream streamType="3" id="290" selected="' +
                        (b'1" decision="burn"/>' if selected else b'0" decision="copy"/>'))
            body = body.replace(b"</Part>", subtitle+b"</Part>")
            if path.path.endswith("/decision"):
                body = body.replace(b"<Media videoCodec=", b'<Media subtitleDecision="' +
                                    (b'burn" videoCodec=' if selected else b'none" videoCodec='))
                assert query["subtitleStreamID"] == ["290"] and query["subtitles"] == ["burn"]
        if http_state["kind"].startswith("lab-"):
            burn = http_state["selected"] == 290 and http_state["kind"] != "lab-decision-error"
            streams = "".join(
                f'<Stream streamType="3" id="{sid}" selected="{int(http_state["selected"] == sid)}" '
                f'decision="{"burn" if burn and sid == 290 else "copy"}"/>'
                for sid in (290, 291))
            body = (f'<MediaContainer><Video ratingKey="{http_state["rating_key"]}">'
                    f'<Media videoCodec="mpeg2video" subtitleDecision="{"burn" if burn else "none"}">'
                    '<Part id="700" file="/data/movies/MiSTerPlex_P1_30ff2997/RGB601_test.mkv">'
                    + streams + '</Part></Media></Video></MediaContainer>').encode()
            if http_state["kind"] == "lab-concurrent" and path.path.endswith("/decision"):
                http_state["selected"] = 291
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

for kind in ("partial", "chunked", "empty", "bad-status", "trickle", "short"):
    http_state = {"starts": [], "stops": [], "kind": kind}
    release_stream.clear()
    partial = root/f"build/pms-read-test-{kind}"
    partial.mkdir(exist_ok=True)
    for name in ("source.json", "decision.json", "capture.log", "delivered-network.ts",
                 "delivered.ts", "failure-result.json"):
        (partial/name).unlink(missing_ok=True)
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    live_env = dict(env, PLEX_BASE=f"http://127.0.0.1:{server.server_port}")
    command = ["python3", str(script)]
    if kind == "trickle":
        command = ["python3", "-c",
                   "import runpy,sys; ns=runpy.run_path(sys.argv.pop(1)); "
                   "ns['capture'].__globals__['CAPTURE_WALL_SECONDS']=1.2; sys.exit(ns['main']())",
                   str(script)]
    try:
        result = subprocess.run(command+["--seconds", "4",
                                 "--stream-read-timeout", "1",
                                 "--output", str(partial.relative_to(root))],
                                env=live_env, capture_output=True, timeout=20)
    finally:
        release_stream.set()
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=3)
    assert result.returncode == 1 and token.encode() not in result.stdout + result.stderr
    expected = b"\x47"+b"\x00"*187 if kind in ("partial", "chunked", "short") else b""
    if kind != "bad-status":
        assert (partial/"delivered-network.ts").read_bytes() == expected
    failure = json.loads((partial/"failure-result.json").read_text())
    assert failure["stage"] == ("stream-open" if kind == "bad-status" else
                                "stream-first-read" if kind in ("empty", "trickle") else "stream-read")
    assert failure["received_network_bytes"] == failure["preserved_network_bytes"] == len(expected)
    assert failure["error_type"] == ("BadStatusLine" if kind == "bad-status" else
                                     "Failure" if kind in ("trickle", "short") else "TimeoutError")
    if kind == "trickle":
        assert failure["detail"] == "capture wall-time limit"
        assert failure["capture_wall_limit_seconds"] == 1.2
    assert not failure["syntax_evaluated"] and not failure["encoded_profile_rejection"]
    assert not failure["retry_performed"] and failure["own_session_stop_request_succeeded"]
    assert not failure["own_session_absence_verified"]
    assert len(http_state["starts"]) == 1 and http_state["starts"] == http_state["stops"]
    for f in partial.iterdir():
        assert token.encode() not in f.read_bytes()

for kind in ("subtitle-unconfirmed", "subtitle-confirmed", "source-codec-rejected"):
    diagnostic = root/f"build/pms-read-test-{kind}"
    diagnostic.mkdir(exist_ok=True)
    for name in ("source.json", "decision.json", "failure-result.json"):
        (diagnostic/name).unlink(missing_ok=True)
    http_state = {"starts": [], "stops": [], "kind": kind}
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    selection_args = (["--require-source-codec", "mpeg2video"] if kind == "source-codec-rejected"
                      else ["--subtitle-stream-id", "290"])
    try:
        result = subprocess.run(["python3", str(script), "--decision-only",
                                 "--output", str(diagnostic.relative_to(root))]+selection_args,
                                env=dict(env, PLEX_BASE=f"http://127.0.0.1:{server.server_port}"),
                                capture_output=True, timeout=20)
    finally:
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=3)
    confirmed = kind == "subtitle-confirmed"
    assert result.returncode == (77 if confirmed else 1)
    if kind == "source-codec-rejected":
        assert not (diagnostic/"decision.json").exists() and not http_state["stops"]
        assert json.loads((diagnostic/"source.json").read_text())["media"]["videoCodec"] == "h264"
    else:
        assert json.loads((diagnostic/"decision.json").read_text())["requested_burn_confirmed"] is confirmed
        assert len(http_state["stops"]) == 1
    assert not http_state["starts"]
    assert token.encode() not in result.stdout+result.stderr
    assert not (diagnostic/"delivered-network.ts").exists()

for kind in ("lab-none", "lab-original", "lab-already", "lab-ignore-set",
             "lab-decision-error", "lab-set-error", "lab-restore-error", "lab-concurrent", "lab-protected"):
    diagnostic = root/f"build/pms-read-test-{kind}"
    diagnostic.mkdir(exist_ok=True)
    for name in ("source.json", "decision.json", "failure-result.json",
                 "subtitle-selection.json", "subtitle-selection.json.next"):
        (diagnostic/name).unlink(missing_ok=True)
    original = 291 if kind == "lab-original" else 290 if kind == "lab-already" else 0
    rating_key = 145 if kind == "lab-protected" else 900
    scope = root/"build/pms-test-lab-scope.json"
    scope.write_text(json.dumps({
        "allow_temporary_part_subtitle_selection": True, "rating_key": rating_key,
        "part_id": 700, "subtitle_stream_id": 290,
        "container_file": "/data/movies/MiSTerPlex_P1_30ff2997/RGB601_test.mkv",
        "source_sha256": "a"*64}))
    http_state = {"starts": [], "stops": [], "puts": [], "kind": kind,
                  "selected": original, "rating_key": rating_key}
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    try:
        result = subprocess.run(["python3", str(script), "--decision-only",
                                 "--subtitle-stream-id", "290", "--lab-fixture-scope", str(scope),
                                 "--output", str(diagnostic.relative_to(root))],
                                env=dict(env, PLEX_BASE=f"http://127.0.0.1:{server.server_port}",
                                         MISTERPLEX_BASELINE_KEY=f"/library/metadata/{rating_key}"),
                                capture_output=True, timeout=20)
    finally:
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=3)
        scope.unlink()
    assert not http_state["starts"] and token.encode() not in result.stdout+result.stderr
    assert result.returncode == (77 if kind in ("lab-none", "lab-original", "lab-already") else 1)
    if kind == "lab-protected":
        assert not http_state["puts"] and not (diagnostic/"subtitle-selection.json").exists()
    else:
        recovery = json.loads((diagnostic/"subtitle-selection.json").read_text())
        assert recovery["original_selected_subtitle"] == original
        assert recovery["restore_required"] == (kind != "lab-already")
        assert recovery["restore_verified"] == (kind not in ("lab-already", "lab-restore-error", "lab-concurrent"))
        assert http_state["selected"] == (290 if kind == "lab-restore-error" else
                                         291 if kind == "lab-concurrent" else original)
        assert http_state["puts"] == ([] if kind == "lab-already" else
                                      [290] if kind in ("lab-ignore-set", "lab-concurrent") else [290, original])
    for f in diagnostic.iterdir():
        assert token.encode() not in f.read_bytes()

complete = root/"build/pms-read-test-complete"
complete.mkdir(exist_ok=True)
for name in ("source.json", "decision.json", "capture.log", "delivered-network.ts",
             "delivered.ts", "encoder-session.json"):
    (complete/name).unlink(missing_ok=True)
fixture = root/"build/pms-loopback-source.ts"
subprocess.run(["ffmpeg", "-v", "error", "-nostdin", "-f", "lavfi", "-i", "testsrc2=s=320x240:r=24",
                "-frames:v", "96", "-c:v", "libx264", "-profile:v", "baseline", "-threads", "1",
                "-f", "mpegts", "-y", str(fixture)], check=True, timeout=30)
http_state = {"starts": [], "stops": [], "kind": "complete", "body": fixture.read_bytes()}
server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
server.daemon_threads = True
server_thread = threading.Thread(target=server.serve_forever, daemon=True)
server_thread.start()
try:
    assert module.capture(SimpleNamespace(
        prototype="ip", mode="240p", fps="24", filter="on", subtitle_stream_id=None,
        offset=0, seconds=4, stream_read_timeout=1), complete, {
            "PLEX_BASE": f"http://127.0.0.1:{server.server_port}",
            "PLEX_TOKEN": token, "MISTERPLEX_BASELINE_KEY": "/library/metadata/1"}) == 0
finally:
    server.shutdown()
    server.server_close()
    server_thread.join(timeout=3)
assert (complete/"delivered-network.ts").read_bytes() == http_state["body"]
count = subprocess.check_output(["ffprobe", "-v", "error", "-count_frames", "-select_streams", "v:0",
                                 "-show_entries", "stream=nb_read_frames", "-of", "json",
                                 str(complete/"delivered.ts")], timeout=20)
assert json.loads(count)["streams"][0]["nb_read_frames"] == "96"
assert len(http_state["starts"]) == 1 and http_state["starts"] == http_state["stops"]
assert not (complete/"measurement.json").exists()  # Synthetic HTTP transport, not PMS provenance.
fixture.unlink()

color_root = root/"build/pms-color-source-test"
color_assets = color_root/"assets/plex-profiles"
color_assets.mkdir(parents=True, exist_ok=True)
for name in ("create_color_fixture.sh", "qualification-subtitle.srt"):
    shutil.copyfile(root/"assets/plex-profiles"/name, color_assets/name)
color_fixture = color_root/"build/pms-profile-inputs/MiSTerPlex_P1_RGB601_MPEG2_23976_Subtitle_30ff2997.mkv"
color_fixture.unlink(missing_ok=True)
result = subprocess.run(["bash", str(color_assets/"create_color_fixture.sh")],
                        capture_output=True, timeout=60)
assert result.returncode == 0, result.stderr.decode()
source_streams = json.loads(subprocess.check_output(
    ["ffprobe", "-v", "error", "-show_streams", "-of", "json", str(color_fixture)], timeout=20))["streams"]
video = next(s for s in source_streams if s["codec_type"] == "video")
assert video["codec_name"] == "mpeg2video" and (video["width"], video["height"]) == (640, 480)
assert video["sample_aspect_ratio"] == "1:1" and video["display_aspect_ratio"] == "4:3"
assert video["r_frame_rate"] == "24000/1001" and video["color_range"] == "tv"
assert all(video[k] == "smpte170m" for k in ("color_space", "color_transfer", "color_primaries"))
subtitle = next(s for s in source_streams if s["codec_type"] == "subtitle")
assert subtitle["codec_name"] == "subrip" and subtitle["disposition"]["default"] == 1
assert subtitle["disposition"]["forced"] == 1
original_sha = module.hashlib.sha256(color_fixture.read_bytes()).hexdigest()
assert subprocess.run(["bash", str(color_assets/"create_color_fixture.sh")],
                      capture_output=True, timeout=10).returncode == 1
assert module.hashlib.sha256(color_fixture.read_bytes()).hexdigest() == original_sha
color_fixture.unlink()

for prototype, fps, filtering in (("ip", "24", "on"), ("idr", "24000/1001", "off")):
    capture = root / f"build/pms-offline-container-{prototype}"
    capture.mkdir(exist_ok=True)
    for f in capture.iterdir():
        assert f.is_file()
        f.unlink()
    gop = 1 if prototype == "idr" else 24
    flags = ("cabac=0:bframes=0:ref=1:weightp=0:8x8dct=0:partitions=none:"
             "vbv-maxrate=4000:vbv-bufsize=1000:"
             f"scenecut=0:threads=1:slices=1:qpmin=10:qpmax=40:keyint={gop}:"
             f"no-deblock={int(filtering == 'off')}")
    subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"testsrc2=s=320x240:r={fps}",
                    "-frames:v", "48", "-c:v", "libx264", "-profile:v", "baseline",
                    "-x264-params", flags, "-f", "mpegts", "-y", str(capture/"delivered.ts")],
                   check=True)
    shutil.copyfile(capture/"delivered.ts", capture/"delivered-network.ts")
    (capture/"source.json").write_text(json.dumps({
        "media": {"videoCodec": "h264", "width": "320", "height": "240", "aspectRatio": "1.333333"},
        "streams": [{"streamType": "1", "frameRate": fps}],
        "subtitle_burn_requested": False}))
    args = SimpleNamespace(prototype=prototype, fps=fps, filter=filtering, mode="240p",
                           reserved_experiment=False, require_full_size=True, input_ts="offline",
                           max_au_bytes=contract["max_au_bytes"],
                           transport_max_au_bytes=contract["max_au_bytes"], allow_rate_conversion=False,
                           max_vcl_rbsp_bytes=None, require_limited_bt601=False)
    module.analyze(args, capture)
    report = json.loads((capture/"measurement.json").read_text())
    assert report["origin"] == "offline-container" and report["cadence_ok"]
    assert report["default_decoded_frames"] == 48 and report["remux_pts_rebase_ticks"] == 0
    assert report["source_film_rate_native"] and not report["release_au_limit_selected"]
    assert report["hardware_compatibility_checked"] is False
    assert report["geometry_complete"] and report["source_dar_metadata"] == "1.333333"
    assert report["sps_geometry"][0]["macroblocks_per_picture"] == 300
    assert report["probe_contract_ok"] and not report["limited_bt601_signaling_ok"]
    assert report["matrix2_rendering_default"] == "limited-bt601"
    assert report["matrix2_default_scope"] == "explicitly-unqualified-first-IDR-bringup"
    assert report["matrix2_default_applied_by_probe"] is False
    if prototype == "ip":
        args.require_limited_bt601 = True
        try:
            module.analyze(args, capture)
        except module.Failure as error:
            assert "limited-BT601 scanout color signaling" in str(error)
        else:
            raise AssertionError("unspecified matrix falsely qualified fixed BT601 scanout")
        report = json.loads((capture/"measurement.json").read_text())
        assert report["syntax_ok"] and not report["probe_contract_ok"]
        assert report["probe_rejection"] == "color-signaling"
        assert not report["limited_bt601_signaling_ok"]
        args.require_limited_bt601 = False
    if prototype == "idr":
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i",
                        f"testsrc2=s=320x240:r={fps}", "-frames:v", "48", "-c:v", "libx264",
                        "-profile:v", "baseline", "-x264-params",
                        flags.replace("vbv-bufsize=1000", "vbv-bufsize=4000"),
                        "-f", "mpegts", "-y", str(capture/"delivered.ts")], check=True)
        shutil.copyfile(capture/"delivered.ts", capture/"delivered-network.ts")
        try:
            module.analyze(args, capture)
        except module.Failure as error:
            assert "encoder-reported VBV/QP settings differ" in str(error)
        else:
            raise AssertionError("old installed VBV4000 falsely passed revised VBV1000 profile")
        report = json.loads((capture/"measurement.json").read_text())
        assert report["syntax_ok"] and report["default_decoder_ok"]
        assert report["x264_option_mismatches"]["vbv_bufsize"]["reported"] == "4000"
print("test_pms_baseline_live_gate: OK complete/partial/chunked/empty/invalid/trickled/short HTTP, secret negatives, "
      "scoped subtitle set/verify/restore,RGB601 MPEG2 source,24/23976 PTS/ordinary-decode; offline only")
PY
