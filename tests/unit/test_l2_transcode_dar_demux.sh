#!/usr/bin/env bash
# review 19:24 L2 rows at the FPGA demux (build/test_l2_transcode_dar_demux, the host build of
# arm/misterplexd/av_inproc_decode.cpp with source_aspect.hpp firstAuSourceAspect).
#  L1-L10: H264's ladder clips (review/crop/ladder/today, live proxy 00d07de0; MD5SUMS 294bee0f
#    checked; ladder-plxa-table 0cea5eee §2/§6/§7) played as transcodes with the movie's metadata
#    DAR. They send the stream DAR: 624 -> 13:10, 640 and the 320 crf240 rung -> 4:3. r3 sent the
#    metadata.
#  Mutant MUT_L2_METADATA_WINS (the metadata wins on a transcode: the double letterbox) must be
#    RED on every row whose metadata differs from the stream. Bug requires L1-L6/L8/L10 (marked
#    "req").
#  pmsA (712f28d3, no VUI) as a transcode with 16:9 metadata: no stream DAR, so r3's 16:9.
#  A direct play is untouched: pasp P1 (VUI 16:9) with 4:3 metadata still sends 4:3.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${L2_DEMUX_BIN:-$ROOT/build/test_l2_transcode_dar_demux}"
MUT="${L2_DEMUX_MUT_BIN:-$ROOT/build/test_l2_transcode_dar_demux_mut_metadata_wins}"
# MPX_LAB_FIXTURES is the lab clip directory. No default path: an unset or
# missing directory skips. A directory that is present with a bad MD5SUMS
# still fails.
REVIEW="${MPX_LAB_FIXTURES:-}"
if [[ -z "$REVIEW" || ! -d "$REVIEW" ]]; then
  echo "SKIP test_l2_transcode_dar_demux: MPX_LAB_FIXTURES is unset or not a directory"
  exit 0
fi
TODAY="$REVIEW/crop/ladder/today"
TREE="$(git -C "$ROOT" rev-parse --short=8 'HEAD^{tree}' 2>/dev/null || echo unknown)"
DIRTY="$(git -C "$ROOT" status --porcelain --untracked-files=no 2>/dev/null | wc -l)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fail=0
red=0
echo "tree $TREE (HEAD $(git -C "$ROOT" rev-parse --short=8 HEAD 2>/dev/null || echo -), dirty=$DIRTY)"
[[ -x "$BIN" && -x "$MUT" ]] || { echo "FAIL missing $BIN or $MUT"; exit 1; }

# The demux refuses PTS-less access units (as in prod); raw ES are stream-copied into MPEG-TS.
tsof() {
  local out="$T/$(basename "$1").ts"
  [[ -f "$out" ]] || ffmpeg -v error -y -fflags +genpts -framerate 24 -i "$1" -c copy -f mpegts "$out"
  echo "$out"
}
field() { sed -n "s/.* $1=\([^ ]*\).*/\1/p" <<<"$2"; }
ratio_eq() { # a:b c:d
  local a=${1%%:*} b=${1##*:} c=${2%%:*} d=${2##*:}
  (( a * d == c * b ))
}
row() { # label clip metaX metaY want req(0/1)
  local label=$1 clip=$2 mx=$3 my=$4 want=$5 req=$6 out sent r3 mout msent md5
  md5="$(md5sum "$clip" | cut -c1-8)"
  clip="$(tsof "$clip")"
  out="$("$BIN" "$clip" 1 "$mx" "$my" 2>&1 | grep -E '^L2 ' || true)"
  sent="$(field sent "$out")"
  r3="$(field r3 "$out")"
  if [[ "${sent%%/*}" == "$want" && "${sent##*/}" == demux ]]; then
    echo "ok   $label ($md5): sent=$sent r3=$r3"
  else
    echo "FAIL $label ($md5): ${out:-no L2 line} (want sent=$want/demux)"
    fail=1
  fi
  mout="$("$MUT" "$clip" 1 "$mx" "$my" 2>&1 | grep -E '^L2 ' || true)"
  msent="$(field sent "$mout")"
  if ratio_eq "$mx:$my" "$want"; then
    [[ "${msent%%/*}" == "$mx:$my" ]] && echo "n/a  $label: mutant sends the same $mx:$my (metadata equals the stream)" \
      || { echo "FAIL $label: mutant sent ${msent:-error}"; fail=1; }
  elif [[ "${msent%%/*}" == "$mx:$my" ]]; then
    echo "RED  $label: MUT_L2_METADATA_WINS sends $mx:$my (double letterbox), want $want$([[ $req == 1 ]] && echo ' [req]')"
    red=$((red + 1))
  else
    echo "FAIL $label: mutant sent ${msent:-error}, expected the metadata $mx:$my"
    fail=1
  fi
}

if [[ -f "$TODAY/MD5SUMS" ]] && (cd "$TODAY" && md5sum -c --quiet MD5SUMS); then
  echo "today/: MD5SUMS OK ($(md5sum "$TODAY/MD5SUMS" | cut -c1-8)), ladder-goldens.json $(md5sum "$TODAY/ladder-goldens.json" | cut -c1-8)"
  row "L1 624 rung, 16:9 title"            "$TODAY/ladder_s169_624x480.264" 16 9    13:10 1
  row "L2 640 rung, 16:9 title"            "$TODAY/ladder_s169_640x480.264" 16 9    4:3   1
  row "L3 624 rung, 2.35 title (47:20)"    "$TODAY/ladder_s235_624x480.264" 47 20   13:10 1
  row "L4 640 rung, 2.35 title (47:20)"    "$TODAY/ladder_s235_640x480.264" 47 20   4:3   1
  row "L5 624 rung, 2.39 title (239:100)"  "$TODAY/ladder_s239_624x480.264" 239 100 13:10 1
  row "L5 640 rung, 2.39 title (239:100)"  "$TODAY/ladder_s239_640x480.264" 239 100 4:3   1
  row "L6 624 rung, 4:3 title"             "$TODAY/ladder_s43_624x480.264"  4 3     13:10 1
  row "L7 640 rung, 4:3 title"             "$TODAY/ladder_s43_640x480.264"  4 3     4:3   0
  row "L8 624 rung, 16:9 anamorphic title" "$TODAY/ladder_a169_624x480.264" 16 9    13:10 1
  row "L8 640 rung, 16:9 anamorphic title" "$TODAY/ladder_a169_640x480.264" 16 9    4:3   1
  row "L9 624 rung, 4:3 anamorphic title"  "$TODAY/ladder_a43_624x480.264"  4 3     13:10 0
  row "L9 640 rung, 4:3 anamorphic title"  "$TODAY/ladder_a43_640x480.264"  4 3     4:3   0
  # L10: the 240p rung the daemon requests (w=320&h=240&hq=1, no allidr: the proxy's crf240
  # branch, plex_resolve.cpp:2274-2279; table 0cea5eee §7). s43/a43 are the controls.
  row "L10 320 crf rung, 16:9 title"           "$TODAY/ladder_s169_320x240crf.264" 16 9    4:3 1
  row "L10 320 crf rung, 2.35 title"           "$TODAY/ladder_s235_320x240crf.264" 47 20   4:3 1
  row "L10 320 crf rung, 2.39 title"           "$TODAY/ladder_s239_320x240crf.264" 239 100 4:3 1
  row "L10 320 crf rung, 16:9 anamorphic"      "$TODAY/ladder_a169_320x240crf.264" 16 9    4:3 1
  row "L10 320 crf rung, 4:3 title (control)"  "$TODAY/ladder_s43_320x240crf.264"  4 3     4:3 0
  row "L10 320 crf rung, 4:3 anamorphic (ctl)" "$TODAY/ladder_a43_320x240crf.264"  4 3     4:3 0
  row "u169 624 rung (source SAR unset)"   "$TODAY/ladder_u169_624x480.264" 16 9    13:10 0
  row "u169 640 rung (source SAR unset)"   "$TODAY/ladder_u169_640x480.264" 16 9    4:3   0
  row "u169 320 crf rung (= s169 crf)"     "$TODAY/ladder_u169_320x240crf.264" 16 9 4:3   0
else
  echo "FAIL $TODAY missing or MD5SUMS mismatch"
  fail=1
fi

PMSA="$REVIEW/crop/goldens/clips/pmsA640x360_first60.h264"
if [[ -f "$PMSA" && "$(md5sum "$PMSA" | cut -c1-8)" == 712f28d3 ]]; then
  out="$("$BIN" "$(tsof "$PMSA")" 1 16 9 2>&1 | grep -E '^L2 ' || true)"
  [[ "$(field sent "$out")" == 16:9/provided ]] && echo "ok   pmsA (712f28d3) transcode, no stream DAR, 16:9 metadata: $out" \
    || { echo "FAIL pmsA: ${out:-none} (want sent=16:9/provided)"; fail=1; }
else
  echo "FAIL pmsA: $PMSA missing or not 712f28d3"
  fail=1
fi
P1="$REVIEW/crop/pasp/clips/P1_vui_only.ts"
if [[ -f "$P1" ]]; then
  out="$("$BIN" "$P1" 0 4 3 2>&1 | grep -E '^L2 ' || true)"
  [[ "$(field sent "$out")" == 4:3/provided && "$(field r3 "$out")" == 4:3/provided ]] \
    && echo "ok   direct pasp P1 (VUI 16:9), 4:3 metadata: unchanged from r3: $out" \
    || { echo "FAIL direct P1: ${out:-none} (want sent=r3=4:3/provided)"; fail=1; }
fi

if [[ "$fail" != 0 ]]; then
  echo "test_l2_transcode_dar_demux: FAIL (tree $TREE)"
  exit 1
fi
echo "test_l2_transcode_dar_demux: OK (tree $TREE; mutant RED on $red rows)"
