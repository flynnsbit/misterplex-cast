# Installing MiSTerPlex rc5

These steps install the ready-made package from the [Releases page](https://github.com/flynnsbit/misterplex-cast/releases). They take about ten minutes. Back up your SD card first.

You need a computer with `ssh` and `scp` (built into Windows 10 and 11, macOS and Linux) and your MiSTer's root password. In every command, replace `<mister-ip>` with your MiSTer's IP address. You can find it in the MiSTer main menu under the network settings, or on your router.

> **Keep your settings file.** Your settings live in `/media/fat/linux/misterplex.conf` on the MiSTer: your Plex server address, your Plex token and the MiSTer's name. If that file already exists, keep it. **Never copy `misterplex.conf.example` over it.** Step 5 below only copies the example when there is no settings file yet.

## 1. Download and check the package

Download `misterplex-rc5.tar.gz` and `SHA256SUMS` from the release, then check and extract it:

```
sha256sum -c SHA256SUMS --ignore-missing
tar -xzf misterplex-rc5.tar.gz
cd misterplex-rc5
sha256sum -c SHA256SUMS
```

Every line should say `OK`. On a Mac, use `shasum -a 256 -c` instead of `sha256sum -c`. Run the rest of the steps from inside the extracted folder.

## 2. Make the folders on the MiSTer

```
ssh root@<mister-ip> "mkdir -p /media/fat/misterplex /media/fat/_Utility /media/fat/linux"
```

## 3. Stop MiSTerPlex if it is running, then copy the program

```
ssh root@<mister-ip> /media/fat/misterplex/bin/misterplexd-boot-rc3.sh stop
scp -r bin scripts docs licenses root@<mister-ip>:/media/fat/misterplex/
```

On a first install the stop command doesn't exist yet. Skip it and copy.

## 4. Copy the on-card AirPlay helper

```
scp -r linux/gcast root@<mister-ip>:/media/fat/linux/
```

Do this on upgrades too: this folder is where rc5's AirPlay changes live.

## 5. Settings file: copy the example only if you have none

```
scp conf/misterplex.conf.example root@<mister-ip>:/tmp/misterplex.conf.example
ssh root@<mister-ip> 'test -f /media/fat/linux/misterplex.conf || cp /tmp/misterplex.conf.example /media/fat/linux/misterplex.conf; rm -f /tmp/misterplex.conf.example'
```

This never replaces an existing settings file. If you run your own Plex server, open `/media/fat/linux/misterplex.conf` on the MiSTer and set:

```
PLEX_BASE=http://<your-plex-server>:32400
PLEX_TOKEN=<your-token>
```

`PLEX_TOKEN` is only needed for browsing on the TV and for finding shared servers. If you only cast from the Plex app, you can leave the file as it is.

## 6. Back up any cores that are already there

```
ssh root@<mister-ip> 'stamp=$(date +%Y%m%d%H%M%S); for f in /media/fat/PlexOB.rbf /media/fat/_Utility/PlexOB.rbf /media/fat/_Utility/Plex.rbf; do [ -f "$f" ] && cp -a "$f" "$f.bak.$stamp"; done; true'
```

## 7. Install the cores

The PlexOB core and the MiSTerPlex program must come from the same release; the program refuses to play on any other PlexOB build. The command checks the core first and changes nothing if the check fails.

```
scp cores/PlexOB.rbf cores/PlexOB.rbf.md5 root@<mister-ip>:/tmp/
scp cores/Plex.rbf root@<mister-ip>:/media/fat/_Utility/Plex.rbf
ssh root@<mister-ip> 'set -eu
  want=$(cut -d" " -f1 /tmp/PlexOB.rbf.md5); got=$(md5sum /tmp/PlexOB.rbf | cut -d" " -f1)
  [ "$got" = "$want" ] || { echo "PlexOB.rbf check failed, not installing" >&2; exit 1; }
  for d in /media/fat/PlexOB.rbf /media/fat/_Utility/PlexOB.rbf; do cp /tmp/PlexOB.rbf "$d.tmp"; sync; mv -f "$d.tmp" "$d"; done
  rm -f /tmp/PlexOB.rbf /tmp/PlexOB.rbf.md5'
```

## 8. Copy the certificate bundle

YouTube and secure connections need it.

```
scp linux/misterplex-cacert.pem root@<mister-ip>:/media/fat/linux/
```

## 9. Turn on start at boot

```
ssh root@<mister-ip> /media/fat/misterplex/bin/misterplexd-boot-rc3.sh install
```

This adds one line to `/media/fat/linux/user-startup.sh` (and saves a backup of that file first):

```
/media/fat/misterplex/bin/misterplexd-boot-rc3.sh "$1" &
```

Keep the `"$1"`: the MiSTer passes `stop` through it at shutdown.

## 10. Reboot

```
ssh root@<mister-ip> reboot
```

In about a minute the TV shows the MiSTerPlex ready screen with the MiSTer's name and the kinds of casting that are on. YouTube and AirPlay appear a few seconds after the others.

## Upgrading from an earlier build

1. Stop the program and back up your settings file:

   ```
   ssh root@<mister-ip> /media/fat/misterplex/bin/misterplexd-boot-rc3.sh stop
   ssh root@<mister-ip> 'if [ -f /media/fat/linux/misterplex.conf ]; then cp -a /media/fat/linux/misterplex.conf /media/fat/linux/misterplex.conf.bak.$(date +%Y%m%d%H%M%S); fi'
   ```

2. Do steps 3, 4, 6, 7, 8 and 9. **Skip step 5**, or run it as written (it leaves an existing file alone). Never copy `misterplex.conf.example` over `/media/fat/linux/misterplex.conf`.
3. Reboot once.

## If something goes wrong

The log is on the MiSTer and is cleared at every reboot:

```
ssh root@<mister-ip> tail -n 100 /tmp/misterplexd-boot-rc3.log
```

MiSTerPlex hides Plex tokens in its log, but check before you share it.

## Uninstall

```
ssh root@<mister-ip> /media/fat/misterplex/bin/misterplexd-boot-rc3.sh uninstall
ssh root@<mister-ip> "rm -rf /media/fat/misterplex /media/fat/linux/misterplex /media/fat/linux/gcast /media/fat/linux/misterplex-cacert.pem"
```

Then put back the `.bak.<date>` cores from step 6 (or delete `/media/fat/PlexOB.rbf`, `/media/fat/_Utility/PlexOB.rbf` and `/media/fat/_Utility/Plex.rbf`), and reboot. Delete `/media/fat/linux/misterplex.conf` too only if you don't want to keep your settings.
