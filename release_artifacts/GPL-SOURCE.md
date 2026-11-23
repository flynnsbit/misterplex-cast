# GPL corresponding source (sec-cleanup-1101)

MiSTerPlex code, the FPGA core and the daemons, is GPL-3.0-or-later, matching owenb321/MiSTer_h264_decoder.
Corresponding source: https://github.com/flynnsbit/misterplex-cast

The FPGA source (RTL, Quartus project, MiSTer framework sys/) is in fpga/ of this repository; upstream: https://github.com/owenb321/MiSTer_h264_decoder (GPL-3.0-or-later) at eac10641c5af0b808d332bb61344ee2dfdba4271.
MIT exception: the upstream hps/ directory stays MIT. Its LICENSE is hps/LICENSE (vendored in-repo as third_party/owenb-h264ring/LICENSE, and in the export beside vendor-upstream/hps/). That MIT grant does not cover the core or the daemons.

The public export ships one RBF, cores/MiSTerPlex.rbf (the 0x0B1101C5 bitstream below), plus misterplexd source. Other binaries below stay in this private tree. Each release asset that contains a binary also carries a source tarball (scripts/package_release.sh, NOTICE).

## Shipped core

The installed file is cores/MiSTerPlex.rbf. It is release_artifacts/plexob-yc-0b1101c5/MiSTerPlex.rbf.

- BUILD_ID: 0x0B1101C5
- SEED: 10
- Source commit: 5588134126b965c8095eb2fa7cf3655f958579da
- md5: 96eb13d2cfc966fa56528703d0b542cf

The earlier file release_artifacts/plexob-yc-0b2403a7/PlexOB.rbf (md5 ecae698fa3220149fd767b9623137864, BUILD_ID 0x0B2403A7) stays in the tree. The daemon still accepts that id. The export FPGA tree is 5588134, the commit this bitstream was built from.

| RBF | md5 | commit that added it | exact build commit |
|---|---|---|---|
| release_artifacts/plexob-yc-0b1101c5/MiSTerPlex.rbf | 96eb13d2cfc966fa56528703d0b542cf | this release branch | 5588134126b965c8095eb2fa7cf3655f958579da (BUILD_ID 0x0B1101C5, seed 10) |
| release_artifacts/plexob-yc-0b2403a7/PlexOB.rbf | ecae698fa3220149fd767b9623137864 | 7f2b165 | earlier 240p bitstream; not this package's core |
| release_artifacts/plexob-yc-0b59c11a/PlexOB.rbf | c8b346048d680cc410a447490f57a820 | 0e3cdfc | UNVERIFIED: needs maintainer to confirm |
| release_artifacts/v0.2.0-glass-baseline/Plex.rbf | dfebf2bfd08dd70b473b587dd7e81848 | e0f942e | UNVERIFIED: needs maintainer to confirm |
| release_artifacts/v0.3.0-lab-stable/Plex.rbf | 41adb98c7a630b541091c22ce291be68 | e0f942e | UNVERIFIED: needs maintainer to confirm |
| release_artifacts/v0.3.0/Plex.rbf | 41adb98c7a630b541091c22ce291be68 | e0f942e | UNVERIFIED: needs maintainer to confirm |
| release_artifacts/v0.4.0/Plex.rbf | 1c6ed06fe832fb54259d4f4ce504ccae | e0f942e | UNVERIFIED: needs maintainer to confirm |
| release_artifacts/v0.9.0-pre-paired/cores/Plex_240p15.rbf | 4d6efef954acf7b33747f35ac2878c1b | e0f942e | UNVERIFIED: needs maintainer to confirm |
| release_artifacts/v0.9.0-pre-paired/cores/Plex_480i.rbf | 61db00e7d54efad7c1a456b127b798bd | e0f942e | UNVERIFIED: needs maintainer to confirm |
| release_artifacts/v0.9.0-pre-paired/cores/Plex_480p.rbf | 07f54d9f8f0eda2fe75d9cc314f6de54 | e0f942e | UNVERIFIED: needs maintainer to confirm |
| release_artifacts/v0.9.0-pre-paired/cores/Plex_720p24.rbf | 0eea3580a5a0dacf60bf65c50499bcc9 | e0f942e | UNVERIFIED: needs maintainer to confirm |

## Daemons (GPL-3.0-or-later)

misterplexd source is arm/misterplexd/, host/libmisterplex/, and third_party/owenb-h264ring/ (MIT hps client), built by the top-level Makefile (`make plexd` on a host, `make arm-plexd-daemon` for the MiSTer). The four binaries below are the shipped daemon builds in this private tree. Their exact build commits are unverified; the export ships the source from the export branch head instead of these binaries.

| Binary | md5 | licence |
|---|---|---|
| release_artifacts/v0.2.0-glass-baseline/misterplexd | 7cd10b4d438c714a9b8c4766dc982d59 | GPL-3.0-or-later (MiSTerPlex) plus LGPL-2.1-or-later libav and UxPlay parts; see NOTICE |
| release_artifacts/v0.3.0-lab-stable/misterplexd | 06c5735a2f85114688f0ff2ac36e4fd4 | GPL-3.0-or-later (MiSTerPlex) plus the same linked components |
| release_artifacts/v0.9.0-pre-paired/bin/misterplexd.480p | 5f1c861486844f4c83bf32a2112cfbb6 | GPL-3.0-or-later (MiSTerPlex) plus the same linked components |
| release_artifacts/v0.9.0-pre-paired/bin/misterplexd.720p24 | 0ffc1483677ef5733971d4f18e4624e4 | GPL-3.0-or-later (MiSTerPlex) plus the same linked components |

Helper programs that ship beside the daemon in a release package, not as extra binaries in this map:

| Helper | licence | corresponding source |
|---|---|---|
| linux/gcast/ffmpeg | GPL-3.0-or-later (FFmpeg built --enable-gpl --enable-version3 with libx264) | FFmpeg 8.1.2, x264, OpenSSL 3.0.15 tarballs named in NOTICE |
| linux/gcast/ffprobe | LGPL-2.1-or-later with OpenSSL | the same FFmpeg tarball, without --enable-gpl |
| yt-dlp (on-card helper) | Unlicense, plus the packed CPython and PyInstaller bootloader licences in NOTICE | upstream release named in NOTICE; update_ytdlp.sh pins are opt-in |

No other helper daemon binary is shipped from release_artifacts/.
