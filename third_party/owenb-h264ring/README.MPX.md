# owenb h264ring client library (vendored, MIT)

- **Upstream:** https://github.com/owenb321/MiSTer_h264_decoder, directory `hps/`, commit `eac1064` (2026-10-15).
- **License:** MIT. See `LICENSE`, which is upstream's `hps/LICENSE` and states that it covers the contents of `hps/`. The rest of the upstream repository (the core RTL and `clients/`) is GPL-3.0-or-later and is **not** vendored here. Do not copy any file from outside `hps/` into this tree.
- **Provenance:** `lib/h264ring_cavlc_tab.h` and `include/h264ring_layout.h` were generated upstream by tools under `tools/`, which are GPL. Upstream's NOTICE puts generated `hps/` output under the `hps/` licence (MIT).
- **Changes:** none. Files are byte-identical to upstream `eac1064`. Local edits, if ever needed, must be marked `MPX-FORK` and listed here.
- **Used by:**
  - `host/libmisterplex/owenb_ring.{hpp,cpp}` (the OwenbRing backend);
  - `tests/unit/test_owenb_ring.cpp`, which attaches the library to a heap memory backend and a fake core.
- **Not used:**
  - `h264ring_launch_core()`: it hard-codes CORENAME `H264`; core loading stays in misterplexd.
  - `h264ring_open_devmem()`: OwenbRing maps DDR itself through `OwenbDdrWindow`, so tests can mock it.

## md5 (eac1064)
```
185d9c63609a2bfd876c1a9db2343338  LICENSE
8f4743df704ab2beb5a07d8d817226a9  include/h264ring.h
e3eda2903e1e1284aa55298990ebff8f  include/h264ring_cavlc.h
6587cd0a2b9c0d62a7d788e39e604283  include/h264ring_layout.h
7deac6d9fd004bfca425776d7416fae4  lib/h264ring.c
df9c4be40eb86634efa822a1faccb59c  lib/h264ring_au.c
58606875c43e9f5955a75d0b705ab9f0  lib/h264ring_cavlc.c
024193260e01be8d55f1bdb454a45848  lib/h264ring_cavlc_tab.h
39b026ac174e8db8e8199a5d02a06cdb  lib/h264ring_dump.c
a83ff636a56a4410f95309e965926c66  lib/h264ring_linux.c
92ec5c78a0a269a8cd2af800d14e20cd  lib/h264ring_priv.h
```
