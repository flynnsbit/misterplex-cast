# MiSTerPlex screenshots

Live HDMI captures from a test MiSTer, covering the **Ready** banner, **Scripts** configurator (`configure-misterplex.sh` main menu and Settings dialog), **Plex browser**, **AirPlay**, **YouTube**, **Live TV / Streaming Guide**, **playback picture modes**, and **core OSD settings**. All captures are unmodified PNG frames with no fabricated screens and no private LAN IPs, server URLs, tokens, or lab names (Settings dialog captured with TEST-NET placeholder `192.0.2.10:32400`).

Click any image to open the full-size capture.

## Status, Scripts & Casting (HDMI)

| Ready screen (`AirPlay · YouTube`) | MiSTer Scripts (`configure-misterplex`) | Core OSD (`Service: Live TV`) |
| --- | --- | --- |
| [![Ready screen showing AirPlay and YouTube](screenshots/hdmi/ready-airplay-youtube.png)](screenshots/hdmi/ready-airplay-youtube.png) | [![MiSTer Scripts menu highlighting configure-misterplex](screenshots/hdmi/scripts-list-configure-misterplex.png)](screenshots/hdmi/scripts-list-configure-misterplex.png) | [![Core OSD with Service set to Live TV](screenshots/hdmi/osd-livetv-core.png)](screenshots/hdmi/osd-livetv-core.png) |
| Flags: `FRIENDLY_NAME`, `GAIRPLAY`, `GYT`, `PLEXTV_LINK`, `IDLE_SCREEN`, `UI_SAFE_MARGIN` | Unified `/media/fat/Scripts/configure-misterplex.sh` entry | Flags: `OSD_CONTROL`, `SPELUNKR` (`Service: Live TV`) |

| AirPlay video playback | AirPlay audio (cover art & scrubber) | YouTube playback (`Big Buck Bunny`) |
| --- | --- | --- |
| [![AirPlay video playback](screenshots/hdmi/airplay-playback.png)](screenshots/hdmi/airplay-playback.png) | [![AirPlay audio Now Playing with album cover art](screenshots/hdmi/airplay-audio-coverart.png)](screenshots/hdmi/airplay-audio-coverart.png) | [![YouTube Big Buck Bunny playback](screenshots/hdmi/youtube-playback.png)](screenshots/hdmi/youtube-playback.png) |
| Flags: `GAIRPLAY`, `AIRPLAY_XCODE` / `GCAST_XCODE`, `AIRPLAY_MIRROR`, `REMEMBER_PICTURE` | Flags: `GAIRPLAY`, `GAIRPLAY_PORT` (pad `A` toggles full-screen art) | Flags: `GYT`, `YT_CAST_TITLE`, `YT_VOLUME`, `YT_NEXT_PREV`, `YT_CAPTIONS`, `YT_LIVE` |

| Live TV Streaming Guide (90s Prevue grid) | Live TV Channel Overlay (`ON AIR` banner) |
| --- | --- |
| [![Live TV Streaming Guide 90s Prevue channel grid](screenshots/hdmi/streaming-guide.png)](screenshots/hdmi/streaming-guide.png) | [![Live TV 90s Prevue channel overlay over broadcast](screenshots/hdmi/live-tv-channel-overlay.png)](screenshots/hdmi/live-tv-channel-overlay.png) |
| Flags: `SPELUNKR`, `SPELUNKR_CHANNEL`, `SPELUNKR_BACKWHEN`, `PLUTO_LIVE`, `SPELUNKR_ASPECT` | Matching 90s Prevue dial plate, `ON AIR` badge, clock, `UP NEXT`, and progress bar |

## Scripts configurator (`configure-misterplex.sh`)

HDMI stills of the v2.0.0 4-item `dialog` menu and 8-row **Settings** dialog (**Scripts > configure-misterplex**). The four main-menu items are actions, not conf keys: **1 Settings**, **2 Restart MisterPlex**, **3 Repair / Reinstall**, **4 Uninstall**.

| `configure-misterplex.sh` main menu | `configure-misterplex.sh` Settings dialog |
| --- | --- |
| [![configure-misterplex 4-item main menu](screenshots/hdmi/configure-main-menu.png)](screenshots/hdmi/configure-main-menu.png) | [![configure-misterplex Settings 8-option dialog](screenshots/hdmi/configure-settings-dialog.png)](screenshots/hdmi/configure-settings-dialog.png) |

**1 Settings** opens the 8-option dialog (shown with TEST-NET placeholder `192.0.2.10:32400` and `My Plex Player`), which writes `/media/fat/linux/misterplex.conf`. Row-to-flag map:

| Settings row | conf key | CLI flag |
| --- | --- | --- |
| Plex server | `PLEX_SERVERS` | `--server` |
| plex.tv link | `PLEXTV_LINK` | `--link` |
| Analog output | `ANALOG_RASTER` | `--raster` |
| Text subtitles | `SUBTITLE_LANG` (`off`, or a language code when on) | `--subtitles` |
| Subtitle language | `SUBTITLE_LANG` | `--sub-lang` |
| Menu size | `UI_SIZE` | `--ui-size` |
| Safe margin | `UI_SAFE_MARGIN` | `--safe-margin` |
| Player name | `FRIENDLY_NAME` | `--name` |

## Plex browser (HDMI)

Captured read-only on the test MiSTer (pre-v1.0.0 1x layout; see Placeholders below for the v2.0.0 2x HDMI layout):

| Home | Movie library | Title detail |
| --- | --- | --- |
| [![Plex home rows](screenshots/hdmi/plex-browser-home.png)](screenshots/hdmi/plex-browser-home.png) | [![Plex movie library grid](screenshots/hdmi/plex-browser-library.png)](screenshots/hdmi/plex-browser-library.png) | [![Plex title detail for batteries not included](screenshots/hdmi/plex-browser-title-detail.png)](screenshots/hdmi/plex-browser-title-detail.png) |
| Flags: `AMBIENT`, `CRT_LOOK`, `WATCHED_BADGES`, `HIDE_EMPTY_HUBS`, `UI_SIZE`, `UI_SAFE_MARGIN` | Flags: `GRID_PAGE`, `AZ_JUMP`, `FILTER_4_3`, `WATCHED_BADGES`, `UI_SIZE` | Flags: `SETSTREAMS`, `SUBTITLE_LANG`, `FORCED_SUBS`, `REMEMBER_TRACKS`, `MARK_WATCHED` |

## Playback picture modes (HDMI)

Playback titles: *The Fifth Element* (widescreen) and *Star Trek: The Next Generation* (4:3). Scenes advance between captures; these are not same-frame geometry comparisons. Related flags: `PICTURE_14_9`, `REMEMBER_PICTURE`, `AUTO_PICTURE_MODE`, `CHAPTERS`, `IN_PLAY_OPTIONS`, core `Analog Aspect` and `Aspect Ratio`.

| Mode | Playback | Selected mode |
| --- | --- | --- |
| Widescreen: Letterbox | [![Widescreen letterbox playback](screenshots/hdmi/widescreen-letterbox.png)](screenshots/hdmi/widescreen-letterbox.png) | [![Letterbox selected during playback](screenshots/hdmi/in-play-picture-menu.png)](screenshots/hdmi/in-play-picture-menu.png) |
| Widescreen: 14:9 | [![Widescreen 14 by 9 playback](screenshots/hdmi/widescreen-14-9.png)](screenshots/hdmi/widescreen-14-9.png) | [![14 by 9 selected during playback](screenshots/hdmi/widescreen-14-9-selected.png)](screenshots/hdmi/widescreen-14-9-selected.png) |
| Widescreen: Zoom | [![Widescreen zoom playback](screenshots/hdmi/widescreen-zoom.png)](screenshots/hdmi/widescreen-zoom.png) | [![Zoom 4 by 3 selected](screenshots/hdmi/widescreen-zoom-selected.png)](screenshots/hdmi/widescreen-zoom-selected.png) |
| Widescreen: Fit / Stretch | [![Widescreen fit playback](screenshots/hdmi/widescreen-fit.png)](screenshots/hdmi/widescreen-fit.png) | [![Stretch selected during widescreen playback](screenshots/hdmi/widescreen-fit-selected.png)](screenshots/hdmi/widescreen-fit-selected.png) |
| 4:3: Native | [![Native 4 by 3 episode playback](screenshots/hdmi/four-three-native.png)](screenshots/hdmi/four-three-native.png) | [![Native 4 by 3 playback menu](screenshots/hdmi/four-three-picture-menu.png)](screenshots/hdmi/four-three-picture-menu.png) |
| 4:3: Stretch | [![Stretched 4 by 3 episode playback](screenshots/hdmi/four-three-stretch.png)](screenshots/hdmi/four-three-stretch.png) | [![Stretch selected during 4 by 3 playback](screenshots/hdmi/four-three-stretch-selected.png)](screenshots/hdmi/four-three-stretch-selected.png) |

## Menus, settings & other screens (HDMI)

| In-play aspect controls (`OSD_CONTROL`) | Core settings (`ANALOG_RASTER`, `DEINTERLACE`) | Playback options (`IN_PLAY_OPTIONS`) |
| --- | --- | --- |
| [![Open aspect ratio controls during playback](screenshots/hdmi/in-play-aspect-options.png)](screenshots/hdmi/in-play-aspect-options.png) | [![Native core video and analog settings](screenshots/hdmi/core-settings.png)](screenshots/hdmi/core-settings.png) | [![Playback options page](screenshots/hdmi/in-play-options.png)](screenshots/hdmi/in-play-options.png) |

| Safe-area menu entry (`OVERSCAN_*`) | Safe-area adjustment (`OVERSCAN_LEFT`) | Loading Plex (`MPX_IDLE_CARD`) | Ready / status (`AMBIENT`, `REPORT_CODE`) |
| --- | --- | --- | --- |
| [![Safe area selected in the program menu](screenshots/hdmi/screen-safe-area.png)](screenshots/hdmi/screen-safe-area.png) | [![Screen safe-area edge adjustment](screenshots/hdmi/screen-safe-area-setting.png)](screenshots/hdmi/screen-safe-area-setting.png) | [![Loading Plex screen](screenshots/hdmi/loading-plex.png)](screenshots/hdmi/loading-plex.png) | [![Restored production ready status](screenshots/hdmi/ready-status.png)](screenshots/hdmi/ready-status.png) |

## Placeholders (missing shots — pending capture)

The following shots remain pending post-release capture:

| Placeholder | Intended filename | Status / reason missing |
| --- | --- | --- |
| v2.0.0 2x PlexUI browse shell (`FONT-2X` + 2x MiSTerPlex logo on HDMI 720p / 960x480) | `screenshots/hdmi/plex-browser-home-2x.png` | Existing glass 720p/960x480 captures showed lab server hostnames/IPs |
| Analog CRT captures (240p, 480i, S-Video/composite) | `screenshots/analog/` | Blocked: analog capture hardware absent; see [analog directory](screenshots/analog/README.md) |
