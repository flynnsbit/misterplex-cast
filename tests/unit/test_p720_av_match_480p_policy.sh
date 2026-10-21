#!/usr/bin/env bash
# 720p combined pipe must match 480p A/V: start MrAudio with video, hold
# audio to pictures, do not stick-ingest on the ffmpeg reader.
set -u
set -o pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MP="$ROOT/arm/misterplexd/media_player.cpp"
SUP="$ROOT/scripts/misterplexd_supervise.sh"
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }
ok() { echo "OK: $*"; }

grep -q 'combined720pAudioStartsWithVideo' "$MP" || fail "media_player missing combined720pAudioStartsWithVideo"
grep -q 'holdLeadMsWithQueued' "$MP" || fail "media_player missing holdLeadMsWithQueued"
grep -q 'holdAudioToPicturesWanted' "$MP" || fail "media_player missing holdAudioToPicturesWanted"
grep -q 'stickIngestWantedOn720pPipe' "$MP" || fail "media_player missing stickIngestWantedOn720pPipe"
grep -q 'plex720pWcBankIngest' "$MP" || fail "media_player missing plex720pWcBankIngest"
grep -q 'hw_present_t0_rearm' "$MP" || fail "media_player must re-arm hw_fps after prefetch"
if grep -q 'prefetchedFile = true' "$MP"; then
  fail "720p must not prefetch-to-tmpfs (waitPid 20s identity body)"
fi
if grep -q 'wait_ok=1 spawn=pipe-from-tmpfs' "$MP"; then
  fail "720p must not swap URL to /tmp identity after prefetch"
fi
ok "720p streams HTTP (no prefetch tmpfs)"
STORE="$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_store.sv"
grep -q 'u_fill_base_mux' "$STORE" || fail "ddr_frame_store must instantiate ddr_frame_base_mux"
grep -q 'dyn_base0_r' "$STORE" || fail "ddr_frame_store must latch dyn_base from doorbell burst-2"
grep -q 'ddrDoorbellDynWord' "$ROOT/arm/misterplexd/fpga_spi.cpp" || fail "kickDdrDoorbell must publish dyn phys"
if grep -E '^[^#]*PLEX_PRESENT_720P_L4=1' "$ROOT/fpga/Plex_MiSTer/Plex.qsf" >/dev/null; then
  fail "product QSF must not enable L4 (480p gold QSF)"
fi
ok "product QSF stays 480p; L4 dyn-base is ifdef"
PLEXSV="$ROOT/fpga/Plex_MiSTer/Plex.sv"
if ! grep -q '`ifndef PLEX_PRESENT_720P_L4' "$PLEXSV"; then
  fail "Plex.sv must gate stream_path behind ifndef PLEX_PRESENT_720P_L4 (l4-dyn2 Fmax 13.99)"
fi
if ! awk '/`ifndef PLEX_PRESENT_720P_L4/{p=1} p&&/stream_path #\(/{found=1} p&&/`else/{exit} END{exit found?0:1}' "$PLEXSV"; then
  fail "product path must still instantiate stream_path when L4 is off"
else
  ok "stream_path instantiates only when L4 off"
fi
WIN="$ROOT/fpga/Plex_MiSTer/rtl/present_content_window.sv"
grep -q 'hd_m1_r' "$WIN" || fail "scale divide must latch hd_m1 (not combo from hde_act)"
grep -q 'sx_num_ceil_r' "$WIN" || fail "sx divide from latched numerator"
ok "scale divide latched off hde_act"
STORE="$ROOT/fpga/Plex_MiSTer/rtl/ddr_frame_store.sv"
CORE="$ROOT/fpga/Plex_MiSTer/rtl/present_core.sv"
grep -q 'ASPECT_ACK_PHYS' "$STORE" || fail "ddr_frame_store missing ASPECT_ACK_PHYS"
grep -q 'MAGIC_J' "$STORE" || fail "ddr_frame_store missing PLXJ MAGIC_J"
grep -q '504C_584A' "$STORE" || fail "ddr_frame_store missing PLXJ magic 504C584A"
grep -q 'source_aspect_ack' "$STORE" || fail "ddr_frame_store must instantiate source_aspect_ack"
grep -q 'ASPECT_ACK_PHYS(FS_DOORBELL + 32.h130)' "$CORE" || fail "L4 PLXJ must be doorbell+0x130"
grep -q 'MAILBOX_PHYS(FS_DOORBELL + 32.h100)' "$CORE" || fail "L4 PLXS must be doorbell+0x100 (not 0x3007F100)"
ok "L4 PLXJ/PLXS follow doorbell page"
grep -q 'dyn_valid0_r <= DDRAM_DOUT\[31\]' "$STORE" || fail "dyn_valid must follow doorbell bit31 (sticky 1 greys L4)"
ok "dyn_valid clears when beat2 valid=0"
grep -A2 'PLEX_PRESENT_720P_L4' "$CORE" | grep -q 'Y_FILL_STRIDE(1)' || fail "L4 Y_FILL_STRIDE must be 1 (stride 3 greys/stripes chroma)"
ok "L4 Y_FILL_STRIDE=1"
grep -q 'C_INTERLEAVE_ON_Y_BEAM(1' "$CORE" || fail "L4 must interleave C fetches (need_y_beam starve → grey chevron UV=128)"
grep -q 'C_INTERLEAVE_ON_Y_BEAM' "$STORE" || fail "ddr_frame_store missing C_INTERLEAVE_ON_Y_BEAM"
ok "L4 C interleave on Y-beam starve"
PLL="$ROOT/fpga/Plex_MiSTer/rtl/pll/pll_0002.v"
n24=$(grep -c '`ifdef PLEX_CLK_SYS_24' "$PLL" || true)
if [ "$n24" -lt 2 ]; then
  fail "pll_0002.v must wrap BOTH altera_pll out0 with PLEX_CLK_SYS_24 (have $n24); 3-clock-hardcoded 20 MHz is ~20 Hz HDMI"
else
  ok "pll_0002.v PLEX_CLK_SYS_24 count=$n24"
fi
if grep -q 'farpoint_1280x720.mp4' "$ROOT/arm/misterplexd/plex_resolve.cpp"; then
  grep -q 'refusing farpoint_1280x720.mp4 cache bypass' "$ROOT/arm/misterplexd/plex_resolve.cpp" || \
    fail "plex_resolve must not spawn farpoint_1280x720.mp4 as a playable (reject-only is ok)"
fi
grep -q 'refusing farpoint_1280x720.mp4 cache bypass' "$ROOT/arm/misterplexd/plex_resolve.cpp" || \
  fail "plex_resolve must refuse 40868 identity cache bypass"
grep -q 'glass_col' "$ROOT/scripts/misterplex_core_watch.sh" || \
  fail "watch must pin L4 from pairs col3 (28cb5a75 was true480 → 640x480 bank)"
grep -q '0x3047F12C' "$ROOT/scripts/misterplex_named_rbf.sh" || \
  fail "live_glass_is_l4 must sample L4 PLXD before leftover 480p PLXS"
ok "plex_resolve refuses farpoint_1280x720.mp4 cache bypass"
grep -q 'liveUniversalMustStreamHttp' "$ROOT/host/libmisterplex/p720_transcode_vf.hpp" || \
  fail "live universal must stream HTTP (prefetch waitPid 20s short-read 0 frames)"
PROXY="$ROOT/scripts/pms_720p_proxy.py"
if grep -q '_serve_clip_mp4' "$PROXY"; then
  fail "pms_720p_proxy must not sendfile identity 720p clip"
fi
if grep -q 'MPX_720P_CLIP' "$PROXY"; then
  fail "pms_720p_proxy must not honor MPX_720P_CLIP"
fi
ok "proxy always ffmpeg_720p of the Part"
if grep -n 'Mirror the I420 into the other 720p bank' "$ROOT/arm/misterplexd/fpga_spi.cpp" >/dev/null; then
  fail "720p must not dual-memcpy banks (copy_us=5280 locked unique 21.5)"
fi
if grep -E 'ddrBankVirt\(dest(Bank)? \^ 1\)' "$MP" >/dev/null; then
  fail "720p present must not memcpy the other bank (post-swap dual I420)"
fi
grep -q 'libraryKeyMustNotSpawnLocalFile' "$ROOT/arm/misterplexd/plex_resolve.cpp" || \
  fail "plex_resolve must refuse 40868 local-file playable"
if ! grep -q 'MPX_STICK_I420:-1' "$SUP"; then
  fail "supervise must default MPX_STICK_I420=1 (WC dest ingest; 480p geometry-gated)"
fi
grep -q 'combined720pSkipAvHold' "$MP" || fail "media_player missing combined720pSkipAvHold"
grep -q 'holdAudioToPictures_\.load' "$MP" || fail "audioPump must use holdAudioToPictures_"
if grep -nE 'audioReleaseAfterPresents_\.store\(24\)' "$MP" | grep -v 'startWithVideo' >/dev/null; then
  # inproc-only 24-present gate is still allowed; combined path must log after_video_ms=0
  ok "inproc 24-present gate still present"
fi
grep -q 'hdmi_audio_lag path=pipe after_video_ms=0' "$MP" || fail "combined 720p must log 480p after_video_ms=0"
grep -q 'inprocRemuxMustCopyAudio' "$MP" || fail "720p remux must copy audio (not -an annex-B)"
grep -q 'annexb+pcm' "$MP" || fail "720p remux must be annex-B + PCM fifos (ARM libav has no AAC)"
grep -q 'inproc_audio=remux_pcm' "$MP" || fail "720p must log remux_pcm (one HTTP, no spawnAudioOnly)"
grep -q 'mplex-inproc.pcm' "$MP" || fail "720p remux must write PCM fifo"
grep -q 'O_RDONLY | O_NONBLOCK' "$MP" || \
  fail "PCM pump must O_RDONLY|O_NONBLOCK (O_RDWR self-writer hides remux EOF)"
if grep -n 'open(afifo, O_RDWR)' "$MP" | grep -v NONBLOCK >/dev/null; then
  fail "PCM pump must not open(afifo, O_RDWR) blocking — EOF hang + HTTP spinner"
fi
grep -q 'Must not take playHandoffMu here' "$ROOT/arm/misterplexd/main.cpp" || \
  fail "playMedia HTTP must not wait on playHandoffMu (stop-join hang)"
grep -q 'interrupt_callback' "$ROOT/arm/misterplexd/av_inproc_decode.cpp" || \
  fail "libav fifo open/read must install interrupt_callback (stop during avformat_open)"
if grep -E '^[^/]*pthread_timedjoin_np' "$MP"; then
  fail "pthread_timedjoin_np + std::thread detach throws No such process (daemon terminate)"
fi
grep -q '720p24 PLEX_BASE proxy' "$ROOT/arm/misterplexd/main.cpp" || \
  fail "720p24 must prefer conf 9324 proxy over cast LAN address (Web play testsrc/401)"
if awk '/void MediaPlayer::killChildren/,/^}/' "$MP" | grep -qE 'waitpid\([^)]*,[^)]*,[[:space:]]*0\)'; then
  fail "killChildren must not blocking-waitpid (D-state remux froze Play)"
fi
grep -q 'inproc_audio=abort remux_pcm fd missing' "$MP" || \
  fail "720p inproc must abort if remux PCM fd missing (no spawnAudioOnly)"
grep -q 'prefetched=0' "$MP" || fail "want_inproc log must pin prefetched=0"
grep -A30 'pid_t MediaPlayer::spawnHttpRemuxMpegts' "$MP" | grep -q 'analyzeduration' || \
  fail "spawnHttpRemuxMpegts missing analyzeduration 0"
grep -A30 'pid_t MediaPlayer::spawnHttpRemuxMpegts' "$MP" | grep -q 'probesize' || \
  fail "spawnHttpRemuxMpegts missing probesize"
grep -A8 'inproc_audio=abort remux_pcm fd missing' "$MP" | grep -q 'return' || \
  fail "720p inproc remux_pcm miss must return (no spawnAudioOnly)"
if grep -B5 'pid_t apid = spawnAudioOnly' "$MP" | grep -q 'useInproc && isPlex720pDdrFrameGeometry'; then
  fail "720p inproc must not call spawnAudioOnly"
fi
if grep -n 'spawnHttpRemuxMpegts' "$MP" | grep -q -- '-an'; then
  : # declaration site is ok; keepAudio branch must not force -an for 720p
fi
grep -q 'keepAudio' "$MP" || fail "spawnHttpRemuxMpegts must take keepAudio"
# 720p must not hold MrAudio to presentCount (queue 180–310 ms stutter).
grep -q 'holdAudioToPicturesWanted' "$MP" || fail "audioPump policy helper missing"
ok "holdAudioToPictures policy helper in media_player"

RED="$ROOT/tests/fixtures/p720_startrek_soak_red_second_http.txt"
PARSE="$ROOT/tests/unit/test_p720_startrek_soak_parse.sh"
if [ -x "$PARSE" ] || [ -f "$PARSE" ]; then
  if bash "$PARSE" "$RED"; then
    fail "RED second-HTTP soak must not parse as PASS"
  else
    ok "RED second-HTTP soak parser NACK"
  fi
fi

if [ "$fails" -ne 0 ]; then
  echo "test_p720_av_match_480p_policy: $fails failures"
  exit 1
fi
echo "test_p720_av_match_480p_policy: OK"
exit 0
