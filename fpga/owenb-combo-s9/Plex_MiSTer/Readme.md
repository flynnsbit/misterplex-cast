# Plex core (MiSTerPlex)

Native present core for the MiSTerPlex media client.

## Phase 1 features

- Progressive color bars / grid / ramp
- **Present cadence** (24/30/60/12 content fps vs 50/60 display)
- Continuous audio tone on `CLK_AUDIO`
- OSD options for pattern, content FPS, mute, NTSC/PAL

## Build

From monorepo root:

```bash
./scripts/build_rbf.sh
```

Or:

```bash
mister-dev build $HOME/misterplex-cast/fpga/Plex_MiSTer --qpf Plex
```

## Deploy

```bash
./scripts/deploy_plex_core.sh
```

## Design notes

FPGA owns vsync. Unique content index advances only when
`floor(n * content_fps / display_hz)` increases — same math as
`host/libmisterplex/cadence.hpp` (see `make unit`).
