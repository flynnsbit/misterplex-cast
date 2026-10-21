#!/usr/bin/env bash
# Y/C fit recipe for rel/yc-plexob-1019. Quartus is not invoked by `check`.
# Inputs are the d17b8df2 set (E1-1 emu, A_E2 qsf settings, stock ascal, SEED 10
# after the re-fit in fit/yc/TIMING-CLOSURE.md) plus the Y/C delta: MISTER_DISABLE_YC unset, BUILD_ID
# 0x0B59C11A (not the dead 0x0B59C119 stamp), word 87 [3:0], ring_in, and the ring_stat 88-95 walk.
# sys/ stays the eac1064 pin. This script does not patch the scaler.
#
#   launch_yc.sh check     # md5 and source guards, no Quartus
#   launch_yc.sh fit       # isolated copy, then one Quartus compile (fit host only)
set -euo pipefail
MODE="${1:-check}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$ROOT"
EACSHA=eac10641c5af0b808d332bb61344ee2dfdba4271
FIT_NAME=plexob-yc-0b59c11a
QSF8=55648a13
SEED_PIN=10
EMU8=f6019d72
RING_IN8=e7a9e890
RING8=1a2d93aa

pin8() {
  local got
  got=$(md5sum "$1" | cut -c1-8)
  [ "$got" = "$2" ] || { echo "ABORT md5 $1 got $got want $2" >&2; exit 1; }
}

md5full() { md5sum "$1" | cut -c1-32; }

# Copy the staged bitstream into releases/ only when rc is 0 and the gate
# printed an exact GATE_PASS=1 line. The rc test is before the copy. On any
# other result the same-named .rbf and its .rbf.json manifest are removed
# from releases/.
yc_publish_rbf() {
  local staged="$1" rel="$2" gate="$3" rc="${4:-0}" base dest manifest
  base=$(basename "$staged")
  dest="$rel/$base"
  manifest="${dest}.json"
  if [ "$staged" = "$dest" ]; then
    echo "ABORT staged rbf is in releases/" >&2
    return 1
  fi
  rm -f "$dest"
  if [ "$rc" = 0 ] && printf '%s\n' "$gate" | grep -qx 'GATE_PASS=1'; then
    mkdir -p "$rel"
    cp -f "$staged" "$dest"
    echo "RBF_PATH=$dest"
    return 0
  fi
  rm -f "$manifest"
  return 1
}

if [ "$MODE" = "publish" ]; then
  if [ -z "${2:-}" ] || [ -z "${3:-}" ]; then
    echo "usage: launch_yc.sh publish STAGED_RBF RELEASES_DIR [RC]" >&2
    exit 2
  fi
  rc="${4:-0}"
  if yc_publish_rbf "$2" "$3" "$(cat)" "$rc"; then
    exit 0
  fi
  exit 1
fi

# Pins and source guards for one tree (the git checkout, or the isolated copy).
yc_guard() {
  local B="$1"
  pin8 "$B/vendor/H264.qsf" "$QSF8"
  pin8 "$B/vendor/h264/emu.sv" "$EMU8"
  pin8 "$B/vendor/h264/cdc.sv" 6587ec58
  pin8 "$B/vendor/h264/dec_stub.sv" 5538da65
  pin8 "$B/vendor/h264/disp_reader.sv" 570492fe
  pin8 "$B/vendor/h264/h264.sdc" f7df5d82
  pin8 "$B/vendor/h264/ring_in.sv" "$RING_IN8"
  pin8 "$B/vendor/h264/ring_stat.sv" "$RING8"
  pin8 "$B/fit/variants/q1b/mpx_cdc_hold_targeted.sdc" 24c10f8f
  if [ -f "$B/vendor/sys/ascal.vhd" ]; then
    pin8 "$B/vendor/sys/ascal.vhd" 1b17376e
  else
    echo "ascal.vhd not in $B (stock copy arrives from sync_vendor.sh)"
  fi
  local E="$B/vendor/h264/emu.sv"
  local Q="$B/vendor/H264.qsf"
  local R="$B/vendor/h264/ring_stat.sv"
  grep -q 'cdc_snap #(.W(26)) ar_xing' "$E" && grep -q 'if (ar_tick && ar_prim)' "$E" || {
    echo "ABORT E1-1 missing" >&2; exit 1; }
  grep -q "st_sys_ix == 7'd87" "$E" || { echo "ABORT word 87 arm missing" >&2; exit 1; }
  grep -q "st_sys_ix == 7'd88) ? {32'h4D505842" "$E" || { echo "ABORT word 88 MPXB splice missing" >&2; exit 1; }
  grep -q '40..47 -> 88..95' "$R" && grep -q "st_free >= 7'd56" "$R" || {
    echo "ABORT ring_stat 88-95 walk missing" >&2; exit 1; }
  grep -q "MPX_PLEXOB_BUILD_ID=32'h0B59C11A" "$Q" || { echo "ABORT BUILD_ID" >&2; exit 1; }
  grep -q 'VERILOG_MACRO "MISTER_DISABLE_YC' "$Q" && { echo "ABORT MISTER_DISABLE_YC is set" >&2; exit 1; }
  grep -q 'MISTER_DISABLE_ADAPTIVE=1' "$Q" || { echo "ABORT adaptive macro missing" >&2; exit 1; }
  grep -q 'FITTER_AGGRESSIVE_ROUTABILITY' "$Q" && { echo "ABORT aggressive routability" >&2; exit 1; }
  grep -q 'OPTIMIZE_POWER_DURING_FITTING OFF' "$Q" || { echo "ABORT TPLFIT Block A missing" >&2; exit 1; }
  [ "$(grep -c '^set_instance_assignment -name ALLOW_SYNCH_CTRL_USAGE OFF -to "ascal:ascal|o_[vh]_poly_pix\*"' "$Q")" = 2 ] || {
    echo "ABORT E2 lines" >&2; exit 1; }
  local q
  q=$(grep -E '^set_global_assignment -name SEED ' "$Q" | awk '{print $4}')
  [ "$q" = "$SEED_PIN" ] || { echo "ABORT SEED $q" >&2; exit 1; }
  grep -q 'LL_ENABLED' "$Q" && { echo "ABORT LogicLock" >&2; exit 1; }
  if grep -q '0B593F92' "$Q"; then echo "ABORT historical BUILD_ID in the fit qsf" >&2; exit 1; fi
  if grep -q '0B59C119' "$Q"; then echo "ABORT dead BUILD_ID 0x0B59C119 in the fit qsf" >&2; exit 1; fi
  local b
  b=$(grep -c '^set_location_assignment LAB_X' "$Q" || true)
  [ "${b:-0}" = 0 ] || { echo "ABORT Block B pins" >&2; exit 1; }
}

yc_guard "$ROOT"
if [ -d vendor/sys ] && [ -n "${OWENB_VENDOR_ROOT:-}" ]; then
  diff -rq vendor/sys "$OWENB_VENDOR_ROOT/sys" >/dev/null || {
    echo "ABORT vendor/sys differs from the eac1064 pin" >&2
    exit 1
  }
fi

# sync_vendor.sh must exclude every committed fork, or it would overwrite this qsf.
REPO="$(cd "$ROOT/../../.." && pwd)"
tracked=$(git -C "$REPO" ls-files fpga/owenb-combo-s9/PlexOB_480p/vendor)
sync=$(cat sync_vendor.sh)
while IFS= read -r line; do
  [ -n "$line" ] || continue
  rel=${line#*vendor/}
  grep -q "$rel" <<<"$sync" || { echo "ABORT sync does not exclude $rel" >&2; exit 1; }
done <<<"$tracked"

# Fit-mode contract. check enforces it so a no-arg docker launch cannot come back.
grep -q -- '--compile' "$SCRIPT" && grep -q -- '--name' "$SCRIPT" && grep -q "$FIT_NAME" "$SCRIPT" || {
  echo "ABORT fit leg does not pass --compile --name $FIT_NAME" >&2; exit 1; }
grep -q 'NOTIFY_SILENT' "$SCRIPT" || { echo "ABORT NOTIFY_SILENT missing" >&2; exit 1; }
grep -q "$EACSHA" "$SCRIPT" || { echo "ABORT eac1064 pin missing" >&2; exit 1; }
grep -q 'RESULT.env' "$SCRIPT" || { echo "ABORT RESULT.env missing" >&2; exit 1; }
grep -q 'sync_vendor.sh' "$SCRIPT" || { echo "ABORT fit does not run sync_vendor.sh" >&2; exit 1; }
grep -q 'output_files' "$SCRIPT" || { echo "ABORT stale output_files are not removed" >&2; exit 1; }
grep -q -- '-e NOTIFY_SILENT' run_quartus_docker.sh || {
  echo "ABORT run_quartus_docker.sh does not pass NOTIFY_SILENT" >&2; exit 1; }
grep -q -- '-v /build' run_quartus_docker.sh || {
  echo "ABORT run_quartus_docker.sh does not mount /build" >&2; exit 1; }
grep -q 'tools/.notify_url' sync_vendor.sh || {
  echo "ABORT sync_vendor.sh does not exclude tools/.notify_url" >&2; exit 1; }
grep -q 'status --porcelain --untracked-files=no' "$SCRIPT" || {
  echo "ABORT fit does not require a clean owenb tree" >&2; exit 1; }
grep -E -q 'status --porcelain[[:space:]]+sys([^[:alnum:]_]|$)' "$SCRIPT" && {
  echo "ABORT eac clean check is still limited to sys/" >&2; exit 1; }
grep -q 'vendor/tools/.notify_url' "$SCRIPT" || {
  echo "ABORT fit copy can still keep tools/.notify_url" >&2; exit 1; }
grep -q 'FIT/vendor/db' "$SCRIPT" || { echo "ABORT fit does not remove vendor/db" >&2; exit 1; }
grep -q 'timing_gate.py' "$SCRIPT" || { echo "ABORT timing gate missing" >&2; exit 1; }
grep -q 'FIT MARGINAL' "$SCRIPT" || { echo "ABORT a missed timing bar is not marked" >&2; exit 1; }
grep -q 'ring_in:ring_in_inst' "$SCRIPT" || { echo "ABORT map hierarchy gate missing" >&2; exit 1; }
grep -q -- '--map' "$SCRIPT" || { echo "ABORT map report is not passed to the gate" >&2; exit 1; }
grep -q 'GATE_PASS=1' "$SCRIPT" || { echo "ABORT GATE_PASS missing" >&2; exit 1; }
grep -q 'yc_publish_rbf' "$SCRIPT" || { echo "ABORT releases copy is not gated on GATE_PASS" >&2; exit 1; }
grep -q 'rbf.json' "$SCRIPT" || { echo "ABORT a failed gate can leave the rbf.json manifest" >&2; exit 1; }
grep -F -q '[ "$gate_rc" = 0 ]' "$SCRIPT" || { echo "ABORT gate rc is not checked before publish" >&2; exit 1; }
grep -F -q 'yc_publish_rbf "$staged" "$rel" "$gate_out" "$gate_rc"' "$SCRIPT" || {
  echo "ABORT publish does not receive the gate rc" >&2; exit 1; }
grep -q 'h264/ring_in.sv' "$SCRIPT" || { echo "ABORT ring_in.sv pin missing" >&2; exit 1; }

echo "YC recipe check OK sha=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo uncommitted) qsf=$QSF8 emu=$EMU8 ring_in=$RING_IN8 ring=$RING8 seed=$SEED_PIN"

if [ "$MODE" = "check" ]; then
  exit 0
fi
if [ "$MODE" != "fit" ]; then
  echo "usage: launch_yc.sh check|fit" >&2
  exit 2
fi
if [ "${YC_FIT_CONFIRM:-}" != 1 ]; then
  echo "ABORT fit not confirmed (set YC_FIT_CONFIRM=1 on the fit host). check already passed." >&2
  exit 2
fi

EAC="${OWENB_VENDOR_ROOT:?set OWENB_VENDOR_ROOT to an owenb321 MiSTer_h264_decoder checkout at eac1064}"
# F1: unpinned inputs (mpx_*.sv) are part of the compile. Refuse a dirty checkout.
if [ -n "$(git -C "$REPO" status --porcelain --untracked-files=no)" ]; then
  echo "ABORT owenb tree has uncommitted changes" >&2
  exit 1
fi
# F2: the whole upstream tree, not only sys/. Ignored Quartus dirs are not in git status.
[ "$(git -C "$EAC" rev-parse HEAD)" = "$EACSHA" ] && [ -z "$(git -C "$EAC" status --porcelain)" ] || {
  echo "ABORT eac1064 pin moved or the upstream tree is dirty ($EAC)" >&2
  exit 1
}
for d in db incremental_db output_files; do
  if [ -e "$EAC/$d" ]; then
    echo "ABORT stale $EAC/$d" >&2
    exit 1
  fi
done
grep -q "^new pass=1000 fail=0 of 1000" "${E1_TB_SUMMARY:?set E1_TB_SUMMARY to the E1-1 TB summary.txt}" || {
  echo "ABORT E1-1 TB not 1000/1000" >&2
  exit 1
}

FIT="${YC_FIT_ROOT:?set YC_FIT_ROOT to the fit copy}"
mkdir -p "$(dirname "$FIT")"
rsync -a --delete \
  --exclude 'vendor/db' --exclude 'vendor/output_files' --exclude 'vendor/incremental_db' \
  --exclude 'vendor/releases' --exclude 'fit.log' \
  --exclude 'vendor/tools/.notify_url' \
  "$ROOT/" "$FIT/"
OWENB_VENDOR_ROOT="$EAC" "$FIT/sync_vendor.sh"
rm -f "$FIT/vendor/tools/.notify_url"
diff -rq "$FIT/vendor/sys" "$EAC/sys" >/dev/null || {
  echo "ABORT vendor/sys differs from eac1064 sys/" >&2
  exit 1
}
[ -f "$FIT/vendor/sys/ascal.vhd" ] || { echo "ABORT stock ascal missing after sync" >&2; exit 1; }
yc_guard "$FIT"
grep -q '^derive_clock_uncertainty' "$FIT/vendor/sys/sys_top.sdc" || {
  echo "ABORT derive_clock_uncertainty" >&2
  exit 1
}
if grep -rhE '^\s*set_clock_uncertainty' "$FIT/vendor" "$FIT/fit" --include='*.sdc' | grep -v -- '-add' | grep -q .; then
  echo "ABORT non -add set_clock_uncertainty" >&2
  exit 1
fi
rm -rf "$FIT/vendor/db" "$FIT/vendor/output_files" "$FIT/vendor/incremental_db"

while :; do
  n=$(docker ps --format '{{.Names}}' | grep -c '^quartus_' || true)
  h=$(pgrep -c quartus_fit || true)
  [ "${n:-0}" -lt 7 ] && [ "${h:-0}" -lt 7 ] && break
  echo "$(date '+%T %Z') WAIT cap 7 (docker=$n host_fit=$h)"
  sleep $((50 + RANDOM % 20))
done

{
  echo "VARIANT=YC_0B59C11A"
  echo "COMMIT=$(git -C "$REPO" rev-parse HEAD)"
  echo "EAC_SHA=$EACSHA"
  echo "SYS_CLEAN=1"
  echo "SYS_EQ_EAC1064=1"
  echo "OWENB_CLEAN=1"
  echo "EAC_CLEAN=1"
  echo "SEED=$SEED_PIN"
  echo "NOTIFY_SILENT=1"
  echo "MD5_QSF=$(md5full "$FIT/vendor/H264.qsf")"
  echo "MD5_EMU=$(md5full "$FIT/vendor/h264/emu.sv")"
  echo "MD5_RING_IN=$(md5full "$FIT/vendor/h264/ring_in.sv")"
  echo "MD5_RING_STAT=$(md5full "$FIT/vendor/h264/ring_stat.sv")"
  echo "MD5_ASCAL=$(md5full "$FIT/vendor/sys/ascal.vhd")"
  echo "LAUNCH=$(date '+%F %T %Z') slot=$((n + 1))/7"
} > "$FIT/RESULT.env"

export NOTIFY_SILENT=1
echo "FIT compile $FIT_NAME via run_quartus_docker.sh --compile (one fit at a time)"
set +o pipefail
bash "$FIT/run_quartus_docker.sh" --compile --name "$FIT_NAME" | tee "$FIT/fit.log"
compile_rc=${PIPESTATUS[0]}
set -o pipefail
if [ "$compile_rc" != 0 ]; then
  echo "ABORT compile rc=$compile_rc" >&2
  exit "$compile_rc"
fi

echo "QSF_MD5_AFTER=$(md5full "$FIT/vendor/H264.qsf")" >> "$FIT/RESULT.env"
pin8 "$FIT/vendor/H264.qsf" "$QSF8"
sof="$FIT/vendor/output_files/H264.sof"
[ -f "$sof" ] || { echo "ABORT H264.sof missing after compile" >&2; exit 1; }
rel="$FIT/vendor/releases"
# build_release drops the rbf in releases/ before this gate. Park it in
# output_files and copy it back only when the gate rc is 0 and GATE_PASS=1.
set +o pipefail
produced=$(ls -1t "$rel"/*"${FIT_NAME}"*.rbf 2>/dev/null | head -1 || true)
set -o pipefail
[ -n "$produced" ] && [ -f "$produced" ] || { echo "ABORT rbf missing after compile" >&2; exit 1; }
staged="$FIT/vendor/output_files/$(basename "$produced")"
mv -f "$produced" "$staged"
{
  echo "MD5_SOF=$(md5full "$sof")"
  echo "MD5_RBF=$(md5full "$staged")"
} >> "$FIT/RESULT.env"
# F3: pll_hdmi setup >= -0.3 ns, every other clock >= 0, and the entity table
# has ring_in:ring_in_inst with non-zero ALMs. Slacks land in RESULT.env either
# way. GATE_PASS=1 only when timing and the map both pass. A miss does not
# print FIT done and does not leave the rbf or its rbf.json manifest in releases/.
map="$FIT/vendor/output_files/H264.map.rpt"
set +e
gate_out=$(python3 "$(dirname "$SCRIPT")/timing_gate.py" --sta "$FIT/vendor/output_files/H264.sta.rpt" --log "$FIT/fit.log" --rbf "$staged" --map "$map")
gate_rc=$?
set -e
if printf '%s\n' "$gate_out" | grep -qx 'MAP_RING_IN=0' && printf '%s\n' "$gate_out" | grep -qx 'TIMING_PASS=1'; then
  yc_publish_rbf "$staged" "$rel" "$gate_out" 1 >/dev/null || true
  echo "ABORT map failure still set TIMING_PASS=1" >&2
  exit 1
fi
printf '%s\n' "$gate_out" >> "$FIT/RESULT.env"
# Check the exit code before publishing. Copy only when rc is 0 and the gate
# printed an exact GATE_PASS=1 line; otherwise drop the rbf and the rbf.json.
if [ "$gate_rc" = 0 ] && printf '%s\n' "$gate_out" | grep -qx 'GATE_PASS=1'; then
  if rbf_line=$(yc_publish_rbf "$staged" "$rel" "$gate_out" "$gate_rc"); then
    printf '%s\n' "$rbf_line" >> "$FIT/RESULT.env"
    rbf="${rbf_line#RBF_PATH=}"
    echo "FIT done qsf=$QSF8 sof=$(md5full "$sof") rbf=$(md5full "$rbf")"
    exit 0
  fi
else
  yc_publish_rbf "$staged" "$rel" "$gate_out" "$gate_rc" >/dev/null || true
fi
if [ "$gate_rc" != 0 ] && printf '%s\n' "$gate_out" | grep -qx 'GATE_PASS=1'; then
  echo "ABORT GATE_PASS=1 with rc=$gate_rc" >&2
  exit 1
fi
if printf '%s\n' "$gate_out" | grep -qx 'MAP_RING_IN=0'; then
  echo "ABORT map entity table missing ring_in:ring_in_inst with non-zero ALMs" >&2
else
  echo "FIT MARGINAL rc=$gate_rc qsf=$QSF8" >&2
fi
[ "$gate_rc" != 0 ] || gate_rc=1
exit "$gate_rc"
