# MiSTer CAPS bridge (P0 → P3)

Owenb feeds video on **ring v1.x** (`0x31400000` / `0x31800000`). misterplexd core-switch still probes **absolute** CAPS at `0x30140050`..`0x30140088` (`mailbox_abi_spec.hpp`).

## RTL

`mpx_mister_caps_shim.sv` — on `probe_valid`, publishes MPC0..7 (same packing as `fake_plex_core.hpp::answerProbe`).

Parameters must match `host/libmisterplex/owenb_caps_plan.hpp` (`BUILD_ID=0x4F800001`).

## Integration (next fit)

1. Map a lightweight-bridge slave at the MiSTerPlex CAPS physical window (or reuse the stock DDR ctrl region if the top already aliases it).
2. Drive `probe_valid` from the same event that answers a **Probe** on the Plex ring path (P3 may translate PLXB probes; P0 may use a dedicated HPS poke or a minimal PLXB shim).
3. Write `mpc_word0..7` to the mapped addresses; invalidate MPC7 before publishing (module already does).
4. Do **not** overload owenb `H264RING_CORE3` words 80–87; status extension needs a `ring_layout.py` fork.

## Host

- CAPS plan: `owenb_caps_plan.hpp` (not in `fpga_build_ids.hpp` until P3 daemon backend).
- Ring status: `owenb_status_shim.hpp` (future ctrl extension).

## Gate

P0: lint + one Quartus close on vendor `H264.qsf` with both MPX SV files listed.  
Glass: OSD load **PlexOB_480p**, idle bars; later 2 min Plex Web cast + dual capture per `BATCH-OWENB-PORT-1016.md`.
