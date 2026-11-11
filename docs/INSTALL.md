# Installing MiSTerPlex on a MiSTer

This guide sets up MiSTerPlex on a MiSTer FPGA (DE10-Nano) with two scripts from the release package:

- `install-misterplex.sh` installs or updates everything. It is safe to run again.
- `configure-misterplex.sh` changes your settings, step by step or with flags.

It takes about 5 minutes. The last step, **linking the MiSTer to your Plex account at https://plex.tv/link (section 4)**, is required: until it is done, Browse Plex on the TV shows **FETCH FAILED**. Below, the MiSTer's IP address is written `<mister-ip>` (for example `192.0.2.50`).

## Quick start (easy install)

If you just want it running:

1. Download and unpack the release package on your computer.
2. In the unpacked folder, run `sh install-misterplex.sh --remote <mister-ip>` and enter the MiSTer root password when asked.
3. Reboot the MiSTer, load **MiSTerPlex** from `_Utility`, and link it at https://plex.tv/link with the code on the screen (section 4).

The installer puts the core at `/media/fat/_Utility/MiSTerPlex.rbf` (one copy), the program and boot script (`misterplexd-boot-rc3.sh`) in `/media/fat/misterplex/bin/`, and creates `/media/fat/linux/misterplex.conf` from the example only if you don't already have one. Settings and boot files it replaces are backed up first as `<name>.bak.<date>`; a replaced core is kept in `/media/fat/misterplex/backup/`. The installer needs no chmod, owners or symlinks, so it works on the exFAT card. The sections below explain each step.

## At a glance: how the install works

A release has **one FPGA core**, `MiSTerPlex.rbf`, which the installer puts in `/media/fat/_Utility/` only (one copy; an older root copy `/media/fat/MiSTerPlex.rbf` is removed so the menu does not list the core twice). There is no separate `Plex.rbf` or `PlexOB.rbf` to install any more. It also has the **misterplexd** program, which starts by itself at boot: the installer adds one line to `/media/fat/linux/user-startup.sh` that runs `misterplexd-boot-rc3.sh`. That script starts misterplexd under a small supervisor (`misterplexd_supervise.sh`), which restarts it if it crashes. `BOOT_SUPERVISE=0` in the settings file turns the supervisor off. You never need to start the program yourself. Just load **MiSTerPlex** from `_Utility`.

## 1. What you need

- A MiSTer (DE10-Nano) on your home network (wired is best), with a recent MiSTer menu and Linux.
- The MiSTer's IP address (shown on the MiSTer menu screen, or in your router's device list).
- SSH turned on, with the root password (the stock MiSTer password is `1`; change it if you can).
- A Plex Media Server you can reach, and a Plex account.
- A computer with `ssh` and `scp` (macOS, Linux, or Windows with OpenSSH).
- The release package from https://github.com/flynnsbit/misterplex-cast/releases, unpacked on that computer.

## 2. Install

Pick one way. Both do the same thing.

**A. From your computer** (in the unpacked package folder):

```sh
sh install-misterplex.sh --remote <mister-ip>
```

It checks the package (`SHA256SUMS`), copies it to `/tmp` on the MiSTer, runs the installer there, and cleans up. You'll be asked for the root password (several times unless you use an SSH key). Add `--user <name>` if you don't log in as `root`.

**B. On the MiSTer** (copy the unpacked folder over first):

```sh
scp -r misterplex-package root@<mister-ip>:/tmp/
ssh root@<mister-ip>
sh /tmp/misterplex-package/install-misterplex.sh
```

What the installer does:

1. Stops a running MiSTerPlex, if there is one.
2. Copies the program, scripts, docs and licences to `/media/fat/misterplex/` (the old `bin` folder is kept as a backup).
3. Creates `/media/fat/linux/misterplex.conf` from the example **only if you don't have one**. An existing file is backed up and kept.
4. Checks `MiSTerPlex.rbf` against its md5 and against the release pin list, and puts it in `/media/fat/_Utility/` only. A different old core there is moved to `/media/fat/misterplex/backup/`; a root `/media/fat/MiSTerPlex.rbf` left by an older installer is removed (or moved to that backup folder if its md5 differs), and old `MiSTerPlex.rbf.bak.*` files are moved there too. Only the pinned release core (md5 `96eb13d2cfc966fa56528703d0b542cf`) is accepted. The core used to be called `PlexOB.rbf`; once the new core is in place and checked, the installer removes an old `/media/fat/PlexOB.rbf`, `/media/fat/_Utility/PlexOB.rbf`, `/media/fat/_Console/PlexOB.rbf`, and `/media/fat/cores/PlexOB.rbf` only after the new file's md5 matches. A mismatch leaves the old file in place.
5. Copies the casting helpers to `/media/fat/linux/` (checked against `MD5SUMS`).
6. Names the player `misterplex-<last three parts of its IP>`, for example `misterplex-0.2.50`, unless you already set a name.
7. Runs `/media/fat/misterplex/bin/misterplexd-boot-rc3.sh install`, which adds this line to `/media/fat/linux/user-startup.sh` (only once):

   ```sh
   /media/fat/misterplex/bin/misterplexd-boot-rc3.sh "$1" &
   ```

It only writes MiSTerPlex files; it does not touch other cores or your MiSTer settings.

Useful options: `--dry-run` (show what would happen, change nothing), `--reboot` (reboot when done), `--package DIR`, `--help`.

## 3. Configure

The defaults work for most people. To change settings:

```sh
ssh -t root@<mister-ip> sh /media/fat/misterplex/bin/configure-misterplex.sh
```

It asks a few questions, with your current value as the default (press Enter to keep it):

- **Plex server**: your server address (`http://<server-ip>:32400`), or none to find it on the network.
- **plex.tv sign-in**: show a `plex.tv/link` code on the TV (on by default).
- **Analog video**: `480i`, `240p`, `auto`, or `default` (use the core's own setting). For `240p`, open the F12 menu on MiSTerPlex and save the settings once first.
- **Subtitles** and the subtitle language.
- **Menu size** and the **safe margin** for a CRT (0–20).
- **Player name** (what the Plex app shows).

Without questions (for scripts), use flags, for example:

```sh
sh /media/fat/misterplex/bin/configure-misterplex.sh -y --server http://<server-ip>:32400 --raster 240p --subtitles on --sub-lang en
```

Run it with `--help` for every flag and the matching `MPX_*` environment variables. `--dry-run` shows the result without writing; `--fresh` starts again from the example file but keeps your player name. Your old file is always saved as `misterplex.conf.bak.<date>` first.

Every other option is explained inside `/media/fat/linux/misterplex.conf`. Restart MiSTerPlex (section 6) after a change.

## 4. First start and link to Plex (required)

```sh
ssh root@<mister-ip> reboot
```

When the MiSTer is back, load **MiSTerPlex** from `_Utility`. The MISTERPLEX Ready screen shows two rows, for example:

```text
Ready · AirPlay · YouTube
plex.tv/link ABCD
```

(The first row lists only the receivers that are running, so it may be just `Ready`. `ABCD` stands for your own four-character code.)

**Link the MiSTer to your Plex account now:**

1. On your phone or computer, go to **https://plex.tv/link**.
2. Sign in to your Plex account.
3. Enter the code from the Ready screen.

The code row disappears once the MiSTer is linked. The sign-in is saved in `/media/fat/linux/misterplex.conf` (`PLEXTV_TOKEN`), so you only do this once; re-running the installer or `configure-misterplex.sh` keeps it.

> **Until you link, Browse Plex on the TV shows `FETCH FAILED`.** Casting from the Plex app on your phone still works without linking, because the phone sends its own sign-in. Browsing your libraries on the TV needs the link.

Other things the code row can say:

- `plex.tv/link: connecting, retrying`: the MiSTer can't reach plex.tv yet. Wait, or check its network and date/time.
- `Plex link error, retrying`: plex.tv refused the request several times. It keeps retrying; restart MiSTerPlex (section 6) if it doesn't clear.
- A code that stops working has expired; the MiSTer shows a new one by itself. Use the code that is on screen now.

The current code is also in `/media/fat/misterplex/plex_link.txt` on the MiSTer, if the TV is hard to read:

```sh
ssh root@<mister-ip> cat /media/fat/misterplex/plex_link.txt
```

If you turned linking off (`PLEXTV_LINK=0`), no code is shown and Browse Plex stays at `FETCH FAILED` unless a Plex sign-in is already in the settings file. Turn it back on with `configure-misterplex.sh -y --link on`.

## 5. Check that it works

- The `plex.tv/link` row has gone from the Ready screen, and Browse Plex on the TV lists your libraries (not `FETCH FAILED`).
- Open the Plex app on your phone, start a video, and tap the cast button. Your player name should be listed.
- From a computer, this should print a short XML reply that mentions MiSTerPlex:

  ```sh
  curl -s http://<mister-ip>:3005/resources
  ```

- On your phone, open `http://<mister-ip>:3005/misterplex/status` for the status page.
- The start-up log is at `/tmp/misterplexd-boot-rc3.log` on the MiSTer.

## 6. Restarting

```sh
ssh root@<mister-ip> '/media/fat/misterplex/bin/misterplexd-boot-rc3.sh stop; /media/fat/misterplex/bin/misterplexd-boot-rc3.sh start'
```

Don't use `killall`; the boot script handles stopping cleanly.

## 7. Updating (or repairing)

Unpack the new package and run the installer again, the same way as in section 2. Your settings and player name are kept, and the old files are backed up. Then reboot.

To see new options, compare your settings with `/media/fat/misterplex/conf/misterplex.conf.example`, or run `configure-misterplex.sh` again.

To update just YouTube support:

```sh
ssh root@<mister-ip> /media/fat/misterplex/bin/update_ytdlp.sh
```

## 8. Uninstalling and going back

Turn off start at boot (this stops MiSTerPlex and removes its line from `user-startup.sh`):

```sh
ssh root@<mister-ip> /media/fat/misterplex/bin/misterplexd-boot-rc3.sh uninstall
```

Or run the uninstall script, which does that and also removes what the installer created (your settings and player identity are kept unless `--purge`):

```sh
ssh root@<mister-ip> sh /media/fat/misterplex/bin/uninstall-misterplex.sh
```

It removes the core (in `_Utility`, plus any old root copy and `.bak.<date>` copies), the program files in `/media/fat/misterplex/`, `/media/fat/linux/misterplex-cacert.pem` and the installer's `.bak.<date>` copies of it and of `misterplex.conf`. In `/media/fat/linux/airplay` and `/media/fat/linux/gcast` it removes only the files the installer put there (listed in `.misterplex-files`, or `MD5SUMS` for older installs); any other file you keep there stays, and so does the folder. A listed entry that is an absolute path, has a `..` component, or sits in a folder that resolves outside `airplay`/`gcast` is skipped with a warning; a listed symlink is unlinked itself, never followed. Without `--purge` it keeps your settings and everything under `/media/fat/misterplex/` that the installer did not place (it removes only `bin`, `scripts`, `docs`, `licenses`, `plexui` and `conf` there): `/media/fat/linux/misterplex.conf`, the fallback conf, `identity`, `backup/`, `airplay_key.pem`, `yt_screen_id`, `reports/`, `ca/` and any other file there. `.bak` copies are removed only when the installer recorded making them (`misterplex/.misterplex-created`); a `.bak` you or another tool made is never touched, and the `user-startup.sh` `.bak` copies the installer made go only with `--purge`. `--purge` removes those too. Add `--dry-run` to see what it would do, or `--keep-core` to leave `MiSTerPlex.rbf` in place (`--remove-program` is still accepted and does nothing extra). Running `install-misterplex.sh` again reinstalls.

To go back to the previous core before uninstalling, copy the one the installer kept (replace `<date>` with the stamp shown by `ls`):

```sh
ssh root@<mister-ip> 'ls /media/fat/misterplex/backup/MiSTerPlex.rbf.*'
ssh root@<mister-ip> 'cp /media/fat/misterplex/backup/MiSTerPlex.rbf.utility.<date> /media/fat/_Utility/MiSTerPlex.rbf'
```

If you are going back to a release from before the rename (older builds named the core `PlexOB.rbf`), install that older release package again instead.

To go back to your previous settings, copy the newest `/media/fat/linux/misterplex.conf.bak.<date>` over `misterplex.conf`.

## Troubleshooting

- **The installer stops with a checksum error.** The download is damaged. Download and unpack the package again.
- **Browse Plex shows `FETCH FAILED`.** The MiSTer is not linked to your Plex account yet. Look for the `plex.tv/link ABCD` row on the Ready screen and enter that code at https://plex.tv/link (section 4). If there is no code row, check that `PLEXTV_LINK` is not `0` (`configure-misterplex.sh -y --link on`). If it is already linked, check that the Plex server is on and reachable from the MiSTer.
- **MiSTer isn't in the cast list.** Make sure MiSTerPlex is loaded and your phone is on the same network (not guest Wi-Fi). Run the `curl` check in section 5. Look at `/tmp/misterplexd-boot-rc3.log`.
- **"Daemon not running" on the TV.** Restart it (section 6). If it keeps happening, check the log and run the installer again.
- **No link code, or `plex.tv/link: connecting, retrying` stays on screen.** The MiSTer can't reach plex.tv. Check its network and the date/time.
- **240p doesn't take effect.** Open the F12 menu on MiSTerPlex, save the settings once, then restart.
- **Switching S-video or composite (YC) output on or off.** Two settings must agree: the `vga_mode` line in `MiSTer.ini`, and **Analog Output** in the core's F12 menu. The MiSTer only applies the change when the core loads, so changing the menu while the core is running does not switch the output.
  - **YC on:** in `MiSTer.ini` set `vga_mode=svideo` (or `vga_mode=cvbs` for composite). For NTSC you may also need the `ntsc_mode` line. In the F12 menu on MiSTerPlex set **Analog Output** to **S-Video** (or **Composite**), and pick **NTSC Color** if needed. Save the settings, then load MiSTerPlex from `_Utility` again.
  - **YC off (RGB or YPbPr):** in `MiSTer.ini` set `vga_mode=rgb` (or `vga_mode=ypbpr`). In the F12 menu set **Analog Output** to **RGB**. Save the settings, then load MiSTerPlex again.
  - If the two don't agree, the picture will be wrong: `svideo` in the ini with **RGB** in the menu sends the wrong timing to the S-video/composite output, and `rgb` in the ini with **S-Video** in the menu gives RGB at 15 kHz interlaced. `misterplex.conf` has no option for this; the F12 Analog Output menu and `vga_mode` are the only controls.
- **Remote (friend's) server won't play.** Remote HEVC is converted to H.264 by default (`REMOTE_H264_TRANSCODE`). Some servers can't convert; the TV will say so. Set `REMOTE_H264_TRANSCODE=0` only to try the original file.
- **AirPlay video is refused.** The MiSTer plays up to 720p at 30 fps, 8-bit H.264. Lower the quality on the sender.
- **YouTube stops working.** Run `update_ytdlp.sh`.
- **Picture is cut off on a CRT.** Use the program menu's Safe area, or raise the safe margin with `configure-misterplex.sh`.
- **Reporting a problem.** Set `REPORT_CODE=1` to get a short code and a log bundle with private details removed.

## Safety

MiSTerPlex is for your home network. **Never forward ports 3005, 8008 or 7000 to the internet.**

Playback tokens: product builds send `X-Plex-Token` as an HTTP header (libav `headers`, curl `-K`). A loopback relay (#274) keeps the token off ffmpeg argv (`-headers` and the `-i` URL). Test-only `MUT_340_*` / `MUT_274_HEADERS_ARGV` restore the old query/argv forms. If the relay cannot bind, ffmpeg may still receive `-headers` with the token (readable in `/proc/<pid>/cmdline`); that fallback is the remaining single-user residual.
