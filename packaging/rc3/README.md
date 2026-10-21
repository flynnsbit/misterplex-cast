# MiSTerPlex

MiSTerPlex turns your MiSTer FPGA into a living-room media player. You can cast to it from Plex, YouTube and AirPlay, or browse your Plex library right on the TV with a controller. The video is decoded in hardware by the MiSTer's FPGA, and it plays over HDMI or on a CRT through the analog output.

![How MiSTerPlex fits together](docs/architecture.png)

*Your phone or the on-screen library tells the MiSTer what to play. The MiSTer fetches the video from your Plex server, YouTube or your Apple device. The PlexOB core on the FPGA decodes it and sends it to your TV.*

> **This is the rc5 pre-release.** It's for testing, and some things are still rough. Back up your SD card before you install it. The start-at-boot file is still named `misterplexd-boot-rc3.sh`. That name stays, so a card that already boots with it keeps working.

---

## Contents

- [What you can do](#what-you-can-do)
- [What you need](#what-you-need)
- [Quick install](#quick-install)
- [Using MiSTerPlex](#using-misterplex)
- [Keeping YouTube working](#keeping-youtube-working)
- [Troubleshooting and known issues](#troubleshooting-and-known-issues)
- [Uninstall](#uninstall)
- [Advanced](#advanced)
- [Source and license](#source-and-license)
- [Thanks](#thanks)

---

## What you can do

**Cast from Plex.** Open the Plex app on your phone or tablet, or Plex Web in a browser, and pick your MiSTer as the player. Play, pause, seek, skip and stop all work from the app. When you cast an episode, the next one in the queue plays on its own.

**Cast from YouTube.** Tap the cast button in the YouTube app and pick your MiSTer. Videos play at up to 720p. Play, pause and seek work from your phone, and stop usually does (the first stop after a cold boot can be lost; see Known issues). MiSTerPlex looks up YouTube videos on the MiSTer itself, so you don't need a computer running alongside it.

**Cast with AirPlay.** Your MiSTer shows up as an AirPlay receiver on iPhones, iPads and Macs. AirPlay works on the MiSTer by itself; a helper on a computer is optional. See [Casting with AirPlay](#casting-with-airplay).

**Browse Plex on the TV.** With a controller or keyboard, you can open a simple library list on screen, pick a movie or show, and play it. You don't need your phone.

**Watch at up to 720p.** The PlexOB core decodes H.264 video in hardware at up to 1280×720 and 30 frames per second. Over HDMI you get a sharp picture at the video's own size. On a CRT you can use 480i or 480p.

**Let Plex do the converting.** If a video won't play directly on the MiSTer (HEVC/H.265, 1080p or 4K, 60 fps, 10-bit, or surround sound), MiSTerPlex asks your Plex server to convert it to something the MiSTer can play. It picks a lower bitrate for servers outside your home, and steps down by itself if the server can't keep up. You don't need to install anything on your Plex server.

**Plug in and go.** After a reboot, MiSTerPlex loads its core and starts by itself. You land on a ready screen that shows your MiSTer's name and which kinds of casting are on (Plex, AirPlay, YouTube).

**Names itself.** If you don't give it a name, your MiSTer names itself after its network address (for example `misterplex-` followed by the last three numbers of its IP address) and keeps that name across reboots. A name you set yourself is never changed.

**Keeps the clock right.** YouTube and secure websites need the correct date. If your MiSTer boots with the wrong time, MiSTerPlex sets it from the MiSTer's real-time clock if you have one, or asks the internet for the time.

**Analog S-Video and composite (untested).** The core includes S-Video and composite output, but nobody has tried them on a real TV yet.

**Coming later:** 240p output, subtitles, picking audio and subtitle tracks from your phone, and a nicer library browser with artwork. These are not in this release.

---

## What you need

- **A MiSTer** (DE10-Nano) on your home network, wired or Wi-Fi. No SDRAM board needed; PlexOB uses DDRAM only.
- **SSH access to the MiSTer** and a computer with `ssh` and `scp`. On Windows 10 and 11 these come built in. You'll need your MiSTer's root password.
- **About 200 MB** free on the SD card. The package, the unpacked YouTube helper, and the on-card AirPlay helper together need about that much.
- **For Plex:** a Plex account, and at least one Plex Media Server you can play from. That can be your own server, or one a friend has shared with you. See [Your own server, or someone else's?](#your-own-server-or-someone-elses) below.
- **For YouTube:** the YouTube app on a phone or tablet on the same network as the MiSTer, and internet access for the MiSTer.
- **For AirPlay:** an iPhone, iPad or Mac on the same network. AirPlay works on the MiSTer by itself; a helper on a computer is optional (`GCAST_XCODE` overrides the on-card helper).

Your phone and the MiSTer must be on the same home network. That's how the cast menu finds the MiSTer. Your Plex server doesn't have to be at home.

### Your own server, or someone else's?

**You run your own Plex Media Server.** This is the simplest setup. Put your server's address in the settings file during install, and both casting and on-screen browsing will work.

**You only watch servers that other people share with you.** Casting from the Plex app is the main way to do this. When you cast, the Plex app tells the MiSTer where the server is and how to log in, so it can work with no settings at all. A few things to know:

- The server's owner must have **Remote Access** turned on, and the server must be reachable directly over the internet. Servers that only work through Plex's relay probably won't work.
- The MiSTer fetches video from remote servers over a plain (not encrypted) connection. If the owner has set **Secure connections** to **Required**, casting may fail. "Preferred" is fine.
- Remote playback depends on the owner's upload speed. MiSTerPlex asks remote servers for a lower bitrate than home servers.
- Some shared servers report a home network address that the MiSTer can't reach. In that case the MiSTer asks plex.tv for the server's public address. For that to work, put your own Plex token in `PLEX_TOKEN` in the settings file. See [Finding your Plex token](#finding-your-plex-token).
- On-screen browsing shows only **one** server: the one in `PLEX_BASE`. You can point it at a shared server, but this hasn't been tested yet. See [Browsing a shared server on the TV](#browsing-a-shared-server-on-the-tv).

Casting from a shared server through plex.tv works. Casting from a shared server addressed by its local network address can fail, and browsing a shared server on the TV is untested.

---

## Quick install

Download the release package from the [Releases page](https://github.com/flynnsbit/misterplex-cast/releases) and extract it on your computer. Open a terminal in the extracted folder, and run the steps below in order. Replace `<mister-ip>` with your MiSTer's IP address. You can find it in the MiSTer main menu under the network settings, or on your router.

**1. Check the download.**

```
sha256sum -c SHA256SUMS
```

Every line should say `OK`. On a Mac, use `shasum -a 256 -c SHA256SUMS`.

**2. Make the folders on the MiSTer.**

```
ssh root@<mister-ip> "mkdir -p /media/fat/misterplex /media/fat/_Utility /media/fat/linux"
```

**3. Copy the program and its files.** Stop MiSTerPlex first if it is already running, or this copy can fail with "Text file busy".

```
ssh root@<mister-ip> /media/fat/misterplex/bin/misterplexd-boot-rc3.sh stop
scp -r bin scripts docs licenses root@<mister-ip>:/media/fat/misterplex/
```

On a first install the stop command is not there yet. Skip it and copy.

**4. Copy the on-card AirPlay helper.**

```
scp -r linux/gcast root@<mister-ip>:/media/fat/linux/
```

That is `linux/gcast/gcast_xcode.py`, `linux/gcast/run-gcast-xcode.sh`, `linux/gcast/ffmpeg`, and `linux/gcast/ffprobe`. The boot hook starts it on 127.0.0.1 port 18781 and stops it on stop and uninstall.

**5. Copy the settings file, only if you do not already have one.** These two commands copy the example to the MiSTer and install it as `/media/fat/linux/misterplex.conf` only when that file doesn't exist yet. An existing settings file (your server address, token, and device name) is never replaced.

```
scp conf/misterplex.conf.example root@<mister-ip>:/tmp/misterplex.conf.example
ssh root@<mister-ip> 'test -f /media/fat/linux/misterplex.conf || cp /tmp/misterplex.conf.example /media/fat/linux/misterplex.conf; rm -f /tmp/misterplex.conf.example'
```

If you have your own Plex server, open `/media/fat/linux/misterplex.conf` on the MiSTer and add this line, using your server's address:

```
PLEX_BASE=http://<your-plex-server>:32400
```

If you'd also like to browse on the TV, add your Plex token on the next line (see [Finding your Plex token](#finding-your-plex-token)):

```
PLEX_TOKEN=<your-token>
```

If you only cast to shared servers, you can skip this step. Everything else in the file can stay as it is.

**6. Back up any cores that are already there.**

```
ssh root@<mister-ip> 'stamp=$(date +%Y%m%d%H%M%S); for f in /media/fat/PlexOB.rbf /media/fat/_Utility/PlexOB.rbf /media/fat/_Utility/Plex.rbf; do [ -f "$f" ] && cp -a "$f" "$f.bak.$stamp"; done; true'
```

**7. Install the cores.** The PlexOB core goes in both places the MiSTer looks for it. The command checks the file first and won't replace anything if the check fails.

```
scp cores/PlexOB.rbf cores/PlexOB.rbf.md5 root@<mister-ip>:/tmp/
scp cores/Plex.rbf root@<mister-ip>:/media/fat/_Utility/Plex.rbf
ssh root@<mister-ip> 'set -eu
  want=$(cut -d" " -f1 /tmp/PlexOB.rbf.md5); got=$(md5sum /tmp/PlexOB.rbf | cut -d" " -f1)
  [ "$got" = "$want" ] || { echo "PlexOB.rbf check failed, not installing" >&2; exit 1; }
  for d in /media/fat/PlexOB.rbf /media/fat/_Utility/PlexOB.rbf; do cp /tmp/PlexOB.rbf "$d.tmp"; sync; mv -f "$d.tmp" "$d"; done
  rm -f /tmp/PlexOB.rbf /tmp/PlexOB.rbf.md5'
```

**8. Copy the certificate bundle.** YouTube and secure connections need it.

```
scp linux/misterplex-cacert.pem root@<mister-ip>:/media/fat/linux/
```

**9. Turn on start at boot.** The `install` verb only adds the boot line. The hook starts the helper. This adds one line to the MiSTer's `user-startup.sh`, and saves a backup of that file first.

```
ssh root@<mister-ip> /media/fat/misterplex/bin/misterplexd-boot-rc3.sh install
```

**10. Reboot.**

```
ssh root@<mister-ip> reboot
```

In about a minute the TV shows the MiSTerPlex ready screen with your MiSTer's name. It lists which kinds of casting are on. YouTube appears on the list a little later than the others, once it has connected. The first boot also unpacks the YouTube helper, so YouTube works even before any update.

**Upgrading from an earlier test build?** Stop the program, keep the settings file you already have, copy the new files, then reboot.

```
ssh root@<mister-ip> /media/fat/misterplex/bin/misterplexd-boot-rc3.sh stop
ssh root@<mister-ip> 'if [ -f /media/fat/linux/misterplex.conf ]; then cp -a /media/fat/linux/misterplex.conf /media/fat/linux/misterplex.conf.bak.$(date +%Y%m%d%H%M%S); fi'
```

Then do steps 3, 4, 6, 7, 8, and 9. Skip step 5. Never copy `misterplex.conf.example` over `/media/fat/linux/misterplex.conf`. Reboot when the copy is done. A reboot loads the new core. The program also resets the picture when it starts, so a restart without a reboot no longer leaves the previous frame on screen.

---

## Using MiSTerPlex

### Casting from Plex

1. Make sure the MiSTer is on the ready screen.
2. In the Plex app or Plex Web, start playing a movie or episode.
3. Tap the cast or "Select player" button and pick your MiSTer's name.

You can control playback from the app: play, pause, seek, skip forward and back, and stop. When you cast an episode with more in the queue, the next one starts when it ends.

If the video won't play directly on the MiSTer, your Plex server converts it while you watch. That's normal, but it uses the server's processor. On a slow or busy server, the first picture can take a little longer.

### Casting from YouTube

1. Open the YouTube app on a phone or tablet on the same network.
2. Tap the cast button and pick your MiSTer's name.
3. Play a video.

Videos play at up to 720p and 30 frames per second. You can play, pause and seek from your phone, and a playlist moves on to the next video by itself. Stop from your phone usually works, but the first stop after a cold boot can be lost; press stop again or stop from Plex. Captions, volume and skipping within the queue from your phone don't work yet.

After a reboot, the MiSTer appears in the YouTube cast list once its YouTube connection is ready. That usually takes a few seconds after the ready screen shows up.

### Casting with AirPlay

AirPlay works on the MiSTer by itself; a helper on a computer is optional. Your MiSTer shows up as an AirPlay receiver with the same name you see on the ready screen. "AirPlay" is on that screen only when the on-card helper is up and its `/probe` self-test passes. `GCAST_XCODE` overrides the on-card helper: when it is set, that computer helper is the one that must pass.

The release ships `linux/gcast/gcast_xcode.py`, `linux/gcast/run-gcast-xcode.sh`, `linux/gcast/ffmpeg`, and `linux/gcast/ffprobe`. Install copies them with `scp -r linux/gcast root@<mister-ip>:/media/fat/linux/`. The `install` verb turns on the boot hook. The hook starts the helper on 127.0.0.1 port 18781 before MiSTerPlex, and stops it on stop and uninstall. YouTube does not use this helper. YouTube stays on the MiSTer's own yt-dlp path unless a healthy `GCAST_XCODE` is set.

To use a computer instead, run the same program there (`python3 gcast_xcode.py --bind 0.0.0.0 --port 18780`) and set `GCAST_XCODE`. `192.0.2.10` is a documentation-range example:

```
GCAST_XCODE=http://192.0.2.10:18780
```

With the helper running:

- **Videos** that apps send with AirPlay can play, including https and HLS.
- **Music and other audio** can play.
- **Screen mirroring** isn't supported.
- **Protected content**, like Apple TV+ or other DRM video, can't play. You'll see a message on screen.

Run a computer helper only on your home network. It has no password, and anyone who can reach it can ask it to fetch and convert video. The on-card helper listens on 127.0.0.1 only.

### Browsing Plex with a controller

You can browse the Plex server in your settings file (`PLEX_BASE`) straight from the TV.

1. On the ready screen, press **A** (or **Enter** on a keyboard) to open the library. **Down** also opens it.
2. Use **Up** and **Down** to move, and **A** / **Enter** or **Right** to open a library or play a title.
3. Press **B** (or **Esc**) to go back.

You can also press **Start** or **Y** (or **Tab** or **M** on a keyboard) to open the program menu. When nothing is playing, it has "Browse the library". During playback it has Pause/Resume, Back 10 s, Forward 30 s, Stop and Library.

The library is a simple text list for now. It shows one server, and it needs `PLEX_TOKEN` unless your server lets devices on your network in without signing in. Artwork, search and multiple servers are planned.

### Controls during playback

| Controller | Keyboard | What it does |
|---|---|---|
| A | Space or Enter | Play / pause |
| Right | Right arrow | Skip forward 30 seconds |
| Left | Left arrow | Skip back 10 seconds |
| B | Esc or Backspace | Stop and return to the ready screen |
| Start or Y | Tab or M | Program menu |

To set up a controller, use the MiSTer menu: press **F12** (or your OSD button), then **Define joystick buttons**. You can change how far the skip buttons jump in the settings file (`SKIP_FORWARD_MS` and `SKIP_BACK_MS`).

### Output modes

Press **F12** while the PlexOB core is running to open its menu. These settings are saved by the MiSTer.

- **Video Output**
  - **Auto** (the default) picks for you, based on your `MiSTer.ini`. On a CRT set up for 15 kHz, it uses Interlaced. On a 31 kHz VGA monitor, it uses Progressive. Otherwise it uses Native for HDMI.
  - **Interlaced** is 480i, for CRT TVs.
  - **Progressive** is 480p.
  - **Native (HDMI)** shows the video at its own size, up to 1280×720, and lets the MiSTer's HDMI scaler fit it to your TV. This gives the sharpest HDMI picture.
- **Refresh Rate:** Auto, 59.94 Hz, 60 Hz or 50 Hz.
- **Aspect Ratio:** Auto, 4:3 or 16:9.
- **Analog Aspect:** Auto, Fit, Letterbox or Crop, for widescreen video on a 4:3 TV.
- **Deinterlace** (Weave, Bob or Blend), **HDMI Deinterlace** and **Anti-Twitter**, for interlaced video.
- **Analog Dither:** On or Off.
- **Analog Output:** RGB, S-Video or Composite. S-Video and composite are included but **untested**.
- **NTSC Color:** NTSC, PAL60 or PAL-M, for S-Video and composite.

**240p isn't available yet.** It's planned for a future release.

---

## Keeping YouTube working

YouTube changes often, and the YouTube helper on the MiSTer needs updates to keep up. MiSTerPlex checks for an update by itself at most once a day, after the MiSTer's clock is set and it's online. To update it right now, run:

```
ssh root@<mister-ip> /media/fat/misterplex/bin/update_ytdlp.sh
```

If YouTube stops working for everyone at once, a fix usually comes out within a few days, and this update picks it up.

---

## Troubleshooting and known issues

### Common problems

| Problem | What to try |
|---|---|
| The MiSTer doesn't appear in the cast list | Check that your phone and the MiSTer are on the same network, and that the TV shows the ready screen. Some routers block devices on Wi-Fi from finding each other ("client isolation" or "AP isolation"). Turn that off. |
| The TV shows the MiSTer menu or colour bars after a reboot | Start at boot isn't set up. Run step 9 of the install again, then reboot. |
| It casts, but nothing plays | Check that the Plex server is online and reachable from the MiSTer. For a shared server, see [Your own server, or someone else's?](#your-own-server-or-someone-elses). |
| Buffering or stuttering from a remote server | The server's upload speed may be too low. Try a different title or a less busy time. MiSTerPlex already asks remote servers for a lower bitrate. |
| YouTube is missing from the ready screen | Wait a minute after boot. YouTube appears once it's connected. The ready screen updates when that connection comes up, including after a restart that did not reboot the MiSTer. |
| YouTube won't play anything | Update the YouTube helper (see [Keeping YouTube working](#keeping-youtube-working)). Check that the MiSTer's date is right: `ssh root@<mister-ip> date`. |
| The library won't open, or it's empty | Set `PLEX_BASE` and `PLEX_TOKEN` in `/media/fat/linux/misterplex.conf`, then reboot. |
| Garbled or oddly coloured picture after upgrading | Reboot the MiSTer. |

### Seeing the log

If something goes wrong, the log usually says why:

```
ssh root@<mister-ip> tail -n 100 /tmp/misterplexd-boot-rc3.log
```

The log is cleared at every reboot, so look at it before you restart. MiSTerPlex hides Plex tokens in its log, but check before you share it with anyone.

### What's new in rc5

- **AirPlay video plays at full speed.** Video that is already H.264 at up to 720p and 30 frames per second is passed straight through by the on-card helper instead of being re-encoded. It plays at about 30 frames per second (rc4 managed 2 to 3).
- **AirPlay picks the right quality from a web stream.** When a stream offers several qualities, the helper picks the best one the MiSTer can pass straight through. Streams that redirect to another address still play.
- **AirPlay audio no longer drops sound,** and the music screen uses less of the processor.
- **The last video frame no longer stays up for the whole song** after an AirPlay video (see the short delay below).
- **Stopping YouTube from the phone should work more often.** The YouTube connection reconnects by itself when it goes quiet, so the phone's stop usually gets through. In testing the stop arrived 3 times out of 3, but the first stop after a cold boot can still be lost (see below). If it ever gets stuck, stop from Plex and restart the program.

### Known issues in this release

The full list is in `KNOWN-ISSUES.md` in the source repository.

AirPlay:

- **AirPlay was tested with lab senders only, not a real iPhone.** No real iPhone, iPad or Mac has been tried with rc5 yet.
- **Some web streams can't be played over AirPlay** (streams that carry a certain broadcast information table make the on-card helper crash, so the cast is refused). Apple's own streams and most web video are fine.
- **After an AirPlay video, AirPlay audio shows the frozen last video frame for about 10 seconds** before the music screen appears. The sound plays normally during that time.
- **Video the MiSTer can't pass straight through** (1080p, 60 fps, 10-bit) is re-encoded on the card at only about 2 to 3 frames per second. A helper on a computer (`GCAST_XCODE`) avoids this.
- **Some streams' sound gets re-encoded** when it isn't AAC stereo. The picture still plays at full speed, but the processor runs flat out.
- **An AirPlay video can keep playing if the phone drops off without stopping it,** and AirPlay audio started after that doesn't take over the screen. Stopping from Plex recovers it.
- **AirPlay appears on the ready screen a few seconds after start-up,** once the on-card helper passes its self-test.
- **If a computer AirPlay helper stops,** the ready screen keeps showing AirPlay until it next redraws.

YouTube:

- **The first YouTube stop after a cold boot can be lost.** Press stop again, or stop from Plex.
- **After seeking in YouTube, the ready screen sometimes shows for a few seconds** before the video comes back.
- **The ready screen may briefly drop YouTube** if the YouTube connection is refused and has to start over.

Plex and video:

- **Casting from a shared server** addressed by its local network address can fail; casting through plex.tv works. Browsing a shared server on the TV is untested.
- **The progress bar in the Plex app** may not move while you cast, even though playback is fine.
- **S-Video and composite** haven't been tested on a real TV.
- **240p output** isn't available yet.
- **Subtitles aren't supported** with the PlexOB core yet, and you can't change audio or subtitle tracks from your phone.

---

## Uninstall

1. Stop MiSTerPlex and remove the start-at-boot line:

```
ssh root@<mister-ip> /media/fat/misterplex/bin/misterplexd-boot-rc3.sh uninstall
```

That stops the program and the on-card helper, and deletes the `misterplexd-boot-rc3.sh` line from `/media/fat/linux/user-startup.sh` and from `_user-startup.sh` when that file exists. `Plex.rbf` is left in place as the fallback core. You can still restore a backup of `user-startup.sh` (`user-startup.sh.bak.<date>`) if you prefer.
2. Put your old cores back. Rename the `.bak.<date>` files from step 6 to their original names. If there weren't any, delete `/media/fat/PlexOB.rbf`, `/media/fat/_Utility/PlexOB.rbf` and `/media/fat/_Utility/Plex.rbf`.
3. Delete the MiSTerPlex files:

   ```
   ssh root@<mister-ip> "rm -rf /media/fat/misterplex /media/fat/linux/misterplex /media/fat/linux/gcast /media/fat/linux/misterplex.conf /media/fat/linux/misterplex-cacert.pem"
   ```

4. Reboot.

---

## Advanced

This part is for manual setups and for changing settings. You don't need it for a normal install.

### Where things live

| Path on the MiSTer | What it is |
|---|---|
| `/media/fat/misterplex/bin/misterplexd` | The MiSTerPlex program |
| `/media/fat/misterplex/bin/misterplexd-boot-rc3.sh` | The start-at-boot script |
| `/media/fat/linux/misterplex.conf` | Your settings file |
| `/media/fat/linux/misterplex-cacert.pem` | Trusted certificates for secure connections |
| `/media/fat/linux/misterplex/yt-dlp/` | The YouTube helper, unpacked at first boot |
| `/media/fat/PlexOB.rbf` and `/media/fat/_Utility/PlexOB.rbf` | The PlexOB core (two identical copies) |
| `/media/fat/_Utility/Plex.rbf` | The older Plex core, used only as a fallback |
| `/media/fat/misterplex/airplay_key.pem`, `yt_screen_id` | AirPlay and YouTube identity, created on first run |
| `/tmp/misterplexd-boot-rc3.log` | The log (cleared at reboot) |

**Use `/media/fat/linux/misterplex.conf` for your settings.** If an older start line in `user-startup.sh` already passes `--conf`, the boot script uses that path first. Otherwise it uses `/media/fat/linux/misterplex.conf`. The older path `/media/fat/misterplex/misterplex.conf` is the fallback when neither of those is set. Some older notes and scripts still mention the old path.

### What the boot script does

The line that step 9 adds to `user-startup.sh` is:

```
/media/fat/misterplex/bin/misterplexd-boot-rc3.sh "$1" &
```

Keep the `"$1"`. The MiSTer passes `stop` through it at shutdown, so the script can stop MiSTerPlex cleanly.

At boot, the script:

1. Loads the PlexOB core if a different core is running. It tries `/media/fat/PlexOB.rbf` first, then `/media/fat/_Utility/PlexOB.rbf`, then `/media/fat/_Utility/Plex.rbf`.
2. Waits up to 60 seconds for the network.
3. Picks the device name. It keeps `FRIENDLY_NAME` and `PLAYER_ID` from your settings file if they're there. If not, it names the MiSTer after its IP address and saves that name once.
4. Starts the on-card AirPlay helper on 127.0.0.1 port 18781, then starts MiSTerPlex with Plex, AirPlay and YouTube turned on, using `/media/fat/linux/misterplex.conf`. AirPlay is on the ready card only when the helper is up and its `/probe` self-test passes. `GCAST_XCODE` overrides the on-card helper.
5. In the background, unpacks the bundled YouTube helper if needed, and updates it at most once a day once the clock is right.

### Applying changes to settings

After you edit the settings file, reboot the MiSTer. That's the most reliable way. You can also restart just MiSTerPlex:

```
ssh root@<mister-ip> "/media/fat/misterplex/bin/misterplexd-boot-rc3.sh stop; /media/fat/misterplex/bin/misterplexd-boot-rc3.sh start"
```

### Settings you might change

The settings file has one setting per line, written as `NAME=value`. Lines that start with `#` are ignored. The file that ships has many more settings than are listed here. Most of them are for testing or for the older Plex core, so leave them alone unless you know you need them.

**Plex servers**

| Setting | What it does |
|---|---|
| `PLEX_BASE` | The address of your main Plex server, like `http://<your-plex-server>:32400`. The on-screen library uses this server. When you cast, the app's own server address wins, so this is just the fallback. |
| `PLEX_SERVERS` | Extra servers as a comma-separated list. You can also add more `PLEX_BASE=` lines. They're used as fallbacks when a cast doesn't say which server to use. |
| `PLEX_HOST` | A shortcut. Give just a host name and MiSTerPlex adds `http://` and `:32400`. |
| `PLEX_TOKEN` | Your Plex token. The on-screen library needs it, and it helps the MiSTer find shared servers through plex.tv. Casting usually works without it, because the app sends its own token. Keep it private. |

**Name**

| Setting | What it does |
|---|---|
| `FRIENDLY_NAME` | The name you see in cast menus and on the ready screen. If it's empty, the boot script picks one from the IP address and saves it here. |
| `PLAYER_ID` | A unique ID for this MiSTer. The boot script sets it along with the name. If you run more than one MiSTer, make sure each has its own. |

**Playback**

| Setting | What it does |
|---|---|
| `SKIP_FORWARD_MS` | How far Right / skip forward jumps, in milliseconds. The default is `30000` (30 seconds). |
| `SKIP_BACK_MS` | How far Left / skip back jumps. The default is `10000` (10 seconds). |
| `SKIP_MS` | Sets both of the above to the same value. |
| `AUTO_NEXT` | `1` (the default) plays the next item in a cast queue when one ends. `0` stops after each one. |
| `MPX_GATE_ERROR_SCREEN` | `on` (the default) shows the reason on screen when a video can't play. `off` keeps it in the log only. |

**Helpers**

| Setting | What it does |
|---|---|
| `GCAST_XCODE` | Optional. The address of a helper on a computer, like `http://192.0.2.10:18780`. When this is set, it overrides the on-card helper. Leave it empty to use the on-card helper on 127.0.0.1 port 18781. `192.0.2.10` is a documentation-range example. |
| `YTDLP_BIN` | Use a different YouTube helper (yt-dlp) program. You normally don't need this. |

**Settings for the older Plex core only**

These settings affect only the fallback `Plex.rbf` core, not PlexOB: `DECODE`, `TRANSCODE_PROFILE`, `WEAK_RES`, `WEAK_BITRATE`, `PRESENT`, `OSD_CONTROL`, `IDLE_SCREEN`, `SUBTITLES`, `SUBTITLE_STREAM`, `MATCH_SOURCE_HZ`, `SOURCE_FPS`, `AUDIO_DELAY_MS`, `AV_OFFSET_MS`, `AV_PRESENT_LEAD_MS`, `AV_RESYNC_DROP_MS`, `AV_CONTENT_FPS` and `AUDIO_CLOCK_PPM`. With PlexOB, MiSTerPlex picks the conversion settings by itself (see below). `MPX_LEGACY_CORE_PREFIX` is fixed inside the program, and the file's value is ignored.

### How Plex conversion works with PlexOB

MiSTerPlex tells your Plex server what the MiSTer can play:

- H.264 video (Baseline, Main or High), up to level 4.1
- Up to 1280×720 at 30 frames per second, 8-bit
- Up to 8 Mbit/s from a server at home, or 6 Mbit/s from a remote server
- AAC audio, up to 2 channels

If a file already fits, it plays as is. If it doesn't, the server converts it. If the MiSTer can't keep up with a particular stream, MiSTerPlex asks for a lower bitrate and remembers that for that server next time. You don't need a custom profile on your server.

### Finding your Plex token

Plex explains how to find your token here: [Finding an authentication token / X-Plex-Token](https://support.plex.tv/articles/204059436-finding-an-authentication-token-x-plex-token/). In short: in Plex Web, open any movie, choose **Get Info**, then **View XML**. The address that opens ends with `X-Plex-Token=` followed by your token.

Your token works like a password for your Plex account. Don't post it or share your settings file.

### Browsing a shared server on the TV

This hasn't been tested yet, but here's how to try it. In Plex Web, open a title from the shared server, choose **Get Info**, then **View XML**. The address that opens starts with that server's address, for example `https://<numbers>.<id>.plex.direct:32400`, and ends with a token for that server. Set:

```
PLEX_BASE=https://<numbers>.<id>.plex.direct:32400
PLEX_TOKEN=<the token from that address>
```

Then reboot. Only that one server shows in the library.

### Optional helpers

AirPlay works on the MiSTer by itself; a helper on a computer is optional. You don't need a computer on the network for Plex, YouTube, or AirPlay. `GCAST_XCODE` overrides the on-card helper.

**The YouTube helper on the MiSTer (yt-dlp).** It's included and set up automatically. It finds the actual video stream for each YouTube video. It's what [Keeping YouTube working](#keeping-youtube-working) updates. YouTube does not go through the on-card AirPlay helper.

**The on-card AirPlay helper.** `linux/gcast/gcast_xcode.py` runs under the MiSTer's Python 3.9. `linux/gcast/ffmpeg` is a separate GPL build with libx264 and OpenSSL. `linux/gcast/ffprobe` is LGPL-2.1-or-later with OpenSSL and does not link libx264. The program's own libav is LGPL and does not re-encode. The boot script starts `linux/gcast/run-gcast-xcode.sh` on 127.0.0.1 port 18781. The ready screen shows AirPlay only after `/probe` self-test passes. See `licenses/gcast-ffmpeg/NOTICE`.

**A helper on a computer.** Optional. The same program, on a computer, overrides the on-card helper when `GCAST_XCODE` is set (for example `http://192.0.2.10:18780`). `192.0.2.10` is a documentation-range example. Start it with `python3 gcast_xcode.py --bind 0.0.0.0 --port 18780`. Run it only on your home network. It has no password, and anyone who can reach it can ask it to fetch and convert video.

**`scripts/plex_menu.sh` and `scripts/plex_browse.sh`.** These are command-line tools. They list your Plex libraries and start playback on the MiSTer over SSH, without the TV menu. They look for the settings file at the old path, so pass it to them:

```
/media/fat/misterplex/scripts/plex_menu.sh --conf /media/fat/linux/misterplex.conf
```

**`scripts/pms_720p_proxy.py`.** This is an older test tool. It ran on a computer in front of a Plex server and converted video to a light 720p stream for the older Plex core. You don't need it with PlexOB, which plays 720p in hardware.

### Picking the output with MiSTer.ini

The PlexOB core's **Auto** video output follows your `MiSTer.ini`:

- **Interlaced (480i)** if you've set up a 15 kHz analog TV (`composite_sync=1`, `vga_mode=ypbpr`, or `vga_sog=1`, with `vga_scaler=0`), or `direct_video=1`, or if Analog Output is S-Video or Composite.
- **Progressive (480p)** for a 31 kHz VGA monitor (`forced_scandoubler=1` with `vga_scaler=0`).
- **Native (HDMI)** otherwise.

To use these only for MiSTerPlex, put them in a `[PlexOB]` section of `MiSTer.ini`. Some older docs in the package describe settings for the older Plex core and a `[Plex]` section. Those don't apply to PlexOB. `docs/release-notes-v0.3.0.md`, `docs/release-notes-v0.4.0.md`, `docs/release-notes-v0.9.0-pre.md`, `docs/subtitles-burnin.md`, and `docs/match-source-hz.md` are historical notes. The install for this package is this README.

### Other files in the package

`SHA256SUMS` at the top of the package is the check for this download. `linux/SHA2-256SUMS` is yt-dlp's own upstream list. Only the bundled zip is in this package, so checking that upstream list reports files that are not here. Use the top-level `SHA256SUMS`.

`linux/yt-dlp_linux_armv7l.zip` is the same file as `bin/yt-dlp_linux_armv7l.zip`. The install steps copy the `bin/` tree. The boot hook uses the `bin/` copy when the linux copy is not the one it finds first.

The shell scripts under `bin/` are the same bytes as the matching files under `scripts/`. That includes `scripts/misterplexd-boot-rc3.sh`, `scripts/misterplex_pair_conf.sh`, `scripts/misterplex_cast_ready.sh`, `scripts/misterplex_core_watch.sh`, and `scripts/misterplex_named_rbf.sh`.

`bin/push_frame` and `bin/set_status` are small lab tools. `scripts/stage_plexob_yc_rbf.sh` is the packager's core check, not an install step. `kmod/mplex_ddr/` is kernel-module source for a lab frame path. You do not need any of those for a normal install.

### Network ports

Most home networks need no changes. If you use a firewall, MiSTerPlex uses these ports on the MiSTer:

| Port | What it's for |
|---|---|
| TCP 3005, UDP 32412 | Plex casting and discovery |
| TCP 7000 (and 7002 for audio) | AirPlay |
| TCP 8008 | YouTube discovery |
| UDP 5353 | Device discovery (Bonjour/mDNS) |

The MiSTer also needs to reach your Plex server (usually port 32400), plex.tv, YouTube and GitHub (for YouTube helper updates).

---

## Source and license

MiSTerPlex is free software under the GNU General Public License, version 3 or (at your option) any later version (SPDX: `GPL-3.0-or-later`). The program `bin/misterplexd` is a combined work with GPL-3.0 libraries, so that binary is distributed under the GNU General Public License version 3. The full source is at [github.com/flynnsbit/misterplex-cast](https://github.com/flynnsbit/misterplex-cast).

source is published at https://github.com/flynnsbit/misterplex-cast/releases/tag/rc5 (GPL-3.0-or-later).

The PlexOB core is a GPL-3.0-or-later fork of owenb321's MiSTer_h264_decoder. The Plex core is built on the GPL MiSTer framework. The MiSTerPlex program includes code from the projects listed below, under their own licenses, which are all compatible with GPLv3. The full license texts, versions and source links are in the `licenses/` folder of the release package.

---

## Thanks

MiSTerPlex is built on the work of these projects. Thank you to everyone who made them.

| Project | What it gives MiSTerPlex | License |
|---|---|---|
| [MiSTer FPGA (MiSTer-devel)](https://github.com/MiSTer-devel): [Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer) and the [core template](https://github.com/MiSTer-devel/Template_MiSTer) | The MiSTer platform, its menu and core loading, and the framework the PlexOB and Plex cores are built on | GPL-2.0-or-later (core framework), GPL-3.0 (Main_MiSTer) |
| [MiSTer_h264_decoder by owenb321](https://github.com/owenb321/MiSTer_h264_decoder) | The hardware H.264 decoder that PlexOB is forked from, and the ring client the program uses to feed it | GPL-3.0-or-later, client MIT |
| [UxPlay by FDH2](https://github.com/FDH2/UxPlay) | The AirPlay receiver library | LGPL-2.1-or-later, parts GPL-3.0 |
| [RPiPlay](https://github.com/FD-/RPiPlay) | The AirPlay crypto code UxPlay inherits | GPL-3.0-or-later |
| playfair (via UxPlay) | The AirPlay FairPlay handshake | GPL-3.0 |
| [csrp by Tom Cocagne](https://github.com/cocagne/csrp) | SRP pairing for AirPlay (via UxPlay) | MIT |
| [llhttp](https://github.com/nodejs/llhttp) | HTTP parsing for AirPlay (via UxPlay) | MIT |
| mdnsd (via UxPlay) | Network discovery, so Apple devices can find the MiSTer | LGPL-2.1-or-later |
| [libplist (libimobiledevice)](https://github.com/libimobiledevice/libplist) | Apple property-list support for AirPlay | LGPL-2.1-or-later |
| [FFmpeg](https://ffmpeg.org/) | Opening streams in the program (LGPL libav), and the on-card helper: `linux/gcast/ffmpeg` (GPL-3.0-or-later, with libx264) and `linux/gcast/ffprobe` (LGPL-2.1-or-later with OpenSSL) | LGPL-2.1-or-later; helper ffmpeg GPL-3.0-or-later |
| [x264](https://code.videolan.org/videolan/x264) | The video encoder in the on-card AirPlay helper ffmpeg | GPL-2.0-or-later |
| [OpenSSL](https://github.com/openssl/openssl) | Secure (HTTPS and TLS) connections | Apache-2.0 |
| [yt-dlp](https://github.com/yt-dlp/yt-dlp) | Finding YouTube video streams | Unlicense |
| [Python](https://www.python.org/) and [PyInstaller](https://pyinstaller.org/) | The Python runtime packed inside the yt-dlp release for the MiSTer, and the Python cast helpers | PSF License; GPL-2.0 with bootloader exception |
| [stb by Sean Barrett](https://github.com/nothings/stb) | Image loading, resizing and font drawing for the Plex menus | Public domain |
| [QOI by Dominic Szablewski](https://github.com/phoboslab/qoi) | A fast image format for cached menu artwork | MIT |
| [Spleen by Frederic Cambus](https://github.com/fcambus/spleen) | The 8×16 font on the library and error screens | BSD-2-Clause |
| [Mozilla CA bundle, via curl](https://curl.se/docs/caextract.html) | The list of trusted certificates for secure connections | MPL-2.0 |
| [curl](https://curl.se/) | Talking to Plex servers (the copy that comes with the MiSTer's Linux) | curl license (MIT-style) |
| [GNU C Library](https://www.gnu.org/software/libc/) and [GCC](https://gcc.gnu.org/) runtime | The C and C++ runtime | LGPL-2.1-or-later; GPL-3.0 with the runtime exception |

### Trademarks

Plex is a trademark of Plex, Inc. AirPlay, iPhone, iPad and Mac are trademarks of Apple Inc. YouTube is a trademark of Google LLC. MiSTer is the name of the MiSTer FPGA project. MiSTerPlex is an independent project and isn't affiliated with or endorsed by Plex, Apple, Google or the MiSTer FPGA project.
