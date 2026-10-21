# Playback controls hardware validation checklist

Run the scripted validation from the deploy-token window:

```bash
./scripts/validate_playback_controls_hw.sh run --rbf path/to/Plex.rbf --daemon build/arm/misterplexd
# emergency restore of the state saved at script start:
./scripts/validate_playback_controls_hw.sh rollback
```

Deploy safety: the script uses `DEPLOY_LOAD=menu ./scripts/deploy_plex_core.sh` for any RBF deploy. Treat deploy exit 3 (Main wedged) or 4 (never returned to Plex) as authoritative; do not trust an SD-card md5 alone.

## Automated uinput path

`tests/hw/osd_keys.py` runs on the MiSTer and creates a temporary
`misterplex-uinput-keys` virtual keyboard. The validation script stages it under
`/media/fat/misterplex/validation/` and injects F12/Space/Esc/Left/Right as real
press/release pairs, so keyboard checks exercise Main → hps_io → core → `PLXI`
mailbox rather than a daemon shortcut.

## PASS/FAIL points

1. **F12 Load lines** — the script injects F12 and captures the OSD. PASS only if the three file entries render as clean, readable Load lines for RGB565 frame, PCM s16le stereo, and H.264 Annex-B. FAIL if the old fragments appear: `*.raw,RGB,565, fr,ame`, `*.raw, s1,6le , st,ere,o`, or `*.H.2,64 ,ann,ex-,B e,...`.
2. **Controller mapping** — use F12 → Define buttons and map Play/Pause, Stop, Skip Fwd, Skip Back before the controller half of the script.
3. **Overlay** — each local command should show the correct icon/state, progress near the current time, skip direction/delta for Left/Right, and then auto-hide without dirty pixels.
4. **Casting app sync** — keep Plex phone/web visible. A local press should show state/time change on the app within about one Companion long-poll (~400 ms).
5. **PMS progress** — use a real library item, not `testsrc`. After Stop mid-item, confirm Plex shows Resume/On Deck; or set `PMS_BASE`, `PMS_TOKEN`, and `PMS_RATING_KEY` for the script to check `viewOffset`.
6. **Rollback if needed** — if controls double-fire, mailbox is not live, overlay is wrong, edge check fails, or the daemon/core misbehaves, run the rollback command before releasing the device.

## Scripted checks

The script stages a DDR-only mailbox probe, proves `PLXI` and `seq`/`cmd_seq` activity, verifies exactly one mailbox event per keyboard/controller press, checks Companion timeline response, parses drift/drop/CPU from logs, and runs the G-VID1 marker capture via `scripts/gen_edge_markers.py` + `scripts/check_edges.py`.
