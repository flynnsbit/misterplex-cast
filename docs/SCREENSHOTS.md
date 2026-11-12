# MisterPlex screenshots

Live captures from the MiSTer, taken on **2026-11-08** using
a capture PC's HDMI digital capture. These are 1280x720 PNG frames;
the black borders, scanline look, and menu wording are the device's actual output.
Two frames have on-screen identifiers covered.
Click an image to open the full-size capture.

**Analog coverage is blocked:** the MS2109 analog capture was absent from both
USB enumeration and `/dev/v4l/by-id/`. Only the digital capture's video and
metadata nodes were present. No HDMI image has been relabeled as analog.
[The analog directory](screenshots/analog/README.md) records the outstanding set.

## Plex browser

The browser was navigated read-only: no Play, watched-state, or library-edit
actions were selected on the shared server.

| Home | Movie library |
| --- | --- |
| [![Plex home rows](screenshots/hdmi/plex-browser-home.png)](screenshots/hdmi/plex-browser-home.png) | [![Plex movie library grid](screenshots/hdmi/plex-browser-library.png)](screenshots/hdmi/plex-browser-library.png) |

### Title detail

[![Plex title detail for batteries not included](screenshots/hdmi/plex-browser-title-detail.png)](screenshots/hdmi/plex-browser-title-detail.png)

## Playback

Playback titles came from a lab Plex server: *The Fifth Element* (widescreen) and *Star Trek: The Next Generation*
(4:3). Playback was driven through existing Companion controls and the daemon's
keyboard input. This is screenshot documentation, not a Plex Web cast sign-off.

The mode-selection captures alongside each clear playback frame show the live
menu label. Scenes advance between captures; these are not same-frame geometry
comparisons. This installed build calls zoom **Zoom 4:3**, and its Fit mode's
program-menu label is **Stretch** (the core menu calls it **Fit**).

| Mode | Playback | Selected mode |
| --- | --- | --- |
| Widescreen: Letterbox | [![Widescreen letterbox playback](screenshots/hdmi/widescreen-letterbox.png)](screenshots/hdmi/widescreen-letterbox.png) | [![Letterbox selected during playback](screenshots/hdmi/in-play-picture-menu.png)](screenshots/hdmi/in-play-picture-menu.png) |
| Widescreen: 14:9 | [![Widescreen 14 by 9 playback](screenshots/hdmi/widescreen-14-9.png)](screenshots/hdmi/widescreen-14-9.png) | [![14 by 9 selected during playback](screenshots/hdmi/widescreen-14-9-selected.png)](screenshots/hdmi/widescreen-14-9-selected.png) |
| Widescreen: Zoom | [![Widescreen zoom playback](screenshots/hdmi/widescreen-zoom.png)](screenshots/hdmi/widescreen-zoom.png) | [![Zoom 4 by 3 selected](screenshots/hdmi/widescreen-zoom-selected.png)](screenshots/hdmi/widescreen-zoom-selected.png) |
| Widescreen: Fit / Stretch | [![Widescreen fit playback](screenshots/hdmi/widescreen-fit.png)](screenshots/hdmi/widescreen-fit.png) | [![Stretch selected during widescreen playback](screenshots/hdmi/widescreen-fit-selected.png)](screenshots/hdmi/widescreen-fit-selected.png) |
| 4:3: Native | [![Native 4 by 3 episode playback](screenshots/hdmi/four-three-native.png)](screenshots/hdmi/four-three-native.png) | [![Native 4 by 3 playback menu](screenshots/hdmi/four-three-picture-menu.png)](screenshots/hdmi/four-three-picture-menu.png) |
| 4:3: Stretch | [![Stretched 4 by 3 episode playback](screenshots/hdmi/four-three-stretch.png)](screenshots/hdmi/four-three-stretch.png) | [![Stretch selected during 4 by 3 playback](screenshots/hdmi/four-three-stretch-selected.png)](screenshots/hdmi/four-three-stretch-selected.png) |

## Menus and settings

The live UI cycles picture modes from the program menu; it does not have a
separate expanded aspect dropdown. The native core settings are open below,
with **Aspect Ratio** highlighted and **Analog Aspect** visible.

| In-play aspect controls | Core settings |
| --- | --- |
| [![Open aspect ratio controls during playback](screenshots/hdmi/in-play-aspect-options.png)](screenshots/hdmi/in-play-aspect-options.png) | [![Native core video and analog settings](screenshots/hdmi/core-settings.png)](screenshots/hdmi/core-settings.png) |

| Safe-area menu entry | Safe-area adjustment |
| --- | --- |
| [![Safe area selected in the program menu](screenshots/hdmi/screen-safe-area.png)](screenshots/hdmi/screen-safe-area.png) | [![Screen safe-area edge adjustment](screenshots/hdmi/screen-safe-area-setting.png)](screenshots/hdmi/screen-safe-area-setting.png) |

### Playback options

The options screen shows the live build's **Picture size pending** status.

[![Playback options page](screenshots/hdmi/in-play-options.png)](screenshots/hdmi/in-play-options.png)

## Other screens

| Loading Plex | Ready / status after restoring production |
| --- | --- |
| [![Loading Plex screen](screenshots/hdmi/loading-plex.png)](screenshots/hdmi/loading-plex.png) | [![Restored production ready status](screenshots/hdmi/ready-status.png)](screenshots/hdmi/ready-status.png) |

The transient standalone now-playing banner was not captured reliably; the
in-play menus above show the current title and, for the widescreen title, its
position instead.

## Capture and lease notes

Capture device:

```text
/dev/v4l/by-id/usb-USB3.0_HD_Audio_Capture_USB3.0_HD_Video_Capture-video-index0
```

Each capture discarded 45 warm-up frames before saving one PNG:

```sh
ffmpeg -hide_banner -loglevel error \
  -f v4l2 -input_format mjpeg -video_size 1280x720 \
  -i /dev/v4l/by-id/usb-USB3.0_HD_Audio_Capture_USB3.0_HD_Video_Capture-video-index0 \
  -vf 'select=gte(n\,45)' -frames:v 1 -y OUT.png
```

Browser input used `/player/navigation/*` with a client identifier. Program and
core menus used the existing `/dev/uinput` keyboard helper, with temporary M/Tab aliases.

Production on the MiSTer was stopped **before playback**. An isolated configuration
and a read-only PMS proxy were used for lab-title playback; the proxy rejected
timeline writes and mutation routes. Safe-area editing was canceled. Production
was restored through its existing boot hook afterward, including its supervisor
and AirPlay helper. No binary or core was deployed, no other unit was contacted, and
the production configuration, daemon, and RBF remained unchanged.

These captures were taken on a pre-v1.0.0 build.
Credentials and private session logs are not included in this gallery.
