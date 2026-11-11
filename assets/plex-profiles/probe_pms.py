#!/usr/bin/env python3
"""Bounded, token-safe PMS experiment; no video decoder in the product path."""
import argparse
import contextlib
import fractions
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
MAX_BYTES = 64 * 1024 * 1024
CAPTURE_WALL_SECONDS = 120
VBV_BUFFER_BITS = 1_000_000
VBV_RATE_BITS = 4_000_000
PROBE_ABI = 6


class Failure(Exception):
    pass


class SafeParser(argparse.ArgumentParser):
    def error(self, message):
        self.exit(2, "invalid arguments; credentials are accepted only via existing config/env\n")


def need(condition, message):
    if not condition:
        raise Failure(message)


def config():
    values = {}
    paths = [os.environ.get("MISTERPLEX_CONF", os.environ.get("MISTER_CONF", ""))]
    if not paths[0]:
        paths = [ROOT / "assets/misterplex.conf", Path.home() / ".config/misterplex/misterplex.conf"]
    for path in paths:
        if path and Path(path).is_file():
            for line in Path(path).read_text().splitlines():
                if "=" in line and not line.lstrip().startswith("#"):
                    key, value = line.split("=", 1)
                    values[key.strip()] = value.strip()
            break
    # A token is accepted only in process environment/existing config, never argv or artifacts.
    for key in ("PLEX_BASE", "PLEX_TOKEN", "MISTERPLEX_BASELINE_KEY", "PLEX_KEY"):
        if key in os.environ:
            values[key] = os.environ[key]
    return values


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise Failure("PMS redirect refused (credentials stay on selected server)")


def process(args, out=None, timeout=90):
    env = os.environ.copy()
    env.pop("PLEX_TOKEN", None)
    result = subprocess.run(args, stdout=subprocess.PIPE if out is None else out,
                            stderr=subprocess.PIPE, env=env, timeout=timeout)
    need(result.returncode == 0, f"{Path(args[0]).name} failed (local capture; no secrets logged)")
    return result.stdout


def read_stream_piece(response, maximum, timeout):
    if response.isclosed():
        return b""
    try:
        # CPython's HTTP/HTTPS response owns this socket through its buffered reader.
        response.fp.raw._sock.settimeout(max(0.001, timeout))
    except (AttributeError, OSError):
        raise Failure("cannot enforce the streaming read deadline")
    # read(n) can wait for a whole32KiB even after useful partial data arrived.
    return response.read1(maximum)


@contextlib.contextmanager
def capture_deadline():
    need(signal.getitimer(signal.ITIMER_REAL)[0] == 0, "capture requires an unused wall-time alarm")
    previous = signal.getsignal(signal.SIGALRM)

    def expired(unused_signal, unused_frame):
        raise Failure("capture wall-time limit")

    # A socket idle timeout alone does not bound trickled HTTP chunk framing or a blocked pipe.
    signal.signal(signal.SIGALRM, expired)
    signal.setitimer(signal.ITIMER_REAL, CAPTURE_WALL_SECONDS)
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous)


def vcl_byte_sizes(data):
    starts = list(re.finditer(b"\x00\x00\x00\x01|\x00\x00\x01", data))
    nal_max, rbsp_max, count = 0, 0, 0
    for index, start in enumerate(starts):
        first = start.end()
        end = starts[index+1].start() if index+1 < len(starts) else len(data)
        if first >= end or data[first] & 31 not in (1, 5):
            continue
        payload = data[first+1:end]
        offset, removed = 0, 0
        while offset+2 < len(payload):
            if payload[offset:offset+3] == b"\x00\x00\x03":
                removed += 1
                offset += 3
            else:
                offset += 1
        nal_max = max(nal_max, end-first)
        rbsp_max = max(rbsp_max, len(payload)-removed)
        count += 1
    return nal_max, rbsp_max, count


def analyze(args, work):
    ts = work / "delivered.ts"
    annexb = work / "delivered.264"
    process(["ffmpeg", "-v", "error", "-nostdin", "-i", str(ts), "-map", "0:v:0",
             "-c:v", "copy", "-bsf:v", "h264_mp4toannexb", "-an", "-f", "h264", "-y", str(annexb)])
    probe = [str(ROOT / "build/pms_baseline_probe"), "--annexb", str(annexb),
             "--prototype", args.prototype, "--mode", args.mode, "--fps", args.fps,
             "--filter", args.filter, "--json", str(work / "syntax.json")]
    probe += ["--max-au-bytes", str(args.max_au_bytes)]
    if args.max_vcl_rbsp_bytes is not None:
        probe += ["--max-vcl-rbsp-bytes", str(args.max_vcl_rbsp_bytes)]
    if args.reserved_experiment:
        probe.append("--reserved-experiment")
    if args.require_full_size:
        probe.append("--require-full-size")
    if args.require_limited_bt601:
        probe.append("--require-limited-bt601")
    # Keep syntax failures and independent characterization, not only successful samples.
    env = os.environ.copy()
    env.pop("PLEX_TOKEN", None)
    result = subprocess.run(probe, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, timeout=90)
    (work / "syntax.log").write_bytes(result.stdout)
    print(result.stdout.decode(errors="replace"), end="")
    au_limit_rejected = result.returncode != 0 and b"AU exceeds selected bound=" in result.stdout
    rbsp_limit_rejected = result.returncode != 0 and b"VCL RBSP exceeds selected bound=" in result.stdout
    color_rejected = result.returncode != 0 and any(message in result.stdout for message in (
        b"unsupported full-range", b"unsupported matrix_coefficients", b"signaling unproven"))
    packet_data = json.loads(process([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_packets", "-show_streams",
        "-show_entries", "packet=pts,dts,duration,size,flags:stream=codec_name,profile,width,height,"
        "coded_width,coded_height,pix_fmt,has_b_frames,refs,time_base,avg_frame_rate,r_frame_rate,"
        "sample_aspect_ratio,display_aspect_ratio,color_range,color_space,color_transfer,color_primaries",
        "-of", "json", str(ts)]))
    (work / "packets.json").write_text(json.dumps(packet_data, indent=2) + "\n")
    streams = packet_data.get("streams", [])
    packets = packet_data.get("packets", [])
    need(len(streams) == 1 and len(packets) >= 48, "need at least48 complete video AUs")
    stream = streams[0]
    rate = fractions.Fraction(args.fps)
    tb = fractions.Fraction(stream["time_base"])
    need(all("pts" in p and "dts" in p for p in packets), "missing framePTS/DTS")
    pts = [int(p["pts"]) for p in packets]
    dts = [int(p["dts"]) for p in packets]
    deltas = [b - a for a, b in zip(pts, pts[1:])]
    expected_ticks = 1 / rate / tb
    # 23.976 at90kHz alternates3753/3754 ticks. 24 uses3750.
    allowed = {expected_ticks.numerator // expected_ticks.denominator,
               -(-expected_ticks.numerator // expected_ticks.denominator)}
    cadence_ok = all(d > 0 and d in allowed for d in deltas)
    cadence_ok &= abs(fractions.Fraction(pts[-1]-pts[0]) - expected_ticks*(len(pts)-1)) <= 1
    cadence_ok &= pts == dts
    raw_timing = {}
    network = work / "delivered-network.ts"
    if network.is_file():
        raw_packets = json.loads(process([
            "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_packets",
            "-show_entries", "packet=pts,dts,duration,size", "-of", "json", str(network)]))
        (work / "network-packets.json").write_text(json.dumps(raw_packets, indent=2)+"\n")
        raw_video = raw_packets.get("packets", [])[:len(packets)]
        need(len(raw_video) == len(packets) and all("pts" in p and "dts" in p for p in raw_video),
             "network capture lacks original framePTS")
        raw_pts = [int(p["pts"]) for p in raw_video]
        raw_dts = [int(p["dts"]) for p in raw_video]
        raw_deltas = [b-a for a, b in zip(raw_pts, raw_pts[1:])]
        cadence_ok &= raw_deltas == deltas and raw_pts == raw_dts
        raw_timing = {"network_first_pts": raw_pts[0], "network_last_pts": raw_pts[-1],
                      "network_pts_delta_ticks": sorted(set(raw_deltas)),
                      "remux_pts_rebase_ticks": pts[0]-raw_pts[0]}
    duration = fractions.Fraction(pts[-1]-pts[0]) * tb + 1/rate
    sizes = [int(p["size"]) for p in packets]
    syntax_path = work / "syntax.json"
    syntax_data = json.loads(syntax_path.read_text()) if syntax_path.is_file() else {}
    observed_au_max = max(max(sizes), syntax_data.get("max_au_bytes", 0))
    max_vcl_nal, max_vcl_rbsp, vcl_count = vcl_byte_sizes(annexb.read_bytes())
    geometry = syntax_data.get("geometry", [])
    geometry_complete = bool(geometry) and (
        sum(g["pictures"] for g in geometry) == syntax_data.get("frames") and
        sum(g["pictures"]*g["macroblocks_per_picture"] for g in geometry) == syntax_data.get("mb"))
    unspecified_matrix = any(g.get("color", {}).get("matrix_coefficients") == 2 for g in geometry)
    signature = re.search(rb"x264 - core [\x20-\x7e]{1,8192}", annexb.read_bytes())
    encoder_options = {}
    if signature and b"options: " in signature.group():
        wanted = {"keyint", "keyint_min", "bframes", "ref", "cabac", "analyse", "deblock",
                  "8x8dct", "weightp", "qpmin", "qpmax", "slices", "vbv_maxrate",
                  "vbv_bufsize", "threads", "scenecut"}
        for item in signature.group().decode("ascii").split("options: ", 1)[1].split():
            if "=" in item:
                key, value = item.split("=", 1)
                if key in wanted:
                    encoder_options[key] = value
    expected_options = {"vbv_maxrate": "4000", "vbv_bufsize": "1000",
                        "qpmin": "10", "qpmax": "40"}
    encoder_option_mismatches = {key: {"expected": value, "reported": encoder_options.get(key)}
                                for key, value in expected_options.items()
                                if encoder_options.get(key) != value}
    bitrate = sum(sizes) * 8 / float(duration)
    max_window = 0
    j, window_bytes = 0, 0
    for i, size in enumerate(sizes):
        window_bytes += size
        while j < i and (pts[i]-pts[j])*tb >= 1:
            window_bytes -= sizes[j]
            j += 1
        max_window = max(max_window, window_bytes)
    # VBV token bucket: no interval can exceed rate*time + buffer, with mux/NAL headroom.
    bucket, vbv_ok = VBV_BUFFER_BITS/8, True
    for i, size in enumerate(sizes):
        if i:
            bucket = min(VBV_BUFFER_BITS/8, bucket + float((pts[i]-pts[i-1])*tb)*VBV_RATE_BITS/8)
        bucket -= size
        vbv_ok &= bucket >= -8192
    decode_ok = True
    try:
        process(["ffmpeg", "-v", "error", "-xerror", "-err_detect", "explode", "-threads", "1",
                 "-nostdin", "-i", str(ts), "-map", "0:v:0", "-an", "-fps_mode", "passthrough",
                 "-f", "framemd5", "-y", str(work / "default-decoder.framemd5")])
    except Failure:
        decode_ok = False
    decoded_frames = 0
    hashes = work / "default-decoder.framemd5"
    if hashes.is_file():
        decoded_frames = sum(bool(line.strip()) and not line.startswith("#")
                             for line in hashes.read_text().splitlines())
    decode_ok &= decoded_frames == len(packets)
    with (work / "headers.trace").open("wb") as log:
        trace = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "trace", "-nostdin",
                                "-i", str(ts), "-map", "0:v:0", "-c:v", "copy", "-an",
                                "-bsf:v", "trace_headers", "-f", "null", "-"],
                               stdout=subprocess.DEVNULL, stderr=log, env=env, timeout=90)
    report = {
        "origin": "offline-container" if args.input_ts else "real-PMS",
        "profile": profile_name(args), "prototype": args.prototype, "filter": args.filter,
        "declared_fps": str(rate), "frames": len(packets), "time_base": str(tb),
        "first_pts": pts[0], "last_pts": pts[-1], "pts_delta_ticks": sorted(set(deltas)),
        "cadence_ok": cadence_ok, "duration_seconds": float(duration),
        "max_packet_au_bytes": max(sizes), "video_bitrate_bps": bitrate,
        "max_1second_video_bits": max_window*8, "vbv_4000k_1000k_ok": vbv_ok,
        "au_limit_bytes": args.max_au_bytes, "transport_max_au_bytes": args.transport_max_au_bytes,
        "au_limit_source": ("caller-supplied" if getattr(args, "consumer_limit_explicit", True)
                            else "transport-ceiling-only"),
        "hardware_compatibility_checked": False,
        "packet_au_headroom_bytes": args.max_au_bytes-max(sizes),
        "packet_au_headroom_fraction": 1-max(sizes)/args.max_au_bytes,
        "observed_max_au_bytes": observed_au_max,
        "observed_au_headroom_bytes": args.max_au_bytes-observed_au_max,
        "observed_au_headroom_fraction": 1-observed_au_max/args.max_au_bytes,
        "max_vcl_nal_bytes_without_startcode": max_vcl_nal,
        "max_vcl_rbsp_bytes": max_vcl_rbsp,
        "vcl_nal_count": vcl_count,
        "vcl_rbsp_limit_bytes": args.max_vcl_rbsp_bytes,
        "vcl_rbsp_headroom_bytes": (None if args.max_vcl_rbsp_bytes is None
                                    else args.max_vcl_rbsp_bytes-max_vcl_rbsp),
        "release_au_limit_selected": False,
        "default_decoder_ok": decode_ok,
        "syntax_ok": bool(syntax_data.get("syntax_complete", result.returncode == 0)),
        "probe_contract_ok": result.returncode == 0,
        "probe_rejection": ("au-byte-limit" if au_limit_rejected else
                            "vcl-rbsp-byte-limit" if rbsp_limit_rejected else
                            "color-signaling" if color_rejected else
                            "profile-syntax" if result.returncode else None),
        "default_decoded_frames": decoded_frames,
        "trace_headers_ok": trace.returncode == 0, "stream": stream,
        "sha256_ts": hashlib.sha256(ts.read_bytes()).hexdigest(),
        "sha256_h264": hashlib.sha256(annexb.read_bytes()).hexdigest(),
        "full_size_required": args.require_full_size,
        "sps_geometry": geometry,
        "geometry_complete": geometry_complete,
        "limited_bt601_signaling_ok": syntax_data.get("limited_bt601_signaling_ok", False),
        "limited_bt601_required": args.require_limited_bt601,
        "matrix2_rendering_default": "limited-bt601" if unspecified_matrix else None,
        "matrix2_default_scope": ("explicitly-unqualified-first-IDR-bringup"
                                  if unspecified_matrix else None),
        "matrix2_default_applied_by_probe": False,
        "reserved": args.mode != "240p", "playback_accepted": False,
        "x264_encoder_signature": b"x264 - core" in annexb.read_bytes(),
        "x264_reported_options": encoder_options,
        "x264_option_mismatches": encoder_option_mismatches,
        "hardware_encoder_qualified": False,
        **raw_timing,
    }
    if (work / "source.json").is_file():
        source = json.loads((work / "source.json").read_text())
        source_rates = [s["frameRate"] for s in source["streams"]
                        if s.get("streamType") == "1" and "frameRate" in s]
        native_rate = bool(source_rates) and all(
            abs(float(fractions.Fraction(r)) - float(rate)) < 0.0005 for r in source_rates)
        report.update(source_codec=source["media"].get("videoCodec", "unknown"),
                      source_frame_rates=source_rates, source_film_rate_native=native_rate,
                      source_dar_metadata=source["media"].get("aspectRatio"),
                      source_coded_width_metadata=source["media"].get("width"),
                      source_coded_height_metadata=source["media"].get("height"),
                      subtitle_burn_requested=source["subtitle_burn_requested"],
                      subtitle_visual_verified=False)
    (work / "measurement.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PMS_PROFILE_MEASURED " + json.dumps({k: report[k] for k in (
        "origin", "frames", "pts_delta_ticks", "cadence_ok", "max_packet_au_bytes",
        "video_bitrate_bps", "syntax_ok", "default_decoder_ok", "au_limit_bytes",
        "observed_au_headroom_bytes", "probe_contract_ok", "limited_bt601_signaling_ok")},
        sort_keys=True))
    if unspecified_matrix:
        print("PMS_COLOR_ASSUMPTION matrix2 is legal unspecified; unqualified first-IDR rendering "
              "may default to limited-BT601, not signaled or color-qualified; probe applies no RGB conversion")
    need(not au_limit_rejected,
         f"observed AU exceeds selected {args.max_au_bytes}-byte consumer/transport limit; "
         "this compatibility rejection does not mean the H264 stream is malformed")
    need(not rbsp_limit_rejected, f"VCL RBSP exceeds selected {args.max_vcl_rbsp_bytes}-byte "
         "decoder RAM limit; this is separate from the encoded AU/ring budget")
    need(not color_rejected, "limited-BT601 scanout color signaling is unsupported or unproven; "
         "do not relabel pixels or apply ARM video conversion")
    need(result.returncode == 0, "emitted syntax violates selected profile (see syntax.log)")
    need(geometry_complete, "syntax report lacks complete actual per-SPS geometry/MB accounting")
    need(vcl_count == syntax_data["frames"] and max_vcl_rbsp == syntax_data["max_vcl_rbsp_bytes"],
         "independent VCL byte accounting differs from the full syntax walk")
    if "source_film_rate_native" in report:
        need(report["source_film_rate_native"] or args.allow_rate_conversion,
             "source rate is unknown/differs from selected film rational; a separately labelled "
             "--allow-rate-conversion experiment is not native film-rate qualification")
    need(cadence_ok, "PTS violates exact film rate/no-reordering contract")
    need(max(sizes) <= args.max_au_bytes and vbv_ok, "AU/VBV bound violated")
    need(decode_ok and trace.returncode == 0, "independent default decode/header trace failed")
    need(report["x264_encoder_signature"], "software x264 encoder signature not observed; "
         "hardware/other encoder is not qualified for these experiments")
    need(not encoder_option_mismatches,
         "encoder-reported VBV/QP settings differ from selected XML revision; "
         "observed small AUs do not prove the installed encoder bound")
    syntax = json.loads((work / "syntax.json").read_text())
    need(syntax["frames"] == len(packets), "demuxed AU count differs from complete slice pictures")
    print("PASS characterized candidate stream; NOT FPGA conformance, quality, or playback acceptance")


def profile_name(args):
    tier = "480" if args.mode in ("480i", "480p") else args.mode
    rate = "24" if args.fps in ("24", "24/1") else "23976"
    return f"MiSTerPlex-FPGA-{args.prototype.upper()}-{tier}-{rate}-filter-{args.filter}"


def subtitle_decision_summary(decision, requested_id):
    choices = {"burn", "copy", "transcode", "directplay", "directstream", "none", "ignore", "skip"}

    def normalized(value):
        return value if value in choices else "unspecified"

    decisions = sorted({normalized(n.attrib["subtitleDecision"]) for n in decision.iter()
                        if "subtitleDecision" in n.attrib})
    streams = []
    for n in decision.iter("Stream"):
        if n.attrib.get("streamType") != "3":
            continue
        identifier = n.attrib.get("id", "")
        streams.append({
            "id": int(identifier) if re.fullmatch(r"[0-9]{1,20}", identifier) else None,
            "selected": {"0": False, "1": True}.get(n.attrib.get("selected")),
            "decision": normalized(n.attrib.get("decision")),
        })
    confirmed = requested_id is not None and any(
        s["id"] == requested_id and s["selected"] is not False and
        (s["decision"] == "burn" or (s["selected"] is True and "burn" in decisions))
        for s in streams)
    return {"requested_subtitle_stream_id": requested_id, "subtitle_decisions": decisions,
            "subtitle_streams": streams, "requested_burn_confirmed": confirmed,
            "subtitle_visual_verified": False}


class LabSubtitleSelection:
    """Explicit, reversible per-Part preference change on one new lab fixture."""

    def __init__(self, scope_path, key, requested_id, source, work):
        scope = json.loads(Path(scope_path).read_text())
        need(isinstance(scope, dict) and scope.get("allow_temporary_part_subtitle_selection") is True,
             "lab fixture scope must explicitly authorize temporary per-Part selection")
        for name in ("rating_key", "part_id", "subtitle_stream_id"):
            need(type(scope.get(name)) is int and scope[name] > 0, "invalid lab fixture scope identifier")
        need(scope["rating_key"] not in {36, 37, 38, 40, 139, 143, 145},
             "older captured/user items are excluded from lab subtitle selection")
        need(key == f'/library/metadata/{scope["rating_key"]}' and
             requested_id in (None, scope["subtitle_stream_id"]), "lab fixture scope/key/stream mismatch")
        filename = scope.get("container_file", "")
        need(isinstance(filename, str) and filename.startswith("/") and
             ".." not in Path(filename).parts and "rgb601" in Path(filename).name.lower(),
             "scope must identify the exact new RGB601 fixture file")
        digest = scope.get("source_sha256", "")
        need(isinstance(digest, str) and re.fullmatch(r"[0-9a-f]{64}", digest),
             "scope needs the operator-verified source SHA256 from import")
        self.scope, self.work, self.requested_id = scope, work, requested_id
        self.original = self.selected(source)
        self.restore_required = False
        self.record = {
            "scope": "temporary persistent per-Part preference; NOT session-scoped",
            "rating_key": scope["rating_key"], "part_id": scope["part_id"],
            "container_file": filename, "source_sha256_at_import": digest,
            "source_hash_rechecked_by_harness": False,
            "original_selected_subtitle": self.original, "requested_subtitle": requested_id,
            "selection_attempted": False, "selection_verified": False,
            "restore_required": False, "restore_attempted": False, "restore_verified": False,
        }

    def selected(self, source):
        item = source.find(".//Video")
        need(item is not None and item.attrib.get("ratingKey") == str(self.scope["rating_key"]),
             "lab fixture metadata identity changed")
        media = item.findall("Media")
        need(len(media) == 1 and media[0].attrib.get("videoCodec") == "mpeg2video",
             "lab selection requires one MPEG2 media version")
        parts = media[0].findall("Part")
        need(len(parts) == 1 and parts[0].attrib.get("id") == str(self.scope["part_id"]) and
             parts[0].attrib.get("file") == self.scope["container_file"],
             "lab fixture Part/file identity mismatch")
        streams = [s for s in parts[0].findall("Stream") if s.attrib.get("streamType") == "3"]
        need(any(s.attrib.get("id") == str(self.scope["subtitle_stream_id"]) for s in streams),
             "scoped lab subtitle is absent from Part")
        selected = [s.attrib.get("id", "") for s in streams if s.attrib.get("selected") == "1"]
        need(len(selected) <= 1 and all(re.fullmatch(r"[0-9]{1,20}", s) for s in selected),
             "ambiguous original subtitle selection")
        return int(selected[0]) if selected else 0

    def save(self):
        target = self.work / "subtitle-selection.json"
        pending = self.work / "subtitle-selection.json.next"
        with pending.open("w") as output:
            json.dump(self.record, output, indent=2)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        pending.replace(target)

    def put(self, request, stream_id):
        query = urllib.parse.urlencode({"subtitleStreamID": stream_id, "allParts": "0"})
        with request(f'/library/parts/{self.scope["part_id"]}?{query}', method="PUT") as response:
            response.read(1024)

    def select(self, request, reload_source):
        if self.requested_id is None:
            return
        if self.original != self.requested_id:
            self.record.update(selection_attempted=True, restore_required=True)
            self.save()  # Persist recovery data before even an ambiguously successful PUT.
            self.restore_required = True
            self.put(request, self.requested_id)
            need(self.selected(reload_source()) == self.requested_id,
                 "lab subtitle selection was not confirmed by fresh metadata")
        self.record["selection_verified"] = True
        self.save()

    def restore(self, request, reload_source):
        if not self.restore_required:
            return
        self.record["restore_attempted"] = True
        try:
            current = self.selected(reload_source())
            need(current in (self.original, self.requested_id),
                 "lab subtitle selection changed concurrently; do not overwrite another selection")
            if current != self.original:
                self.put(request, self.original)
            need(self.selected(reload_source()) == self.original, "lab subtitle restore verification failed")
            self.record["restore_verified"] = True
        except (Failure, OSError, http.client.HTTPException, ET.ParseError) as error:
            self.record["restore_error_type"] = type(error).__name__
            raise
        finally:
            self.save()


def capture(args, work, values):
    base = values.get("PLEX_BASE", "").rstrip("/")
    token = values.get("PLEX_TOKEN", "")
    key = values.get("MISTERPLEX_BASELINE_KEY", values.get("PLEX_KEY", ""))
    if not base or not token or not key:
        print("SKIP-NOT-PASS: need PLEX_BASE, existing secret-safe PLEX_TOKEN/config, "
              "and MISTERPLEX_BASELINE_KEY; no live check performed", file=sys.stderr)
        return 77
    parsed = urllib.parse.urlsplit(base)
    need(parsed.scheme in ("http", "https") and parsed.hostname and not parsed.username and
         not parsed.password and not parsed.query and not parsed.fragment, "invalid PMS base")
    need(key.startswith("/library/metadata/") and key.rsplit("/", 1)[1].isdigit(),
         "key must be local /library/metadata/N")
    need("\r" not in token and "\n" not in token, "invalid token")
    session = "mplex-p1-" + uuid.uuid4().hex
    headers = {"X-Plex-Token": token, "X-Plex-Client-Identifier": session,
               "X-Plex-Client-Profile-Name": profile_name(args), "X-Plex-Product": "MiSTerPlex",
               "X-Plex-Version": "1", "X-Plex-Platform": "Linux", "X-Plex-Device": "MiSTer",
               "X-Plex-Provides": "player"}
    opener = urllib.request.build_opener(NoRedirect, urllib.request.ProxyHandler({}))

    def request(path, timeout=10, method="GET"):
        return opener.open(urllib.request.Request(base+path, headers=headers, method=method), timeout=timeout)

    def xml(path):
        with request(path) as r:
            body = r.read(4*1024*1024+1)
        need(len(body) <= 4*1024*1024, "PMS metadata response too large")
        return ET.fromstring(body)

    source = xml(key)
    item = source.find(".//Video")
    need(item is not None, "metadata is not a video")
    media = item.find("Media")
    need(media is not None, "metadata contains no media")
    if args.subtitle_stream_id is not None:
        need(any(s.attrib.get("streamType") == "3" and
                 s.attrib.get("id") == str(args.subtitle_stream_id)
                 for s in media.findall(".//Stream")),
             "requested subtitle stream is absent from selected media")
    wanted = {"videoCodec", "audioCodec", "width", "height", "aspectRatio", "videoFrameRate",
              "bitrate", "duration", "container"}
    metadata = {"ratingKey": key.rsplit("/", 1)[1],
                "media": {k: v for k, v in media.attrib.items() if k in wanted},
                "streams": [{k: v for k, v in stream.attrib.items() if k in {
                    "id", "streamType", "codec", "profile", "width", "height", "frameRate",
                    "bitDepth", "selected", "forced", "key"} and k != "key"}
                    for stream in media.findall(".//Stream")],
                "subtitle_stream_id": args.subtitle_stream_id,
                "subtitle_burn_requested": args.subtitle_stream_id is not None}
    (work / "source.json").write_text(json.dumps(metadata, indent=2) + "\n")
    required_codec = getattr(args, "require_source_codec", None)
    need(required_codec is None or media.attrib.get("videoCodec") == required_codec,
         "source codec does not match the bounded campaign requirement")
    scope_path = getattr(args, "lab_fixture_scope", None)
    selection = (LabSubtitleSelection(scope_path, key, args.subtitle_stream_id, source, work)
                 if scope_path else None)
    params = {"hasMDE": "1", "path": key, "mediaIndex": "0", "partIndex": "0",
              "protocol": "http", "container": "mpegts", "fastSeek": "1",
              "directPlay": "0", "directStream": "0", "directStreamAudio": "0",
              "location": "lan", "copyts": "1", "session": session, "offset": str(args.offset),
              "videoQuality": "60", "videoResolution": "320x240", "maxVideoBitrate": "4000",
              "videoCodec": "h264", "audioCodec": "aac", "audioChannels": "2",
              "videoProfile": "baseline", "videoLevel": "30", "videoFrameRate": args.fps,
              "subtitles": "none"}
    if args.subtitle_stream_id is not None:
        params.update(subtitles="burn", subtitleStreamID=str(args.subtitle_stream_id),
                      subtitleSize="100")
    query = urllib.parse.urlencode(params)
    prefix = "/video/:/transcode/universal/"
    proc = None
    received = 0
    stage = "decision"
    failure_record = None
    try:
        if selection is not None:
            stage = "lab-subtitle-select"
            selection.select(request, lambda: xml(key))
        stage = "decision"
        decision = xml(prefix+"decision?"+query)
        errors = [n for n in decision.iter() if
                  n.attrib.get("code", "").isdigit() and int(n.attrib["code"]) >= 400]
        need(not errors, "PMS decision rejected transcode")
        subtitle_state = subtitle_decision_summary(decision, args.subtitle_stream_id)
        (work / "decision.json").write_text(json.dumps(subtitle_state, indent=2)+"\n")
        if args.subtitle_stream_id is not None:
            need(subtitle_state["requested_burn_confirmed"],
                 "PMS decision does not confirm requested subtitle selection/burn; "
                 "see decision.json and any scoped subtitle-selection.json")
        else:
            need("burn" not in subtitle_state["subtitle_decisions"] and
                 not any(s["decision"] == "burn" for s in subtitle_state["subtitle_streams"]),
                 "PMS decision burns subtitles despite explicit subtitles=none")
        if getattr(args, "decision_only", False):
            print("SKIP-NOT-PASS: decision-only diagnostic; no emitted bytes or qualification",
                  file=sys.stderr)
            return 77
        stage = "stream-open"
        with request(prefix+"start.mp4?"+query, timeout=args.stream_read_timeout) as stream:
            content_type = stream.headers.get("Content-Type", "").split(";", 1)[0].lower()
            need(content_type in ("video/mp2t", "video/mpegts", "application/octet-stream"),
                 "PMS did not return the requested MPEG-TS body")
            env = os.environ.copy()
            env.pop("PLEX_TOKEN", None)
            with (work / "capture.log").open("wb") as log, \
                    (work / "delivered-network.ts").open("wb") as raw:
                proc = subprocess.Popen([
                    "ffmpeg", "-v", "error", "-nostdin", "-i", "pipe:0", "-map", "0:v:0",
                    "-map", "0:a:0?", "-c", "copy", "-t", str(args.seconds), "-f", "mpegts",
                    "-y", str(work / "delivered.ts")], stdin=subprocess.PIPE,
                    stdout=subprocess.DEVNULL, stderr=log, env=env, bufsize=0)
                begin = time.monotonic()
                with capture_deadline():
                    while proc.poll() is None:
                        remaining = CAPTURE_WALL_SECONDS-(time.monotonic()-begin)
                        need(remaining > 0, "capture wall-time limit")
                        stage = "stream-first-read" if received == 0 else "stream-read"
                        chunk = read_stream_piece(stream, min(32768, MAX_BYTES-received+1),
                                                  min(args.stream_read_timeout, remaining))
                        if not chunk:
                            need(stream.length in (None, 0),
                                 "PMS HTTP response ended before declared body length")
                            break
                        received += len(chunk)
                        need(received <= MAX_BYTES, "capture64MiB limit")
                        stage = "capture-preserve"
                        raw.write(chunk)
                        raw.flush()
                        stage = "demux-feed"
                        try:
                            pending = memoryview(chunk)
                            while pending:
                                written = proc.stdin.write(pending)
                                need(written is not None and written > 0, "demux pipe did not accept data")
                                pending = pending[written:]
                        except BrokenPipeError:
                            break
                stage = "demux-finalize"
                try:
                    proc.stdin.close()
                except BrokenPipeError:
                    pass
                need(proc.wait(timeout=20) == 0, "PMS response could not be demuxed")
        stage = "encoder-telemetry"
        try:
            sessions = xml("/transcode/sessions")
            fields = {"videoCodec", "audioCodec", "videoDecision", "audioDecision", "sourceVideoCodec",
                      "transcodeHwRequested", "transcodeHwEncoding", "transcodeHwDecoding",
                      "transcodeHwFullPipeline", "width", "height", "videoFrameRate", "speed"}
            state = [{k: v for k, v in n.attrib.items() if k in fields}
                     for n in sessions.iter() if n.attrib.get("key", "").endswith(session) or
                     n.attrib.get("session") == session or n.attrib.get("sessionId") == session]
            (work / "encoder-session.json").write_text(json.dumps(state, indent=2)+"\n")
        except (OSError, http.client.HTTPException, ET.ParseError, Failure):
            (work / "encoder-session.json").write_text('{"encoder_telemetry":"unavailable"}\n')
    except (Failure, OSError, http.client.HTTPException, ET.ParseError, subprocess.TimeoutExpired) as error:
        failure_record = {
            "stage": stage, "error_type": type(error).__name__,
            "detail": str(error) if isinstance(error, Failure) else type(error).__name__,
            "received_network_bytes": received,
            "capture_byte_limit": MAX_BYTES, "capture_wall_limit_seconds": CAPTURE_WALL_SECONDS,
            "stream_read_timeout_seconds": args.stream_read_timeout,
            "requested_seconds": args.seconds, "capture_complete": False,
            "syntax_evaluated": False, "encoded_profile_rejection": False, "retry_performed": False,
        }
        raise
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                if failure_record is not None:
                    failure_record["demux_stop_timeout"] = True
        if proc is not None and not proc.stdin.closed:
            proc.stdin.close()
        stop_ok = False
        try:
            with request(prefix+"stop?"+urllib.parse.urlencode({"session": session})) as r:
                r.read(1024)
            stop_ok = True
        except (OSError, http.client.HTTPException, Failure):
            pass
        restore_failed = False
        if selection is not None:
            try:
                selection.restore(request, lambda: xml(key))
            except (Failure, OSError, http.client.HTTPException, ET.ParseError):
                restore_failed = True
                print("FAIL: scoped lab subtitle restoration failed; inspect subtitle-selection.json",
                      file=sys.stderr)
        if failure_record is not None:
            failure_record["own_session_stop_attempted"] = True
            failure_record["own_session_stop_request_succeeded"] = stop_ok
            failure_record["own_session_absence_verified"] = False
            if selection is not None:
                failure_record["lab_subtitle_restore_required"] = selection.restore_required
                failure_record["lab_subtitle_restore_verified"] = selection.record["restore_verified"]
            network = work / "delivered-network.ts"
            try:
                failure_record["preserved_network_bytes"] = network.stat().st_size if network.exists() else 0
            except OSError:
                failure_record["preserved_network_bytes"] = None
            try:
                (work / "failure-result.json").write_text(json.dumps(failure_record, indent=2)+"\n")
            except OSError:
                print("FAIL: could not persist capture failure diagnostics", file=sys.stderr)
        need(not restore_failed, "lab subtitle preference restoration is unverified; manual repair required")
    return 0


def main():
    parser = SafeParser(description=__doc__)
    parser.add_argument("--prototype", choices=("idr", "ip"), default="ip")
    parser.add_argument("--mode", choices=("240p", "480p", "480i", "720p"), default="240p")
    parser.add_argument("--fps", choices=("24", "24/1", "24000/1001"), default="24")
    parser.add_argument("--filter", choices=("on", "off"), default="on")
    parser.add_argument("--seconds", type=int, default=8)
    parser.add_argument("--stream-read-timeout", type=int, default=30,
                        help="single-request streaming read timeout in seconds,1..60; no retries")
    parser.add_argument("--offset", type=int, default=0)
    parser.add_argument("--subtitle-stream-id", type=int)
    parser.add_argument("--require-source-codec", choices=("mpeg2video",),
                        help="reject a mismatched campaign source before decision/start")
    parser.add_argument("--lab-fixture-scope",
                        help="explicit allowlist for temporary new-lab-fixture-only Part subtitle selection")
    parser.add_argument("--decision-only", action="store_true",
                        help="metadata/decision diagnostic only; never starts a stream or qualifies bytes")
    parser.add_argument("--output", default="build/pms-profile-capture")
    parser.add_argument("--input-ts", help="offline analysis only; cannot claim PMS provenance")
    parser.add_argument("--reserved-experiment", action="store_true")
    parser.add_argument("--require-full-size", action="store_true")
    parser.add_argument("--require-limited-bt601", action="store_true",
                        help="require proven limited-range BT.601 matrix signaling for fixed scanout")
    parser.add_argument("--allow-rate-conversion", action="store_true",
                        help="explicit non-native-rate experiment, never native film-rate qualification")
    parser.add_argument("--max-au-bytes", type=int,
                        help="default from shared transport ABI; lower to negotiated encoded-AU capacity")
    parser.add_argument("--max-vcl-rbsp-bytes", type=int,
                        help="independent decoder RAM limit after VCL de-escaping")
    args = parser.parse_args()
    try:
        need(4 <= args.seconds <= 30 and args.offset >= 0, "seconds must be4..30 and offset nonnegative")
        need(1 <= args.stream_read_timeout <= 60, "stream read timeout must be1..60 seconds")
        need(args.subtitle_stream_id is None or args.subtitle_stream_id >= 0, "subtitle stream id")
        need(not args.decision_only or not args.input_ts, "decision-only requires a live PMS request")
        need(args.require_source_codec is None or not args.input_ts,
             "source-codec requirement needs live source metadata")
        need(not args.lab_fixture_scope or not args.input_ts, "lab fixture scope requires live metadata")
        need(args.mode == "240p" or (args.reserved_experiment and args.input_ts),
             "480/720 RESERVED: only explicit offline analysis, no larger PMS profile advertised")
        need(not Path(args.output).is_absolute() and ".." not in Path(args.output).parts,
             "output must be a project-relative directory")
        for tool in ("ffmpeg", "ffprobe"):
            if not shutil.which(tool):
                print(f"SKIP-NOT-PASS: {tool} required", file=sys.stderr)
                return 77
        need((ROOT / "build/pms_baseline_probe").is_file(), "build/pms_baseline_probe is missing")
        try:
            contract = json.loads(process([str(ROOT / "build/pms_baseline_probe"), "--probe-contract"]))
        except (Failure, ValueError):
            raise Failure("profile harness/probe ABI mismatch; rebuild or use a verified frozen harness")
        need(isinstance(contract, dict) and contract.get("abi") == PROBE_ABI and
             isinstance(contract.get("max_au_bytes"), int) and contract["max_au_bytes"] > 0,
             "profile harness/probe ABI mismatch; rebuild or use a verified frozen harness")
        args.transport_max_au_bytes = contract["max_au_bytes"]
        args.consumer_limit_explicit = args.max_au_bytes is not None
        if args.max_au_bytes is None:
            args.max_au_bytes = args.transport_max_au_bytes
        need(0 < args.max_au_bytes <= args.transport_max_au_bytes,
             f"AU limit must be1..{args.transport_max_au_bytes} bytes")
        need(args.max_vcl_rbsp_bytes is None or
             0 < args.max_vcl_rbsp_bytes <= args.transport_max_au_bytes,
             "VCL RBSP limit must be positive and inside the transport envelope")
        values = config() if not args.input_ts else {}
        if not args.input_ts and not all((values.get("PLEX_BASE"), values.get("PLEX_TOKEN"),
                values.get("MISTERPLEX_BASELINE_KEY", values.get("PLEX_KEY")))):
            print("SKIP-NOT-PASS: missing PLEX_BASE, secret-safe PLEX_TOKEN/config, or "
                  "MISTERPLEX_BASELINE_KEY", file=sys.stderr)
            return 77
        work = Path(args.output)
        need(not work.exists() or not any(work.iterdir()), "output must be new/empty; preserve previous experiments")
        work.mkdir(parents=True, exist_ok=True)
        if args.input_ts:
            need(Path(args.input_ts).stat().st_size <= MAX_BYTES, "input exceeds64MiB")
            shutil.copyfile(args.input_ts, work / "delivered.ts")
        else:
            rc = capture(args, work, values)
            if rc:
                return rc
        analyze(args, work)
        return 0
    except (Failure, OSError, http.client.HTTPException, ValueError, KeyError,
            ET.ParseError, subprocess.TimeoutExpired) as error:
        # Never stringify network exceptions: they can contain authentication material.
        detail = str(error) if isinstance(error, Failure) else type(error).__name__
        print("FAIL pms_profile_experiment: " + detail, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
