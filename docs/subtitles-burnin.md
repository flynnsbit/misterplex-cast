# Subtitles burn-in plan (Phase 4)

> Not the rc4 PlexOB path. Subtitles are not supported on PlexOB. This note is a plan for an older decode path.

Goal: readable captions on CRT/LCD without a full on-OSD subtitle renderer.

## Constraints

| Path | Decode owner | Notes |
|------|--------------|--------|
| **STREAM=0** (current product) | dual-A9 FFmpeg → RGB → fb0/MrAudio | CPU budget tight; avoid libass on every cast |
| **STREAM=1** | host I-recon + F3 stub / future FPGA | Subtitles **not** in pixel path yet |
| **FPGA present** | frame_store / ascal | No soft-sub compositor; burn-in must be in the raster |

Therefore: **burn subtitles into the video before present**, prefer **PMS-side** over ARM-side.

## Preferred: PMS universal burn-in (`SUBTITLES=burn`)

When conf has `SUBTITLES=burn`, `misterplexd` adds to the weak universal ladder:

```text
&subtitles=burn
&subtitleStreamID=N   # optional, from SUBTITLE_STREAM=
```

Pros:

- ARM only decodes already-composited H.264 (same weak ladder cost).
- Works for casted library keys (`/library/metadata/N`).
- Language/stream selection can later map from cast `subtitleStreamID=` query.

Cons:

- Requires PMS to honor burn-in for the chosen stream.
- Forced transcode (already true for weak ladder).

## Optional: FFmpeg filter (`SUBTITLES=ffmpeg`, STREAM=0 only)

For **local file** paths (`/media/...` playable, not `http`), media_player appends:

```text
-vf scale=...,pad=...,subtitles=/path/to/file:si=N
```

Pros: works offline without PMS.  
Cons: needs libass-enabled FFmpeg on MiSTer; **not** applied to HTTP/universal URLs (too heavy / path escape issues).

## STREAM=1 / FPGA roadmap

1. Keep PMS burn-in as the only supported product mode for cast.
2. Host recon path: optional post-recon alpha blit of ASS (future; not scheduled).
3. FPGA: no soft-sub engine planned until after residual/decode solid; burn-in remains ARM or PMS.

## Conf knobs

```text
# off | burn | ffmpeg
SUBTITLES=off
# Optional stream id for burn/ffmpeg (PMS subtitleStreamID / FFmpeg si=)
# SUBTITLE_STREAM=0
```

## Cast control (future)

Wire `setStreams` / `subtitleStreamID` from companion → `WeakLadder` + re-resolve seek.  
Until then, conf defaults apply to all sessions.

## Verification

1. `SUBTITLES=burn` + cast title with forced English sub → pixels show captions on fb0.
2. `SUBTITLES=ffmpeg` + `playMedia` with local `key=/path/to.mkv` on host smoke.
3. `SUBTITLES=off` — no `subtitles=` in universal URL (unit: inspect resolve ladder build; HW: log).

## Non-goals (this phase)

- Selectable on-device subtitle menu UI
- PGS/VobSub without PMS burn
- External SRT sidecar discovery on SD
