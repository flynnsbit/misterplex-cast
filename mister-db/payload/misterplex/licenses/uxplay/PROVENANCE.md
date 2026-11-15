# UxPlay (libairplay) statically linked into bin/misterplexd

Version: commit 4e95a9ec013c52d8555d88dbc55abe346e94d56e (FDH2/UxPlay)
Source URL: https://github.com/FDH2/UxPlay/commit/4e95a9ec013c52d8555d88dbc55abe346e94d56e
Build: scripts/build_arm_airplay.sh compiles lib/*.c after
tools/gcast/uxplay-patches/apply_gcast_uxplay_hardening.py,
apply_gcast_uxplay_ptth.py, apply_gcast_uxplay_zerodur.py, apply_gcast_uxplay_admit.py,
apply_gcast_uxplay_origin.py (refuses requests carrying an Origin header),
apply_gcast_uxplay_handoff.py (no SIGSEGV on AirPlay playlist delete during a YouTube handoff),
apply_gcast_uxplay_hdr431.py (finishes 431 overlong-header responses and closes
that connection),
apply_gcast_uxplay_twitterav.py with gcast_twitterav.h (maps X twitterav://video-s.twimg.com/
play locations to https://; any other twitterav host is still rejected).

License of the objects in libairplay.a:
- Most lib/*.c and lib/*.h that carry a header are LGPL-2.1-or-later (COPYING.LGPLv2.1).
- lib/crypto.c and lib/crypto.h are GPL-3.0-or-later. LICENSE is the GNU GPL version 3 text shipped at the root of that tree.
- lib/srp.c is MIT; see ../csrp/.
- lib/playfair, lib/llhttp, and lib/mdnsd are separate notices; see ../playfair, ../llhttp, and ../mdnsd.
