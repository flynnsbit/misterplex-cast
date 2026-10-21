#!/usr/bin/env bash
# GCAST host tests: every gcast unit test, each mutant must fail, a short sanitizer fuzz run.
# usage: tests/gcast/run_gcast_tests.sh [ROOT] [OUT]   (make gcast-tests)
set -u
ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}; OUT=${2:-$ROOT/build/gcast-tests}; CXX=${CXX:-g++}
mkdir -p "$OUT"; cd "$ROOT"; bad=0
FL="-std=c++17 -O1 -Iarm/misterplexd -Ihost -pthread"
run(){ # name src expect(pass|fail) [defines...]
  local n=$1 s=$2 e=$3; shift 3
  if ! $CXX $FL "$@" "$s" ${RUN_LIBS:-} -o "$OUT/$n" 2>"$OUT/$n.build.log"; then echo "BUILD-FAIL $n"; bad=1; return; fi
  "$OUT/$n" >"$OUT/$n.log" 2>&1; local rc=$?
  if { [ $e = pass ] && [ $rc = 0 ]; } || { [ $e = fail ] && [ $rc != 0 ]; }; then echo "ok   $n ($(tail -n 1 "$OUT/$n.log"))"
  else echo "BAD  $n expected $e rc=$rc ($(tail -n 1 "$OUT/$n.log"))"; bad=1; fi
}
run core tests/unit/test_gcast_core.cpp pass
run yt_skip_stop tests/unit/test_yt_skip_stop.cpp pass
run yt_skip_stop.MUT_CAST_STOP_ANY_NONE tests/unit/test_yt_skip_stop.cpp fail -DMUT_CAST_STOP_ANY_NONE
run yt_skip_stop.MUT_SEEK_ALWAYS_TEARDOWN tests/unit/test_yt_skip_stop.cpp fail -DMUT_SEEK_ALWAYS_TEARDOWN
run lounge_ready tests/unit/test_lounge_ready.cpp pass
run core.MUT_CR_YT_ANYID tests/unit/test_gcast_core.cpp fail -DMUT_CR_YT_ANYID
run core.MUT_CR_NO_STANDALONE tests/unit/test_gcast_core.cpp fail -DMUT_CR_NO_STANDALONE
run core.MUT_CR_YT_OB_LADDER tests/unit/test_gcast_core.cpp fail -DMUT_CR_YT_OB_LADDER
run core.MUT_CR_ONE_RECV tests/unit/test_gcast_core.cpp fail -DMUT_CR_ONE_RECV
run lounge tests/unit/test_gcast_lounge.cpp pass
for m in MUT_LP_NO_CAP MUT_LP_NO_DEDUP MUT_LP_NO_VIDCHECK; do run lounge.$m tests/unit/test_gcast_lounge.cpp fail -D$m; done
run dial tests/unit/test_gcast_dial.cpp pass
for m in MUT_DIAL_ANY_ST MUT_DIAL_NO_BODYCAP; do run dial.$m tests/unit/test_gcast_dial.cpp fail -D$m; done
run http tests/unit/test_gcast_http.cpp pass
run http.MUT_HTTP_NO_CHUNKCAP tests/unit/test_gcast_http.cpp fail -DMUT_HTTP_NO_CHUNKCAP
run osd tests/unit/test_gcast_osd.cpp pass
for m in MUT_GOSD_NO_CAP MUT_GOSD_READY_FLAGS_OFF; do run osd.$m tests/unit/test_gcast_osd.cpp fail -D$m; done
run caps tests/unit/test_gcast_caps.cpp pass
for m in MUT_CAPS_NO_PAIRCAP MUT_CAPS_NO_DEADLINE; do run caps.$m tests/unit/test_gcast_caps.cpp fail -D$m; done
run transcode_offset tests/unit/test_transcode_offset.cpp pass arm/misterplexd/plex_resolve.cpp
run transcode_offset.MUT_TO_NO_YTREMUX tests/unit/test_transcode_offset.cpp fail -DMUT_TO_NO_YTREMUX arm/misterplexd/plex_resolve.cpp
run stalled_pic_watchdog tests/unit/test_stalled_pic_watchdog.cpp pass
run stalled_pic_watchdog.MUT_SPW_COUNT_PAUSED tests/unit/test_stalled_pic_watchdog.cpp fail -DMUT_SPW_COUNT_PAUSED
run dns_label tests/unit/test_gcast_dns_label.cpp pass
for m in MUT_DNSL_KEEP_DOTS MUT_DNSL_NO_CAP; do run dns_label.$m tests/unit/test_gcast_dns_label.cpp fail -D$m; done
run lab_endpoints tests/unit/test_lab_endpoints.cpp pass
for m in MUT_LE_ANY_HOST MUT_LE_CONF_LADDER MUT_LE_XCODE_EMPTY_FALLS_THROUGH; do run lab_endpoints.$m tests/unit/test_lab_endpoints.cpp fail -D$m; done
PUI="-DMPX_PLEXUI_UNIT_TEST -pthread -Ihost -Ihost/libmisterplex/plexui $(for f in controller data api_cache http font images disk_cache render; do printf "host/libmisterplex/plexui/plexui_%s.cpp " $f; done)"   # = Makefile PLEXUI_SRC
run plexui tests/unit/test_plexui.cpp pass $PUI
run plexui.MUT_PLEXUI_IGNORE_BROWSE_FLAGS tests/unit/test_plexui.cpp fail -DMUT_PLEXUI_IGNORE_BROWSE_FLAGS $PUI
run plexui.MUT_PLEXUI_OPEN_KEEPS_PLAY tests/unit/test_plexui.cpp fail -DMUT_PLEXUI_OPEN_KEEPS_PLAY $PUI
run core_id_select tests/unit/test_core_id_select.cpp pass -Ihost
run core_id_select.MUT_MRT_IGNORE_PROGRESSIVE tests/unit/test_core_id_select.cpp fail -DMUT_MRT_IGNORE_PROGRESSIVE -Ihost
run mem_stat tests/unit/test_mem_stat.cpp pass

run airplay_advertise tests/unit/test_airplay_advertise.cpp pass -DMPX_SOURCE_ROOT='"'"$ROOT"'"'
run airplay_fe_safety tests/unit/test_airplay_fe_safety.cpp pass
run airplay_fe_safety.MUT_AP_NO_VOLUME_WIRE tests/unit/test_airplay_fe_safety.cpp fail -DMUT_AP_NO_VOLUME_WIRE
run airplay_fe_safety.MUT_AP_MIRROR_BIT_ON tests/unit/test_airplay_fe_safety.cpp fail -DMUT_AP_MIRROR_BIT_ON
# AirPlay RAOP audio (host libavcodec aac + alac + swresample)
AVL=$(pkg-config --cflags --libs libavcodec libswresample libavutil 2>/dev/null)
if [ -n "$AVL" ]; then
  export RUN_LIBS="$AVL"
  run raop_audio tests/unit/test_raop_audio.cpp pass -DMPX_HAVE_LIBAV
  for m in MUT_RA_VOL_IGNORE MUT_RA_FANOUT_NO_CAP MUT_RA_NO_FLUSH MUT_RA_NO_RESAMPLE MUT_CR_RAOP_ANYWAV; do
    run raop_audio.$m tests/unit/test_raop_audio.cpp fail -DMPX_HAVE_LIBAV -D$m; done
  unset RUN_LIBS
else echo "BAD  raop_audio: host libavcodec/libswresample dev not found (pkg-config)"; bad=1; fi
if python3 tools/gcast/test_gcast_xcode.py >"$OUT/xcode.log" 2>&1; then echo "ok   gcast_xcode ($(tail -n 1 "$OUT/xcode.log"))"; else echo "BAD  gcast_xcode"; bad=1; fi
if python3 tools/gcast/test_gcast_xcode_yt.py >"$OUT/xcode_yt.log" 2>&1; then echo "ok   gcast_xcode_yt ($(tail -n 1 "$OUT/xcode_yt.log"))"; else echo "BAD  gcast_xcode_yt"; bad=1; fi
if GCAST_MUT=nopipenull python3 tools/gcast/test_gcast_xcode_yt.py >"$OUT/xcode_yt.nopipenull.log" 2>&1; then echo "BAD  gcast_xcode_yt.nopipenull survived"; bad=1; else echo "ok   gcast_xcode_yt.nopipenull ($(tail -n 1 "$OUT/xcode_yt.nopipenull.log"))"; fi
if GCAST_MUT=aonly python3 tools/gcast/test_gcast_xcode.py >"$OUT/xcode.aonly.log" 2>&1; then echo "BAD  gcast_xcode.aonly survived"; bad=1; else echo "ok   gcast_xcode.aonly ($(tail -n 1 "$OUT/xcode.aonly.log"))"; fi
if GCAST_MUT=hlslevel python3 tools/gcast/test_gcast_xcode.py >"$OUT/xcode.hlslevel.log" 2>&1; then echo "BAD  gcast_xcode.hlslevel survived"; bad=1; else echo "ok   gcast_xcode.hlslevel ($(tail -n 1 "$OUT/xcode.hlslevel.log"))"; fi
run mem_stat.MUT_MEMSTAT_NO_UPPER tests/unit/test_mem_stat.cpp fail -DMUT_MEMSTAT_NO_UPPER
run protected tests/unit/test_gcast_protected.cpp pass   # FIELD-1018 F4
for m in MUT_F4_NO_URL MUT_F4_NO_SAMPLE_AES; do run protected.$m tests/unit/test_gcast_protected.cpp fail -D$m; done
FUZZ_ITERS=${FUZZ_ITERS:-20000} run fuzz tests/gcast/fuzz_parsers.cpp pass -g -fsanitize=address,undefined -fno-sanitize-recover=all
run fuzz.MUT_LP_NO_VIDCHECK tests/gcast/fuzz_parsers.cpp fail -DMUT_LP_NO_VIDCHECK
[ $bad = 0 ] && echo "gcast-tests: ALL OK" || echo "gcast-tests: FAILURES"
exit $bad
