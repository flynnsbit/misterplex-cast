#!/usr/bin/env python3
"""Inject key sequences into MiSTer via /dev/uinput (run ON the MiSTer).

Used to drive the Plex core OSD from CI/lab automation: MiSTer_cmd has no key
injection, so the only way to exercise the real user path (Main_MiSTer parses
CONF_STR -> user turns a knob -> status word changes -> misterplexd applies it)
is to present a virtual keyboard.

usage: osd_keys.py f12 down down down enter esc
       osd_keys.py playpause skipfwd skipback stop
       osd_keys.py --hold 20 f12          # leave the device alive for a capture
       osd_keys.py --list
"""
import argparse
import atexit
import fcntl
import os
import struct
import sys
import time

UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502
EV_SYN, EV_KEY = 0, 1

KEYS = {
    "f12": 88, "esc": 1, "enter": 28, "up": 103, "down": 108,
    "left": 105, "right": 106, "space": 57,
    # Playback-control aliases matching fpga/Plex_MiSTer/Plex.sv:
    # Space=Play/Pause, Esc=Stop, E0 Right=Skip Fwd, E0 Left=Skip Back.
    "playpause": 57, "play-pause": 57, "pause": 57, "resume": 57,
    "stop": 1, "skipfwd": 106, "skip-fwd": 106, "skipforward": 106,
    "skipback": 105, "skip-back": 105,
}

DEFAULT_NAME = "misterplex-uinput-keys"


def parse_args(argv):
    p = argparse.ArgumentParser(
        description="Inject MiSTer keyboard events via /dev/uinput.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("keys", nargs="*", help="key names or playback aliases to press")
    p.add_argument("--hold", type=float, default=0.0,
                   help="seconds to keep the virtual keyboard alive after the sequence")
    p.add_argument("--detect-wait", type=float, default=6.0,
                   help="seconds to wait for Main_MiSTer to discover the new input device")
    p.add_argument("--press-ms", type=float, default=120.0,
                   help="press duration for each key")
    p.add_argument("--gap-ms", type=float, default=450.0,
                   help="gap after each key release")
    p.add_argument("--name", default=DEFAULT_NAME,
                   help="virtual device name shown in /proc/bus/input/devices")
    p.add_argument("--list", action="store_true", help="list supported key names")
    return p.parse_args(argv)


def open_keyboard(name):
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    for code in sorted(set(KEYS.values())):
        fcntl.ioctl(fd, UI_SET_KEYBIT, code)
    raw_name = name.encode("ascii", "replace")[:79].ljust(80, b"\0")
    os.write(fd, raw_name + struct.pack("HHHH", 3, 0x1234, 0x5678, 1) +
             struct.pack("i", 0) + b"\0" * (4 * 64 * 4))
    fcntl.ioctl(fd, UI_DEV_CREATE)
    return fd


def emit(fd, ev_type, code, value):
    os.write(fd, struct.pack("llHHi", 0, 0, ev_type, code, value))


def press(fd, code, press_s, gap_s):
    emit(fd, EV_KEY, code, 1)
    emit(fd, EV_SYN, 0, 0)
    time.sleep(press_s)
    emit(fd, EV_KEY, code, 0)
    emit(fd, EV_SYN, 0, 0)
    time.sleep(gap_s)


def main(argv):
    args = parse_args(argv)
    if args.list:
        print("\n".join(sorted(KEYS)))
        return 0
    if not args.keys:
        print("no keys requested; use --list to see names", file=sys.stderr)
        return 2

    codes = []
    for a in args.keys:
        code = KEYS.get(a.lower())
        if code is None:
            print("unknown key: %s" % a, file=sys.stderr)
            return 2
        codes.append((a, code))

    fd = open_keyboard(args.name)
    destroyed = False

    def cleanup():
        nonlocal destroyed
        if not destroyed:
            destroyed = True
            try:
                fcntl.ioctl(fd, UI_DEV_DESTROY)
            finally:
                os.close(fd)

    atexit.register(cleanup)
    try:
        # Main_MiSTer discovers new input devices via inotify and re-enumerates;
        # keys sent before that lands are dropped on the floor.
        time.sleep(max(0.0, args.detect_wait))
        for _, code in codes:
            press(fd, code, args.press_ms / 1000.0, args.gap_ms / 1000.0)
        if args.hold > 0:
            time.sleep(args.hold)
        print("sent: %s via %s" % (" ".join(a for a, _ in codes), args.name))
        return 0
    finally:
        cleanup()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
