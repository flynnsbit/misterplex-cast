#!/usr/bin/env bash
# HISTORICAL: not the Y/C fit. Pins BUILD_ID 0x0B593F92, emu md5 10ff9922, and MISTER_DISABLE_YC via A_E2.
# The Y/C fit is fit/yc/launch_yc.sh. This launcher must not be used for 0x0B59C11A.
# COMBO: tplfit A-only tree (owenb-par-tplfit-launch.sh A: qsfperf d0a5a5d03 + tplfit H264.qsf.A @66f04727,
# the 4c09f6c9 s3 config) + E1-1 emu.sv (0c10b13d7, incl. ar_prim) + E2 (ALLOW_SYNCH_CTRL_USAGE OFF on ascal
# o_v/o_h_poly_pix*) + F1030 stamp (QSF BUILD_ID 0x0B593F92 + emu word-88 MPXB splice + ring_stat 88-95 walk).
# Guards = tplfit launcher's (stock ascal 1b17376e, vendor/sys == eac1064 sys/, pinned md5s, no LogicLock,
# no aggr routability, no Block B pins, cap 7) + E1-1 / E2 / F1030 / uncertainty checks. Waits for a slot.
# NOTIFY_SILENT=1. Never programs, loads or glasses anything. Usage: E1_COMBO_SHA=<sha> launch_combo.sh <seed>
set -euo pipefail
SEED="${1:?seed}"; CSHA="${E1_COMBO_SHA:?set E1_COMBO_SHA}"
CWT="${COMBO_WT:?set COMBO_WT}"; P=fpga/PlexOB_480p
SRC="${OWENB_QSFPERF_WT:?set OWENB_QSFPERF_WT}/fpga/PlexOB_480p"
WT="${TPLFIT_WT:?set TPLFIT_WT}"; TSHA=66f0472752f66d543936842c889818dc2a9d5d7e
EAC="$OWENB_VENDOR_ROOT"; EACSHA=eac10641c5af0b808d332bb61344ee2dfdba4271
ROOT="${COMBO_FIT_ROOT:?}/combo-a-e11-e2-f1030-s${SEED}/PlexOB_480p"
IMAGE="${QUARTUS_DOCKER_IMAGE:-raetro/quartus:mister}"; NAME="quartus_par_combo_s${SEED}"
docker ps -a --format '{{.Names}}' | grep -qx "$NAME" && { echo "EXISTS $NAME"; exit 5; }
[ "$(git -C "$EAC" rev-parse HEAD)" = "$EACSHA" ] && [ -z "$(git -C "$EAC" status --porcelain sys)" ] || { echo "ABORT eac1064 pin moved/dirty"; exit 1; }
[ "$(git -C "$SRC" rev-parse HEAD)" = d0a5a5d03cd6eb60ed76deffabf5298c52a6b21b ] || { echo "ABORT qsfperf src moved"; exit 1; }
grep -q "^new pass=1000 fail=0 of 1000" "${E1_TB_SUMMARY:?set E1_TB_SUMMARY to the E1-1 TB summary.txt}" || { echo "ABORT E1-1 TB not 1000/1000"; exit 6; }
mkdir -p "$(dirname "$ROOT")"
rsync -a --delete --exclude 'vendor/db' --exclude 'vendor/output_files' --exclude 'vendor/incremental_db' \
  --exclude 'vendor/releases' --exclude 'fit.log' "$SRC/" "$ROOT/"
git -C "$WT"  show "$TSHA:$P/vendor/sys/ascal.vhd" > "$ROOT/vendor/sys/ascal.vhd"
git -C "$CWT" show "$CSHA:$P/fit/variants/combo/H264.qsf.A_E2" > "$ROOT/vendor/H264.qsf"
git -C "$CWT" show "$CSHA:$P/vendor/h264/emu.sv" > "$ROOT/vendor/h264/emu.sv"
git -C "$CWT" show "$CSHA:$P/fit/variants/combo/ring_stat-t10-word88.patch" > "$ROOT/ring_stat-t10-word88.patch"
chk() { local g; g=$(md5sum "$ROOT/$1" | cut -c1-8); [ "$g" = "$2" ] || { echo "ABORT md5 $1 got $g want $2" >&2; exit 1; }; }
chk vendor/h264/ring_stat.sv 8a747e1f; chk ring_stat-t10-word88.patch 420c2ab3
patch -s -p1 --forward -d "$ROOT/vendor" < "$ROOT/ring_stat-t10-word88.patch"
grep -q '40..47 -> 88..95' "$ROOT/vendor/h264/ring_stat.sv" && grep -q "st_free >= 7'd56" "$ROOT/vendor/h264/ring_stat.sv" || { echo "ABORT F1030 ring_stat walk"; exit 1; }
sed -i -E "s/^set_global_assignment -name SEED [0-9]+/set_global_assignment -name SEED ${SEED}/" "$ROOT/vendor/H264.qsf"
diff -rq "$ROOT/vendor/sys" "$EAC/sys" >/dev/null || { echo "ABORT vendor/sys differs from eac1064 sys/"; exit 1; }
chk vendor/sys/ascal.vhd 1b17376e; chk vendor/h264/cdc.sv 6587ec58; chk vendor/h264/h264.sdc f7df5d82
chk vendor/h264/dec_stub.sv 5538da65; chk vendor/h264/emu.sv 10ff9922; chk vendor/h264/disp_reader.sv 570492fe
chk fit/variants/q1b/mpx_cdc_hold_targeted.sdc 24c10f8f
E=$ROOT/vendor/h264/emu.sv; Q=$ROOT/vendor/H264.qsf
grep -q 'cdc_snap #(.W(26)) ar_xing' "$E" && grep -q 'if (ar_tick && ar_prim)' "$E" || { echo "ABORT E1-1 missing"; exit 1; }
grep -q "st_sys_ix == 7'd88 ? {32'h4D505842" "$E" || { echo "ABORT F1030 MPXB splice missing"; exit 1; }
grep -q "MPX_PLEXOB_BUILD_ID=32'h0B593F92" "$Q" || { echo "ABORT F1030 BUILD_ID"; exit 1; }
[ "$(grep -c '^set_instance_assignment -name ALLOW_SYNCH_CTRL_USAGE OFF -to "ascal:ascal|o_[vh]_poly_pix\*"' "$Q")" = 2 ] || { echo "ABORT E2 lines"; exit 1; }
q=$(grep -E '^set_global_assignment -name SEED ' "$Q" | awk '{print $4}'); [ "$q" = "$SEED" ] || { echo "ABORT SEED"; exit 1; }
grep -q 'LL_ENABLED' "$Q" && { echo "ABORT LogicLock"; exit 1; }
grep -q 'FITTER_AGGRESSIVE_ROUTABILITY' "$Q" && { echo "ABORT aggr in A"; exit 1; }
[ "$(grep -c '^set_location_assignment LAB_X' "$Q" || true)" = 0 ] || { echo "ABORT Block B pins"; exit 1; }
grep -q '^derive_clock_uncertainty' "$ROOT/vendor/sys/sys_top.sdc" || { echo "ABORT derive_clock_uncertainty"; exit 1; }
if grep -rhE '^\s*set_clock_uncertainty' "$ROOT/vendor" "$ROOT/fit" --include='*.sdc' | grep -v -- '-add' | grep -q .; then echo "ABORT non -add set_clock_uncertainty"; exit 1; fi
while :; do
  n=$(docker ps --format '{{.Names}}' | grep -c '^quartus_' || true); h=$(pgrep -c quartus_fit || true)
  [ "${n:-0}" -lt 7 ] && [ "${h:-0}" -lt 7 ] && break
  echo "$(date '+%T %Z') WAIT cap 7 (docker=$n host_fit=$h)"; sleep $((50 + RANDOM % 20))
done
{ echo "VARIANT=COMBO_A_E11_E2_F1030"; echo "SEED=$q"; echo "COMBO_SHA=$CSHA"; echo "TPLFIT_SHA=$TSHA"; echo "SRC_SHA=$(git -C "$SRC" rev-parse HEAD)"; echo "SYS_EQ_EAC1064=$EACSHA"
  for f in vendor/H264.qsf vendor/sys/ascal.vhd vendor/h264/h264.sdc vendor/h264/cdc.sv vendor/h264/emu.sv vendor/h264/ring_stat.sv; do echo "MD5 $f=$(md5sum "$ROOT/$f"|cut -c1-32)"; done
  echo "LAUNCH=$(date '+%F %T %Z') slot=$((n+1))/7"; } > "$ROOT/RESULT.env"
LOG="$ROOT/fit.log"; : >"$LOG"
docker run -d --restart=no --name "$NAME" -u "$(id -u):$(id -g)" -e HOME=/tmp -e IN_QUARTUS_DOCKER=1 -e NOTIFY_SILENT=1 \
  -v "$ROOT:$ROOT" -v /build -w "$ROOT/vendor" "$IMAGE" \
  bash -c "stdbuf -oL -eL ./build_release.sh --compile --name PlexOB_combo_s${SEED} > ${LOG} 2>&1; echo RC:\$? | tee -a ${LOG}" >/dev/null
echo "$(date '+%T %Z') LAUNCHED $NAME seed=$SEED slot=$((n+1))/7 ascal=1b17376e qsf=$(md5sum "$Q"|cut -c1-8) emu=10ff9922"
