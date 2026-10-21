# PlexOB_480p (OWENB P0 scaffold)

GPL-3.0-or-later fork of `owenb321/MiSTer_h264_decoder` @ `eac1064`.  
Ships beside the existing `Plex_*` cores (`PlexOB_*` names).

## Vendor tree (not vendored in git yet)

```
$OWENB_VENDOR_ROOT
```

Run `./sync_vendor.sh` (uses `OWENB_VENDOR_ROOT` or `$OWENB_VENDOR_ROOT`).
Vendor tree is git-ignored; `MPX-FORK` edits live in this directory only.

## P0 deliverables

1. **Dual ABI** — Core-switch uses standard MiSTer **CAPS** (`0x30140074`, build
   `0x4F800001`). RTL: `mpx_mister_caps_shim.sv` (probe → MPC0..7). Host plan:
   `host/libmisterplex/owenb_caps_plan.hpp`. Owenb ring ctrl words **80–87 are not spare**
   (raster/audio/overlay); `owenb_status_shim.hpp` tracks the planned `ring_layout.py`
   extension, not word-80 overload.
2. **Quartus project** `PlexOB_480p.qsf` — upstream 480p raster, no `stream_path` / our DPB.
3. **Gate:** sim status reg; fit+timing ~98% ALM (seed sweep logged); glass idle bars on `a lab MiSTer`.

## License

Upstream core GPL-3.0-or-later; `hps/` MIT.
