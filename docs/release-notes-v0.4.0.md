# MiSTerPlex v0.4.0

> Historical. The current package is rc5. PlexOB does not offer 240p and does not need an SDRAM board. Install from `README.md`. This note describes an older release.

Multi-resolution cast release: **240p / 480p / 720p**, live PMS encoder match, DDR present,
F12 Content + Display resolution control, and a working Plex Web timeline scrubber.

## Highlights

- **Content + Display resolution (F12):** OSD **Content resolution** (`O[5:4]`: 240p / 480p / 720p)
  chooses the PMS universal ladder. OSD **Display resolution** (`O[15:14]`: Follow content /
  240p / 480p / 720p) chooses the FPGA present bank. With `OSD_CONTROL=1` the daemon applies
  changes live (conf upsert + session retarget / restart at playhead).
- **Content resolution labels:** Product profiles use **240p / 480p / 720p**. Internal banks remain
  320×240, 640×480 (624 coded), 1280×720.
- **Live PMS match:** Changing content resolution retargets the Plex universal weak ladder
  (`videoResolution` + bitrate/profile) and restarts the session at the same playhead. Lab SCORE:
  240p→`320x240`, 480p→`640x480`, 720p→`1280x720` on FOAR cast.
- **Plex Web timeline scrubber:** Companion reports advancing `time=` while casting. PMS
  `/:/timeline` uses real HTTP **2xx** + the **cast token** (poll/subscribe never refresh the cast
  host token). Video-only titles (e.g. Grid720) advance the scrubber while the first frame buffers.
  Unique PMS session ids after daemon restart avoid sticky transcoder stalls.
- **Plex cast E2E:** Playwright + companion remote-player APIs (play/pause/resume/seek/stop/timeline)
  + HDMI-USB glass on **240p, 480p, and 720p**. Video-only sources no longer abort when `AUDIO=on`.
- **DDR always available:** HPS DDR frame store is the shipping present path. **SDRAM stick is
  optional** — not required for 240p/480p/720p. Conf documents `PRESENT_MEM=auto|ddr`.
- **480p geometry:** FOAR+pad contract coded 624 × display 618 × present 640.
- **Companion version:** 0.4.0.

## Ship-accepted present rates (lab, DDR path)

| Mode | Content | Typical pfps | Audio |
|------|---------|--------------|-------|
| 240p | FOAR A+V | ~20–25 | advances with picture |
| 480p | FOAR A+V | ~18–27 | advances with picture |
| 720p | FOAR A+V | ~10–16 | advances with picture |
| 720p | Grid720 video-only | ~18–19 | n/a (source has no audio stream) |

Playback is stable with A/V lock; under-runs are smooth (drops≈0). Further 720p rate headroom
is a future enhancement, not a withheld ship feature.

## Operational notes

- Plex Web “Select Player” → **MiSTerPlex**; cast play/pause/seek/stop track via companion + PMS.
- `OSD_CONTROL=1` (recommended) reads the live core status word. Default content bits = **240p**
  until you change F12 **Content resolution**. With `OSD_CONTROL=0`, conf `DECODE` /
  `TRANSCODE_PROFILE` own the ladder.
- Named `TRANSCODE_PROFILE=240p|480p|720p` owns the weak ladder. Clear sticky `WEAK_BITRATE` /
  `WEAK_RES` when switching modes.
- After installing a new RBF, load Menu then Plex once (or reboot) so the FPGA picks up the bitstream.

## Package

| Artifact | Identity |
|----------|----------|
| `cores/Plex.rbf` | md5 **`1c6ed06fe832fb54259d4f4ce504ccae`** (v9 SEED2, Content+Display OSD) |
| `bin/misterplexd` | static ARM; timeline + session-id + OSD follow |
| decode | libav linked into `bin/misterplexd`. Current packages do not ship a standalone ffmpeg program. |

STA: worst setup slack **−0.027 ns** on `general[2]` (accept-ambiguous); holds positive.  
Package: `VERSION=v0.4.0 scripts/package_release.sh` (MD5 gate built-in).

## Upgrade notes

1. Install the v0.4.0 tarball (`bin/`, `cores/Plex.rbf`, `docs/`, `licenses/`, `scripts/`).
2. Copy `conf/misterplex.conf.example` → `/media/fat/misterplex/misterplex.conf` (or merge keys).
   Recommended product defaults: `PRESENT=fpga`, `AUDIO=on`, `OSD_CONTROL=1`,
   `TRANSCODE_PROFILE=720p` (or your preferred ladder), `DECODE=1280x720`.
3. Load **Plex** from `_Utility` (Menu bounce once after first install).
4. F12 → set **Content resolution** and **Display resolution** (Follow content is fine for most users).
5. Cast from any Plex app; the Web scrubber should move while playing.

## Known limitations

- 720p software decode on the dual-A9 is rate-limited (~10–16 pfps typical with A+V); picture and
  audio stay locked.
- FPGA-side full H.264 reconstruction remains under development; product path uses ARM + FFmpeg.
- `MATCH_SOURCE_HZ` logs cadence hints; true runtime modeline switching is not finished.
- On-device browse/menu scripts need a `PLEX_TOKEN`; casting from a Plex app usually supplies a
  transient token automatically.
