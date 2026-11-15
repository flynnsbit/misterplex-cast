# MisterPlex for update_all / MiSTer Downloader

This folder is a [MiSTer Downloader custom database](https://github.com/MiSTer-devel/Downloader_MiSTer/blob/main/docs/custom-databases.md)
for MisterPlex. It is modeled on [theypsilon/DB-Template_MiSTer](https://github.com/theypsilon/DB-Template_MiSTer)
and uses the same generator (`db_operator.py build` from MiSTer-devel/Distribution_MiSTer).

## Add it to a MiSTer

`db_id` is **`flynnsbit/misterplex`**. It must never change once published.

Add this to the bottom of `/media/fat/downloader.ini`:

```ini
[flynnsbit/misterplex]
db_url = https://raw.githubusercontent.com/flynnsbit/misterplex-cast/refs/heads/castor/update-all-db/mister-db/db.json.zip
```

You can also copy `downloader_flynnsbit_misterplex.ini` from this folder to the root of the SD card,
next to `downloader.ini`. That is a drop-in database.

Then:

1. Run **update_all** (or **downloader**) as usual. It installs the files listed below.
2. Run **Scripts > misterplex_setup** once. Run it again after any update_all that changes MisterPlex.
   Downloader cannot do the steps this script does (see "What the db cannot do").
3. Reboot, then load **MiSTerPlex** from `_Utility`. Link it at https://plex.tv/link with the code on the Ready screen.

If you use Downloader download filters, keep MisterPlex in them, for example `filter = [mister] misterplex misterplexsetup`
in the `[flynnsbit/misterplex]` section.

## What it installs (same paths as `scripts/install-misterplex.sh`)

| On the card (`/media/fat/...`) | What |
|---|---|
| `_Utility/MiSTerPlex.rbf` | the core, exactly one copy |
| `misterplex/bin/misterplexd`, `misterplex/bin/misterplexd-spelunkr` | the daemon and its Spelunkr build |
| `misterplex/bin/*.sh` | boot hook `misterplexd-boot-rc3.sh`, supervisor, mode switch, configure, uninstall, update_ytdlp, ... |
| `misterplex/bin/yt-dlp_linux_armv7l.zip` | yt-dlp 2026.08.19 armv7l, fetched straight from the yt-dlp GitHub release (`external_files.csv`) |
| `misterplex/scripts/`, `misterplex/docs/`, `misterplex/licenses/`, `misterplex/plexui/` | the same folders the release package installs |
| `misterplex/conf/misterplex.conf.example` | default settings (copied to `linux/misterplex.conf` only when that file does not exist) |
| `misterplex/yt_warm_helper.py` | casting warm helper |
| `misterplex/cores/MiSTerPlex.rbf.md5` | md5 of the core; the boot hook checks it before it retires `PlexOB.rbf` |
| `misterplex/linux/...` | CA bundle and the AirPlay / Cast helpers, staged for `misterplex_setup.sh` |
| `misterplex/LICENSE`, `misterplex/NOTICE`, `misterplex/DB-INFO.txt` | licence, notice, and which builds this db ships |
| `Scripts/misterplex_setup.sh` | the post-install step |

## What the db cannot do (done by `Scripts/misterplex_setup.sh`)

Downloader refuses any path under `linux/` and never runs scripts. So the setup script does these steps,
the same way `install-misterplex.sh` does them. It is idempotent.

- Copies `linux/misterplex-cacert.pem`, `linux/airplay/` and `linux/gcast/` into place, checks their MD5SUMS,
  and writes the `.misterplex-files` manifests that `uninstall-misterplex.sh` uses.
- Creates `linux/misterplex.conf` from the example **only if it is missing**. An existing conf, including
  OVERSCAN, FRIENDLY_NAME and every other setting, is never touched.
- Adds the one boot line to `linux/user-startup.sh` through `misterplexd-boot-rc3.sh install`. It never adds it twice.
- Removes the old core name `PlexOB.rbf` at the four fixed paths (`/`, `_Utility/`, `_Console/`, `cores/`).
  Downloader can only delete files that this same db installed earlier. It cannot delete files that a manual
  install left behind. It also moves a root `MiSTerPlex.rbf` copy aside, because the menu would list it as a second core.
- Sets the exec bits where it can. They are ignored on exFAT, and every script is started with `sh`.

The player name (`FRIENDLY_NAME`) is set by the boot hook on first boot, as with the package install.

## Hosting

Downloader cannot authenticate, so every URL must be publicly fetchable. The payload is committed in `payload/`
on this branch. File URLs are pinned to the payload commit:
`base_files_url = https://raw.githubusercontent.com/<repo>/<payload-commit>/mister-db/payload/`.
A later push never changes what an older db points at. The large yt-dlp zip is an external file
that points at the upstream release. Everything is templated in `build_db.sh`
(`MPX_DB_REPO`, `MPX_DB_BRANCH`, `MPX_DB_DIR`, `MPX_DB_ID`).

## Rebuild

```sh
# 1. release stage (same tree install-misterplex.sh installs from), daemon from a pushed commit
PACKAGE_REUSE_ARM_BIN=1 ARM_PLEXD_BIN=... ARM_PLEXD_SPELUNKR_BIN=... OUT_DIR=... scripts/package_release.sh
# 2. payload (core from a pushed commit; md5, BUILD_ID, RTL commit and seed are checked and recorded)
mister-db/build_payload.sh --stage <OUT_DIR>/stage-misterplex --rbf MiSTerPlex.rbf --rbf-md5 <md5> \
  --rbf-build-id <ID> --rbf-rtl <40-hex> --rbf-seed <n> --daemon-rev <40-hex>
git add mister-db/payload && git commit
# 3. db (pins the payload commit), validate, offline install test, commit
mister-db/build_db.sh
mister-db/test_offline_install.sh <Downloader_MiSTer checkout> [yt-dlp zip]
git add mister-db/db.json.zip mister-db/downloader_*.ini && git commit
```

`validate_db.py DB [PAYLOAD] [--remote N]` checks a db, local or by URL, against the payload and,
with `--remote`, against the live URLs.
