# Changelog

All notable changes to MiSTerPlex, written for people running it on a MiSTer.

## v1.0.0 (pre-release, 11 November 2026)

Everything below is new since the **rc5** pre-release, published on the public repo (flynnsbit/misterplex-cast, 21 October 2026). It covers the merged PRs from #16 to #357. Some items have not been checked on a real TV yet; those are marked *(not yet TV-tested)*.

**About optional features.** Many new features are **off by default**. Each has a switch in the settings file, `/media/fat/linux/misterplex.conf`. A feature that is off works exactly like the old build. To turn one on, add a line like `CHAPTERS=1`, then reboot. See the settings file for what each one does.

### Latest fixes (after #402)

- **Embedded mov_text** (MP4/M4V timed text) is drawn on the overlay even when `SOFT_SUBS` is off. The server is not asked to burn it in (#260).

### Latest fixes (PRs #349 to #357, on dev main 443b1d4, not yet in a staged build)

- **Installer: one core, in `_Utility`.** The core is installed only as `/media/fat/_Utility/MiSTerPlex.rbf`; an old root copy is removed so the menu does not show it twice, and replaced cores go to `/media/fat/misterplex/backup/`. The core must match the release pin list. Copies are exFAT-safe (no file modes, owners or symlinks needed) (#349).
- **Uninstall** (`uninstall-misterplex.sh`) removes what the installer created, using an install record; in `linux/airplay` and `linux/gcast` only the installer's own files are removed. Settings, identity and `misterplex/backup/` are kept unless `--purge` (#349).
- **Casting sign-in safety:** the caster's token is sent only to a server whose identity was checked; after a refused cast it is not used for seek or play-queue requests, and an IPv6 home-network cast address is not kept for timeline updates (#355). Timeline fallback and mark-watched gate the token against the host they actually send to (#361, #362). *(not yet TV-tested)*
- **YouTube vs on-card helper (GH#390 / PR #395):** `GCAST_XCODE=http://127.0.0.1:18781` is AirPlay only. YouTube stays on local yt-dlp (`yt-local:`) so a plex-to-YT cast cannot sit on LOADING YOUTUBE waiting for `/yt.ts`. A real remote helper is unchanged.
- **plex.tv/link QR (GH#23):** the Sign-in card QR stays a 27×27 matrix for `https://plex.tv/link` (quiet zone + finder). `MUT_QR_MODULES_1` and `MUT_QR_BLANK` fail `test_link_qr`.
- **YouTube Lounge 410 (GH#24):** a Lounge `410` (gracefulReconnect) rebinds and keeps YouTube on Ready. `MUT_LP_410_DROP` fails `test_gcast_lounge`.
- **Continue Watching (GH#45):** finished titles (viewOffset 0 or at duration) stay off the home row. `MUT_CW_KEEP_FINISHED` fails `test_plexui_continue_watching`.
- **Text captions on 480i (GH#34):** interlaced caption bands stay on even lines. `MUT_CAPTION_ODD_LINES` fails `test_caption_layout`.
- **AirPlay GET /info (GH#369):** the receiver advertises one 1280×720 display at 30 fps. `MUT_AP_INFO_1080` fails `test_airplay_admit`.
- **240p on the ship pin (GH#21):** ANALOG_RASTER=240p is allowed on BUILD `0x0B1101C5`. `MUT_RASTER_NO_1101` fails `test_analog_raster`.
- **480i timeline (GH#20):** 480i reports frame height 480 and field height 240. `MUT_480I_FRAME_IS_FIELD` fails `test_480i_active_height`.
- **720p play poke (GH#19):** play does not rewrite `/dev/MiSTer_cmd` video_mode (that poke drops Ready to colour bars). `MUT_FORCE_RASTER_ON_PLAY` fails `test_core_id_select`.
- **240p CRT (GH#10):** the ship pin does not allow the 31 kHz LCD 240p modeline (`video_mode 640,…25175`). `MUT_GH10_ALLOW_LCD` fails `test_core_id_select`.
- **Y/C on the ship pin (GH#359 / #26):** BUILD `0x0B1101C5` is a Y/C PlexOB id (S-Video / composite OSD). `MUT_YC_NO_1101` fails `test_plexob_yc`.
- **CRT overscan clock (GH#46):** interlaced in-play clock stays 24 frame lines above title-safe. `MUT_CRT_INSET_0` fails `test_inplay_osd`.
- **Auto picture default (GH#55):** `AUTO_PICTURE_MODE` stays off unless conf/env is exactly `advise`. `MUT_AP_DEFAULT_ADVISE` fails `test_auto_picture_mode`.
- **Theme music conf (GH#235):** `THEME_MUSIC` in misterplex.conf is exported as `MPX_THEME_MUSIC` (env still wins). Show Detail / Home pad stay gated on that flag.
- **Owned burn PUT (GH#366):** plex.tv linked servers with `owned=1` fill the burn PUT allow-list even when the JSON catalog cache is empty. `MUT_324_LINKED_OWNERS_SKIP` fails `test_link_discover`. An empty catalog must not wipe that list. If `/identity` lookup fails, an owned plex.tv host URI still allows the PUT.
- **640×272 DAR (GH#366):** Fifth Element transcode `640x272` folds to `40:17` (under the 12-bit PLXA max). FPGA ACK mismatch on glass is separate.
- **Owned burn PUT HTTPS (GH#366):** a plex.direct `https://A-B-C-D.*.plex.direct:32400` part PUT is rewritten to `http://A.B.C.D:32400` (plain HTTP on the LAN). `MUT_366_NO_LAN_REWRITE` fails `test_plexdirect_lan_http`.
- **Plexui over Ready (GH#14):** do not present the idle chevron while Plex UI is visible. `MUT_PLEXUI_IDLE_OVERLAY` fails `test_plexui_paint_ready`. Overlay status rasters below 320×240 do not re-arm the idle card or re-layout OSD (`MUT_IDLE_REARM_TINY` fails `test_idle_raster_credible`).

- **ARM link:** `theme_decode.cpp` is listed once in `arm-plexd-daemon` (it is already in `MPLEX_SRC`).
- **Remote start:** the start probe reads at most 64 KiB by default, with a short retry pause (#357).
- **Tests and release checks (no change on the MiSTer):** a generic log check stops raw URLs with tokens from being logged, and lab addresses in tests use documentation ranges (#354, #356).

### Latest fixes (PRs #328 to #347, on dev main 49b083e, not yet in a staged build)

- **Empty library on a picked server:** the picked server is matched by machine id or any of its addresses; the token from the settings file is used only on a proven machine-id match, and a 401/403 from the server is shown on screen (#328).
- **Start-up when the display is not ready:** the server list and Plex browse start even if the display setup fails at boot (#328); the display setup is then retried with a bounded backoff before the idle, menu and input workers start, and the retry stops once a verified core owns the display (#342, #344).
- **Casting:** an explicit cast server address wins, and the home-network address rewrite needs an exact server identity match (#333 follow-up, #344). While a cast from another server plays, MiSTerPlex no longer probes the default server or sends it timeline updates (#347).
- **Embedded SRT subtitles** are read from the media part instead of a path the server rejects; the read does not follow redirects, so the sign-in stays on the Plex server (#334).
- **Privacy:** every logged URL/command line goes through one masking function, and no raw error output prints a token (#334, #347).
- **Tests and release checks (no change on the MiSTer):** remaining private-network addresses in test fixtures replaced with documentation ranges (#345).

### Latest fixes (PRs #314 to #336, not yet in a staged build)

- **Plex sign-in and servers:** plex.tv requests now send the player's client id (#316), and so does the linked-resources fetch (#319). The newer plex.tv v2 resource reply is read, not just the older Device format (#320). The list of linked servers is kept when the boot script starts with no input attached (#318).
- **Casting from your own network:** fixed how MiSTerPlex identifies a Plex server on your home network during a cast (#336, fixes #333).
- **Subtitles:** embedded subtitles are matched to the file that was actually opened (#317). The opt-in burn-in request (`BURN_SUBS_FIX`) is only sent to servers you own (#329, fixes #324).
- **Tests and release checks (no change on the MiSTer):** the core-gate check now fails if its base commit is missing (#314). The rc3 changelog entry was updated (#315).

### Plex: signing in and browsing on the TV

- **Sign in with a link code.** If no account token is set, the ready screen shows a `plex.tv/link` address and a 4-character code. Type the code on that page to link the MiSTer to your Plex account. If plex.tv can't be reached, the card says it is retrying, and you can get a new code if one fails.
- **Switch Plex Home users** with a PIN from the on-screen menu. *(not yet TV-tested)*
- **Continue Watching** row on the Plex home screen.
- **Library browsing is smoother.** The left sidebar opens with Left. Up/Down scrolls the home rows and the column stays in range. Esc from a title's page after playing returns you to the library, not the home screen. Spacing is better at 240p.
- **More than one server.** Servers that show as offline are probed instead of being dropped, and the connection list for a server is merged so you don't lose a working address.
- **Audio track picker** on a title's detail page.
- Optional browsing extras (all off by default):
  - `GRID_PAGE=1`: library grids load past the first 200 titles.
  - `AZ_JUMP=1`: jump to titles by first letter.
  - `FILTER_4_3=1`: hide widescreen titles on library grids.
  - `WATCHED_BADGES=1`: a check mark on watched posters.
  - `AMBIENT=1`: library art cycles behind the idle screen.
  - `MARK_WATCHED=1`: Mark watched / unwatched, and Remove from Continue Watching, in the program menu.

### Playback

- **Resumes after a network drop.** A direct-played video that loses its connection picks up where it was instead of stopping.
- **Remote servers:** a faster, safer start check, sturdier handling of the server's reply, and a clear on-screen message if a remote server can't convert a video. A title that is missing on your own server goes back to the ready screen instead of showing a confusing error.
- Optional `REMOTE_H264_TRANSCODE=1`: always ask a remote server for an H.264 version that fits the MiSTer (helps with HEVC on friends' servers). Off by default.
- Optional `ALLOW_480P60=1`: play 480p 60 fps video directly instead of converting it.
- Videos that end now finish cleanly, and stopping near the end no longer saves a resume point.
- The Plex app's progress bar stays in sync better while you cast (heartbeat and timeline fixes). Plex Web on your own server may still need the optional workaround in the docs.
- Optional playback extras (all off by default):
  - `CHAPTERS=1`: Previous / Next chapter and a chapter list in the program menu.
  - `MARKERS=1`: Skip Intro, Skip Credits and Up Next. *(not yet TV-tested)*
  - `SCRUB_THUMBS=1`: a preview picture above the seek bar.
  - `IN_PLAY_OPTIONS=1`: an Options page during playback, with Play from start.
  - `PAUSE_DIM=1`: dims the on-screen menus while paused or idle.

### Subtitles and audio tracks

- **Text subtitles on the MiSTerPlex core.** SRT and ASS subtitle files next to a video are drawn on screen. With 4:3 TVs and widescreen video, subtitles sit in the black bar below the picture (`SUBTITLE_POSITION`).
- **Pick subtitles and audio during playback** from the program menu (`SETSTREAMS=1`, off by default).
- **Controller shortcuts** for changing audio and subtitle tracks (off by default; see `docs/CONTROLLER-TRACK-SHORTCUTS.md`).
- **Embedded mov_text** (MP4/M4V timed text) is drawn on screen even with `SOFT_SUBS` off (#260). Optional `SOFT_SUBS=1`: other subtitles built into the file (WebVTT, embedded SRT/ASS).
- Optional `FORCED_SUBS=1`: pick a "forced" subtitle track on its own. Optional `REMEMBER_TRACKS=1`: remember your subtitle and audio pick for the rest of a show. Optional `track_pref_server_sync=1` also PUTs `/library/parts/{id}` so the Plex server stores that pick for the user in every Plex app; **off by default**, use on your own server only (#325).
- **OpenSubtitles fallback.** When a title has no text subtitle, MiSTerPlex can fetch one from OpenSubtitles.com (needs your own free API key; downloads are cached on the card).
- If the server can't send an embedded subtitle, MiSTerPlex can still show it from the file.
- **Burned-in subtitles stay off by default.** `BURN_SUBS_FIX=1` asks the server to burn in picture-based subtitles (PGS/VobSub) that the MiSTer can't draw itself. It's opt-in, makes the server convert the video, and is meant for servers you own. Text subtitles are never burned in.

### Casting: YouTube and AirPlay

- YouTube playlists move to the next video when one ends.
- Seeking in YouTube no longer re-looks-up the video, so it's much faster.
- YouTube video links are hidden in the log.
- Optional YouTube extras (all off by default): `YT_CAPTIONS=1` captions, `YT_CAST_TITLE=1` video title on screen, `YT_VOLUME=1` volume from your phone, `YT_NEXT_PREV=1` next/previous from your phone, `YT_LIVE=1` livestreams. *(captions and livestreams not yet TV-tested)*
- **AirPlay:** video above what the MiSTer can play (over 720p, over 30 fps, HEVC or 10-bit) is refused with a short message on screen instead of failing silently. Fixed errors opening some streams (for example Fubo). Fixed audio problems with some senders. The AirPlay helper has been renamed from `gcast` to `airplay`; the old folder name and `GCAST_*` settings still work.
- Optional AirPlay **screen mirroring** (`AIRPLAY_MIRROR=1`, off by default). *(experimental, not yet TV-tested)*
- Optional **Zaparoo** support (`ZAPAROO=1`): play a Plex title by tapping a Zaparoo card.

### Display, CRT, 240p and overscan

- **CRT overscan-safe layout.** Menus stay inside a safe area (`UI_SAFE_MARGIN`), and a **Large** UI size is available (`UI_SIZE=large`).
- **Per-TV overscan calibration** from the program menu (Safe area). Your settings are saved in the settings file.
- **14:9 picture mode** for widescreen video on a 4:3 TV. Optional `REMEMBER_PICTURE=1` remembers the picture mode per show.
- **240p analog output.** The shipped MiSTerPlex core can output 240p on an analog TV; `ANALOG_RASTER=240p` selects it (save the core's settings once in the F12 menu first). *(not yet TV-tested)*
- Optional `CRT_LOOK=1`: scanline-style looks for the on-screen menus over HDMI.
- `AUTO_PICTURE_MODE=advise` logs the picture mode MiSTerPlex would pick for a video. It doesn't change anything yet.
- The test colour bars are gone. When nothing is playing you see the ready screen, or a "Daemon not running" card if the program isn't running.

### Setup, reliability and the phone status page

- **Phone status page.** Open `http://<mister-ip>:3005/misterplex/status` on your phone to see what's running and playing.
- **Restarts itself if it crashes.** The start-at-boot script now runs MiSTerPlex under a small supervisor. Add `BOOT_SUPERVISE=0` to the settings file to go back to the old behaviour.
- The ready screen names the cast source while a video is loading.
- Optional `REPORT_CODE=1`: shows a short code and saves a log bundle (with private details removed) on the card, to help when you report a problem. Nothing is uploaded.
- Optional keyboard key P opens the program menu on the ready screen (`IDLE_MENU_KEY=1`).
- Install docs never overwrite your existing settings file.

### Fixes

- Fixed a crash in the on-screen Plex browser (artwork loading).
- Fixed a crash pressing Right along an empty row on the home screen.
- Fixed the home screen so remote-server titles can be selected and played.
- Fixed Esc returning to the wrong screen, and end-of-video handling.
- Fixed IPv6-only server addresses: MiSTerPlex falls back to a working IPv4 address.
- Fixed MiSTerPlex picking your router's address as the Plex server.
- Fixed the "Daemon not running" card showing as a black screen.
- AirPlay handing over to YouTube no longer stops the program.

### Security

- Fixed a command-injection bug in how Plex addresses were handled.
- The casting port only answers devices on your home network, and refuses requests from web pages on other sites.
- Your Plex token, OpenSubtitles key and password are no longer visible in the MiSTer's process list for most requests. Accepted risk (single-user MiSTer): a playback URL may still carry `X-Plex-Token` on ffmpeg's command line (`/proc/<pid>/cmdline`) on the device.
- Optional `LAN_AUTH=1`: require a shared secret for MiSTerPlex's own control commands.
- Secure connections (HTTPS) check certificates by default.
- **Never forward port 3005 or 8008 to the internet.**

### Packaging and license

- The release ships **one** FPGA core, `MiSTerPlex.rbf` (built from the PlexOB core; older builds named it `PlexOB.rbf`, and the installer removes that old file). The older `Plex.rbf` is no longer included.
- MiSTerPlex is licensed under the **GNU GPL version 3 or (at your option) any later version** (GPL-3.0-or-later), the same as the upstream PlexOB core. The vendored hps/ client library from owenb321 stays MIT. A `LICENSE` and `NOTICE` file are included, and the source for the core is shipped with the release. Source and releases: https://github.com/flynnsbit/misterplex-cast.
- New `install-misterplex.sh` (one-shot installer, on the MiSTer or with `--remote <mister-ip>` from a computer; safe to re-run, keeps your settings and makes backups) and `configure-misterplex.sh` (guided settings, or flags for scripts).

## rc3 / Phase 2 (draft, built from main `14ee38b4`)

_Naming note: "rc3 / Phase 2" is the release package of the v1.0.0 work above. The rc3 label predates rc5 and is kept for continuity with the staged builds; it is not newer than rc5._

Changes merged in PRs #286 to #298, #303, #306, #307 to #311, and #313. Core: `MiSTerPlex.rbf` (BUILD_ID `0x0B1101C5`). This is a draft entry until the release is tagged.

### Core

- 240p raster and the ss1c core forward-ported into main, BUILD_ID `0x0B1101C5` (#278).
- **The core is now called `MiSTerPlex.rbf`.** Older releases called it `PlexOB.rbf`. The installer puts the new core in `/media/fat/_Utility/` and removes the old `PlexOB.rbf` copies only after the new file's md5 checks out. See the [install guide](docs/INSTALL.md). The rename hook landed in #280.
- S-video / composite: `sys/yc_out.sv` now matches upstream Template_MiSTer `5dda3736` ("register the sine values, not their indexes") in both in-repo copies (#297).

### Security

- The Plex timeline token is sent only to the device that owns the cast (#287).
- A token carried over from an earlier cast is cleared, and timeline pushes are pinned to the same IP (#291).
- Timeline subscriptions are pinned to the subscribing peer, and unsubscribe needs both the id and that peer (#296).

### Install and docs

- New install guide, [docs/INSTALL.md](docs/INSTALL.md), with release notes for the NOTICE and README (#289).
- Troubleshooting note for YC (S-video / composite) output (#290).

### Plex, playback and display

- Subtitles: exported subtitle cues, and a fix to the `-I` include path for the ASan cache test (#303).
- Plex metadata: HTML entities in titles and summaries are decoded once at parse time, so no raw `&amp;` shows in titles (#307).
- 240p: the OSD cursor no longer lands on Send Home (#308).
- Tip A: the default flag set now ships on a new card (#309).
- Subtitles: audio is kept when subtitles are turned off, and the boot test's 3s flake is fixed (#306).
- Plex: the linked account's server is discovered, https bare-IP servers are dialed as plex.direct, and the token is picked by machine id (#310).
- Plex: on-device server picker with per-server tokens, set with `PLEX_SERVER_TOKENS` and `PLEX_SERVER_NAMES` (#311).

### Packaging and tests (no change on the MiSTer)

- Release export: neutral NOTICE rendered from the package template, and a name scan that reports, but does not fail on, maintainer-name mentions (#288, #292, #295, #298).
- Test suites green on main: daemon tests, RTL invariants, gcast pins, boot script, and ASan dependencies (#286, #292, #293, #294).
- Remaining `make unit` failures on main fixed (#313).
