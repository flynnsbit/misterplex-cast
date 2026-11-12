# Corresponding source

Corresponding source: https://github.com/flynnsbit/misterplex-cast

This bundle is an allow-list source export (daemon + FPGA tree + one RBF), not
a card-installable release tarball. It does not ship a prebuilt misterplexd
or SHA256SUMS. Card install uses the release package (bin/, SHA256SUMS). The
installer script here is the same one that package uses.

## cores/MiSTerPlex.rbf
- RBF md5: 76cde669ad5ab2b42091e8adfc267455 (fit artifact plexob-yc-0b1101c5, BUILD_ID 0x0B1101C5, seed 10)
- Sidecar: cores/MiSTerPlex.rbf.md5 (same md5; required by install-misterplex.sh)
- Pin list in scripts/install-misterplex.sh: 76cde669ad5ab2b42091e8adfc267455
- FPGA tree pin: c7e929f4b78b9dc1347d298e2a9c0017bcb01fad
  This is the commit the shipped bitstream was built from (Quartus 17.0.2,
  fpga/owenb-combo-s9/PlexOB_480p/fit/yc/launch_yc.sh with YC_SEED=10; fit
  artifact plexob-yc-0b1101c5-s10_20261112_1509.rbf).
  This export does not rebuild it.
- Fork source in this tree: fpga/owenb-combo-s9/
- Upstream, vendored where sync_vendor.sh reads it (OWENB_VENDOR_ROOT, default
  fpga/owenb-combo-s9/PlexOB_480p/vendor-upstream):
  https://github.com/owenb321/MiSTer_h264_decoder @ eac10641c5af0b808d332bb61344ee2dfdba4271
  Fetched at export time into this tree (complete corresponding source for
  cores/MiSTerPlex.rbf). Manifest: PlexOB_480p/vendor-upstream.md5.
  Required files: vendor-upstream/LICENSE, H264.qsf, hps/LICENSE.
- MIT exception: vendor-upstream/hps/ is MIT. LICENSE: vendor-upstream/hps/LICENSE
- Build: PlexOB_480p/build_plexob_480p.sh (Quartus; run_quartus_docker.sh)

## Prebuilt core
cores/MiSTerPlex.rbf (md5 76cde669ad5ab2b42091e8adfc267455) is the bitstream shipped with this export.
BUILD_ID 0x0B1101C5, seed 10, built from c7e929f4b78b9dc1347d298e2a9c0017bcb01fad. This export does not rebuild the bitstream.
The installer copies it to /media/fat/_Utility/MiSTerPlex.rbf only (one copy).

## misterplexd
- Licence: GPL-3.0-or-later (daemon and the MiSTerPlex FPGA fork).
- Source revision: 6c3679ae9708210f46902a04305a9c956e2701b1
- Docs and installer revision: 6c3679ae9708210f46902a04305a9c956e2701b1
- Paths: arm/misterplexd, host/libmisterplex, third_party/owenb-h264ring, Makefile
- Host build from this tree alone: `make plexd`
- Map: release_artifacts/GPL-SOURCE.md
- Notices: THIRD_PARTY.md

## Accepted risk
Product builds send X-Plex-Token as an HTTP header (libav headers, curl -K).
A loopback relay (#274) keeps the token off ffmpeg argv (-headers and the -i
URL). Test-only MUT_340_* / MUT_274_HEADERS_ARGV restore the old query/argv
forms. If the relay cannot bind, ffmpeg may still receive -headers with the
token (/proc/<pid>/cmdline); that fallback is the remaining single-user
residual. MUT_* macros are test-only; product `make plexd` /
`make arm-plexd-daemon` do not pass -DMUT_*.
