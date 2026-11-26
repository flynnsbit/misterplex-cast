# MisterPlex for update_all / MiSTer Downloader

This folder is a [MiSTer Downloader custom database](https://github.com/MiSTer-devel/Downloader_MiSTer/blob/main/docs/custom-databases.md)
for MisterPlex. It is modeled on [theypsilon/DB-Template_MiSTer](https://github.com/theypsilon/DB-Template_MiSTer)
and uses the same generator (`db_operator.py build` from MiSTer-devel/Distribution_MiSTer).

## Add it to a MiSTer

`db_id` is **`flynnsbit/misterplex`**. It must never change once published.

Add this to the bottom of `/media/fat/downloader.ini`:

```ini
[flynnsbit/misterplex]
db_url = https://raw.githubusercontent.com/flynnsbit/misterplex-cast/refs/heads/main/mister-db/db.json.zip
```

You can also copy `downloader_flynnsbit_misterplex.ini` from this folder to the root of the SD card,
next to `downloader.ini`. That is a drop-in database.

Then:

1. Run **update_all** (or **downloader**) as usual. It installs the files listed below.
2. Run **Scripts > configure-misterplex** once: the first run finishes the install by itself (same steps as **Repair / Reinstall**). Choose Repair / Reinstall after any update_all that changes MisterPlex. The same menu has Settings, Restart MisterPlex, Repair / Reinstall and Uninstall. Switching between Plex and Live TV is done from the core OSD Service switch, not from Scripts.
   Downloader cannot do the steps this script does (see "What the db cannot do").
3. Reboot, then load **MiSTerPlex** from `_Utility`. Link it at https://plex.tv/link with the code on the Ready screen.

If you use Downloader download filters, keep MisterPlex in them, for example `filter = [mister] misterplex configuremisterplex`
in the `[flynnsbit/misterplex]` section.

## What it installs

Exactly 28 runtime files (`validate_db.py` REQUIRED; same set as `packaging/runtime-files.txt`) from the **newest published GitHub release** of flynnsbit/misterplex-cast
(the release package `misterplex-<tag>.tar.gz`, checked against the release `SHA256SUMS`).
The paths are the same ones `scripts/install-misterplex.sh` writes. Docs, licences, LICENSE/NOTICE
and release notes are not put on the MiSTer; they stay in the release package and the repo.
`RELEASE` in this folder names the release the current db was built from.

| On the card (`/media/fat/...`) | What |
|---|---|
| `_Utility/MiSTerPlex.rbf` | the core from the release, exactly one copy |
| `misterplex/bin/` | `misterplexd`, `misterplexd-spelunkr`, the boot hook `misterplexd-boot-rc3.sh` and the other package `bin/` scripts |
| `misterplex/bin/yt-dlp_linux_armv7l.zip` | yt-dlp, fetched from the upstream yt-dlp release when the bytes match (`external_files.csv`) |
| `misterplex/plexui/Roboto-{Regular,Bold}.ttf`, `misterplex/plexui/misterplex-ad-break.jpg`, `misterplex/conf/misterplex.conf.example` | Plex UI fonts, Pluto ad-break stand-by card, default settings |
| `misterplex/yt_warm_helper.py` | casting warm helper |
| `misterplex/cores/MiSTerPlex.rbf.md5` | the release md5 slot; the boot hook checks it before it retires `PlexOB.rbf` |
| `misterplex/linux/...` | CA bundle and the AirPlay / Cast helpers, staged for the setup step in `Scripts/configure-misterplex.sh` |
| `Scripts/configure-misterplex.sh` | the one MisterPlex Scripts entry (menu; setup = first run / Repair) |

## What the db cannot do (done by the setup step in `Scripts/configure-misterplex.sh`)

Downloader refuses any path under `linux/` and never runs scripts. So the setup script does these steps,
the same way `install-misterplex.sh` does them. It is idempotent.

- Copies `linux/misterplex-cacert.pem` and `linux/airplay/` into place, checks the MD5SUMS,
  and writes the `.misterplex-files` manifest that `uninstall-misterplex.sh` uses.
- Removes what older installs left that the runtime set no longer ships: `misterplex/licenses/`, `docs/`,
  `scripts/`, `bin/misterplex_core_watch.sh` and its two libraries, `plexui/LICENSE-Roboto.txt`, and the old
  `linux/gcast/` copy (only the files its manifest lists). The conf is never touched.
- Creates `linux/misterplex.conf` from the example **only if it is missing**. An existing conf, including
  OVERSCAN, FRIENDLY_NAME and every other setting, is never touched.
- Adds the one boot line to `linux/user-startup.sh` through `misterplexd-boot-rc3.sh install`. It never adds it twice.
- Removes the old core name `PlexOB.rbf` at the four fixed paths (`/`, `_Utility/`, `_Console/`, `cores/`).
  Downloader can only delete files that this same db installed earlier. It cannot delete files that a manual
  install left behind. It also moves a root `MiSTerPlex.rbf` copy aside, because the menu would list it as a second core.
- Sets the exec bits where it can. They are ignored on exFAT, and every script is started with `sh`.

The player name (`FRIENDLY_NAME`) is set by the boot hook on first boot, as with the package install.

## Hosting

Downloader cannot authenticate, so every URL must be publicly fetchable. Downloader archives only
accept ZIP, and the release package is a `.tar.gz`, so the release files are unpacked into `payload/`
on this branch. File URLs are pinned to the payload commit:
`base_files_url = https://raw.githubusercontent.com/<repo>/<payload-commit>/mister-db/payload/`.
A later push never changes what an older db points at. Everything is templated in `build_db.sh`
(`MPX_DB_REPO`, `MPX_DB_BRANCH`, `MPX_DB_DIR`, `MPX_DB_ID`).

## Rebuild

`.github/workflows/mister-db-release.yml` does this on every published release (and on
workflow_dispatch). It uses only the built-in `GITHUB_TOKEN`, makes normal commits and does a normal push.
GitHub only fires those triggers for a workflow file on the default branch.
By hand, on this branch:

```sh
mister-db/regen_from_release.sh [TAG]      # newest published release when TAG is empty
mister-db/test_offline_install.sh <Downloader_MiSTer checkout> [yt-dlp zip]
git push origin HEAD:refs/heads/main
```

`validate_db.py DB [PAYLOAD] [--remote N]` checks a db, local or by URL, against the payload and,
with `--remote`, against the live URLs.
