# Changelog

All notable changes to MiSTerPlex, written for people running it on a MiSTer.

## v2.1.0 (27 November 2026)

Minor feature release bringing a complete AirPlay Audio (RAOP / Apple Music) overhaul with instant connect, dropout-free direct PCM playback, and a CRT-friendly cover-art Now Playing screen, plus a 1990s Prevue Blue & Gold redesign for the Live TV Streaming Guide and matching Channel Overlay.

### Features

- **AirPlay Audio (RAOP) Now Playing screen & cover art (`78dfa4f1`, `4658eaea`).** Added a vertically stacked, 4:3/CRT-friendly AirPlay Audio Now Playing layout displaying album artwork on top with track/artist/album metadata, codec badge (`AIRPLAY · AAC-LC` / `ALAC`), and amber progress scrubber below across **720p**, **480p/480i**, and **240p**. Pressing pad **A** toggles full-screen album cover art and returns to the standard layout.
- **Apple Music & iTunes desktop/mobile compatibility (`ea04db03`, `e6995217`).** Added clean-room FairPlay v2 (`0x02`) support for Apple Music / iTunes on macOS and Windows alongside classic RAOP `ANNOUNCE` + SDP `SETUP` negotiation.
- **1990s Prevue Blue & Gold Live TV Streaming Guide (`5d415fd9`).** Redesigned the Live TV / Spelunkr guide into a 1990s Prevue Blue & Gold 2-line channel grid with a top featured box (wrapped full show title, `NEXT:` preview, remaining time, progress bar), local clock, page counter, bordered `CH` cells, and full-page `L`/`R` paging so long show titles are never truncated.
- **Matching 90s Prevue Live TV Channel Overlay (`5d415fd9`, `7ccb421c`).** Redesigned the Live TV channel info / tune banner to match the Prevue guide with a gold `CH` dial plate, service badge (`S` / `BW` / `V` / `P`), `ON AIR` / `TUNING` status pill, local clock, `UP NEXT`, remaining time, and elapsed progress bar with playhead pip.
- **Updated live HDMI screenshot gallery (`7ccb421c`).** Added live 1280×720 HDMI captures of the new 90s Prevue Streaming Guide (`docs/screenshots/hdmi/streaming-guide.png`), Live TV Channel Overlay (`docs/screenshots/hdmi/live-tv-channel-overlay.png`), and AirPlay Audio Cover Art (`docs/screenshots/hdmi/airplay-audio-coverart.png`) to `README.md` and `docs/SCREENSHOTS.md`.

### Fixes

- **Eliminated ~30 s Apple Music AirPlay connect delay (`8ebe68f9`).** Added mDNS `NSEC` negative host replies for unmatched `AAAA` / `HTTPS` / `SVCB` queries so macOS / iOS / Windows Apple Music resolves the MiSTer immediately instead of waiting on mDNS timeouts.
- **Eliminated AirPlay audio dropouts & CPU contention (`e94592c0`, `3284d6e1`).** Routed AirPlay RAOP audio directly to `OwenbRing` PCM and moved cover-art JPEG/PNG decoding onto an async worker (removing the heavy `showspectrum`/x264 transcode loop), and stopped idle YouTube Lounge 5 Hz TLS reconnect churn while AirPlay is active.
- **AirPlay audio stream-copy & faster fallback AAC encode (`76f91ff7`, `flynnsbit/misterplex-cast#4`).** Stream-copies MPEG-TS-native audio codecs directly and speeds up fallback AAC encoding in the AirPlay helper.
- **Live TV channel overlay metadata preservation (`7ccb421c`).** Fixed `MediaPlayer::play()`'s `UiModel` reset and same-channel `UiBus::armPlayPending()` handoffs clearing the Prevue banner's service badge, clock, `UP NEXT`, remaining time, and progress bar right when video playback starts.

### Known issues

- **Live TV OSD service-switch timing.** Switching between Plex and Live TV via the core F12 **Service** menu typically takes ~6–8 s on a warm card (plus ~8 s for the first Live TV channel tune), and longer if the F12 OSD stays open during the switch.
- **Picture subtitles (PGS / VobSub).** Text subtitles (`SRT`/`ASS`) render on the overlay; picture subtitles require opt-in server burn-in (`BURN_SUBS_FIX=1` / `AUTO_BURN=1`).
- **Outputs not yet verified on real TVs.** S-Video, composite, analog CRT captures (`docs/screenshots/analog/`), CEA 1080p, and `HDMI Native 960x480` 2x browse layout await real-TV verification.
- **Optional flags awaiting end-to-end TV testing.** `YT_CAPTIONS=1` and `YT_LIVE=1` (off by default) await end-to-end TV testing.
- **LAN-only network trust.** Never forward TCP `3005`, TCP `7000`, or TCP `8008` to the internet or an untrusted network.

## v2.0.1 (25 November 2026)

Bugfix and polish release over v2.0.0, fixing Vevo/Pluto post-ad-break A/V desync, Pluto 55 s `PREFETCH TIMEOUT`, and cold-boot Spelunkr guide landing, plus a Cold-Boot Guide and consolidated known-issues documentation.

### Fixes

- **Pluto / Vevo Ad Break card timing sync.** `MediaPlayer::adFramePositionMs()` now polls the FPGA hardware `RC_PIC` decoded-frame counter (`OwenbPlayer::coreStatus().picsReconstructed`) directly on `OwenbRing` sessions instead of the demux-thread position (`adLastPosMs_`), so the Ad Break card waits until buffered DDR frames actually finish playing out on screen before showing right at the end of the video instead of overlaying several seconds early.
- **Installer & configurator fixes.** Aligned `configure-misterplex.sh` (`--safe-margin` CLI, dialog, and help) to `0`–`15` matching `misterplexd`'s `UI_SAFE_MARGIN` clamp, and updated `install-misterplex.sh` to start `misterplexd` immediately at the end of a live MiSTer install when `--reboot` is not passed.
- **Unsupported audio codec direct-play fix (DTS/`dca`, TrueHD, MLP, MP2, etc.).** Direct-play gating now checks `audioCodecSupportedForDirectPlay()` against the static `libavcodec` decoder set; titles whose selected audio track is not compiled in (such as DTS 5.1 MKVs) now automatically force PMS universal transcoding to stereo AAC instead of playing video with silence, and `openAudioDecoder()` logs a clear warning if a decoder ever fails to open.
- **Remote Plex server locality & first-click HTTP 401 fix.** Public/WAN seed URLs now classify as `remote` immediately on cold boot instead of `local`; `resolveServerBases()` probes candidate URLs in parallel with immediate cancellation of losing probes; `mergeInto()` preserves `plex.tv` tokens/ownership; and `openLibraries()` / `runSearch()` automatically refresh credentials and retry on missing token or HTTP 401.
- **On-device Plex Browse additions.** Added **Recommended** (Continue Watching / On Deck / Recently Added), persistent **Last Page** resume per server (`last_pages.json`), and server-wide **Search** at the top of Libraries (keeping initial focus on the first real library), plus episode `S01E03` / `Show · S01E03` formatting, 3 px amber Grid progress bars, and `Y` server refresh, while keeping `0.0` heap allocations/event on the nav benchmark.
- **Unified controller buttons across Idle, Plex Browse, Plex Playback, and Live TV.** Standardized **Start** (`ObNav::Menu`) to always open/close the MiSTerPlex sub-menu, **A** = Select / Play-Pause / Tune, **B** = Back / Close / Stop (inert on Live TV when guide is closed), **X** = Search (Browse) / Streaming Guide toggle (Live TV) / Skip Forward (Playback), and **Y** = Grid/List or Refresh (Browse) / Channel Info banner (Live TV) / Skip Back (Playback). Fixed OSD yielding over Browse and Live TV Guide and full-band surface restore when closing the sub-menu over Browse.
- **Vevo / Pluto ad-break → next video A/V sync.** On Pluto ad-skip content return, `LiveRelay` invokes `MediaPlayer::reopenLiveAfterAdBreak()` to cleanly reopen `/yt-live.ts` on a fresh `-copyts -avoid_negative_ts make_zero` first spawn (running `AvSource::avStartGate()` `skip_gop` and flushing `OwenbRing` STC) while holding the Ad Break card until aligned picture runs (`reopen=1`, `ad card cleared why=ad_reopen_picture`). `live_disc::notePacket()` also invalidates the opposite stream's stale pre-splice raw PTS on any discontinuity rebase so a cross-stream splice triggers one rebase instead of two, and `OwenbPlaySession::readOne()` honors a queued reopen `Seek` on intentional old-socket EOF.
- **Cold-boot Live TV (`Spelunkr`) lands on the Streaming Guide (`481f7634`).** On cold daemon start / first core open (`!preferCurrent`), Spelunkr opens the Streaming Guide by design even when `SPELUNKR_LAST_CHANNEL` is set; in-session core reopens continue resuming the current or saved last channel.
- **Pluto 55 s `PREFETCH TIMEOUT` fix (`49499247`).** `HeaderCacheAvio::doRead()` updates header-fetch progress only while recording (stopping steady-state playback on another channel from poisoning a concurrent Pluto tune's zap window), `tryPlutoLiveOpenRetry()` re-arms `zapWindowRestarts`/`zapWindowStartMs` for bounded retries, `avStartGate()` sets a single-consumption `skip_gop` progress guard, and `guideThr` yields between manifest fetches while a playjob is active.
- **Documentation & test hygiene.** Added a Cold-Boot Guide to `README.md` and `packaging/rc3/README.md`, verified flag tables and clean `configure-misterplex` screenshots, updated `test_spelunkr_auto_video.py` for the `onPlexObReopen()` refactor, and verified `test_cold_boot_land_guide`, `test_pluto_55s_timeout_fix`, `test_spelunkr_schedule`, `test_live_disc_rebase`, and `test_pluto_adskip_relay` (with mutants).

### Known issues

- **Live TV OSD service-switch timing.** Switching between Plex and Live TV via the core F12 **Service** menu typically takes ~6–8 s on a warm card (plus ~8 s for the first Live TV channel tune), and longer if the F12 OSD stays open during the switch.
- **Picture subtitles (PGS / VobSub).** Text subtitles (`SRT`/`ASS`) render on the overlay; picture subtitles require opt-in server burn-in (`BURN_SUBS_FIX=1` / `AUTO_BURN=1`).
- **Outputs not yet verified on real TVs.** S-Video, composite, analog CRT captures (`docs/screenshots/analog/`), CEA 1080p, and `HDMI Native 960x480` 2x browse layout await real-TV verification.
- **Optional flags awaiting end-to-end TV testing.** `YT_CAPTIONS=1` and `YT_LIVE=1` (off by default) await end-to-end TV testing.
- **LAN-only network trust.** Never forward TCP `3005` or TCP `8008` to the internet or an untrusted network.

## v2.0.0 (25 November 2026)

Major release consolidating the full MiSTerPlex v2.0.0 package, unified `Scripts > configure-misterplex` menu, `update_all` / MiSTer Downloader support, 2x HDMI/480p PlexUI browse shell, Live TV / Spelunkr improvements, AirPlay stability fixes, and the `BUILD_ID 0x0B112501` (seed 14, md5 `7f048508861967f857a5aacff79bfb6f`) FPGA core.

### Features

- **Unified `Scripts > configure-misterplex` configurator and `update_all` support.** Installs to `/media/fat/Scripts/configure-misterplex.sh` with a 4-item interactive `dialog` menu (Settings, Restart MisterPlex, Repair / Reinstall, Uninstall), automatic first-run setup when invoked after `update_all`, 14 CLI flags, 10 user-facing `MPX_*` environment variables, and `--selftest`. Forces `BURN_SUBS_FIX=0` and `AUTO_BURN=0` on every settings save.
- **Seed-14 `MiSTerPlex.rbf` core (`0x0B112501`, md5 `7f048508`).** Default core pin updated from `96eb13d2` (`0x0B1101C5`) to `7f048508` (`0x0B112501`, `YC_SEED=14`), updating `sys/ascal.vhd` to `Template_MiSTer baaeff6` (`o_state` reset at vsync) to eliminate HDMI snow/tearing after 480i OSD switching.
- **2x PlexUI browse shell (`FONT-2X`) for HDMI / 960x480 / 480p.** ~2x scaled browse text, tile layout, and 2x MiSTerPlex logo header on HDMI 720p, HDMI Native 960x480, and 480p outputs, with whole-row server-title rail scrolling and 480i kept at 1x.
- **Faster Plex library browsing and search navigation.** Visible posters load first with up to 3 concurrent poster requests while remaining catalog pages fetch in the background; `Servers` host/locality and `Libraries` titles ellipsize cleanly before the right column; typed Search preserves `searchOrigin_` so Back cannot wipe `Servers`.
- **Cast-target advertisement gated on active core.** Plex companion / cast targets advertise only while a MiSTerPlex core is loaded.
- **HDMI 1280x720 OB request floor (`2000k`).** Prevents Plex browse playback from falling back to `720x405` on 1280x720 HDMI output.
- **Pluto / Live TV (Spelunkr) ad-pod path skip, live remux reserve, and ad-break card.** Ships `misterplex-ad-break.jpg` in the package, skips ad pods by path, grows a live remux reserve, and tunes cleanly across Pluto ad pods.
- **Full-package installer (`install-misterplex.sh`).** Installs one core in `/media/fat/_Utility/MiSTerPlex.rbf`, writes `<path>.bak.<stamp>` backups when files change, saves superseded cores in `/media/fat/misterplex/backup/`, preserves `misterplex.conf` and tokens (seeding `PLUTO_LIVE=1`, `AIRPLAY_MIRROR=1`, `CRT_LOOK` off when missing), installs both `linux/airplay/` and `linux/gcast/`, and cleans up legacy `Scripts/misterplex_setup.sh` and `misterplex/{licenses,docs,scripts}/`.

### Fixes

- **Never auto-load a core at boot or daemon start.** `misterplexd-boot-rc3.sh` and `misterplex_mode.sh` (`6369fa6d`) never load a core on boot and never assign or export `MPX_MODE_SWITCH`; service switching strips unexpected environment overrides cleanly.
- **Honor `0x0B112501` Video Output / 240p.** Added `0x0B112501` to the daemon's recognized PlexOB build ID tables so non-480i video modes (480p, 720p, 240p) are not forced to 480i.
- **Ping-pong back buffer & skipped dirty-band recovery.** `ob-plexui` keeps the ping-pong back buffer seeded and never shared with the OSD, and performs a full `memcmp` scan blit whenever a dirty-band generation was skipped (fixes partial/black menu reopen frames).
- **Live TV / Pluto 1066 & Vevo playback stability.** Added first-spawn A/V start gate for split Pluto renditions (`Vevo` start blocks / silent audio), fixed first-spawn PCM cancel/stall livelock, gave the Pluto remux open a dedicated 15s window without re-paying the first-byte path, raised first-stall restart threshold above 6s, fixed ad-hold timeout key release and watchdog exemption end, prevented failed-tune clears from wiping Guide landings, added Pluto 1080p capability fallback (`NO NEXT` fix), prevented long-lead silent starts, painted wiped PlexOB overlays while holding a silent audio key, and added core-gate `reopen2` fallback (`d849e330`) for the 1066 reopen black-screen stall.
- **AirPlay reliability and URL refresh.** Held re-sent `/play` of the active stream with `/playback-info` restore and signed-URL refresh; pinned live running entries against `/admit` eviction; preserved VOD playhead on reopen; fixed cookie, VOD, and LAN `/refresh`; and re-resolved once on 403/410 from server or device `ffmpeg`.
- **Release & source-export packaging gates (`G4`, `G5`, `N1`).** Updated `package_release.sh` required `libav` protocol check (`file,http,tcp,pipe,https,tls,udp,crypto`), `tools/export/make-export.sh` (`0x0B112501` seed 14 `SOURCE.md` and `README.md` export anchors), and scrubbed lab-host identifiers from build targets.

### Known issues

- **On-card helper `GCONV_PATH` SDT wrapper fix.** `run-airplay-xcode.sh` and `run-gcast-xcode.sh` populate `/tmp/mpx-gconv-stub/gconv-modules` and export `GCONV_PATH` so the bundled static `ffmpeg`/`ffprobe` do not abort (`rc=134`) on DVB SDT-bearing MPEG-TS or HLS streams.
- **Pluto TV / 1066 reopen & service-switch timing.** `PLUTO_LIVE=1` is on by default in the shipped `misterplex.conf.example` (`AIRPLAY_MIRROR=1` is also on by default in the shipped conf; `CRT_LOOK` stays off by default); the first Pluto try after startup may report `PREFETCH TIMEOUT` (retry after ~16 s). Rare 1066 reopen black screen (~10.1 s, 1/4 on glass) and ~30 s first tune after a service switch; `HDMI Native 960x480` 2x layout is unscored on live glass.
- **Installer `_Utility` cleanup & `UI_SAFE_MARGIN` clamp.** `install-misterplex.sh` moves any legacy `/media/fat/MiSTerPlex.rbf` into `/media/fat/misterplex/backup/` (leaving `_Utility/MiSTerPlex.rbf`); `configure-misterplex.sh --safe-margin` accepts `0`–`20` while `misterplexd` clamps above `15`.
- **Analog CRT & S-Video/composite hardware verification pending.** Analog CRT captures (`docs/screenshots/analog/`) and S-Video/composite TV validation remain post-release work; CEA 1080p output was not measured.
- **Unverified optional / guard features (post-release).** `AIRPLAY_MIRROR=1`, YouTube `YT_CAPTIONS=1` and `YT_LIVE=1`, shared-server TV browsing, and the companion browser-origin CSRF/CORS guard (`MPX_COMPANION_NO_ORIGIN` / `Origin` checks) have not been TV/glass-tested end-to-end.
- **`update_all` database publish pending final tag.** `mister-db/` workflow and `db.json.zip` pin update for `v2.0.0` follow public release tagging.

## v1.0.1 keep-chain (not a v1.0.2 release)

This is the v1.0.1 keep-chain for the batch / lab-class payload: installer and docs brought over from the v1.0.2 work, plus later keep-chain fixes, on the v1.0.1 code base. It is not a v1.0.2 release. The build id and the full list are filled in when a named cut is made.

- **One installer name.** Every package now has one installer, `install-misterplex.sh`, run from the unpacked package folder (`sh install-misterplex.sh`). The v1.0.1 minimal package called it `install.sh`; the README, `docs/INSTALL.md` and the package `README.txt` now give one command.
- **The minimal installer does the setup steps itself.** `install-misterplex.sh` now adds the start-at-boot line to `/media/fat/linux/user-startup.sh` (once, with a `.bak.<stamp>` copy), moves an old different core at `/media/fat/MiSTerPlex.rbf` into the backup folder (the boot script would load it first), and writes a `restore.sh` that undoes the install. A run that changes nothing makes no backup folder, so a fresh card no longer gets an empty one; old `PlexOB.rbf` files, if present, are saved first on every run, so then a re-run does make one. An existing settings file is never changed. If a step fails, it stops, names the step and prints the `restore.sh` command, which undoes everything done so far. `--dry-run` and `--no-boot-line` are available. The manual steps are kept in the docs only as a fallback. *(Gated on a fresh install and an update over v1.0.1 on a real MiSTer.)*
- **Install docs match the package.** The README, `docs/INSTALL.md` and the package `README.txt` describe what the minimal package installs and where: the core in `_Utility` only, the boot line, the settings file, old `PlexOB.rbf` files, undo with `restore.sh`, and the release tag vs the build id the program reports.
- **v1.0.1 core accepted.** The v1.0.1 core (`MiSTerPlex.rbf`, md5 `042f86171b562a61694da46860b89d89`, BUILD_ID `0x0B111401`, seed 10) is on the installer's core pin list, so installing over a v1.0.1 card or repackaging that core is no longer refused. The earlier pins stay accepted.
- *(AirPlay, 1062 / 1066, and other keep-chain items are listed with the named cut, not under a v1.0.2 heading.)*

## v1.0.0 (pre-release, 11 November 2026)

Everything below is new since the **rc5** pre-release, published on the public repo (flynnsbit/misterplex-cast, 21 October 2026). It covers the merged PRs from #16 to #357. Some items have not been checked on a real TV yet; those are marked *(not yet TV-tested)*.
- **Casting sign-in safety:** the caster's token is sent only to a server whose identity was checked; after a refused cast it is not used for seek or play-queue requests, and an IPv6 home-network cast address is not kept for timeline updates (#355). Timeline fallback and mark-watched gate the token against the host they actually send to (#361, #362). *(not yet TV-tested)*
- **Picture modes on every source:** Program-menu Letterbox / 14:9 / Zoom 4:3 / Stretch / Fit-crop apply on the shared OwenbRing scaler, including AirPlay video, screen mirroring, Plex cast, YouTube cast, and Spelunkr. *(not yet TV-tested)*
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
- AirPlay **screen mirroring** (`AIRPLAY_MIRROR=1`, **on by default** in shipped `misterplex.conf.example`; built-in code default stays off when unset). *(experimental, not yet TV-tested)*
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
