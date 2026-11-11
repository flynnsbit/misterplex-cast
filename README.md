# MiSTerPlex

**Watch your Plex library, and cast YouTube and AirPlay video, on a MiSTer FPGA, on a CRT or an HDMI TV.**

MiSTerPlex turns a MiSTer (DE10-Nano) into a Plex player. A custom FPGA core, **MiSTerPlex** (`MiSTerPlex.rbf`, built from the PlexOB fork of MiSTer_h264_decoder), decodes the video in hardware. A small program on the MiSTer's Linux side, **misterplexd**, talks to Plex, YouTube and AirPlay.

The core file used to be called `PlexOB.rbf`; from rc3 it is `MiSTerPlex.rbf`, loaded from `_Utility`. To install or update, follow the **[install guide](docs/INSTALL.md)**.

> This is pre-release software. Things may break. Keep backups of your SD card.

## What it does

- **Cast from the Plex app.** On your phone or computer, tap the cast button and pick **MiSTer**. Play, pause, seek and stop from your phone.
- **Browse Plex on the TV.** Use a controller or keyboard with the on-screen Plex browser: home rows, Continue Watching, libraries, and a detail page for each title.
- **Cast YouTube** from the YouTube app to the MiSTer.
- **AirPlay** video from an iPhone, iPad or Mac.
- **Made for CRTs.** 480i and 240p analog output, a CRT-safe menu area, per-TV overscan trim, 4:3 / 14:9 / letterbox picture modes, and a Large menu size. S-video/composite works when `vga_mode` in `MiSTer.ini` and the core's **Analog Output** menu agree (see [Troubleshooting](docs/INSTALL.md#troubleshooting)).
- **Subtitles.** Text subtitles (SRT, ASS) drawn on screen, with an optional OpenSubtitles fallback.
- **Phone status page** at `http://<mister-ip>:3005/misterplex/status`.
- **Restarts itself** if the program crashes.

### Limits

- Video up to **720p at 30 fps, 8-bit H.264**. Anything else is converted by your Plex server, or refused for AirPlay.
- Picture-based subtitles (PGS/VobSub) need the server to burn them in, which is off by default.
- Home network only. **Never forward ports 3005, 8008 or 7000 to the internet.**

## Screenshots

The screenshot gallery is not included in this source export.

## What you need

- A MiSTer FPGA (DE10-Nano) with a current MiSTer menu, on your home network (wired is best).
- SSH access to the MiSTer (user `root`).
- A Plex Media Server and a Plex account.
- A computer with `ssh` and `scp`.
- A TV: an analog CRT (through a MiSTer analog board or adapter) or HDMI.

## How the install works

A release has **one FPGA core**, `MiSTerPlex.rbf`, which the installer puts in `/media/fat/_Utility/` only (one copy; an older root copy `/media/fat/MiSTerPlex.rbf` is removed so the menu does not list the core twice). There is no separate `Plex.rbf` or `PlexOB.rbf` to install any more. It also has the **misterplexd** program, which starts by itself at boot: the installer adds one line to `/media/fat/linux/user-startup.sh` that runs `misterplexd-boot-rc3.sh`. That script starts misterplexd under a small supervisor (`misterplexd_supervise.sh`), which restarts it if it crashes. `BOOT_SUPERVISE=0` in the settings file turns the supervisor off. You never need to start the program yourself. Just load **MiSTerPlex** from `_Utility`.

## Upgrading from rc5

rc5 installed the core in two places, `/media/fat/` and `/media/fat/_Utility/`. The new installer keeps a single copy in `/media/fat/_Utility/` and removes the old copy in `/media/fat/`.

## Quick start

1. Download the release package from https://github.com/flynnsbit/misterplex-cast/releases and unpack it.
2. From the unpacked folder, run the installer against your MiSTer (or copy the folder to the MiSTer and run `sh install-misterplex.sh` there). See the **[install guide](docs/INSTALL.md)**:

   ```sh
   sh install-misterplex.sh --remote <mister-ip>
   ```

   It copies the files, checks and installs the MiSTerPlex core, keeps your settings, and runs the boot script (`/media/fat/misterplex/bin/misterplexd-boot-rc3.sh install`). That adds this line to `/media/fat/linux/user-startup.sh`, so MiSTerPlex starts at boot:

   ```sh
   /media/fat/misterplex/bin/misterplexd-boot-rc3.sh "$1" &
   ```

3. Optional: change settings with `ssh -t root@<mister-ip> sh /media/fat/misterplex/bin/configure-misterplex.sh`.
4. Reboot, load **MiSTerPlex** from `_Utility`, and sign in with the `plex.tv/link` code shown on the TV.

The boot script also takes `uninstall`, `start` and `stop`. To remove MiSTerPlex, run `sh /media/fat/misterplex/bin/uninstall-misterplex.sh`: it keeps your settings and player identity (add `--purge` to remove them) and, in `/media/fat/linux/airplay` and `/media/fat/linux/gcast`, removes only the files it installed. To update YouTube support, run `/media/fat/misterplex/bin/update_ytdlp.sh`.

`update_ytdlp.sh` checks a network download against upstream `SHA2-256SUMS` from the same GitHub origin (corruption only; no signature). A `YTDLP_LOCAL_ZIP` is checked only when `YTDLP_LOCAL_SUMS` or a pin is set. Optional pins: `YTDLP_PIN_VERSION` (release tag instead of latest) and `YTDLP_PIN_SHA256` (exact 64-hex of the zip; mismatch or a malformed value refuses the install). Set both: a SHA pin alone still follows latest, so it refuses every new upstream release until you bump it.

## Settings

All settings live in one file: **`/media/fat/linux/misterplex.conf`**. The install copies a default (`conf/misterplex.conf.example` in the package) only if you don't already have one. Each setting is explained inside the file. Reboot after a change.

## Optional features

These are **off by default**. Turn one on by adding a line such as `CHAPTERS=1` to the settings file.

| Area | Setting | What it does |
| --- | --- | --- |
| Browsing | `GRID_PAGE` | Library grids load past 200 titles |
| | `AZ_JUMP` | Jump to titles by first letter |
| | `FILTER_4_3` | Hide widescreen titles on library grids |
| | `WATCHED_BADGES` | Check mark on watched posters |
| | `MARK_WATCHED` | Mark watched / unwatched, remove from Continue Watching |
| | `AMBIENT` | Library art behind the idle screen |
| Playback | `CHAPTERS` | Chapter list and skip in the program menu |
| | `MARKERS` | Skip Intro, Skip Credits, Up Next |
| | `SCRUB_THUMBS` | Preview picture while seeking |
| | `IN_PLAY_OPTIONS` | Options page and Play from start during playback |
| | `PAUSE_DIM` | Dim menus while paused or idle |
| | `ALLOW_480P60` | Play 480p 60 fps video directly |
| | `REMOTE_H264_TRANSCODE` | Ask remote servers for H.264 the MiSTer can play |
| Subtitles and audio | `SETSTREAMS` | Pick subtitles and audio during playback |
| | `SOFT_SUBS` | Subtitles built into the video file |
| | `FORCED_SUBS` | Pick a forced subtitle track on its own |
| | `REMEMBER_TRACKS` | Remember subtitle and audio picks per show |
| | `track_pref_server_sync` | Also PUT the pick to `/library/parts/{id}` on the Plex server (off by default; writes to the server; use on your own server only) |
| | `BURN_SUBS_FIX` | Ask **your own** server to burn in picture subtitles |
| YouTube | `YT_CAPTIONS`, `YT_CAST_TITLE`, `YT_VOLUME`, `YT_NEXT_PREV`, `YT_LIVE` | Captions, title, volume, next/previous, livestreams |
| Display | `CRT_LOOK` | Scanline looks for menus over HDMI |
| | `REMEMBER_PICTURE` | Remember the picture mode per show |
| Other | `ZAPAROO` | Play a Plex title from a Zaparoo card |
| | `REPORT_CODE` | Save a log bundle (private details removed) for bug reports |
| | `IDLE_MENU_KEY` | Keyboard P opens the program menu |
| | `LAN_AUTH` | Require a shared secret for MiSTerPlex's own control commands |

Settings that are on by default, which you can turn off, include `PLEXTV_LINK` (sign-in code) and `BOOT_SUPERVISE` (automatic restart).

See [CHANGELOG.md](CHANGELOG.md) for what changed in each release.

## Source and license

This project is free software under the **GNU General Public License, version 3 or (at your option) any later version** (GPL-3.0-or-later), the same licence as the upstream PlexOB core by owenb321. The vendored hps/ client library from owenb321 stays under its own MIT licence (packaging/licenses/owenb-h264ring). See [LICENSE](LICENSE) and [NOTICE](NOTICE). MiSTerPlex.rbf is built from the PlexOB core. The complete corresponding source is this repository, https://github.com/flynnsbit/misterplex-cast. Source revision: 6c3679ae9708210f46902a04305a9c956e2701b1. The casting helper's licence and checksums are in packaging/licenses/gcast-ffmpeg; its source is not in this repository. Source code and releases: https://github.com/flynnsbit/misterplex-cast (downloads: https://github.com/flynnsbit/misterplex-cast/releases).

## Thanks

MiSTerPlex is built on these projects:

| Project | Role | License |
|---|---|---|
| [MiSTer FPGA (MiSTer-devel)](https://github.com/MiSTer-devel) | Platform, menu, core loading | GPL-2.0-or-later / GPL-3.0 |
| [MiSTer_h264_decoder by owenb321](https://github.com/owenb321/MiSTer_h264_decoder) | PlexOB H.264 core and ring client | GPL-3.0 / client MIT |
| [UxPlay](https://github.com/FDH2/UxPlay) / [RPiPlay](https://github.com/FD-/RPiPlay) | AirPlay receiver | LGPL-2.1-or-later / GPL-3.0 |
| [FFmpeg](https://ffmpeg.org/) | libav in the daemon; on-card helper | LGPL-2.1-or-later; helper GPL-3.0-or-later |
| [x264](https://code.videolan.org/videolan/x264) | Encoder in the on-card helper | GPL-2.0-or-later |
| [OpenSSL](https://www.openssl.org/) | HTTPS / TLS | Apache-2.0 |
| [yt-dlp](https://github.com/yt-dlp/yt-dlp) | YouTube streams | Unlicense |
| [stb](https://github.com/nothings/stb) / [QOI](https://github.com/phoboslab/qoi) / [Spleen](https://github.com/fcambus/spleen) | Menu images and fonts | public domain / MIT / BSD-2-Clause |
| [Mozilla CA bundle](https://curl.se/docs/caextract.html) | TLS trust store | MPL-2.0 |

## Trademarks

Plex, YouTube, AirPlay and other names belong to their owners. MiSTerPlex is not affiliated with or endorsed by Plex, Google or Apple.
