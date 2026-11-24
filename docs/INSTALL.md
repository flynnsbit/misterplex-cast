# Installing MiSTerPlex on a MiSTer

This guide sets up MiSTerPlex on a MiSTer FPGA (DE10-Nano). Below, the MiSTer's IP address is written `<mister-ip>` (for example `192.0.2.50`).

There are two kinds of release package. Both are installed with the same command, **`sh install-misterplex.sh`**, run from the unpacked package folder:

- **Minimal package** (v1.0.2, `misterplex-<build>.tar.gz`; `<build>` is the build id on the release page): the core, the program and its boot scripts. Sections 1, 2 and 4 to 8 cover it.
- **Full package** (built by `scripts/package_release.sh`; not published for v1.0.2): everything, plus `configure-misterplex.sh` for settings. Section 2C covers it.

The last step, **linking the MiSTer to your Plex account at https://plex.tv/link (section 4)**, is required: until it is done, Browse Plex on the TV shows **FETCH FAILED**.

## Quick start (v1.0.2)

1. Download `misterplex-<build>.tar.gz` and `MD5SUMS` from the [v1.0.2 release](https://github.com/flynnsbit/misterplex-cast/releases/tag/v1.0.2), check it (`grep ' misterplex-<build>.tar.gz$' MD5SUMS | md5sum -c -`) and unpack it.
2. `scp -r misterplex-<build> root@<mister-ip>:/tmp/` then `ssh root@<mister-ip> 'cd /tmp/misterplex-<build> && sh install-misterplex.sh'`.
   The installer also adds the start-at-boot line and moves an old root core aside (section 2A).
3. Reboot the MiSTer. MiSTerPlex loads by itself. Link it at https://plex.tv/link with the code on the screen (section 4).

## At a glance: how the install works

A release has **one FPGA core**, `MiSTerPlex.rbf`, which goes in `/media/fat/_Utility/` only. There is no separate `Plex.rbf` or `PlexOB.rbf` to install any more. The **misterplexd** program lives in `/media/fat/misterplex/bin/` and starts by itself at boot from one line in `/media/fat/linux/user-startup.sh` that runs `misterplexd-boot-rc3.sh`. After a reboot that script loads MiSTerPlex from the MiSTer menu by itself, then starts misterplexd. When `misterplexd_supervise.sh` is installed (full package, or an earlier install), misterplexd runs under it and is restarted if it crashes; `BOOT_SUPERVISE=0` in the settings file turns that off. If you open another core later, load **MiSTerPlex** from `_Utility` to come back.

## 1. What you need

- A MiSTer (DE10-Nano) on your home network (wired is best), with a recent MiSTer menu and Linux.
- The MiSTer's IP address (shown on the MiSTer menu screen, or in your router's device list).
- SSH turned on, with the root password (the stock MiSTer password is `1`; change it if you can).
- A Plex Media Server you can reach, and a Plex account.
- A computer with `ssh` and `scp` (macOS, Linux, or Windows with OpenSSH).
- The release package, unpacked on that computer.

## 2. Install

### 2A. Minimal package (v1.0.2)

The package holds `MiSTerPlex.rbf`, `misterplexd`, `owenb_idle_card`, `misterplexd-boot-rc3.sh`, `misterplex_mode.sh`, `install-misterplex.sh`, `README.txt` and `MD5SUMS`.

```sh
scp -r misterplex-<build> root@<mister-ip>:/tmp/
ssh root@<mister-ip>
cd /tmp/misterplex-<build> && sh install-misterplex.sh
```

Use `/tmp`: it is cleared at the next reboot, so the package folder is not left on the SD card. (If you copy it to `/media/fat/` instead, delete it after the install.)

What `install-misterplex.sh` does in the minimal package:

1. Checks every file against `MD5SUMS`. Every payload file must be listed; on a mismatch or a missing file it stops and installs nothing.
2. Installs `MiSTerPlex.rbf` to `/media/fat/_Utility/MiSTerPlex.rbf`, and `misterplexd` (twice, as `misterplexd` and `misterplexd-spelunkr`), `owenb_idle_card`, `misterplexd-boot-rc3.sh` and `misterplex_mode.sh` to `/media/fat/misterplex/bin/`. Each file is written as `<file>.new` and then renamed, and files that are already the same are skipped.
3. If `/media/fat/MiSTerPlex.rbf` exists (left by an older installer) and differs from the new core, moves it into the backup folder. The boot script loads a root copy before the one in `_Utility`.
4. Leaves `/media/fat/linux/misterplex.conf` alone if it exists. The minimal package has no example settings file, so on a new card none is created and MiSTerPlex uses built-in defaults.
5. Turns on start at boot, unless the line is already there: it runs `misterplexd-boot-rc3.sh install`, which adds this line once to `/media/fat/linux/user-startup.sh` (and to `_user-startup.sh` if that file exists), keeping a copy of the old file as `user-startup.sh.bak.<stamp>`:

   ```sh
   /media/fat/misterplex/bin/misterplexd-boot-rc3.sh "$1" &
   ```

6. Before each change, saves what it will replace to `/media/fat/misterplex/backup-<time>/` and adds the undo step to `restore.sh` there, so `restore.sh` is complete even if a later step fails. `restore.sh` stops MiSTerPlex, puts `user-startup.sh` back exactly as it was (which removes the boot line if this run added it), puts the saved files back, and deletes the files and folders this run created. On a new card with no old `PlexOB.rbf` the folder holds `restore.sh` and the small files it needs (such as the saved `user-startup.sh` or conf state files). Old `PlexOB.rbf` files are saved first, on every run (about 4 MB each), so with those on the card even a re-run with nothing else to change makes a new backup folder; without them, when nothing changes, no folder is made. You can delete old `backup-<time>` folders you no longer need.

It does not stop a running MiSTerPlex; the new files take effect at the next reboot. Options: `--dry-run` (show what it would do, change nothing) and `--no-boot-line` (skip step 5). Licence texts, the README, `MD5SUMS` and the installer stay in the package folder.

### 2B. After the minimal install

- **Reboot.** MiSTerPlex loads by itself; link it at https://plex.tv/link the first time (section 4).
- **If the installer can't** finish a step, it stops with `FAILED at step: …`, exits with an error and prints the `sh /media/fat/misterplex/backup-<time>/restore.sh` command; `restore.sh` is kept complete before every change, so it undoes everything done so far. Run it and reboot, or fix the cause and run the installer again. Only if it keeps failing, do the two setup steps by hand: `sh /media/fat/misterplex/bin/misterplexd-boot-rc3.sh install`, and `mv /media/fat/MiSTerPlex.rbf /media/fat/misterplex/backup-<time>/`.
- **Old `PlexOB.rbf`.** A core under the old name (`/media/fat/PlexOB.rbf`, `_Utility/`, `_Console/` or `cores/PlexOB.rbf`) is usually left in place. If an earlier full install left `/media/fat/misterplex/cores/MiSTerPlex.rbf.md5` and it matches the new core, the boot script removes those old files (at install, or at a later boot or mode switch); the installer saves a copy of each one first on every run, with or without the boot line, and `restore.sh` puts back any that are gone. The boot script prefers `_Utility/MiSTerPlex.rbf` unless your settings file sets `PLEX_RBF`; you can delete an old file that is left.
- **Settings (optional on a new install).** v1.0.2 does not create `/media/fat/linux/misterplex.conf`; without one MiSTerPlex uses built-in defaults. To start from the example file, download [`assets/misterplex.conf.example`](https://raw.githubusercontent.com/flynnsbit/misterplex-cast/v1.0.2/assets/misterplex.conf.example) and copy it to `/media/fat/linux/misterplex.conf` only if that file does not exist.
- **Not in v1.0.2.** The AirPlay helper and its ffmpeg, yt-dlp (YouTube), the CA bundle, the menu fonts, the restart supervisor, `configure-misterplex.sh`, `uninstall-misterplex.sh` and `update_ytdlp.sh`. An earlier install keeps the copies it has. On a new card, AirPlay, YouTube and automatic restart need those files from a full package.

### 2C. Full package

A full package (from `scripts/package_release.sh`, with `cores/`, `bin/`, `conf/`, `linux/`, `Scripts/` and `SHA256SUMS`) is installed with the same command, `sh install-misterplex.sh`, from its top level. v1.0.2 is not published as a full package; in this repository the script is `scripts/install-misterplex.sh`, and it needs the full package layout around it.

**From your computer** (in the unpacked full package folder):

```sh
sh install-misterplex.sh --remote <mister-ip>
```

It checks the package (`SHA256SUMS`), copies it to `/tmp` on the MiSTer, runs the installer there, and cleans up. You'll be asked for the root password (several times unless you use an SSH key). Add `--user <name>` if you don't log in as `root`.

**On the MiSTer** (copy the unpacked folder over first):

```sh
scp -r misterplex-package root@<mister-ip>:/tmp/
ssh root@<mister-ip>
sh /tmp/misterplex-package/install-misterplex.sh
```

What the full installer does:

1. Stops a running MiSTerPlex, if there is one.
2. Copies the program, scripts and fonts to `/media/fat/misterplex/` (the old `bin` folder is kept as a backup). Licence texts, docs and READMEs are not copied to the MiSTer.
3. Creates `/media/fat/linux/misterplex.conf` from the example **only if you don't have one**. An existing file is backed up and kept.
4. Checks `MiSTerPlex.rbf` against its md5 and against the release pin list, and puts it in `/media/fat/_Utility/` only. A different old core there is moved to `/media/fat/misterplex/backup/`; a root `/media/fat/MiSTerPlex.rbf` left by an older installer is removed (or moved to that backup folder if its md5 differs), and old `MiSTerPlex.rbf.bak.*` files are moved there too. Only the core whose md5 is in the package's `cores/MiSTerPlex.rbf.md5` (and on the release pin list) is accepted; check the md5 against the release page for your version (v1.0.1: `042f86171b562a61694da46860b89d89`). The core used to be called `PlexOB.rbf`; once the new core is in place and checked, the installer removes an old `/media/fat/PlexOB.rbf`, `/media/fat/_Utility/PlexOB.rbf`, `/media/fat/_Console/PlexOB.rbf`, and `/media/fat/cores/PlexOB.rbf` only after the new file's md5 matches. A mismatch leaves the old file in place.
5. Copies the casting helpers to `/media/fat/linux/` (checked against `MD5SUMS`).
6. Names the player `misterplex-<last three parts of its IP>`, for example `misterplex-0.2.50`, unless you already set a name.
7. Runs `/media/fat/misterplex/bin/misterplexd-boot-rc3.sh install`, which adds this line to `/media/fat/linux/user-startup.sh` (only once):

   ```sh
   /media/fat/misterplex/bin/misterplexd-boot-rc3.sh "$1" &
   ```

It only writes MiSTerPlex files; it does not touch other cores or your MiSTer settings.

Useful options: `--dry-run` (show what would happen, change nothing), `--reboot` (reboot when done), `--package DIR`, `--help`.

## 3. Configure

The defaults work for most people. The v1.0.2 minimal package does not include `configure-misterplex.sh`; without it, edit `/media/fat/linux/misterplex.conf` directly (section 2B shows where the example file comes from). A full package installs it as `/media/fat/Scripts/configure-misterplex.sh` (it also shows up in the MiSTer **Scripts** menu); older installs have it in `/media/fat/misterplex/bin/`. To change settings with it:

```sh
ssh -t root@<mister-ip> bash /media/fat/Scripts/configure-misterplex.sh
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
bash /media/fat/Scripts/configure-misterplex.sh -y --server http://<server-ip>:32400 --raster 240p --subtitles on --sub-lang en
```

Run it with `--help` for every flag and the matching `MPX_*` environment variables. `--dry-run` shows the result without writing; `--fresh` starts again from the example file but keeps your player name. Your old file is always saved as `misterplex.conf.bak.<date>` first.

Every other option is explained inside `/media/fat/linux/misterplex.conf`. Restart MiSTerPlex (section 6) after a change.

## 4. First start and link to Plex (required)

```sh
ssh root@<mister-ip> reboot
```

When the MiSTer is back, the boot script loads MiSTerPlex by itself (you don't need to pick it from `_Utility`; do that only to come back from another core). The MISTERPLEX Ready screen shows two rows, for example:

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

Unpack the new package and run its installer again, the same way as in section 2 (`sh install-misterplex.sh` from the unpacked folder, for either kind of package; the v1.0.1 package's installer was named `install.sh`). Your settings and player name are kept, and the old files are backed up. Then reboot.

To see new options, compare your settings with `/media/fat/misterplex/conf/misterplex.conf.example`, or run `configure-misterplex.sh` again.

To update just YouTube support (on an install that has `update_ytdlp.sh`; the minimal package does not ship it):

```sh
ssh root@<mister-ip> /media/fat/misterplex/bin/update_ytdlp.sh
```

## 8. Uninstalling and going back

**Minimal package.** To undo an install or update, run the `restore.sh` the installer wrote, then reboot:

```sh
ssh root@<mister-ip> 'ls -d /media/fat/misterplex/backup-*'
ssh root@<mister-ip> sh /media/fat/misterplex/backup-<time>/restore.sh
```

If you edited a settings file the install created, `restore.sh` moves it into the backup folder as `misterplex.conf.after-install` instead of deleting it. `restore.sh` works the same after a failed install. It reports `Restore finished with N error(s)` and exits with an error if any undo step failed. If there is no `restore.sh`, stop MiSTerPlex (`misterplexd-boot-rc3.sh stop`), copy the files from the backup folder back by hand (`MiSTerPlex.rbf` to `/media/fat/_Utility/`, the rest to `/media/fat/misterplex/bin/`) and reboot.

To remove a minimal-package-only install, run the boot script's `uninstall` (below), then delete `/media/fat/_Utility/MiSTerPlex.rbf` and `/media/fat/misterplex/`. Your settings file is kept unless you delete it.

**Full installs:**

Turn off start at boot (this stops MiSTerPlex and removes its line from `user-startup.sh`):

```sh
ssh root@<mister-ip> /media/fat/misterplex/bin/misterplexd-boot-rc3.sh uninstall
```

Or run the uninstall script (full installs; the minimal package does not ship it), which does that and also removes what the installer created (your settings and player identity are kept unless `--purge`):

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
