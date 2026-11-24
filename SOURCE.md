# Corresponding source

Corresponding source: https://github.com/flynnsbit/misterplex-cast

This bundle is an allow-list source export (daemon + FPGA tree + one RBF), not
a card-installable release tarball. It does not ship a prebuilt misterplexd
or SHA256SUMS. Card install uses the release package (bin/, SHA256SUMS). The
installer script here is the same one that package uses.

## cores/MiSTerPlex.rbf
- RBF md5: 042f86171b562a61694da46860b89d89 (fit artifact plexob-yc-0b111401-s10, BUILD_ID 0x0B111401, seed 10, Quartus 17.0.2 Lite)
- Sidecar: cores/MiSTerPlex.rbf.md5 (same md5; required by install-misterplex.sh)
- Pin list in scripts/install-misterplex.sh: 042f86171b562a61694da46860b89d89
- FPGA tree pin: 204693323c22d25fc617e511f90352262b8d296b
  This is the commit the shipped bitstream was built from (clean tree; MiSTer
  sys patch set ss1-color-1025 0001+0002, which is in this FPGA tree).
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
cores/MiSTerPlex.rbf (md5 042f86171b562a61694da46860b89d89) is the bitstream shipped with this export.
BUILD_ID 0x0B111401, seed 10, built from 204693323c22d25fc617e511f90352262b8d296b. This export does not rebuild the bitstream.
The installer copies it to /media/fat/_Utility/MiSTerPlex.rbf only (one copy).

## misterplexd
- Licence: GPL-3.0-or-later (daemon and the MiSTerPlex FPGA fork).
- Source revision: 6e55d1ce1a45f9ad3ac6aa3d5abc12d9af22edea
- Docs and installer revision: 6e55d1ce1a45f9ad3ac6aa3d5abc12d9af22edea
- Paths: arm/misterplexd, host/libmisterplex, third_party/owenb-h264ring, Makefile
- Host build from this tree alone: `make plexd`
- ARM build (the shipped binary's recipe), from a git checkout of this tree:
  `bash scripts/repro_arm_build.sh HEAD`
  It fetches pinned upstream sources (sha256-checked tarballs: Arm GNU
  13.2.rel1, FFmpeg 8.1.2, OpenSSL 3.0.15, libplist 2.6.0; UxPlay pinned by
  git revision 4e95a9ec, plus the patches in tools/airplay/uxplay-patches) and
  builds under /tmp/mpx-repro. The exact upstream source archives are also
  attached to the release (README, Source code). The shipped
  binary embeds its private source revision string; a build from this export
  embeds this export's commit id instead and is otherwise the same recipe.
- AirPlay/DIAL start check (qemu-arm-static): `make host-airplay-dial-check HOST_AP_BIN=/tmp/mpx-repro/out/misterplexd.stripped`
- Other shipped files: owenb_idle_card (tools/owenb_idle_card.cpp), boot hook
  scripts/misterplexd-boot-rc3.sh and scripts/misterplexd_supervise.sh, and
  scripts/misterplex_mode.sh, which is byte-identical to the shipped misterplex_mode.sh.
- Map: release_artifacts/GPL-SOURCE.md
- Notices: THIRD_PARTY.md

## Accepted risk
Product builds send X-Plex-Token as an HTTP header (libav headers, curl -K).
A loopback relay (#274) keeps the token off ffmpeg argv (-headers and the -i
URL). Test-only MUT_340_* / MUT_274_HEADERS_ARGV restore the old query/argv
forms. If the relay cannot bind, playback fails closed: ffmpegRelayPrepare()
refuses to put the token on ffmpeg argv. MUT_* macros are test-only; product `make plexd` /
`make arm-plexd-daemon` do not pass -DMUT_*.
