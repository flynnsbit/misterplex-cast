# Corresponding source

Corresponding source: https://github.com/flynnsbit/misterplex-cast

This bundle is an allow-list source export (daemon + FPGA tree + one RBF), not
a card-installable release tarball. It does not ship a prebuilt misterplexd
or SHA256SUMS. Card install uses the release package (bin/, SHA256SUMS). The
installer script here is the same one that package uses.

## cores/MiSTerPlex.rbf
- RBF md5: 7f048508861967f857a5aacff79bfb6f (fit artifact plexob-yc-0b112501, BUILD_ID 0x0B112501, seed 14)
- Sidecar: cores/MiSTerPlex.rbf.md5 (same md5; required by install-misterplex.sh)
- Ship pin 7f048508861967f857a5aacff79bfb6f is a token in scripts/install-misterplex.sh RBF_PINS (older pins stay accepted)
- FPGA tree pin: 33c010f5130e869cbe33124f14c1a667cbaaf53e
  This is the on-branch commit the shipped bitstream corresponds to (cherry-pick
  of MFD 544565ba). The bitstream file is
  release_artifacts/plexob-yc-0b112501/MiSTerPlex.rbf on the daemon revision.
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
cores/MiSTerPlex.rbf (md5 7f048508861967f857a5aacff79bfb6f) is the bitstream shipped with this export.
BUILD_ID 0x0B112501, seed 14, built from 33c010f5130e869cbe33124f14c1a667cbaaf53e. This export does not rebuild the bitstream.
The installer copies it to /media/fat/_Utility/MiSTerPlex.rbf only (one copy).

## misterplexd
- Licence: GPL-3.0-or-later (daemon and the MiSTerPlex FPGA fork).
- Source revision: 347863a406ba423a8989d19089bfa1d4bf55e04b
- Docs and installer revision: 347863a406ba423a8989d19089bfa1d4bf55e04b
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
