#!/usr/bin/env python3
"""Prebuilt deployment policy on a fake card/proc tree; never contacts MiSTer."""
import contextlib
import errno
import fcntl
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import unittest
from unittest import mock
import uuid

ROOT = Path(__file__).resolve().parents[2]
TEST_FILE = Path(__file__).resolve()
spec = importlib.util.spec_from_file_location("candidate_deploy", ROOT / "scripts/deploy_candidate_pair.py")
deploy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)
pidfd_spec = importlib.util.spec_from_file_location("candidate_pidfd", ROOT / "scripts/candidate_pidfd.py")
pidfd = importlib.util.module_from_spec(pidfd_spec)
pidfd_spec.loader.exec_module(pidfd)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def elf(marker):
    data = bytearray(84)
    data[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HH", data, 16, 2, 40)
    struct.pack_into("<I", data, 28, 52)
    struct.pack_into("<HH", data, 42, 32, 1)
    struct.pack_into("<I", data, 52, 1)
    return bytes(data) + marker


def proc_file(root, pid, binary, args, parent=1, start=12345, comm=None):
    directory = root / "proc" / str(pid)
    directory.mkdir(exist_ok=True)
    (directory / "fd").mkdir(exist_ok=True)
    (directory / "cmdline").write_bytes(b"".join(str(arg).encode() + b"\0" for arg in args))
    name = comm or binary.name
    (directory / "comm").write_text(name + "\n")
    fields = ["R", str(parent)] + ["0"] * 17 + [str(start)]
    (directory / "stat").write_text(f"{pid} ({name}) " + " ".join(fields) + "\n")
    (directory / "status").write_text("Uid:\t0\t0\t0\t0\n")
    if not (directory / "maps").exists():
        (directory / "maps").write_text("")
    if not (directory / "exe").is_symlink():
        (directory / "exe").symlink_to(binary)
    if not (directory / "cwd").is_symlink():
        (directory / "cwd").symlink_to(root / "media/fat/misterplex")


def listen(root, pid):
    (root / "proc/net/tcp").write_text("header\n 0: 00000000:0BBD 00000000:0000 0A 0 0 0 0 0 76543\n")
    (root / "proc" / str(pid) / "fd/7").symlink_to("socket:[76543]")


class FakePidfds:
    """The handle binds a fixture process generation, not subsequent PID lookups."""
    def __init__(self, root):
        self.root = root
        self.trace = []
        self.before_send = None
        self.after_open = None

    def open(self, pid):
        self.pid = pid
        directory = self.root / "proc" / str(pid)
        if not directory.exists():
            raise ProcessLookupError(errno.ESRCH, "fixture process exited")
        self.generation = directory.stat().st_ino
        self.handle = 700000 + pid
        self.trace.append(("open", pid, self.handle))
        if self.after_open:
            self.after_open()
        return self.handle

    def exited(self, handle, timeout_ms=0):
        assert handle == self.handle
        self.trace.append(("poll", handle, timeout_ms))
        directory = self.root / "proc" / str(self.pid)
        return not directory.exists() or directory.stat().st_ino != self.generation

    def send(self, handle, number):
        assert handle == self.handle
        assert number == signal.SIGTERM
        self.trace.append(("send", handle, number))
        if self.before_send:
            self.before_send()
        if self.exited(handle):
            raise ProcessLookupError(errno.ESRCH, "bound generation exited")
        fake_device(self.root, "term", str(self.pid))

    def close(self, handle):
        assert handle == self.handle
        self.trace.append(("close", handle))


def fake_main_load(root, state, argument):
    proc = root / "proc"
    main = state.get("main_pid", 100)
    handoff = state.get("main_handoff")
    if handoff:
        (proc / str(main)).rename(proc / ("retired-main-" + str(main)))
        if handoff == "missing":
            return
        replacement = main if handoff == "pid-reuse" else main + 1
        binary = root / "media/fat/MiSTer"
        if handoff == "changed-executable":
            binary.write_bytes(b"unapproved Main replacement")
        target = root / "media/fat/foreign.rbf" if handoff == "wrong-core" else argument
        proc_file(root, replacement, binary, [binary, target],
                  parent=900 if handoff == "foreign-parent" else 1,
                  start=19999 if handoff == "older-process" else 20000 + replacement)
        if handoff == "foreign-owner":
            (proc / str(replacement) / "status").write_text("Uid:\t1\t1\t1\t1\n")
        if handoff == "stopped":
            file = proc / str(replacement) / "stat"
            file.write_text(file.read_text().replace(") R ", ") T ", 1))
        if handoff == "duplicate":
            proc_file(root, replacement + 1000, binary, [binary, target], start=21000)
        state["main_pid"] = replacement
    else:
        (proc / str(main) / "cmdline").write_bytes(
            str(root / "media/fat/MiSTer").encode() + b"\0" + argument.encode() + b"\0")
    (root / "state.json").write_text(json.dumps(state))


def fake_device(root, action, *arguments):
    """Called only by test-injected bash functions; no real signals or core commands."""
    state = json.loads((root / "state.json").read_text())
    proc = root / "proc"
    if action == "audit-exit":
        (proc / "140").rename(proc / "retired-audit-140")
        return 0
    if action == "audit-close":
        (proc / "140/fd/6").unlink()
        return 0
    if action == "audit-reuse":
        proc_file(root, 140, Path("/bin/sh").resolve(), ["/bin/sh", "fixture"],
                  start=99999)
        return 0
    if action == "audit-zombie":
        file = proc / "140/stat"
        file.write_text(file.read_text().replace(") R ", ") Z ", 1))
        return 0
    if action == "finish-load":
        target = state.pop("pending_core", None)
        if target is not None:
            fake_main_load(root, state, target)
        return 0
    if action == "pidfd":
        if arguments == ("probe",):
            if state.get("pidfd_unsupported"):
                print("CANDIDATE_ERROR pidfd-unsupported-kernel")
                return 1
            return 0
        mode, number, start, executable, executable_hash, argv_hash = arguments
        assert mode == "stop"
        api = FakePidfds(root)
        try:
            pidfd.stop(int(number), start, tuple(map(int, executable.split(":"))),
                       executable_hash, argv_hash, api, str(proc))
        except pidfd.Refused as error:
            print("CANDIDATE_ERROR " + str(error))
            return 1
        finally:
            with (root / "handles").open("a") as record:
                record.write(json.dumps(api.trace) + "\n")
        return 0
    argument, = arguments
    with (root / "events").open("a") as events:
        events.write(action + " " + argument + "\n")
    if action == "term":
        pid = int(argument)
        if state.get("timeout_pid") == pid:
            return
        (proc / str(pid)).rename(proc / ("dead-" + str(pid)))
        for child in proc.glob("[0-9]*/stat"):
            text = child.read_text()
            head, tail = text.rsplit(") ", 1)
            fields = tail.split()
            if fields[1] == str(pid):
                fields[1] = "1"
                child.write_text(head + ") " + " ".join(fields) + "\n")
        if pid == 130:
            (proc / "net/tcp").write_text("header\n")
    elif action == "load":
        if not state.get("core_timeout"):
            if state.get("defer_main_handoff"):
                state["pending_core"] = argument
                (root / "state.json").write_text(json.dumps(state))
            else:
                fake_main_load(root, state, argument)
    elif action == "start":
        bundle = Path(argument)
        print("fixture daemon stdout")
        print("fixture daemon stderr", file=sys.stderr)
        if state.get("startup_failure"):
            print("fixture startup failure", file=sys.stderr)
            return 0
        binary = bundle / "misterplexd"
        proc_file(root, 501, binary, [binary, "--name", "MiSTerPlex", "--id",
                                    "misterplex-dev", "--port", "3005", "--conf",
                                    bundle / "misterplex.conf"], comm="mpx-main")
        listen(root, 501)
    else:
        raise AssertionError("unknown fake-device operation")


class FakeSSH:
    def __init__(self, case):
        self.case = case
        self.calls = []
        self.corrupt_stage = False
        self.http_dead = False
        self.mutate_local_arm = False
        self.native_send = False
        self.native_launch = False
        self.retire_main_on_stat = False
        self.main_stat_error = False
        self.audit_failure = None
        self.suffix = None

    def run(self, script):
        self.calls.append(script)
        root = self.case.work
        if self.mutate_local_arm:
            Path(self.case.document["arm"]["path"]).write_bytes(b"late unapproved replacement")
        script = script.replace("/media/fat", str(root / "media/fat"))
        script = script.replace("readonly PROC=/proc", "readonly PROC=" + shlex.quote(str(root / "proc")))
        script = script.replace("readonly CMD=/dev/MiSTer_cmd", "readonly CMD=" +
                                shlex.quote(str(root / "MiSTer_cmd")))
        script = script.replace("readonly LOG_BASE=/run/misterplex-candidates", "readonly LOG_BASE=" +
                                shlex.quote(str(root / "run/misterplex-candidates")))
        helper = f"{shlex.quote(sys.executable)} {shlex.quote(str(TEST_FILE))} --fake-device {shlex.quote(str(root))}"
        overrides = f"""
id() {{ printf '0\\n'; }}
sleep() {{ :; }}
kill() {{
    printf 'NUMERIC_SIGNAL_FORBIDDEN\\n'
    exit 98
}}
pidfd_action() {{ {helper} pidfd "$@"; }}
wget() {{ printf '<MediaContainer><Player machineIdentifier="misterplex-dev"/></MediaContainer>'; }}
"""
        if self.retire_main_on_stat or self.main_stat_error:
            action = "return 1" if self.main_stat_error else f"{helper} finish-load"
            overrides += f"""
stat() {{
    if [[ -n ${{MAIN_REQUEST_CORE:-}} && ${{3:-}} == "$PROC/$MAIN_PID/exe" ]]; then
        {action}
    fi
    command stat "$@"
}}
"""
        if self.audit_failure:
            is_fd = self.audit_failure.startswith("fd-")
            command = "readlink" if is_fd else "grep"
            target = "$PROC/140/fd/6" if is_fd else "$PROC/140/maps"
            action = ""
            if self.audit_failure in ("maps-exit", "fd-exit"):
                action = f"{helper} audit-exit"
            elif self.audit_failure == "fd-close":
                action = f"{helper} audit-close"
            elif self.audit_failure in ("maps-reuse", "maps-reuse-readable", "fd-reuse"):
                action = f"{helper} audit-reuse"
            elif self.audit_failure == "maps-zombie":
                action = f"{helper} audit-zombie"
            result = 'command grep "$@"' if self.audit_failure == "maps-reuse-readable" else "return 2"
            overrides += f"""
{command}() {{
    if [[ ${{MAIN_PID:-}} == 101 && ${{@: -1}} == "{target}" ]]; then
        {action or ':'}
        {result}
    fi
    command {command} "$@"
}}
"""
        if not self.native_launch:
            overrides += f"""
launch_candidate() {{
    {helper} start "$BUNDLE" >&"$DAEMON_LOG_FD" 2>&1
    LAUNCHED_PID=501
}}
"""
        if not self.native_send:
            overrides += f'send_core() {{ {helper} load "$1"; }}\n'
        if self.http_dead:
            overrides += "wget() { return 1; }\n"
        marker = "# Transaction begins here."
        if self.suffix is not None:
            script = script.split(marker)[0] + overrides + self.suffix
        else:
            script = script.replace(marker, overrides + "\n" + marker)
        if self.corrupt_stage:
            needle = 'check_sha "$STAGE/Plex.rbf.part"'
            script = script.replace(needle, 'printf corruption >> "$STAGE/Plex.rbf.part"\n' + needle)
        self.result = subprocess.run(["bash", "-s"], input=script.encode(),
                                     capture_output=True, cwd=root, timeout=30)
        return self.result


class CandidateDeploy(unittest.TestCase):
    def setUp(self):
        self.work = ROOT / "build" / ("candidate-policy-" + uuid.uuid4().hex)
        self.work.mkdir(parents=True)
        self.addCleanup(shutil.rmtree, self.work)
        self.base = self.work / "media/fat/misterplex"
        self.media = self.work / "media/fat"
        for directory in (self.base / "bin", self.base / "run", self.media / "_Utility",
                          self.work / "proc/net", self.work / "policy/tests/unit", self.work / "run"):
            directory.mkdir(parents=True, exist_ok=True)
        self.old_core = b"exact frozen rollback RBF"
        self.old_arm = elf(b"exact frozen rollback ARM")
        for name, data in {
            self.media / "_Utility/Plex_480p.rbf": self.old_core,
            self.base / "bin/misterplexd.480p": self.old_arm,
            self.base / "bin/misterplexd": self.old_arm,
            self.media / "menu.rbf": b"menu",
            self.media / "MiSTer": b"the owned Main executable",
            self.base / "bin/misterplex_core_watch.sh": b"known watcher",
            self.base / "bin/misterplexd_supervise.sh": b"known supervisor",
        }.items():
            name.write_bytes(data)
        self.config = (b"PLEX_TOKEN=REDACTED\n"
                       b"IGNORED=$(touch SHOULD_NOT_EXIST)\n"
                       b"  MPX_H264_FILTER = on\nMPX_H264_FILTER=on\n"
                       b"STREAM=1\nDECODE=640x480\n")
        (self.base / "misterplex.conf").write_bytes(self.config)
        (self.base / "run/candidate-deploy.lock").write_bytes(b"retained lock inode\n")
        self.lock_stat = (self.base / "run/candidate-deploy.lock").stat()
        (self.work / "state.json").write_text("{}")
        (self.work / "events").write_text("")
        os.mkfifo(self.work / "MiSTer_cmd")
        (self.work / "proc/net/tcp").write_text("header\n")
        (self.work / "proc/net/tcp6").write_text("header\n")
        (self.work / "proc/uptime").write_text("200.00 0.00\n")
        proc_file(self.work, 100, self.media / "MiSTer",
                  [self.media / "MiSTer", self.media / "_Utility/Plex_480p.rbf"])
        shell = Path("/bin/sh").resolve()
        proc_file(self.work, 110, shell, ["/bin/sh", self.base / "bin/misterplex_core_watch.sh"])
        proc_file(self.work, 120, shell, ["/bin/sh", self.base / "bin/misterplexd_supervise.sh"], parent=110)
        binary = self.base / "bin/misterplexd"
        proc_file(self.work, 130, binary, [binary, "--name", "MiSTerPlex", "--id",
                                         "misterplex-dev", "--port", "3005", "--conf",
                                         self.base / "misterplex.conf"], parent=120, comm="mpx-main")
        listen(self.work, 130)
        self.document = {
            "schema": 1, "approved_build_id": "1234abcd",
            "current": {"rbf_path": deploy.FROZEN_RBF, "rbf_sha256": digest(self.old_core),
                        "arm_path": deploy.BASE + "/bin/misterplexd", "arm_sha256": digest(self.old_arm),
                        "config_path": deploy.BASE + "/misterplex.conf"},
            "owned_helpers": [
                {"path": deploy.BASE + "/bin/" + name,
                 "sha256": digest((self.base / "bin" / name).read_bytes())}
                for name in ("misterplex_core_watch.sh", "misterplexd_supervise.sh")],
            "plex_base": "http://192.0.2.24:32400",
        }
        rbf = b"approved engineering RBF"
        payloads = {"rbf": rbf, "arm": elf(b"approved new ARM"),
                    "build_inputs": b'{"fpga_video_build_id":"1234abcd"}',
                    "build_result": json.dumps({"rbf_sha256": digest(rbf),
                                                "promotion": "NOT_AUTHORIZED"}).encode()}
        for key, data in payloads.items():
            file = self.work / key
            file.write_bytes(data)
            self.document[key] = {"path": str(file), "sha256": digest(data)}
        self.manifest = self.work / "approved.json"
        for name in ("test_phase1a_rbf_ban.sh", "phase1a_rbf_ban.txt"):
            shutil.copyfile(ROOT / "tests/unit" / name, self.work / "policy/tests/unit" / name)
        self.original_validate = deploy.validate_manifest
        patches = [
            mock.patch.object(deploy, "FROZEN_RBF_SHA", digest(self.old_core)),
            mock.patch.object(deploy, "FROZEN_ARM_SHA", digest(self.old_arm)),
            mock.patch.object(deploy, "validate_manifest",
                              lambda path: self.original_validate(path, root=self.work / "policy")),
        ]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)
        self.ssh = FakeSSH(self)

    def run_deploy(self, mode="menu"):
        self.manifest.write_text(json.dumps(self.document))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            deploy.deploy(self.manifest, mode, self.ssh)
        self.assertNotIn("fixture-secret", out.getvalue())
        return out.getvalue()

    def refused(self, mode="menu"):
        with self.assertRaises(deploy.Refused):
            self.run_deploy(mode)

    def events(self):
        return (self.work / "events").read_text().splitlines()

    def bundle(self):
        bundles = list((self.base / "candidates").glob("1234abcd-*/approved-identity"))
        self.assertEqual(len(bundles), 1)
        return bundles[0].parent

    def preserved(self):
        self.assertEqual((self.base / "misterplex.conf").read_bytes(), self.config)
        self.assertEqual((self.base / "bin/misterplexd").read_bytes(), self.old_arm)
        self.assertEqual((self.media / "_Utility/Plex_480p.rbf").read_bytes(), self.old_core)
        self.assertEqual((self.base / "bin/misterplexd.480p").read_bytes(), self.old_arm)
        self.assertEqual((self.base / "run/candidate-deploy.lock").stat().st_ino, self.lock_stat.st_ino)
        self.assertEqual((self.base / "run/candidate-deploy.lock").read_bytes(), b"retained lock inode\n")

    def private_log(self):
        logs = list((self.work / "run/misterplex-candidates").glob("*/misterplexd.log"))
        self.assertEqual(len(logs), 1)
        self.assertEqual(logs[0].stat().st_mode & 0o777, 0o600)
        self.assertEqual(logs[0].stat().st_uid, os.geteuid())
        self.assertEqual(logs[0].parent.stat().st_mode & 0o777, 0o700)
        return logs[0]

    def expected_process(self):
        directory = self.work / "proc/130"
        info = (directory / "exe").stat()
        return (130, "12345", (info.st_dev, info.st_ino, info.st_uid),
                digest((directory / "exe").read_bytes()), digest((directory / "cmdline").read_bytes()))

    def replace_process(self):
        old = self.work / "proc/130"
        binary = self.base / "bin/misterplexd"
        old.rename(self.work / "proc/replaced-130")
        proc_file(self.work, 130, binary, [binary, "replacement"], start=67890)

    def test_missing_wrong_short_and_zero_hashes_never_call_ssh(self):
        for field in ("rbf", "arm", "build_inputs", "build_result"):
            good = self.document[field].copy()
            for bad in ("", "12345678", "0" * 64, "1" * 64, None):
                with self.subTest(field=field, bad=bad):
                    self.document[field]["sha256"] = bad
                    self.refused()
                    self.assertEqual(self.ssh.calls, [])
            self.document[field] = good
        del self.document["rbf"]["sha256"]
        self.refused()
        self.assertEqual(self.ssh.calls, [])

    def test_missing_or_mismatching_approval_never_calls_ssh(self):
        for build_id in ("", "00000000", "1234abce"):
            self.document["approved_build_id"] = build_id
            self.refused()
        del self.document["approved_build_id"]
        self.refused()
        self.assertEqual(self.ssh.calls, [])

    def test_unbound_failed_fit_has_no_result_or_rbf(self):
        Path(self.document["build_result"]["path"]).unlink()
        self.refused()
        self.assertEqual(self.ssh.calls, [])

    def test_build_result_must_match_rbf_even_if_its_hash_is_approved(self):
        data = b'{"rbf_sha256":"wrong"}'
        Path(self.document["build_result"]["path"]).write_bytes(data)
        self.document["build_result"]["sha256"] = digest(data)
        self.refused()
        self.assertEqual(self.ssh.calls, [])

    def test_dynamic_or_non_arm_artifact_is_not_deployed(self):
        data = bytearray(elf(b"dynamic"))
        struct.pack_into("<I", data, 52, 3)
        Path(self.document["arm"]["path"]).write_bytes(data)
        self.document["arm"]["sha256"] = digest(data)
        self.refused()
        self.assertEqual(self.ssh.calls, [])

    def test_frozen_pair_restrictions_cannot_be_rebound(self):
        self.document["current"]["arm_sha256"] = self.document["arm"]["sha256"]
        self.refused()
        self.assertEqual(self.ssh.calls, [])

    def test_unknown_and_secret_manifest_fields_are_rejected(self):
        self.document["PLEX_TOKEN"] = "never-upload-this"
        self.refused()
        self.assertEqual(self.ssh.calls, [])

    def test_ban_uses_fixed_repository_list_despite_environment(self):
        ban = self.work / "policy/tests/unit/phase1a_rbf_ban.txt"
        with ban.open("a") as output:
            output.write("\n" + self.document["rbf"]["sha256"][:8] + "\n")
        empty = self.work / "empty-ban"
        empty.write_text("")
        with mock.patch.dict(os.environ, {"PHASE1A_RBF_BAN_LIST": str(empty)}):
            self.refused()
        self.assertEqual(self.ssh.calls, [])

    def test_copy_only_preserves_everything_and_does_not_execute_config(self):
        out = self.run_deploy("copy-only")
        self.assertIn("CANDIDATE_STAGED", out)
        self.assertNotIn("CANDIDATE_READY", out)
        self.assertEqual(self.events(), [])
        self.preserved()
        bundle = self.bundle()
        config = (bundle / "misterplex.conf").read_text()
        self.assertIn("PLEX_TOKEN=REDACTED", config)
        self.assertEqual(config.count("MPX_H264_FILTER="), 1)
        self.assertIn("MPX_H264_FILTER=off", config)
        self.assertIn("MPX_H264_PROTOTYPE=idr", config)
        self.assertFalse((self.work / "SHOULD_NOT_EXIST").exists())
        self.assertEqual((bundle / "rollback/current.conf").read_bytes(), self.config)
        self.assertEqual((bundle / "rollback/Plex_480p.rbf").read_bytes(), self.old_core)

    def test_staged_corruption_is_refused_before_any_stop(self):
        self.ssh.corrupt_stage = True
        self.refused()
        self.assertEqual(self.events(), [])
        self.assertIn(b"file-sha256-mismatch", self.ssh.result.stdout)

    def test_each_requested_core_can_handoff_to_a_new_pinned_main(self):
        (self.work / "state.json").write_text('{"main_handoff":"normal"}')
        self.assertIn("CANDIDATE_READY", self.run_deploy())
        self.assertTrue((self.work / "proc/102/stat").exists())
        self.assertEqual([line for line in self.events() if line.startswith("load ")],
                         ["load " + str(self.media / "menu.rbf"),
                          "load " + str(self.bundle() / "Plex.rbf")])
        self.preserved()

    def test_existing_menu_is_not_loaded_again_during_recovery(self):
        (self.work / "proc/100/cmdline").write_bytes(
            str(self.media / "MiSTer").encode() + b"\0" +
            str(self.media / "menu.rbf").encode() + b"\0")
        (self.work / "state.json").write_text('{"main_handoff":"normal"}')
        self.assertIn("CANDIDATE_READY", self.run_deploy())
        self.assertEqual([line for line in self.events() if line.startswith("load ")],
                         ["load " + str(self.bundle() / "Plex.rbf")])
        self.preserved()

    def test_main_retiring_during_identity_read_is_observed_as_handoff(self):
        (self.work / "state.json").write_text(
            '{"main_handoff":"normal","defer_main_handoff":true}')
        self.ssh.retire_main_on_stat = True
        self.assertIn("CANDIDATE_READY", self.run_deploy())
        self.assertEqual(len([line for line in self.events() if line.startswith("load ")]), 2)
        self.preserved()

    def test_unreadable_live_main_identity_is_not_treated_as_retirement(self):
        self.ssh.main_stat_error = True
        self.refused()
        self.assertIn(b"unreadable-main-executable", self.ssh.result.stdout)
        self.assertEqual([line for line in self.events() if line.startswith("load ")],
                         ["load " + str(self.media / "menu.rbf")])
        self.assertFalse(any(line.startswith("start ") for line in self.events()))
        self.preserved()

    def refused_main_handoff(self, kind, error):
        (self.work / "state.json").write_text(json.dumps({"main_handoff": kind}))
        self.refused()
        self.assertIn(error.encode(), self.ssh.result.stdout)
        self.assertEqual([line for line in self.events() if line.startswith("load ")],
                         ["load " + str(self.media / "menu.rbf")])
        self.assertFalse(any(line.startswith("start ") for line in self.events()))
        self.preserved()

    def test_missing_main_handoff_times_out_without_another_load(self):
        self.refused_main_handoff("missing", "core-transition-timeout")

    def test_main_handoff_rejects_a_changed_executable(self):
        self.refused_main_handoff("changed-executable", "main-handoff-executable-changed")

    def test_main_handoff_rejects_a_different_core(self):
        self.refused_main_handoff("wrong-core", "unexpected-main-handoff-core")

    def test_main_handoff_rejects_a_process_older_than_the_command(self):
        self.refused_main_handoff("older-process", "unapproved-main-handoff")

    def test_main_handoff_rejects_a_foreign_parent(self):
        self.refused_main_handoff("foreign-parent", "unapproved-main-handoff")

    def test_main_handoff_rejects_a_foreign_owner(self):
        self.refused_main_handoff("foreign-owner", "foreign-process-owner")

    def test_main_handoff_does_not_resume_a_stopped_process(self):
        self.refused_main_handoff("stopped", "process-not-runnable")

    def test_main_handoff_rejects_duplicate_main_processes(self):
        self.refused_main_handoff("duplicate", "duplicate-main")

    def test_main_handoff_rejects_reuse_of_the_original_pid(self):
        self.refused_main_handoff("pid-reuse", "main-pid-reused")

    def configure_transient_audit(self, failure):
        (self.work / "state.json").write_text('{"main_handoff":"normal"}')
        proc_file(self.work, 140, Path("/bin/sh").resolve(), ["/bin/sh", "fixture"])
        (self.work / "proc/140/fd/6").symlink_to("/dev/null")
        self.ssh.audit_failure = failure

    def test_post_menu_audit_tolerates_a_process_that_has_exited(self):
        self.configure_transient_audit("maps-exit")
        self.assertIn("CANDIDATE_READY", self.run_deploy())
        self.preserved()

    def test_post_menu_audit_does_not_ignore_unreadable_live_mappings(self):
        self.configure_transient_audit("maps-live")
        self.refused()
        self.assertIn(b"unreadable-hardware-mappings", self.ssh.result.stdout)
        self.assertEqual([line for line in self.events() if line.startswith("load ")],
                         ["load " + str(self.media / "menu.rbf")])
        self.preserved()

    def test_post_menu_audit_tolerates_a_descriptor_that_has_closed(self):
        self.configure_transient_audit("fd-close")
        self.assertIn("CANDIDATE_READY", self.run_deploy())
        self.preserved()

    def test_post_menu_audit_tolerates_exit_during_descriptor_read(self):
        self.configure_transient_audit("fd-exit")
        self.assertIn("CANDIDATE_READY", self.run_deploy())
        self.preserved()

    def test_post_menu_audit_tolerates_zombie_mapping_retirement(self):
        self.configure_transient_audit("maps-zombie")
        self.assertIn("CANDIDATE_READY", self.run_deploy())
        self.preserved()

    def test_post_menu_audit_refuses_reused_pid_on_mapping_read_failure(self):
        self.configure_transient_audit("maps-reuse")
        self.refused()
        self.assertIn(b"process-reused-during-hardware-audit", self.ssh.result.stdout)
        self.assertEqual([line for line in self.events() if line.startswith("load ")],
                         ["load " + str(self.media / "menu.rbf")])
        self.preserved()

    def test_post_menu_audit_refuses_reused_pid_even_with_readable_mappings(self):
        self.configure_transient_audit("maps-reuse-readable")
        self.refused()
        self.assertIn(b"process-reused-during-hardware-audit", self.ssh.result.stdout)
        self.assertEqual([line for line in self.events() if line.startswith("load ")],
                         ["load " + str(self.media / "menu.rbf")])
        self.preserved()

    def test_post_menu_audit_refuses_reused_pid_on_descriptor_read_failure(self):
        self.configure_transient_audit("fd-reuse")
        self.refused()
        self.assertIn(b"process-reused-during-hardware-audit", self.ssh.result.stdout)
        self.assertEqual([line for line in self.events() if line.startswith("load ")],
                         ["load " + str(self.media / "menu.rbf")])
        self.preserved()

    def test_live_process_without_maps_is_not_assumed_quiescent(self):
        proc_file(self.work, 140, Path("/bin/sh").resolve(), ["/bin/sh", "fixture"])
        (self.work / "proc/140/maps").unlink()
        self.refused()
        self.assertIn(b"unreadable-hardware-mappings", self.ssh.result.stdout)
        self.assertFalse(any(line.startswith(("load ", "start ")) for line in self.events()))
        self.preserved()

    def test_post_menu_audit_does_not_ignore_unreadable_live_descriptors(self):
        self.configure_transient_audit("fd-live")
        self.refused()
        self.assertIn(b"unreadable-device-descriptor", self.ssh.result.stdout)
        self.assertEqual([line for line in self.events() if line.startswith("load ")],
                         ["load " + str(self.media / "menu.rbf")])
        self.preserved()

    def test_inflight_local_edit_cannot_replace_approved_upload(self):
        expected = Path(self.document["arm"]["path"]).read_bytes()
        self.ssh.mutate_local_arm = True
        self.run_deploy("copy-only")
        self.assertEqual((self.bundle() / "misterplexd").read_bytes(), expected)

    def test_copy_only_ignores_legacy_activation_environment(self):
        with mock.patch.dict(os.environ, {"DEPLOY_LOAD": "core", "DEPLOY_RECOVER": "reboot",
                                         "DEPLOY_START_DAEMON": "1"}):
            self.run_deploy("copy-only")
        self.assertEqual(self.events(), [])

    def test_busy_lease_is_not_deleted_or_bypassed(self):
        with (self.base / "run/candidate-deploy.lock").open("a") as holder:
            fcntl.flock(holder, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.refused()
        self.assertEqual(self.events(), [])
        self.assertIn(b"deployment-lease-busy", self.ssh.result.stdout)
        self.preserved()

    def test_graceful_stop_timeout_has_no_escalation_or_core_load(self):
        (self.work / "state.json").write_text('{"timeout_pid":110}')
        self.refused()
        self.assertEqual(self.events(), ["term 110"])
        self.assertIn(b"graceful-stop-timeout", self.ssh.result.stdout)
        self.preserved()

    def test_supervisor_timeout_does_not_signal_daemon(self):
        (self.work / "state.json").write_text('{"timeout_pid":120}')
        self.refused()
        self.assertEqual(self.events(), ["term 110", "term 120"])

    def test_unknown_helper_refuses_before_staging(self):
        self.document["owned_helpers"] = []
        self.refused()
        self.assertEqual(self.events(), [])
        self.assertFalse((self.base / "candidates").exists())

    def test_option_bearing_or_wrapped_helpers_cannot_escape_discovery(self):
        script = self.base / "bin/misterplex_core_watch.sh"
        for args in (["/bin/sh", "-e", script], ["/bin/bash", "--", script],
                     ["/bin/bash", "-c", f"while true; do {script}; sleep 5; done"]):
            with self.subTest(args=args):
                (self.work / "proc/110/cmdline").write_bytes(
                    b"".join(str(arg).encode() + b"\0" for arg in args))
                self.refused()
                self.assertEqual(self.events(), [])
                self.assertFalse((self.base / "candidates").exists())

    def test_relative_frozen_supervisor_argv_resolves_against_its_cwd(self):
        (self.work / "proc/120/cmdline").write_bytes(
            b"/bin/sh\0bin/misterplexd_supervise.sh\0")
        self.assertIn("CANDIDATE_READY", self.run_deploy())

    def test_exact_argv_discovers_renamed_daemon_without_pidof(self):
        (self.work / "proc/130/comm").write_text("renamed-thread\n")
        self.assertIn("CANDIDATE_READY", self.run_deploy())

    def test_wrong_live_daemon_executable_refuses_before_staging(self):
        (self.base / "bin/misterplexd").write_bytes(elf(b"other daemon"))
        self.refused()
        self.assertEqual(self.events(), [])
        self.assertFalse((self.base / "candidates").exists())

    def test_unknown_active_core_is_never_replaced(self):
        (self.work / "proc/100/cmdline").write_bytes(
            str(self.media / "MiSTer").encode() + b"\0/media/fat/_Console/Other.rbf\0")
        self.refused()
        self.assertEqual(self.events(), [])

    def test_foreign_process_uid_is_not_signalled(self):
        (self.work / "proc/130/status").write_text("Uid:\t1000\t1000\t1000\t1000\n")
        self.refused()
        self.assertEqual(self.events(), [])

    def test_foreign_socket_owner_is_not_accepted(self):
        (self.work / "proc/130/fd/7").unlink()
        self.refused()
        self.assertEqual(self.events(), [])

    def test_owned_child_surviving_term_blocks_core_load(self):
        child = self.base / "bin/ffmpeg"
        child.write_bytes(b"owned child")
        proc_file(self.work, 140, child, [child, "-i", "fixture"], parent=130)
        self.refused()
        self.assertEqual(self.events(), ["term 110", "term 120", "term 130"])
        self.assertIn(b"owned-child-still-running", self.ssh.result.stdout)

    def test_unowned_hardware_writer_is_refused_without_signalling_it(self):
        other = self.work / "unrelated"
        other.write_bytes(b"unrelated hardware owner")
        proc_file(self.work, 990, other, [other])
        (self.work / "proc/990/fd/5").symlink_to("/dev/MrAudio")
        self.refused()
        self.assertEqual(self.events(), ["term 110", "term 120", "term 130"])
        self.assertTrue((self.work / "proc/990/stat").exists())
        self.assertIn(b"another-hardware-owner", self.ssh.result.stdout)

    def test_closed_mem_fd_with_live_mapping_still_blocks_load(self):
        other = self.work / "unrelated"
        other.write_bytes(b"unrelated mapped writer")
        proc_file(self.work, 990, other, [other])
        (self.work / "proc/990/maps").write_text("30000000-31000000 rw-s 0 1:1 5 /dev/mem\n")
        self.refused()
        self.assertEqual(self.events(), ["term 110", "term 120", "term 130"])
        self.assertIn(b"another-hardware-mapping", self.ssh.result.stdout)

    def test_success_uses_ordered_term_single_bounce_and_real_readiness(self):
        unrelated = self.work / "unrelated-ffmpeg"
        unrelated.write_bytes(b"another user's process")
        proc_file(self.work, 990, unrelated, [unrelated, "-i", "other"])
        out = self.run_deploy()
        self.assertIn("CANDIDATE_READY pid=501", out)
        bundle = self.bundle()
        self.assertEqual(self.events(), ["term 110", "term 120", "term 130",
                                         "load " + str(self.media / "menu.rbf"),
                                         "load " + str(bundle / "Plex.rbf"),
                                         "start " + str(bundle)])
        self.assertTrue((self.work / "proc/990/stat").exists())
        self.preserved()

    def test_core_timeout_does_not_retry_or_start_daemon(self):
        (self.work / "state.json").write_text('{"core_timeout":true}')
        self.refused()
        self.assertEqual(self.events(), ["term 110", "term 120", "term 130",
                                         "load " + str(self.media / "menu.rbf")])

    def test_missing_command_endpoint_is_not_created_as_a_regular_file(self):
        (self.work / "MiSTer_cmd").unlink()
        self.ssh.native_send = True
        self.ssh.suffix = 'send_core "$MEDIA/menu.rbf"\n'
        self.refused()
        self.assertIn(b"missing-command-endpoint", self.ssh.result.stdout)
        self.assertFalse((self.work / "MiSTer_cmd").exists())
        self.assertEqual(self.events(), [])

    def test_unread_fifo_write_has_a_bounded_term_only_timeout(self):
        self.ssh.native_send = True
        self.ssh.suffix = 'send_core "$MEDIA/menu.rbf"\n'
        self.refused()
        self.assertIn(b"core-command-write-failed", self.ssh.result.stdout)
        self.assertEqual(self.events(), [])

    def test_missing_endpoint_blocks_menu_before_staging_or_stop(self):
        (self.work / "MiSTer_cmd").unlink()
        self.refused()
        self.assertEqual(self.events(), [])
        self.assertFalse((self.base / "candidates").exists())

    def test_live_pid_without_http_is_not_readiness(self):
        self.ssh.http_dead = True
        self.refused()
        self.assertIn(b"daemon-readiness-timeout", self.ssh.result.stdout)
        self.assertNotIn(b"CANDIDATE_READY", self.ssh.result.stdout)
        self.preserved()

    def test_unsupported_pidfd_refuses_before_staging_or_signalling(self):
        (self.work / "state.json").write_text('{"pidfd_unsupported":true}')
        self.refused()
        self.assertIn(b"pidfd-unsupported-kernel", self.ssh.result.stdout)
        self.assertEqual(self.events(), [])
        self.assertFalse((self.base / "candidates").exists())

    def test_pidfd_is_acquired_before_verification_and_used_for_signal_and_wait(self):
        api = FakePidfds(self.work)
        real_verify = pidfd.verify_identity

        def verified(*args):
            api.trace.append(("verify",))
            return real_verify(*args)

        with mock.patch.object(pidfd, "verify_identity", side_effect=verified):
            pidfd.stop(*self.expected_process(), api, str(self.work / "proc"))
        self.assertLess(api.trace.index(("open", 130, 700130)), api.trace.index(("verify",)))
        self.assertIn(("send", 700130, signal.SIGTERM), api.trace)
        self.assertIn(("poll", 700130, 10000), api.trace)
        self.assertEqual(api.trace[-1], ("close", 700130))
        self.assertEqual(self.events(), ["term 130"])

    def test_identity_change_after_handle_acquisition_refuses_signal(self):
        expected = self.expected_process()
        api = FakePidfds(self.work)
        api.after_open = lambda: (self.work / "proc/130/cmdline").write_bytes(b"changed\0")
        with self.assertRaisesRegex(pidfd.Refused, "process-command-changed"):
            pidfd.stop(*expected, api, str(self.work / "proc"))
        self.assertFalse(any(event[0] == "send" for event in api.trace))
        self.assertEqual(api.trace[-1][0], "close")
        self.assertEqual(self.events(), [])

    def test_exit_and_pid_reuse_after_verification_never_signals_replacement(self):
        expected = self.expected_process()
        api = FakePidfds(self.work)
        real_verify = pidfd.verify_identity

        def verified_then_replaced(*args):
            real_verify(*args)
            self.replace_process()

        with mock.patch.object(pidfd, "verify_identity", side_effect=verified_then_replaced):
            pidfd.stop(*expected, api, str(self.work / "proc"))
        self.assertFalse(any(event[0] == "send" for event in api.trace))
        self.assertTrue((self.work / "proc/130/stat").exists())
        self.assertEqual(self.events(), [])

    def test_exit_between_last_check_and_signal_still_uses_original_handle(self):
        expected = self.expected_process()
        api = FakePidfds(self.work)
        api.before_send = self.replace_process
        pidfd.stop(*expected, api, str(self.work / "proc"))
        self.assertIn(("send", 700130, signal.SIGTERM), api.trace)
        self.assertEqual(api.trace[-1], ("close", 700130))
        self.assertTrue((self.work / "proc/130/stat").exists())
        self.assertEqual(self.events(), [])

    def test_probe_uses_only_signal_zero_and_closes_handle(self):
        api = mock.Mock()
        api.open.return_value = 812
        api.exited.return_value = False
        pidfd.probe(api)
        api.open.assert_called_once_with(os.getpid())
        api.send.assert_called_once_with(812, 0)
        api.close.assert_called_once_with(812)

    def test_kernel_enosys_is_explicit_and_has_no_signal_fallback(self):
        api = mock.Mock()
        api.open.side_effect = OSError(errno.ENOSYS, "unsupported fixture kernel")
        output = io.StringIO()
        with mock.patch.object(pidfd, "Pidfds", return_value=api), contextlib.redirect_stdout(output):
            self.assertEqual(pidfd.main(["probe"]), 1)
        self.assertIn("pidfd-unsupported-kernel", output.getvalue())
        api.send.assert_not_called()

    def test_unknown_userspace_binding_refuses_instead_of_numeric_signal(self):
        with mock.patch.object(pidfd.os, "pidfd_open", None, create=True), \
             mock.patch.object(pidfd.signal, "pidfd_send_signal", None, create=True), \
             mock.patch.object(pidfd.platform, "machine", return_value="unapproved-abi"):
            with self.assertRaisesRegex(pidfd.Refused, "pidfd-unsupported-userspace"):
                pidfd.Pidfds()

    def test_pidfd_binding_cannot_request_kill_or_other_signals(self):
        with mock.patch.object(pidfd.os, "pidfd_open", mock.Mock(), create=True), \
             mock.patch.object(pidfd.signal, "pidfd_send_signal", mock.Mock(), create=True):
            api = pidfd.Pidfds()
            with self.assertRaisesRegex(pidfd.Refused, "pidfd-signal-not-permitted"):
                api.send(812, signal.SIGKILL)
            pidfd.signal.pidfd_send_signal.assert_not_called()

    def test_successful_launch_retains_private_log_without_relaying_contents(self):
        output = self.run_deploy()
        log = self.private_log()
        self.assertIn("fixture daemon stdout", log.read_text())
        self.assertIn("fixture daemon stderr", log.read_text())
        self.assertIn("CANDIDATE_LOG", output)
        self.assertNotIn("fixture daemon", output)

    def test_startup_failure_keeps_both_streams_in_private_log(self):
        (self.work / "state.json").write_text('{"startup_failure":true}')
        self.refused()
        self.assertIn(b"launched-daemon-exited", self.ssh.result.stdout)
        log = self.private_log()
        self.assertIn("fixture daemon stdout", log.read_text())
        self.assertIn("fixture startup failure", log.read_text())
        self.assertNotIn(b"fixture startup failure", self.ssh.result.stdout)
        self.preserved()

    def test_log_directory_with_wrong_permissions_blocks_before_stop(self):
        directory = self.work / "run/misterplex-candidates"
        directory.mkdir(mode=0o755)
        self.refused()
        self.assertIn(b"unsafe-log-directory", self.ssh.result.stdout)
        self.assertEqual(self.events(), [])
        self.assertEqual(directory.stat().st_mode & 0o777, 0o755)

    def test_actual_launch_redirections_preserve_success_and_failure_output(self):
        for code in (0, 23):
            with self.subTest(exit_status=code):
                stub = self.work / ("startup-" + str(code))
                stub.write_text("#!/bin/sh\nprintf 'actual stdout\\n'\nprintf 'actual stderr\\n' >&2\n"
                                f"exit {code}\n")
                self.manifest.write_text(json.dumps(self.document))
                document, payload = deploy.validate_manifest(self.manifest)
                script = deploy.render_remote(document, payload, "menu", f"{code + 1:032x}")
                fake = FakeSSH(self)
                fake.native_launch = True
                fake.suffix = (
                    'mkdir -p "$BUNDLE/run"\n'
                    f'cp {shlex.quote(str(stub))} "$BUNDLE/misterplexd"\n'
                    'chmod 700 "$BUNDLE/misterplexd"\nprepare_private_log\nlaunch_candidate\n'
                    'set +e\nwait "$LAUNCHED_PID"\nresult=$?\n'
                    f'[[ "$result" == {code} ]]\n')
                result = fake.run(script)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                log = self.work / "run/misterplex-candidates" / f"1234abcd-{code + 1:032x}" / "misterplexd.log"
                self.assertEqual(log.stat().st_mode & 0o777, 0o600)
                self.assertEqual(log.read_text(), "actual stdout\nactual stderr\n")
                self.assertNotIn(b"actual stdout", result.stdout)
                self.assertNotIn(b"actual stderr", result.stderr)

    def test_pid_reuse_is_refused_before_signal(self):
        stat = self.work / "proc/130/stat"
        changed = stat.read_text().replace("12345", "67890")
        self.ssh.suffix = (
            'snapshot 130\n'
            f"printf '%s' {shlex.quote(changed)} > {shlex.quote(str(stat))}\n"
            'stop_one 130\n')
        self.refused()
        self.assertIn(b"pid-reused", self.ssh.result.stdout)
        self.assertEqual(self.events(), [])

    def test_entrypoint_dispatch_fails_locally_without_legacy_tools(self):
        fake_bin = self.work / "forbidden-tools"
        fake_bin.mkdir()
        marker = self.work / "UNSAFE_TOOL_CALLED"
        for name in ("make", "sshpass", "ssh", "scp", "reboot", "killall"):
            file = fake_bin / name
            file.write_text("#!/bin/sh\nprintf called >> " + shlex.quote(str(marker)) + "\nexit 91\n")
            file.chmod(0o755)
        env = os.environ.copy()
        env["PATH"] = str(fake_bin) + ":" + env["PATH"]
        for entrypoint in ("deploy_plex_core.sh", "deploy_misterplexd.sh"):
            for args in (["--prebuilt-candidate"], ["--prebuilt-candidate", "--manifest",
                         str(self.work / "missing"), "--mode", "menu"], ["--prebuilt-canddate"]):
                result = subprocess.run(["bash", ROOT / "scripts" / entrypoint, *args],
                                        env=env, capture_output=True)
                self.assertNotEqual(result.returncode, 0)
        self.assertFalse(marker.exists())

    def test_remote_body_has_no_unsafe_fallbacks(self):
        text = (ROOT / "scripts/deploy_candidate_remote.sh").read_text()
        executable_lines = "\n".join(line for line in text.splitlines()
                                     if not line.lstrip().startswith("#"))
        for forbidden in ("killall", "kill -9", "kill -KILL", "reboot", "eval ",
                          "rm ", "make ", "/tmp/", "source ", "set -x"):
            self.assertNotIn(forbidden, executable_lines)
        self.assertEqual(executable_lines.count("kill -TERM"), 0)
        self.assertIn('pidfd_action stop "$pid"', executable_lines)
        self.assertEqual(executable_lines.count('send_core "$'), 2)


class SSHTransportTests(unittest.TestCase):
    def test_bounded_upload_tolerance_preserves_transport_and_failure(self):
        environment = {"MISTER_HOST": "fixture-device", "MISTER_USER": "root",
                       "MISTER_PASS": "fixture-password"}
        script = "printf fixture\n"
        for code in (0, 255):
            with self.subTest(returncode=code):
                result = subprocess.CompletedProcess([], code, b"", b"")
                with mock.patch.dict(os.environ, environment, clear=True), \
                     mock.patch.object(deploy.subprocess, "run", return_value=result) as run:
                    self.assertIs(deploy.SSH().run(script), result)
                run.assert_called_once_with(
                    ["sshpass", "-e", "ssh", "-o", "StrictHostKeyChecking=yes",
                     "-o", "ConnectTimeout=6", "-o", "ServerAliveInterval=15",
                     "-o", "ServerAliveCountMax=3", "root@fixture-device", "bash", "-s"],
                    input=script.encode(), capture_output=True,
                    env={**environment, "SSHPASS": "fixture-password"})


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--fake-device":
        sys.exit(fake_device(Path(sys.argv[2]), sys.argv[3], *sys.argv[4:]))
    else:
        unittest.main()
