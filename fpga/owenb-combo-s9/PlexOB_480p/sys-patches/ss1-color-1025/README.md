# SS1 colour (FPGA Y/C encoder) sys/ backport -- source of the shipped PlexOB core fcd20f46 (0x0B2403A7 + these two patches)
Apply from the root of the eac1064 sys pin (the dir that contains `sys/`), in order:
  patch -p1 < 0001-sys-yc_out-template-5dda3736.patch      # yc_out.sv == Template_MiSTer 5dda3736 (same ports, drop-in)
  patch -p1 < 0002-sys-hps_io-vid_fclks-words-19-20.patch  # video_calc: frame clock count -> Main frame-locked PHASE_INC
Dry-run verified against upstream eac10641 sys/ (md5 of patched yc_out.sv ed47551a, hps_io.sv e1184d9c).
Any sys/ change re-rolls the fit seed: fit/yc/launch_yc.sh applies these after the eac sync, pins both md5s and requires exactly these two files to differ (SYS_EQ_EAC1064=0, SYS_PATCH in RESULT.env). These patches are the GPL source of the shipped core and must ship with it.

2026-10-25 20:40 CT build leg: fit/yc/launch_yc_color.sh (copy of launch_yc.sh; FIT_NAME plexob-ss1c-0b2403a7; applies both patches after sync_vendor,
pins patched yc_out.sv ed47551a / hps_io.sv e1184d9c, requires only those two files to differ from eac1064 sys/). BUILD_ID stays 0x0B2403A7 (daemon allowlist unchanged).
yc.txt.plexob = proposed PlexOB yc.txt lines (verified against Main video.cpp key format and 2^40*fsc/81MHz).

Provenance note (1101): the patched PlexOB vendor `sys/yc_out.sv` (md5 ed47551a,
git blob ce021b1b) is byte-identical to Template_MiSTer 5dda3736 (2026-10-16,
"register the sine values", #120). That is a different file from
`fpga/Plex_MiSTer/sys/yc_out.sv` (blob 04e5f0df), which matches older upstream:
introduced in 69b8a2ac and last present in baaeff60 (2026-08-22 12:37 CT) before
3ea1134 replaced it. The sys/ audit's "baaeff60" refers to the Plex_MiSTer file.

Provenance (0002): the +/- lines of 0002 are byte-equal to upstream Template_MiSTer b4726d2dd8cfad67db3f8ba060aa4e8c13047662 (MikeS11, "YC PHASE_INC full-frame measurement #106"): https://github.com/MiSTer-devel/Template_MiSTer/commit/b4726d2dd8cfad67db3f8ba060aa4e8c13047662 . Only line offsets and context differ, because the base is owenb eac10641 sys/hps_io.sv. Applied to that file, it produces md5 e1184d9c43c4faf9df14c92aaf9ade79, pinned at fit/yc/launch_yc.sh:247. Nothing else touches the vendor hps_io. The added comment reads exactly "// Exact clk_vid edges in one complete frame."

Update (1101, fpga/yc-out-5dda3736-1101): both in-repo copies fpga/Plex_MiSTer/sys/yc_out.sv and
fpga/owenb-combo-s9/Plex_MiSTer/sys/yc_out.sv are now byte-identical to Template_MiSTer
5dda37366d02b04b8448da7b322235c1831af811 sys/yc_out.sv (md5 ed47551a7ed3106a5c229cf20badca8f), same as the
patched vendor file. Ports are unchanged vs 69b8a2a, so no other sys/ file changes.
