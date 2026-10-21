# OB image: keep the framebuffer console off fb0 (OWENB-P1B 0009, T6 gap G15)

On the PlexOB core the overlay shows ARM-drawn pages from fb0 (`h264ring_fb_map`, `third_party/owenb-h264ring/include/h264ring.h:311-330`). `h264ring_fb_map` stops only the cursor blink (`lib/h264ring_linux.c:193-236`). Anything else that fbcon draws lands inside the page the core is showing: a kernel message, a getty on tty1, or MiSTer's F9 terminal.

## 1. Daemon side (in this patch, on by default)

`host/libmisterplex/owenb_fbcon_guard.hpp` `FbconGuard` does three things:
- **While the OB overlay holds fb0:** that is the idle session plus plays, from `mapFb` to `unmapFb` in `owenbOsdConfigFromEnv`, `arm/misterplexd/media_player.cpp`. During that time the guard detaches fbcon from the console layer: `echo 0 > /sys/class/vtconsole/vtconN/bind` for the vtcon whose `name` contains "frame buffer" (kernel `Documentation/fb/fbcon.rst` §D).
- **At the release** (a hop off OB, daemon exit), it attaches fbcon again, but only the vtcons it detached. A console you detached yourself stays detached.
- **Crash recovery:** a marker file (`/tmp/misterplex_ob_fbcon_unbound`) lists what the guard detached. If the daemon dies holding fb0, its next start attaches those vtcons again (`MediaPlayer::owenbFbconAtStart`, called from `main.cpp`). Only paths under `/sys/class/vtconsole/` are accepted from the marker.

`MPX_OB_FBCON=keep` makes the guard only look and log, and change nothing. Use it with the image option below.

Tests: `tests/unit/test_owenb_fbcon.cpp` runs on a temp vtconsole tree, never `/sys`. Its two mutants, `MUT_FBCON_NO_REBIND` and `MUT_FBCON_REBIND_ALL`, fail as required. Source row S7 in `test_owenb_osd` checks the order: detach → map, then unmap → attach.

## 2. Image side (optional, for a dedicated OB image; not applied anywhere by this patch)

To keep fbcon off fb0 for the whole boot, so even boot messages never reach it, add `fbcon=map:1` to the kernel command line. With only fb0 registered, "fbcon=map:1 tells fbcon not to take over the console" (`fbcon.rst` §C.2).

On MiSTer, extra kernel arguments go in `/media/fat/linux/u-boot.txt`; the stock example is `u-boot.txt_example`. Append the option to its `v=` line, for example:

```
v=loglevel=4 fbcon=map:1
```

Keep whatever the line already carries, such as `usbhid.jspoll=1 xpad.cpoll=1`.

Trade-off: the Linux text console, including MiSTer's F9 terminal, then never shows on the TV on that image, for any core. That is why the daemon guard, not this option, is the default: it frees fb0 only while the OB overlay needs it.

With the option set, run the daemon with `MPX_OB_FBCON=keep`. The guard's `imageMapsFbconAway()` reads `/proc/cmdline`, and its log shows that fbcon is already off fb0.

To check on glass (later, after the first closed T10 RBF; not done here):
- `cat /proc/cmdline` shows `fbcon=map:1`;
- `cat /sys/class/vtconsole/vtcon*/name` and `.../bind` show the frame buffer device unbound, or absent;
- `echo hi > /dev/tty1` while the idle card is up leaves the card untouched.

## 3. Not covered

- MiSTer Main's own use of fb0 (its framebuffer terminal and scaler options in `MiSTer.ini`) belongs to Main, not fbcon. While the PlexOB core runs, Main does not draw there; verify this on glass.
- This patch made no device access and no image change. The `u-boot.txt` line above is documentation for the image owner.
