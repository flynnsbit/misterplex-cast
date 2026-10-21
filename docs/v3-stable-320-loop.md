# v0.3.0 @ 320 stable loop — do not re-invent

## Did we already ship a verified 3.0 to GitHub?

| Artifact | Status |
|----------|--------|
| **GitHub Release `v0.3.0`** | **No.** `gh release list` shows only **`v0.2.0`** (2026-07-26) and **`v0.4.0`** (2026-08-09). |
| **Git tag `v0.3.0`** | **Yes** (`cacd8717`, 2026-07-26). Tag message describes playback controls + core `41adb98c`. |
| **Release notes** | `docs/release-notes-v0.3.0.md` is still titled **draft**. |
| **Lab pair card** | **Yes** — 2026-07-29: core `41adb98c` + daemon `06c5735a`, **PRESENT=fb0**, 320×240 (`docs/release.md` § Lab stable pair). |
| **Repo freeze of pair** | Core in `release_artifacts/v0.3.0/Plex.rbf`. Daemon `06c5735a` is checked in at `release_artifacts/v0.3.0-lab-stable/misterplexd`. The lab MiSTer copy is `misterplexd.prev-5a633b5`. |

**Conclusion:** We are **not** re-doing a published GitHub 3.0. We **are** re-using a documented **lab-stable** pair that was never a public GitHub release asset. Stop rediscovering `41adb98c`+`06c5735a` from tribal memory.

Published smooth glass today remains **v0.2.0** (`dfebf2bf`+`7cd10b4d`) — that *is* on GitHub and matches the glass floor.

---

## Goal of this loop

Prove **v0.3 features at 320×240** with **stability ≥ glass floor**, without product H5 thrash:

- Playback overlay / local transport  
- Chevron idle  
- Local B6 play with motion (content identity)  
- Optional: basic A/V present (fb0 path)

**Not in scope:** 480/720 banks, FOAR yuv624, fabric H5, softc freckles.

---

## Pair (frozen)

| | MD5 |
|--|-----|
| RBF | `41adb98c7a630b541091c22ce291be68` |
| Daemon | `06c5735a2f85114688f0ff2ac36e4fd4` |
| Artifacts | `release_artifacts/v0.3.0-lab-stable/` |
| Policy | `tests/fixtures/v030_lab_stable/pair.json` |

Conf: **`PRESENT=both`** (ship-verified; `fb0` alone fails movie glass), `DECODE=320x240`, `STREAM=0`, `OSD_CONTROL=1`, `IDLE_SCREEN=logo`.

**Package:** `dist/misterplex-v0.3.0.tar.gz` (HDMI-USB PASS 2026-08-11).

---

## Isolated loop (parent only; not product H5)

```bash
# Offline integrity
make v3-stable-320-policy

# Deploy pair + chevron + B6 gates (does NOT replace glass floor pin files unless promote)
make v3-stable-320

# After PASS only — optional promote live default away from v0.2 floor:
V3_STABLE_PROMOTE=1 make v3-stable-320
```

Script: `scripts/v3_stable_320_loop.sh`

Rules:

1. **Exclusive of product H5 RBF thrash** — refuse if `Plex.rbf` md5 is in overnight experiment set and `V3_STABLE_FORCE=1` not set; never write `cabe4816`/softc/H5 RBFs.  
2. **Pair only** — refuse if after deploy md5s ≠ policy.  
3. **PRESENT=fb0 only** — refuse conf `PRESENT=fpga` for this pair.  
4. **Glass floor pins stay** — `Plex.GOODBASELINE.dfebf2bf.rbf` and `misterplexd.GOODBASELINE.7cd10b4d` are not deleted.  
5. **Evidence** → `Memory/lab/hdmi-proof/v030_*` + `Memory/lab/status/v3-stable-320-last.txt`.

---

## If the loop fails

Roll back to glass floor:

```bash
# on device
cp -f /media/fat/_Utility/Plex.GOODBASELINE.dfebf2bf.rbf /media/fat/_Utility/Plex.rbf
cp -f /media/fat/misterplex_v2/bin/misterplexd.GOODBASELINE.7cd10b4d /media/fat/misterplex/bin/misterplexd
# PRESENT=fpga DECODE=320x240 IDLE_SCREEN=logo — then menu bounce + start daemon
```

Or host: pin from `release_artifacts/v0.2.0-glass-baseline/` + `make glass-baseline`.
