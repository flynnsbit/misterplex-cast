#!/usr/bin/env bash
# Dispatch a Plex_MiSTer Quartus build to an isolated remote build slot.
set -euo pipefail

usage() {
  cat <<USAGE
Usage: $(basename "$0") SLOT [PROJECT_PATH]

SLOT          Remote build slot name/number (letters, digits, dot, underscore, hyphen).
PROJECT_PATH  Local Quartus project directory to sync. Defaults to fpga/Plex_MiSTer.

Environment:
  MISTER_REMOTE_HOST          SSH host/alias (default: docker)
  MISTER_REMOTE_BASE          Remote slot base (default: <remote-home>/mplex-builds)
  MISTER_REMOTE_DEV           Remote misterfpga-dev path (default: <remote-home>/misterfpga-dev)
  MISTER_REMOTE_PROCESSORS    UNSAFE override for NUM_PARALLEL_PROCESSORS. Default preserves QSF.
  MISTER_REMOTE_ALLOW_PROCESSOR_OVERRIDE=1 is required if MISTER_REMOTE_PROCESSORS is set.
  MISTER_REMOTE_COPY_BACK     1 to rsync reports/RBF back locally, 0 to leave remote-only (default: 1)
  MISTER_REMOTE_REFERENCE_RBF Reference RBF for bit-identity check. Defaults to PROJECT_PATH/output_files/Plex.rbf if present.
  MISTER_REMOTE_ALLOW_UNVERIFIED=1 permits a remote RBF when no reference RBF exists.

Artifacts are copied back to PROJECT_PATH/remote_out/SLOT/ unless copy-back is disabled.
By default the script refuses to finish without bit-identity against a reference RBF,
unless MISTER_REMOTE_ALLOW_UNVERIFIED=1 is set explicitly.
USAGE
}

if [[ $# -lt 1 || $# -gt 2 || "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  [[ $# -eq 0 || "${1:-}" == "-h" || "${1:-}" == "--help" ]] && exit 0 || exit 2
fi

SLOT="$1"
if [[ ! "$SLOT" =~ ^[A-Za-z0-9._-]+$ ]]; then
  echo "Invalid slot '$SLOT': use only letters, digits, dot, underscore, and hyphen." >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="${2:-$ROOT/fpga/Plex_MiSTer}"
if [[ ! -d "$PROJECT" ]]; then
  echo "Project directory not found: $PROJECT" >&2
  exit 2
fi
if [[ ! -f "$PROJECT/Plex.qpf" || ! -f "$PROJECT/Plex.qsf" ]]; then
  echo "Project does not look like Plex_MiSTer (missing Plex.qpf/Plex.qsf): $PROJECT" >&2
  exit 2
fi

HOST="${MISTER_REMOTE_HOST:-docker}"
PROCESSORS="${MISTER_REMOTE_PROCESSORS:-}"
COPY_BACK="${MISTER_REMOTE_COPY_BACK:-1}"
ALLOW_PROCESSOR_OVERRIDE="${MISTER_REMOTE_ALLOW_PROCESSOR_OVERRIDE:-0}"
REFERENCE_RBF="${MISTER_REMOTE_REFERENCE_RBF:-}"
ALLOW_UNVERIFIED="${MISTER_REMOTE_ALLOW_UNVERIFIED:-0}"
if [[ -n "$PROCESSORS" ]]; then
  if [[ "$ALLOW_PROCESSOR_OVERRIDE" != "1" ]]; then
    echo "MISTER_REMOTE_PROCESSORS changes the RBF and is disabled unless MISTER_REMOTE_ALLOW_PROCESSOR_OVERRIDE=1." >&2
    exit 2
  fi
  if [[ ! "$PROCESSORS" =~ ^[0-9]+$ || "$PROCESSORS" -lt 1 ]]; then
    echo "MISTER_REMOTE_PROCESSORS must be a positive integer when set (got '$PROCESSORS')." >&2
    exit 2
  fi
fi
if [[ "$COPY_BACK" != "0" && "$COPY_BACK" != "1" ]]; then
  echo "MISTER_REMOTE_COPY_BACK must be 0 or 1 (got '$COPY_BACK')." >&2
  exit 2
fi
if [[ "$ALLOW_UNVERIFIED" != "0" && "$ALLOW_UNVERIFIED" != "1" ]]; then
  echo "MISTER_REMOTE_ALLOW_UNVERIFIED must be 0 or 1 (got '$ALLOW_UNVERIFIED')." >&2
  exit 2
fi
if [[ -z "$REFERENCE_RBF" && -f "$PROJECT/output_files/Plex.rbf" ]]; then
  REFERENCE_RBF="$PROJECT/output_files/Plex.rbf"
fi
if [[ -n "$REFERENCE_RBF" && ! -f "$REFERENCE_RBF" ]]; then
  echo "Reference RBF not found: $REFERENCE_RBF" >&2
  exit 2
fi
if [[ -z "$REFERENCE_RBF" && "$ALLOW_UNVERIFIED" != "1" ]]; then
  echo "No reference RBF available for bit-identity check." >&2
  echo "Provide MISTER_REMOTE_REFERENCE_RBF or build a local PROJECT_PATH/output_files/Plex.rbf;" >&2
  echo "set MISTER_REMOTE_ALLOW_UNVERIFIED=1 only with parent approval for first remote-only candidates." >&2
  exit 4
fi

REMOTE_HOME="$(ssh "$HOST" 'printf "%s\n" "$HOME"')"
REMOTE_BASE="${MISTER_REMOTE_BASE:-$REMOTE_HOME/mplex-builds}"
REMOTE_DEV="${MISTER_REMOTE_DEV:-$REMOTE_HOME/misterfpga-dev}"
PROJECT_NAME="$(basename "$PROJECT")"
REMOTE_SLOT="$REMOTE_BASE/$SLOT"
REMOTE_PROJECT="$REMOTE_SLOT/$PROJECT_NAME"
LOCAL_OUT="$PROJECT/remote_out/$SLOT"

printf 'Remote host: %s\n' "$HOST"
printf 'Remote slot: %s\n' "$REMOTE_SLOT"
printf 'Remote project: %s\n' "$REMOTE_PROJECT"
printf 'Remote processors: %s\n' "${PROCESSORS:-preserve-qsf}"
printf 'Reference RBF: %s\n' "${REFERENCE_RBF:-UNVERIFIED-OVERRIDE}"

ssh "$HOST" bash -s -- "$REMOTE_SLOT" "$REMOTE_PROJECT" "$REMOTE_DEV" <<'REMOTE_PREP'
set -euo pipefail
remote_slot="$1"
remote_project="$2"
remote_dev="$3"
mkdir -p "$remote_project"
lock_dir="$remote_slot/.build-lock"
if ! mkdir "$lock_dir" 2>/dev/null; then
  echo "Remote slot is locked: $remote_slot" >&2
  exit 1
fi
trap 'rmdir "$lock_dir" 2>/dev/null || true' EXIT
if [[ ! -x "$remote_dev/scripts/mister-dev" ]]; then
  echo "Remote mister-dev not executable at $remote_dev/scripts/mister-dev" >&2
  exit 1
fi
if [[ ! -f "$remote_dev/scripts/lib.sh" ]]; then
  echo "Remote mister-dev lib not found at $remote_dev/scripts/lib.sh" >&2
  exit 1
fi
REMOTE_PREP

rsync -a --delete --delete-excluded \
  --exclude '/db/' \
  --exclude '/incremental_db/' \
  --exclude '/output_files/' \
  --exclude '/remote_out/' \
  --exclude '/greybox_tmp/' \
  --exclude '/build/' \
  --exclude '*.qws' \
  "$PROJECT/" "$HOST:$REMOTE_PROJECT/"

ssh "$HOST" bash -s -- "$REMOTE_SLOT" "$REMOTE_PROJECT" "$REMOTE_DEV" "$PROCESSORS" <<'REMOTE_BUILD'
set -euo pipefail
remote_slot="$1"
remote_project="$2"
remote_dev="$3"
processors="${4:-}"
lock_dir="$remote_slot/.build-lock"
if ! mkdir "$lock_dir" 2>/dev/null; then
  echo "Remote slot is locked: $remote_slot" >&2
  exit 1
fi
trap 'rmdir "$lock_dir" 2>/dev/null || true' EXIT
qsf="$remote_project/Plex.qsf"
if [[ -n "$processors" ]]; then
  if grep -qE '^[[:space:]]*set_global_assignment[[:space:]]+-name[[:space:]]+NUM_PARALLEL_PROCESSORS' "$qsf"; then
    sed -i -E "s/^([[:space:]]*set_global_assignment[[:space:]]+-name[[:space:]]+NUM_PARALLEL_PROCESSORS)[[:space:]]+[0-9]+/\1 ${processors}/" "$qsf"
  else
    printf '\nset_global_assignment -name NUM_PARALLEL_PROCESSORS %s\n' "$processors" >>"$qsf"
  fi
fi
mkdir -p "$remote_slot/build_remote"
log_file="$remote_slot/build_remote/compile.log"
stats_file="$remote_slot/build_remote/docker_stats.tsv"
summary_file="$remote_slot/build_remote/summary.txt"
: >"$stats_file"
# shellcheck source=/dev/null
source "$remote_dev/scripts/lib.sh"
load_env
container="mplex-fit-${USER}-${remote_slot##*/}-$(date +%Y%m%d%H%M%S)"
start_epoch=$(date +%s)
avail_start=$(free -m | awk '/^Mem:/{print $7}')
echo "Project: $remote_project" | tee "$log_file"
echo "Image: $QUARTUS_IMAGE" | tee -a "$log_file"
echo "NUM_PARALLEL_PROCESSORS: $(grep -E 'NUM_PARALLEL_PROCESSORS' "$qsf" | tail -1 || true)" | tee -a "$log_file"
set +e
docker run --rm --name "$container" \
  -v "$remote_project":/build \
  -w /build \
  -u "$(id -u):$(id -g)" \
  "$QUARTUS_IMAGE" \
  quartus_sh --flow compile Plex.qpf >>"$log_file" 2>&1 &
docker_pid=$!
peak_mb=0
while kill -0 "$docker_pid" 2>/dev/null; do
  mem_usage=$(docker stats --no-stream --format '{{.MemUsage}}' "$container" 2>/dev/null | awk '{print $1}' || true)
  avail_now=$(free -m | awk '/^Mem:/{print $7}')
  printf '%s\t%s\t%s\n' "$(date +%s)" "${mem_usage:-NA}" "${avail_now:-NA}" >>"$stats_file"
  if [[ -n "${mem_usage:-}" && "$mem_usage" != "NA" ]]; then
    mem_mb=$(python3 - "$mem_usage" <<'PY'
import sys
s=sys.argv[1].strip()
units=[('GiB',1024),('MiB',1),('KiB',1/1024),('GB',1000),('MB',1),('KB',1/1000),('B',1/(1024*1024))]
for u,m in units:
    if s.endswith(u):
        try:
            print(int(float(s[:-len(u)])*m+0.5))
        except ValueError:
            print(0)
        break
else:
    print(0)
PY
)
    if [[ "$mem_mb" =~ ^[0-9]+$ && "$mem_mb" -gt "$peak_mb" ]]; then
      peak_mb="$mem_mb"
    fi
  fi
  sleep 5
done
wait "$docker_pid"
rc=$?
set -e
end_epoch=$(date +%s)
avail_end=$(free -m | awk '/^Mem:/{print $7}')
wall=$((end_epoch - start_epoch))
{
  echo "exit_code=$rc"
  echo "wall_seconds=$wall"
  echo "peak_docker_mem_mb=$peak_mb"
  echo "available_mem_start_mb=$avail_start"
  echo "available_mem_end_mb=$avail_end"
  echo "stats_file=$stats_file"
  echo "log_file=$log_file"
} >"$summary_file"
cat "$summary_file"
if [[ "$rc" -ne 0 ]]; then
  tail -80 "$log_file" >&2
  exit "$rc"
fi
for f in "$remote_project/output_files/Plex.rbf" "$remote_project/output_files/Plex.sta.rpt" "$remote_project/output_files/Plex.fit.rpt" "$remote_project/output_files/Plex.map.rpt"; do
  if [[ ! -f "$f" ]]; then
    echo "Build succeeded but missing artifact: $f" >&2
    exit 1
  fi
done
REMOTE_BUILD

if [[ "$COPY_BACK" == "1" ]]; then
  mkdir -p "$LOCAL_OUT"
  rsync -a "$HOST:$REMOTE_PROJECT/output_files/Plex.rbf" "$LOCAL_OUT/"
  rsync -a "$HOST:$REMOTE_PROJECT/output_files/Plex.sta.rpt" "$LOCAL_OUT/"
  rsync -a "$HOST:$REMOTE_PROJECT/output_files/Plex.fit.rpt" "$LOCAL_OUT/"
  rsync -a "$HOST:$REMOTE_PROJECT/output_files/Plex.map.rpt" "$LOCAL_OUT/"
  rsync -a "$HOST:$REMOTE_SLOT/build_remote/compile.log" "$LOCAL_OUT/"
  rsync -a "$HOST:$REMOTE_SLOT/build_remote/summary.txt" "$LOCAL_OUT/"
  rsync -a "$HOST:$REMOTE_SLOT/build_remote/docker_stats.tsv" "$LOCAL_OUT/"
  HIER_EXTRA=()
  if grep -E '^[[:space:]]*set_global_assignment[[:space:]]+-name[[:space:]]+VERILOG_MACRO[[:space:]]+"PLEX_PRESENT_720P_L4=1"' \
      "$PROJECT/Plex.qsf" >/dev/null 2>&1; then
    # L4 720p present stubs fabric H.264 (clk_sys Fmax / 291 M10K).
    HIER_EXTRA=(--allow-missing stream_path --allow-missing ddr_bitstream_reader)
  fi
  "$ROOT/scripts/check_quartus_fit_hierarchy.py" \
    --fit-rpt "$LOCAL_OUT/Plex.fit.rpt" \
    --map-rpt "$LOCAL_OUT/Plex.map.rpt" \
    --log "$LOCAL_OUT/compile.log" \
    "${HIER_EXTRA[@]}"
  "$ROOT/scripts/check_quartus_timing.py" --sta-rpt "$LOCAL_OUT/Plex.sta.rpt"
  "$ROOT/scripts/check_timing_exclusions.py" --sta-rpt "$LOCAL_OUT/Plex.sta.rpt"
  RBF="$LOCAL_OUT/Plex.rbf"
  STA="$LOCAL_OUT/Plex.sta.rpt"
  FIT="$LOCAL_OUT/Plex.fit.rpt"
else
  RBF="$REMOTE_PROJECT/output_files/Plex.rbf"
  STA="$REMOTE_PROJECT/output_files/Plex.sta.rpt"
  FIT="$REMOTE_PROJECT/output_files/Plex.fit.rpt"
fi

if [[ "$COPY_BACK" == "1" ]]; then
  MD5="$(md5sum "$RBF" | awk '{print $1}')"
  NEG_SLACK_COUNT="$(grep -cE ';[[:space:]]*-[0-9]+\.[0-9]+[[:space:]]*;' "$STA" || true)"
  LOGIC_UTILIZATION="$(grep -m1 -E '^[[:space:]]*;?[[:space:]]*Logic utilization|Logic utilization' "$FIT" || true)"
else
  read -r MD5 NEG_SLACK_COUNT LOGIC_UTILIZATION < <(ssh "$HOST" bash -s -- "$RBF" "$STA" "$FIT" <<'REMOTE_SUMMARY'
set -euo pipefail
rbf="$1"; sta="$2"; fit="$3"
md5=$(md5sum "$rbf" | awk '{print $1}')
neg=$(grep -cE ';[[:space:]]*-[0-9]+\.[0-9]+[[:space:]]*;' "$sta" || true)
logic=$(grep -m1 -E '^[[:space:]]*;?[[:space:]]*Logic utilization|Logic utilization' "$fit" || true)
printf '%s %s %s\n' "$md5" "$neg" "$logic"
REMOTE_SUMMARY
)
fi
if [[ -n "$REFERENCE_RBF" ]]; then
  if [[ "$COPY_BACK" != "1" ]]; then
    echo "MISTER_REMOTE_COPY_BACK=0 cannot perform local reference comparison; leave copy-back enabled." >&2
    exit 4
  fi
  REF_MD5="$(md5sum "$REFERENCE_RBF" | awk '{print $1}')"
  if ! cmp -s "$REFERENCE_RBF" "$RBF"; then
    printf 'Reference RBF: %s\n' "$REFERENCE_RBF" >&2
    printf 'Reference md5: %s\n' "$REF_MD5" >&2
    printf 'Remote md5:    %s\n' "$MD5" >&2
    echo "FAIL: remote RBF is not bit-identical to reference" >&2
    exit 3
  fi
  printf 'Reference RBF: %s\n' "$REFERENCE_RBF"
  printf 'Reference md5: %s\n' "$REF_MD5"
  printf 'Bit-identity: PASS\n'
else
  printf 'Bit-identity: UNVERIFIED (MISTER_REMOTE_ALLOW_UNVERIFIED=1)\n' >&2
fi
printf 'Artifacts: %s\n' "$([[ "$COPY_BACK" == "1" ]] && printf '%s' "$LOCAL_OUT" || printf '%s' "$REMOTE_PROJECT/output_files")"
printf 'Plex.rbf md5: %s\n' "$MD5"
printf 'Negative-slack rows: %s\n' "$NEG_SLACK_COUNT"
printf 'Logic utilization: %s\n' "${LOGIC_UTILIZATION:-not found}"
