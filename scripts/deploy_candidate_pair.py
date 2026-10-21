#!/usr/bin/env python3
"""Explicit prebuilt pair deployment, adapted from deploy_frozen_baseline.py.

Retains its hash-pinned rollback, SSH bash-on-stdin transport, bounded TERM,
and observed Menu/Plex sequence. Never enters either legacy deploy body.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
from urllib.parse import urlsplit
import uuid

ROOT = Path(__file__).resolve().parents[1]
FROZEN_RBF_SHA = "9d4977936d1b1a3420a3e97df976058573e70a34fd0c8917f2d785a5fe0d07cf"
FROZEN_ARM_SHA = "acfa03d762833994874b13a8aad4f734cf2f5649a3759238764917b6e3b77e7c"
BASE = "/media/fat/misterplex"
FROZEN_RBF = "/media/fat/_Utility/Plex_480p.rbf"
FROZEN_ARM = BASE + "/bin/misterplexd.480p"
HELPERS = {
    BASE + "/" + directory + "/" + name
    for directory in ("bin", "scripts")
    for name in ("misterplex_core_watch.sh", "misterplexd_supervise.sh")
}
OVERLAY = {
    "MPX_VIDEO_BACKEND": "fpga-h264", "MPX_H264_PROTOTYPE": "idr",
    "MPX_H264_FILTER": "off", "PRESENT": "fpga", "AUDIO": "on",
    "OSD_CONTROL": "0", "DECODE": "320x240", "TRANSCODE_PROFILE": "240p",
    "STREAM": "0", "SUBTITLES": "off", "SUBTITLE_STREAM": "0",
    "AUTO_NEXT": "0", "SOURCE_FPS": "auto", "AV_CONTENT_FPS": "auto",
    "MATCH_SOURCE_HZ": "off",
}


class Refused(RuntimeError):
    pass


def sha(data):
    return hashlib.sha256(data).hexdigest()


def exact_keys(value, keys, label):
    if not isinstance(value, dict) or set(value) != set(keys):
        raise Refused(f"{label}: missing or unknown fields")


def hex_value(value, length, label):
    if not isinstance(value, str) or not re.fullmatch("[0-9a-f]{" + str(length) + "}", value):
        raise Refused(f"{label}: require full lowercase hexadecimal identity")
    if value == "0" * length:
        raise Refused(f"{label}: zero identity is not approval")
    return value


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise Refused("duplicate JSON field")
        result[key] = value
    return result


def parse_json(data):
    return json.loads(data, object_pairs_hook=unique_object)


def artifact(item, directory, label):
    exact_keys(item, ("path", "sha256"), label)
    expected = hex_value(item["sha256"], 64, label)
    if not isinstance(item["path"], str) or not item["path"]:
        raise Refused(f"{label}: missing local path")
    path = directory / item["path"]
    if not path.is_file():
        raise Refused(f"{label}: local artifact unavailable")
    data = path.read_bytes()
    if not data or sha(data) != expected:
        raise Refused(f"{label}: local SHA256 mismatch")
    return data


def require_static_arm(data):
    if (len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01" or
            struct.unpack_from("<HH", data, 16) != (2, 40)):
        raise Refused("arm: require a little-endian ARM ELF32 executable")
    offset = struct.unpack_from("<I", data, 28)[0]
    size, count = struct.unpack_from("<HH", data, 42)
    if size < 32 or not count or offset + size * count > len(data):
        raise Refused("arm: invalid ELF program headers")
    for index in range(count):
        if struct.unpack_from("<I", data, offset + size * index)[0] in (2, 3):
            raise Refused("arm: dynamic linkage/interpreter is not a static candidate")


def validate_manifest(path, root=ROOT):
    document = parse_json(path.read_bytes())
    exact_keys(document, ("schema", "approved_build_id", "rbf", "arm", "build_inputs",
                          "build_result", "current", "owned_helpers", "plex_base"), "manifest")
    if type(document["schema"]) is not int or document["schema"] != 1:
        raise Refused("manifest: unsupported schema")
    build_id = hex_value(document["approved_build_id"], 8, "approved_build_id")
    payload = {key: artifact(document[key], path.parent, key)
               for key in ("rbf", "arm", "build_inputs", "build_result")}
    require_static_arm(payload["arm"])
    inputs = parse_json(payload["build_inputs"])
    result = parse_json(payload["build_result"])
    if not isinstance(inputs, dict) or inputs.get("fpga_video_build_id") != build_id:
        raise Refused("approved build ID does not match pinned inputs.json")
    if not isinstance(result, dict) or result.get("rbf_sha256") != document["rbf"]["sha256"]:
        raise Refused("pinned build result does not identify the approved RBF")
    current = document["current"]
    exact_keys(current, ("rbf_path", "rbf_sha256", "arm_path", "arm_sha256",
                         "config_path"), "current")
    for key in ("rbf_sha256", "arm_sha256"):
        hex_value(current[key], 64, "current." + key)
    if current["rbf_path"] == FROZEN_RBF:
        if (current["rbf_sha256"] != FROZEN_RBF_SHA or current["arm_sha256"] != FROZEN_ARM_SHA
                or current["arm_path"] != BASE + "/bin/misterplexd"
                or current["config_path"] != BASE + "/misterplex.conf"):
            raise Refused("current: frozen rollback pairing cannot be changed")
    else:
        candidate = re.fullmatch(re.escape(BASE) +
                                 r"/candidates/[0-9a-f]{8}-[0-9a-f]{32}/Plex\.rbf",
                                 str(current["rbf_path"]))
        directory = str(current["rbf_path"]).removesuffix("/Plex.rbf")
        if (not candidate or current["arm_path"] != directory + "/misterplexd" or
                current["config_path"] != directory + "/misterplex.conf"):
            raise Refused("current: only the frozen pair or an explicit prior candidate is allowed")
    helpers = document["owned_helpers"]
    if not isinstance(helpers, list):
        raise Refused("owned_helpers: require an explicit list (possibly empty)")
    seen = set()
    for helper in helpers:
        exact_keys(helper, ("path", "sha256"), "owned helper")
        if helper["path"] not in HELPERS or helper["path"] in seen:
            raise Refused("owned helper: unknown or duplicate path")
        seen.add(helper["path"])
        hex_value(helper["sha256"], 64, "owned helper")
    url = urlsplit(document["plex_base"])
    if (url.scheme != "http" or not ipaddress.IPv4Address(url.hostname).is_private or
            url.port != 32400 or url.path not in ("", "/") or
            url.username or url.password or url.query or url.fragment):
        raise Refused("plex_base: require a token-free private IPv4 LAN PMS URL on port 32400")
    overlay = {"PLEX_BASE": document["plex_base"].rstrip("/"), **OVERLAY}
    payload["overlay"] = "".join(f"{key}={value}\n" for key, value in overlay.items()).encode()
    # Use the actual repository ban, never an environment-selected replacement.
    ban = root / "tests/unit/test_phase1a_rbf_ban.sh"
    ban_list = root / "tests/unit/phase1a_rbf_ban.txt"
    if not ban.is_file() or not ban_list.is_file():
        raise Refused("historical RBF ban is unavailable")
    env = os.environ.copy()
    env["PHASE1A_RBF_BAN_LIST"] = str(ban_list)
    for digest in (sha(payload["rbf"]), hashlib.md5(payload["rbf"]).hexdigest()):
        checked = subprocess.run(["bash", str(ban), digest], env=env, capture_output=True)
        if checked.returncode:
            raise Refused(f"historical RBF ban refused candidate (exit {checked.returncode})")
    return document, payload


def render_remote(document, payload, mode, transaction):
    if mode not in ("copy-only", "menu") or not re.fullmatch("[0-9a-f]{32}", transaction):
        raise Refused("invalid deployment action/transaction")
    current = document["current"]
    values = {
        "BASE": BASE, "PROC": "/proc", "MEDIA": "/media/fat", "CMD": "/dev/MiSTer_cmd",
        "LOG_BASE": "/run/misterplex-candidates",
        "MODE": mode, "BUILD_ID": document["approved_build_id"], "TRANSACTION": transaction,
        "RBF_SHA": sha(payload["rbf"]), "ARM_SHA": sha(payload["arm"]),
        "FROZEN_RBF_SHA": FROZEN_RBF_SHA, "FROZEN_ARM_SHA": FROZEN_ARM_SHA,
        "OLD_RBF": current["rbf_path"], "OLD_RBF_SHA": current["rbf_sha256"],
        "OLD_ARM": current["arm_path"], "OLD_ARM_SHA": current["arm_sha256"],
        "OLD_CONF": current["config_path"],
    }
    header = "#!/usr/bin/env bash\nset -euo pipefail\numask 077\n"
    header += "".join(f"readonly {key}={shlex.quote(value)}\n" for key, value in values.items())
    header += "declare -A HELPER_HASH=()\n"
    for helper in document["owned_helpers"]:
        header += f"HELPER_HASH[{shlex.quote(helper['path'])}]={helper['sha256']}\n"
    template = (ROOT / "scripts/deploy_candidate_remote.sh").read_text()
    template = template.replace("# INSERT_PIDFD_HELPER", (ROOT / "scripts/candidate_pidfd.py").read_text())
    uploads = []
    names = {"rbf": "Plex.rbf", "arm": "misterplexd", "build_inputs": "inputs.json",
             "build_result": "result.json", "overlay": "overlay.conf"}
    for index, (key, name) in enumerate(names.items()):
        encoded = base64.encodebytes(payload[key]).decode("ascii")
        uploads.append(f'base64 -d > "$STAGE/{name}.part" <<\'MPX_BYTES_{index}\'\n'
                       + encoded + f'MPX_BYTES_{index}\n'
                       + f'check_sha "$STAGE/{name}.part" {sha(payload[key])}\n'
                       + f'mv "$STAGE/{name}.part" "$STAGE/{name}"\n')
    return header + template.replace("# INSERT_VERIFIED_UPLOADS", "".join(uploads))


class SSH:
    """Frozen-baseline transport: fixed bash -s, stdin payload, secret-safe errors."""
    def run(self, script):
        host = os.environ.get("MISTER_HOST", "")
        user = os.environ.get("MISTER_USER", "root")
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.-]*", host) or user != "root":
            raise Refused("candidate transport requires a known host and root device owner")
        env = os.environ.copy()
        env["SSHPASS"] = os.environ["MISTER_PASS"]
        # Large upload heredocs can backpressure the ARM board beyond nine seconds.
        command = ["sshpass", "-e", "ssh", "-o", "StrictHostKeyChecking=yes",
                   "-o", "ConnectTimeout=6", "-o", "ServerAliveInterval=15",
                   "-o", "ServerAliveCountMax=3", f"{user}@{host}", "bash", "-s"]
        return subprocess.run(command, input=script.encode(), env=env, capture_output=True)


def deploy(path, mode, transport=None):
    document, payload = validate_manifest(path)
    script = render_remote(document, payload, mode, uuid.uuid4().hex)
    result = (transport or SSH()).run(script)
    # Never relay raw SSH output, config contents, environment, or daemon logs.
    lines = result.stdout.decode(errors="replace").splitlines()
    safe = [line for line in lines
            if re.fullmatch(r"CANDIDATE_(?:ERROR|STAGED|READY|PHASE|LOG) [a-zA-Z0-9_./:=-]+", line)]
    for line in safe:
        print(line)
    expected = "CANDIDATE_STAGED " if mode == "copy-only" else "CANDIDATE_READY "
    if result.returncode or not any(line.startswith(expected) for line in safe):
        raise Refused(f"remote transaction failed (exit {result.returncode}); no automatic recovery")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--mode", choices=("copy-only", "menu"), required=True)
    args = parser.parse_args(argv)
    try:
        deploy(args.manifest, args.mode)
    except (Refused, OSError, ValueError, TypeError, KeyError, struct.error) as error:
        # OS/JSON errors can contain untrusted input or paths; report only controlled messages.
        detail = str(error) if isinstance(error, Refused) else type(error).__name__
        print("CANDIDATE_REFUSED: " + detail, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
