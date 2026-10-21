# C656 — fix PTS jump clamp (review r0 BLOCK)

Fix round for the PTS jump clamp after review r0 blocked it. Starts from
`291c0d2352bef9916719e2425a33e3d6eb3beae9` (phase 3).
No MiSTer, no PMS, no Quartus, no push. The first `git commit` failed on a
read-only `index.lock` (EACCES, rc=128); the leftover commit was
`57984605820a966b46cb642d1f98449a2d1ee4fe`.

Goal: clamp a decoded-picture PTS that goes backward, or more than 2 frame
durations forward, to `last_ok_pts + frame_duration` in stream ticks, so
480p A/V stay locked at ~24/30 fps. No reopen / seek-from-0.

## Findings

### 1. Clamp inside `MPX_AV_TRACE` — BLOCK — fixed

**Review:** the entire clamp lived in the second `MPX_AV_TRACE(...)` block
after `demux.readAccessUnit`. Product `arm-plexd-daemon` builds with
`ARM_PLEXD_AV_TRACE ?= 0`, so `MPX_AV_TRACE` is empty and the 480p binary
kept only the `last_ok_pts_` reset.

**Fix:** restore the trace-only body (`span.value` of pts/duration/timebase,
else `span.force()`). Call `clampPtsJump` immediately after that macro
closes, before `demuxReturn = read;`. Phase-3 python strip of
`MPX_AV_TRACE(...)` still finds `clampPtsJump`.

**Left open:** none for this finding.

### 2. `packet.pts` assigned microseconds — BLOCK — fixed

**Review:** clamp converted ticks → us, then wrote `last_ok_pts_ +
frame_duration_us_` back into `packet.pts`. Later pacing multiplies that
field by `tbNum * 1e6 / tbDen` again, so a 90 kHz 30 fps clamp ran away
(`66666` ticks treated as ticks → `740733` us).

**Fix:** `clampPtsJump` stays in tick space. Frame duration is
`frameDurationTicks(tbNum, tbDen, fpsNum, fpsDen)` (90 kHz / 30 fps =
3000 ticks). Clamped PTS is `last_ok_pts + frame_duration_ticks`. Unit
test pins a 1 s jump (`90000`) to `6000`, not `66666`.

**Left open:** none. Downstream `packet.pts * timebaseNum * 1000000 /
timebaseDen` is unchanged.

### 3. Backward trip used invented −50 ms — BLOCK — fixed

**Review:** `delta < -50000` missed a one-frame rewind (−33333 us at 30 fps,
−41667 us at 24 fps). Task text is any backward jump.

**Fix:** `if (delta < 0 || delta > 2 * frame_duration_ticks)`. One-frame
backward (−3000 ticks from last=6000) clamps to `9000`. `-50000` is gone.

**Left open:** none.

### 4. Frame duration learned from first demux gap — BLOCK — fixed

**Review:** `frame_duration_us_` started at 0 (clamp inert on AU 0 and AU 1),
then locked the first positive delta if `0 < diff < 500000`. A 200 ms stall
became the 2-frame trip. `fpsNum_` / `fpsDen_` and `kDefaultFpsNum=24`
already existed.

**Fix:** duration comes from `fpsNum_`/`fpsDen_` (else `kDefaultFpsNum` /
`kDefaultFpsDen`) converted into ticks. No 0.5 s cap. First AU seeds
`last_ok_pts_`; the second AU can clamp.

**Left open:** if both timebase and fps are zero/invalid, `frameDurationTicks`
returns 0 and the helper passes PTS through (same as “no duration yet”,
without inventing a gap).

### 5. No test; accept greps matched comments — BLOCK — fixed

**Review:** queue accept `grep -E 'ptsJump|jumpClamp|clampPts|PTS_JUMP'` hit
the `PTS_JUMP clamp` comment and the false `streamPump` comment.
`clampPts` was never a symbol.

**Fix:** `tests/unit/test_pts_jump_clamp.cpp` (no Makefile target; compile
with `g++ -std=c++17 -Ihost`). Accept grep now hits live
`packet.pts = clampPtsJump(...)` at `media_player.cpp:4220`.

Red on `291c0d2352`: header absent (`git cat-file` **true rc=128**);
compile without it **true rc=1** (`fatal error: libmisterplex/pts_jump_clamp.hpp`).
Green: compile **true rc=0**, `./build/test_pts_jump_clamp` **true rc=0**.

**Left open:** the helper is not linked into `make unit` (reviewer: do not
add a Makefile target).

### 6. False `streamPump` comment — Nit — fixed

**Review:** `// PTS data is clamped by the demux loop before reaching this
path (PTS_JUMP clamp).` sat on the STREAM host-recon path. Clamp lives in
`fpgaThreadMain`.

**Fix:** line deleted.

**Left open:** none.

### 7. Dead `ptsClampActive_` — Nit — fixed

**Review:** stored true on clamp, reset on play, never read.

**Fix:** member, playInternal store, and clamp store removed.
`frame_duration_us_` removed with it. `last_ok_pts_ = INT64_MIN` kept.

**Left open:** none.

### 8. Extra indent on new lines — Nit — fixed

**Review:** clamp sat four spaces deeper than the `if (read > 0)` trace body.

**Fix:** the trace body is restored; the live clamp uses the same indent as
`demuxReturn = read;`.

**Left open:** none.

## Files

- `host/libmisterplex/pts_jump_clamp.hpp` — tick-space helper
- `tests/unit/test_pts_jump_clamp.cpp` — host unit (g++ -Ihost)
- `arm/misterplexd/media_player.cpp` — include, clamp outside TRACE, drop
  dead reset + false comment
- `arm/misterplexd/media_player.hpp` — keep `last_ok_pts_`, drop
  `frame_duration_us_` and `ptsClampActive_`

Not touched: High-profile `plex_resolve.cpp` paths, `assets/plex-profiles/MiSTerPlex.xml`,
keepalive, companion playbar, bitrate caps, reopen/seek-from-0.

## Accept

```text
test -f arm/misterplexd/media_player.cpp && test -f arm/misterplexd/media_player.hpp && grep -E -n 'ptsJump|jumpClamp|clampPts|PTS_JUMP' arm/misterplexd/media_player.cpp
```

true rc=0. Grep hits `clampPtsJump` on the live demux path, not a comment.
