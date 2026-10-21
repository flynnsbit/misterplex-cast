# MiSTerPlex v0.3.0

> Historical. The current package is rc5. Install from `README.md`. This note describes an older release.

Stable cast client for everyday use. HDMI-verified package.

**Install from the tarball only** — use the included `Plex.rbf` and `misterplexd` together.  
Config defaults in the package are the ones that were verified (`PRESENT=both`, 320×240).

## New since v0.2.0

- **On-screen playback bar** — play/pause state, progress, and skip feedback on the TV  
- **Local controls** — keyboard (Space / Esc / arrows) and mappable gamepad buttons  
- **Cast app stays in sync** — pause/seek/stop from the MiSTer updates the phone/web app  
- **Plex “watched” / resume** — progress reported to your Plex Media Server  
- **Clearer setup** — ship defaults that match a working HDMI install  

Still included from v0.2: cast from any Plex app, self-contained tarball with `ffmpeg`, auto-next in the queue.

## What this version is for

| | |
|--|--|
| Content size | **320×240** (scaled to your HDMI/VGA output) |
| Best role | Daily stable cast + local control |

For higher content resolutions (480p / 720p), see **v0.4.0** — separate line, still evolving.

## Install (short)

1. Extract the release tarball.  
2. Copy `bin/`, `scripts/`, `licenses/` → `/media/fat/misterplex/`.  
3. Copy `cores/Plex.rbf` → `/media/fat/_Utility/`.  
4. Copy `conf/misterplex.conf.example` → `/media/fat/misterplex/misterplex.conf` and set `PLEX_BASE`.  
5. Start `misterplexd`, load **Plex** from the OSD, cast to **MiSTerPlex**.  

Full steps: [README](https://github.com/flynnsbit/misterplex-cast#install).

## Package identity

| File | MD5 |
|------|-----|
| `cores/Plex.rbf` | `41adb98c7a630b541091c22ce291be68` |
| `bin/misterplexd` | `06c5735a2f85114688f0ff2ac36e4fd4` |

## Known limits

- Picture is 320×240 content (not 720p native).  
- Decode is on the MiSTer ARM CPU (not full FPGA video decode yet).  
- Present rate is typically ~10 frames/s with audio locked — smooth enough for cast, not a PC player.

## Verify on hardware (lab)

Idle logo chevron and local RealGlass/BBB sample verified over HDMI-USB capture with audio present (2026-08-11).
