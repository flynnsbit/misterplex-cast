# Bitstreams shipped in this package

MiSTerPlex.rbf (also at cores/MiSTerPlex.rbf and _Utility/MiSTerPlex.rbf) is GPL-3.0-or-later: that is upstream's licence for MiSTer_h264_decoder (see its NOTICE in the PlexOB source tarball), and it is distributed here as part of a combined work under GPL version 3.
LICENSE is the GNU General Public License version 3.
Files in the upstream h264/ tree carry no per-file licence header; the grant comes from upstream's NOTICE. The sys/ files that carry a header say GPL version 2 or later, or version 3 or later.
It is built from the MiSTer_h264_decoder fork at eac1064 (hps/ of that tree is MIT and is not the bitstream).
The PlexOB RTL in this repository is commit 2b6570a89d1d6f28560287fb978e18fe5c09b4ef
(fpga/owenb-combo-s9/PlexOB_480p, BUILD_ID 0x0B111401, SEED 12).

This package ships MiSTerPlex.rbf as its only FPGA core. GPL-2 in this folder is the GPL version 2 text, kept because the on-card helper's x264 is GPL-2.0-or-later (see licenses/gcast-ffmpeg/NOTICE).
