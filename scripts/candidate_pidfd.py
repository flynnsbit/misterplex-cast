#!/usr/bin/env python3
"""Kernel-bound candidate shutdown; embedded on stdin, never a numeric-kill fallback."""
from __future__ import annotations

import errno
import hashlib
import os
import platform
import re
import select
import signal
import sys
import time

try:
    import ctypes
except ImportError:
    ctypes = None


class Refused(RuntimeError):
    pass


class Pidfds:
    def __init__(self):
        self.native = callable(getattr(os, "pidfd_open", None)) and callable(
            getattr(signal, "pidfd_send_signal", None))
        self.libc = None
        if self.native:
            return
        # Linux v5.15 arch/arm/tools/syscall.tbl, ARM EABI syscall base zero.
        # The MiSTer Python was built without the convenience APIs, not without ctypes.
        if (ctypes is None or sys.platform != "linux" or platform.machine() != "armv7l" or
                ctypes.sizeof(ctypes.c_void_p) != 4):
            raise Refused("pidfd-unsupported-userspace")
        self.libc = ctypes.CDLL(None, use_errno=True)
        self.libc.syscall.restype = ctypes.c_long

    def syscall(self, number, *arguments):
        result = self.libc.syscall(ctypes.c_long(number), *arguments)
        if result < 0:
            raise OSError(ctypes.get_errno(), "pidfd syscall failed")
        return result

    def open(self, pid):
        if self.native:
            return os.pidfd_open(pid, 0)
        return self.syscall(434, ctypes.c_int(pid), ctypes.c_uint(0))

    def send(self, handle, number):
        if number not in (0, signal.SIGTERM):
            raise Refused("pidfd-signal-not-permitted")
        if self.native:
            signal.pidfd_send_signal(handle, number, None, 0)
        else:
            self.syscall(424, ctypes.c_int(handle), ctypes.c_int(number),
                         ctypes.c_void_p(), ctypes.c_uint(0))

    def exited(self, handle, timeout_ms=0):
        poller = select.poll()
        poller.register(handle, select.POLLIN | select.POLLHUP)
        deadline = time.monotonic() + timeout_ms / 1000
        remaining = timeout_ms
        while True:
            try:
                events = poller.poll(remaining)
                break
            except InterruptedError:
                remaining = max(0, int((deadline - time.monotonic()) * 1000))
        if any(flags & (select.POLLERR | select.POLLNVAL) for _, flags in events):
            raise Refused("pidfd-poll-failed")
        return any(flags & (select.POLLIN | select.POLLHUP) for _, flags in events)

    def close(self, handle):
        os.close(handle)


def probe(api):
    handle = api.open(os.getpid())
    try:
        api.send(handle, 0)  # Permission/support probe only: no signal is delivered.
        if api.exited(handle):
            raise Refused("pidfd-self-probe-invalid")
    finally:
        api.close(handle)


def read_proc(directory, name):
    fd = os.open(name, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW, dir_fd=directory)
    with os.fdopen(fd, "rb") as stream:
        data = stream.read(1024 * 1024 + 1)
    if len(data) > 1024 * 1024:
        raise Refused("process-metadata-too-large")
    return data


def verify_identity(pid, start, executable, executable_hash, argv_hash, proc_root="/proc"):
    directory = os.open(f"{proc_root}/{pid}", os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        fields = read_proc(directory, "stat").rsplit(b") ", 1)[-1].split()
        if len(fields) < 20 or fields[19] != str(start).encode():
            raise Refused("pid-reused")
        if fields[0] in (b"Z", b"T", b"t"):
            raise Refused("process-not-runnable")
        owners = [line.split()[1:] for line in read_proc(directory, "status").splitlines()
                  if line.startswith(b"Uid:")]
        if owners != [[b"0", b"0", b"0", b"0"]]:
            raise Refused("process-owner-changed")
        if hashlib.sha256(read_proc(directory, "cmdline")).hexdigest() != argv_hash:
            raise Refused("process-command-changed")
        fd = os.open("exe", os.O_RDONLY | os.O_CLOEXEC, dir_fd=directory)
        with os.fdopen(fd, "rb") as stream:
            info = os.fstat(stream.fileno())
            if (info.st_dev, info.st_ino, info.st_uid) != executable:
                raise Refused("executable-identity-changed")
            digest = hashlib.sha256()
            for block in iter(lambda: stream.read(65536), b""):
                digest.update(block)
            if digest.hexdigest() != executable_hash:
                raise Refused("executable-bytes-changed")
    finally:
        os.close(directory)


def stop(pid, start, executable, executable_hash, argv_hash, api, proc_root="/proc"):
    try:
        handle = api.open(pid)
    except ProcessLookupError:
        return  # Already gone; the caller still checks for respawn and live children.
    try:
        if api.exited(handle):
            return
        try:
            verify_identity(pid, start, executable, executable_hash, argv_hash, proc_root)
        except (FileNotFoundError, ProcessLookupError):
            if api.exited(handle):
                return
            raise Refused("bound-process-metadata-unavailable")
        if api.exited(handle):
            return
        try:
            # The same acquired handle is used even if the numeric PID is now reused.
            api.send(handle, signal.SIGTERM)
        except ProcessLookupError:
            return
        if not api.exited(handle, 10000):
            raise Refused("graceful-stop-timeout")
    finally:
        api.close(handle)


def main(arguments=None):
    args = sys.argv[1:] if arguments is None else arguments
    try:
        api = Pidfds()
        if args == ["probe"]:
            probe(api)
        elif (len(args) == 6 and args[0] == "stop" and
              re.fullmatch(r"[1-9][0-9]*", args[1]) and int(args[1]) <= 2147483647 and
              re.fullmatch(r"[0-9]+", args[2]) and
              re.fullmatch(r"[0-9]+:[0-9]+:[0-9]+", args[3]) and
              all(re.fullmatch(r"[0-9a-f]{64}", value) for value in args[4:])):
            stop(int(args[1]), args[2], tuple(map(int, args[3].split(":"))),
                 args[4], args[5], api)
        else:
            raise Refused("invalid-pidfd-request")
    except Refused as error:
        print("CANDIDATE_ERROR " + str(error))
        return 1
    except OSError as error:
        reason = "pidfd-unsupported-kernel" if error.errno == errno.ENOSYS else (
            "pidfd-operation-failed-errno-" + str(error.errno))
        print("CANDIDATE_ERROR " + reason)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
