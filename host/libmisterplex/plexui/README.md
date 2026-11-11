# Plex TV UI (MiSTer lab)

Premium 640×480 browse UI for `misterplexd`, gated behind environment flags.

## Flags

| Variable | Meaning |
|----------|---------|
| `MPX_BROWSE_V2=1` | Required: R1 idle browse worker + paint path |
| `MPX_BROWSE_MVPB=1` | Required: PLXI commands 6–11 (Up/Down/Page/Back/Info) |
| `MPX_PLEXUI=1` | Enable Plex TV UI instead of text browse rows |
| `MPX_PLEXUI_DIR` | Font directory (default `/media/fat/misterplex/plexui/`). Ships `Roboto-Regular.ttf` + `Roboto-Bold.ttf` (Apache-2.0, ~1.0 MB). Missing fonts fall back to the built-in Spleen 8x16 bitmap. |
| `MPX_PLEXUI_TOKEN_FILE` | Plex account token (first line); falls back to conf `PLEX_TOKEN` |

`MPX_PLEXUI` unset is on. `MPX_PLEXUI=0` opts out. The package copies the Roboto pair to `/media/fat/misterplex/plexui/`.

## Input (PLXI / remote)

| Action | MVP-b command | Plex UI |
|--------|---------------|---------|
| Open browser | Browse (5) | Same |
| Move up/down | Up/Down (6/7) | Focus up/down |
| Move left/right | Skip back/fwd (4/3) | Focus left/right in rows/grid |
| Enter / play | PlayPause (1) | Select (open, detail, play) |
| Back | Back (10) or Stop (2) | Up one level / hide at root |
| Stop during UI play | Stop | Stop and return to browser |
| UI size (normal/large) | Pad **Y** (`ObNav::Aux`) | Toggle `UI_SIZE` and persist to conf. Start still opens the Program panel. Unused on browse before GH#46 (Y opened the same panel as Start). |

## Host screenshots

```bash
make plexui_shot
./build/plexui_shot /path/to/plex_token "odds" /tmp/plexui_ \
  host/libmisterplex/plexui/third_party
```

Key script: `u` `d` `l` `r` select `s` back `b` open `o` pause `.` (400 ms).

## Build

- `make arm-plexd-daemon` — ARM `misterplexd` with plexui linked in
- `make plexui_shot` — host PNG tool
