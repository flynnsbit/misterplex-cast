# Hardware visual decode fixtures

Checked-in inputs for `tests/hw/test_f3_visual_golden.sh`.

- `plex_real_baseline_320x240_57674f2e_mjpeg720_golden.png` — **quarantined
  legacy rollback golden**, captured from `57674f2e4c11551898275e99bd4c3067`
  through the repaired `/dev/video4` MJPEG 1280×720@60 path after pushing
  `tests/fixtures/p3_host_recon/plex_real_baseline_320x240_1f.264`. It is no
  longer a default because `57674f2e` predates the current YUV420 frame-store
  contract. Its `.provenance.json` declares source RBF, 320×240 geometry,
  RGB565 legacy pixel format, BT.601/full colour, and ROI `11,0,160,120`; the
  harness refuses to grade it unless those declarations match the loaded core
  and command-line contract.
- `plex_visual_624x480_1f.264` — one-frame H.264 Baseline/CAVLC Annex-B vector,
  generated from `testsrc2=size=624x480:rate=1` with `cabac=0`, one IDR, no B-slices.
  This is a future target fixture, not the default hardware gate, because it has
  not yet been proven green on rollback `57674f2e`.
- `plex_visual_640x480_golden.png` — generated reference for how the product path
  should display coded 624×480: right-cropped to 618×480, then pillarboxed to
  640×480 with 11 black pixels on each side. Its provenance sidecar marks it as
  `generated_reference`, so the hardware harness refuses to treat it as a
  captured hardware golden.
- `capture_logs/wcap_fe7673bc_yuyv422*.log` — real W-CAP hardware logs from `/dev/video4`
  YUYV422 captures of the characterized bad `fe7673bc959f37fd7da44e8a865f7db3` core.
  They contain V4L2 corrupted buffer diagnostics and are unit fixtures for rc=4
  capture-integrity rejection. The primary log negotiated `yuyv422, 1280x720, 10 fps`
  and reported `1843200 bytes` corrupt (`1280*720*2`); the secondary 640×480 log
  reported `614400 bytes` corrupt (`640*480*2`).

Every PNG used as a hardware golden must have a sibling
`*.png.provenance.json`. Missing sidecars return `rc=9` before pixels are
graded. The geometry is not defined only here; the comparator also reads
`host/libmisterplex/ddr_frame_layout.hpp` and
`fpga/Plex_MiSTer/rtl/ddr_frame_layout_params.svh` and refuses to run if they diverge.
