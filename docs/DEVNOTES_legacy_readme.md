# MiSTerPlex

**Native Plex media client for [MiSTer FPGA](https://mister-devel.github.io/MkDocs_MiSTer/)** — FPGA owns pixels and audio present (console-core philosophy); ARM owns Plex protocol and network.

Not a PC cast app with the DE10 as a dumb display. Lessons and protocol code from mistercast-linux (an earlier private project) are harvested; the product path is a **dedicated `Plex.rbf` + `misterplexd`**.

## Priority

Best attainable **video + sound, perfectly synchronized**, on **CRTs and LCDs**. Full **user-level tests**. Ship feature-rich cast + library playback on-device.

## Architecture (target)

| Resource | Role |
|----------|------|
| **FPGA** | Decode (profile), frame store, vsync present, 3:2/2:2 cadence, audio FIFO |
| **BRAM** | Linebufs, bit FIFOs, audio ring |
| **SDRAM** | Working VRAM / unique frames |
| **DDR3** | Stream buffers, ARM↔FPGA payload |
| **ARM** | GDM, companion, resolve, demux control — **not** the present loop |

See [docs/architecture.md](docs/architecture.md).

## Status

| Phase | State |
|-------|--------|
| **0** Scaffold monorepo | **done** |
| **1** Native present core (color bars + tone + cadence) | **RBF built** (`Plex.rbf`, Quartus 17.0.2) |
| **2** Plex cast companion + ARM media path | **working on hardware** — see below |
| **3** FPGA decode / frame store | **3.3k hybrid** host recon F1 (mae=0); lab RBF green (Template HSync ~60 Hz + `res_dc=-24`); DDR YUV420p F1 in RTL |
| **4** Feature-rich client | **in progress** — multi-server, browse/menu UX, Content FPS hint, scrubber steps, auto-next (parallel to Phase 3) |
| **5** Release / display matrix | **docs + soak + package** — [release.md](docs/release.md), [crt-lcd-matrix.md](docs/crt-lcd-matrix.md) |

### Phase 2 (current on MiSTer `192.0.2.183`)

- `misterplexd` static ARM binary: GDM + companion HTTP `:3005`
- `playMedia` / pause / resume / stop / seek wired
- prePlayHold after stop (timeline stays `buffering@navigation` while cast-bound)
- Slim PMS resolve (Docker-bridge rewrite, weak universal H.264 ladder, local path/URL)
- FFmpeg → raw RGB24 → `/dev/fb0` (MiSTer_fb ascal scanout)
- **Audio:** single-process FFmpeg → s16le stereo @ 48 kHz → `/dev/MrAudio` (SPI DMA)
- Play-queue scrubber fields: `playQueueID` / `playQueueItemID` / `containerKey` / `key`
- Conf: `DECODE=WxH`, `WEAK_RES`, `WEAK_BITRATE` (default 320×240; **480×360 HW-verified**)
- Deploy: `MISTER_HOST=<mister-ip> ./scripts/deploy_misterplexd.sh` (startup hook, conf, free :3005 orphans)

**HW proven:** PMS “The Garden of Delights” → fb0 + MrAudio; suite green including `test_single_process.sh` (1× ffmpeg).

**Phase 3 track:** see [docs/phase3-decode.md](docs/phase3-decode.md) — RGB frame FIFO then H.264 soft-core; dual-A9 stays pegged until decode leaves ARM. Phase 4 UX advances in parallel and must not gate decode work.

### Phase 4 foundation (cast client UX)

Ported/hardened mistercast-linux companion lessons without requiring RBF changes:

| Item | Status |
|------|--------|
| Scrubber bind fields (`key` / `containerKey` / `playQueueID` / `playQueueItemID` / server address) | **done** — unit + `tests/hw/test_playqueue_bind.sh` |
| `viewOffset` / `offset` ms; continue-watching only when cast omits offset | **done** |
| `prePlayHold` + `castBound`: stop ACK/polls stay `buffering@navigation` without media keys | **done** |
| Mirror does not demote live cast; unsubscribe clears hold | **done** |
| Match source Hz / modeline | **cadence + OSD Content FPS**; play path logs PMS → Content FPS hint; switchres **TODO** — [docs/match-source-hz.md](docs/match-source-hz.md) |
| Multi-server conf (`PLEX_SERVERS` / multi `PLEX_BASE`) + cast-selected base | **done** — unit in `test_resolve` |
| On-device/host browse CLI | `scripts/plex_browse.sh` (sections / play / status / stop / seek) |
| On-device interactive menu | `scripts/plex_menu.sh` — pick section/item → `playMedia` on `localhost:3005` |
| Scrubber duration / seekRange after resolve | **done** — bind + setState; seek clamp |
| Scrubber step / skip | **done** — `stepForward`/`stepBack` (±10s), `skipNext` (queue), `skipPrevious` (restart@0 if >3s; queue prev near start) |
| Next-episode stub (EOF → next `playQueue` item via internal play) | **done** — conf `AUTO_NEXT=1` (default) |
| Subtitles burn-in | plan [docs/subtitles-burnin.md](docs/subtitles-burnin.md); conf `SUBTITLES=burn\|ffmpeg` |
| Multi-title soak | `tests/hw/test_soak.sh` (PMS conf auto-discover) |
| Release tarball | `scripts/package_release.sh` / `make package` |

**Multi-server:** `PLEX_SERVERS=http://a:32400,http://b:32400` and/or multiple `PLEX_BASE=` lines. First entry is default; cast `address=`/`port=`/`protocol=` selects the active base; if cast omits address and resolve fails, remaining servers are tried.

**Browse / on-device play:**

```bash
./scripts/plex_browse.sh sections              # needs PLEX_TOKEN + PLEX_BASE
./scripts/plex_browse.sh section <id>
./scripts/plex_browse.sh play <ratingKey>      # → misterplexd playMedia (default 127.0.0.1:3005)
./scripts/plex_browse.sh status | stop | pause | resume | seek <ms>
./scripts/plex_menu.sh                         # interactive TUI (SSH / MiSTer Scripts)
```

### Phase 5 (release / CRT·LCD matrix)

| Item | Status |
|------|--------|
| Install + conf reference | [docs/release.md](docs/release.md) — PRESENT/STREAM matrix, known limits |
| CRT 15 kHz / HDMI checklist | [docs/crt-lcd-matrix.md](docs/crt-lcd-matrix.md) |
| Hardened multi-title soak | Auto-load `/media/fat/misterplex/misterplex.conf` from lab MiSTer; PMS key discovery |
| Package includes `Plex.rbf` when built | `cores/Plex.rbf` from releases/ → output_files/ → mister-dev out |
| Phase 2 cast path | **unchanged** — keep `PRESENT=fb0` `STREAM=0` as safe conf |

```bash
make unit                          # multi-server + Content FPS + companion scrubber/step + browse smoke
make package                       # dist/misterplex-*.tar.gz (ARM + conf + docs + RBF if present)
MISTER_HOST=<mister-ip> ./scripts/deploy_misterplexd.sh
./scripts/plex_browse.sh sections  # needs PLEX_TOKEN + PLEX_BASE (or conf)
./scripts/plex_browse.sh play <ratingKey>   # on-device / host → localhost:3005
./scripts/plex_menu.sh             # interactive library → play
./tests/hw/test_playqueue_bind.sh
SOAK_HOLD_S=5 ./tests/hw/test_soak.sh   # multi-title when PMS conf available
```

Conf example: [assets/misterplex.conf.example](assets/misterplex.conf.example).

**Artifacts:** `misterfpga-dev/out/Plex_MiSTer/Plex.rbf` and `fpga/Plex_MiSTer/releases/Plex.rbf`.  
**Tests:** `make unit` + HW scripts under `tests/hw/`.

## Layout

```text
misterplex/
  fpga/Plex_MiSTer/   # Quartus core → Plex.rbf
  arm/misterplexd/    # HPS daemon
  host/libmisterplex/ # shared algorithms (cadence, …)
  tests/unit|hw/
  docs/
  scripts/
```

## Quick start (dev host)

```bash
cd $HOME/misterplex-cast
make unit                 # cadence math tests
make plexd                # host skeleton daemon
```

### Build RBF (Quartus via misterfpga-dev)

```bash
export PATH=$HOME/misterfpga-dev/bin:$PATH
# once: mister-dev setup
make build-rbf            # long; outputs under misterfpga-dev/out or core tree
```

### OSD (Phase 1 core)

- **Content FPS** 24 / 30 / 60 / 12 — unique advance rate vs display
- **Pattern** bars / bars+block / grid / ramp
- **Audio tone** on/off
- **TV Mode** NTSC / PAL family

Film on fixed 60 Hz: set Content FPS **24** — FPGA holds frames (3:2 density), block motion advances only on unique ticks.

## Relationship to mistercast-linux

| mistercast-linux | MiSTerPlex |
|------------------|------------|
| Groovy UDP present | Native `Plex.rbf` present |
| ARM/host FFmpeg decode as product | Transitional only; FPGA decode is the goal |
| Proven companion + lip-sync lessons | Port into `misterplexd` (Phase 2) |

## License

- FPGA core / sys: GPL-2.0-or-later (MiSTer)
- Host/ARM tools: MIT unless noted
