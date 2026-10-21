# Remote body for deploy_candidate_pair.py; not a standalone entrypoint.
# Paths/approved identities are readonly assignments supplied by that driver.
[[ ${BASH_VERSINFO[0]} -ge 4 ]] || exit 1
fail() { printf 'CANDIDATE_ERROR %s\n' "$1"; exit 1; }
phase() { printf 'CANDIDATE_PHASE %s\n' "$1"; }
trap 'printf "CANDIDATE_ERROR operation-failed\n"' ERR
readonly FROZEN_RBF="$MEDIA/_Utility/Plex_480p.rbf"
readonly FROZEN_ARM="$BASE/bin/misterplexd.480p"
readonly BUNDLE="$BASE/candidates/$BUILD_ID-$TRANSACTION"
readonly STAGE="$BUNDLE.incoming"
readonly LOG_DIR="$LOG_BASE/$BUILD_ID-$TRANSACTION"
readonly DAEMON_LOG="$LOG_DIR/misterplexd.log"
declare -a ARGV=() WATCH=() SUPERVISE=() DAEMONS=() CHILDREN=()
declare -A BIRTH=() EXE=() ARGHASH=() EXEC_HASH=() PARENT=()

regular() {
    [[ -f "$1" && ! -L "$1" && "$(readlink -f "$1")" == "$1" ]] ||
        fail noncanonical-file
}
check_sha() {
    regular "$1"
    local digest
    digest=$(sha256sum "$1")
    [[ ${digest%% *} == "$2" ]] || fail file-sha256-mismatch
}
argv() {
    ARGV=()
    local arg
    while IFS= read -r -d '' arg; do ARGV+=("$arg"); done < "$PROC/$1/cmdline"
}
birth() {
    local line rest
    if ! IFS= read -r line < "$PROC/$1/stat"; then
        [[ ${2:-} == transition && ! -e "$PROC/$1" ]] && return 1
        fail unreadable-process-stat
    fi
    rest=${line##*) }
    read -r -a FIELDS <<< "$rest"
    [[ ${#FIELDS[@]} -ge 20 && ${FIELDS[19]} =~ ^[0-9]+$ &&
       ${FIELDS[1]} =~ ^[0-9]+$ ]] ||
        fail invalid-process-stat
    START=${FIELDS[19]}
    PPID_VALUE=${FIELDS[1]}
    STATE=${FIELDS[0]}
}
snapshot() {
    local pid=$1 start digest
    [[ $pid =~ ^[1-9][0-9]*$ ]] || fail invalid-pid
    birth "$pid"; start=$START
    [[ $STATE != Z && $STATE != T && $STATE != t ]] || fail process-not-runnable
    [[ "$(awk '/^Uid:/ {print $2,$3,$4,$5}' "$PROC/$pid/status")" == "0 0 0 0" ]] ||
        fail foreign-process-owner
    BIRTH[$pid]=$start
    PARENT[$pid]=$PPID_VALUE
    EXE[$pid]=$(stat -Lc '%d:%i:%u' "$PROC/$pid/exe")
    digest=$(sha256sum "$PROC/$pid/exe"); EXEC_HASH[$pid]=${digest%% *}
    digest=$(sha256sum "$PROC/$pid/cmdline"); ARGHASH[$pid]=${digest%% *}
    birth "$pid"
    [[ $START == "$start" ]] || fail process-changed-during-snapshot
}
same_process() {
    local pid=$1 digest
    [[ -r "$PROC/$pid/stat" ]] || return 1
    birth "$pid"
    [[ $START == "${BIRTH[$pid]}" ]] || fail pid-reused
    [[ $STATE != Z ]] || return 1
    [[ "$(stat -Lc '%d:%i:%u' "$PROC/$pid/exe")" == "${EXE[$pid]}" ]] ||
        fail executable-identity-changed
    digest=$(sha256sum "$PROC/$pid/exe")
    [[ ${digest%% *} == "${EXEC_HASH[$pid]}" ]] || fail executable-bytes-changed
    digest=$(sha256sum "$PROC/$pid/cmdline")
    [[ ${digest%% *} == "${ARGHASH[$pid]}" ]] || fail process-command-changed
    [[ "$(awk '/^Uid:/ {print $2,$3,$4,$5}' "$PROC/$pid/status")" == "0 0 0 0" ]] ||
        fail process-owner-changed
}
pidfd_action() {
    PYTHONDONTWRITEBYTECODE=1 python3 - "$@" <<'MPX_PIDFD_PY'
# INSERT_PIDFD_HELPER
MPX_PIDFD_PY
}
stop_one() {
    local pid=$1
    pidfd_action stop "$pid" "${BIRTH[$pid]}" "${EXE[$pid]}" \
        "${EXEC_HASH[$pid]}" "${ARGHASH[$pid]}" || fail pidfd-stop-refused
}
daemon_identity() {
    local pid=$1 binary=$2 digest=$3 conf=$4
    argv "$pid"
    [[ ${#ARGV[@]} == 9 && ${ARGV[0]} == "$binary" &&
       ${ARGV[1]} == --name && ${ARGV[2]} == MiSTerPlex &&
       ${ARGV[3]} == --id && ${ARGV[4]} == misterplex-dev &&
       ${ARGV[5]} == --port && ${ARGV[6]} == 3005 &&
       ${ARGV[7]} == --conf && ${ARGV[8]} == "$conf" ]] ||
        fail unapproved-daemon-command
    snapshot "$pid"
    [[ "$(readlink "$PROC/$pid/exe")" == "$binary" &&
       "${EXE[$pid]}" == "$(stat -Lc '%d:%i:%u' "$binary")" &&
       "${EXEC_HASH[$pid]}" == "$digest" ]] || fail unapproved-daemon-executable
}
discover() {
    WATCH=(); SUPERVISE=(); DAEMONS=()
    local entry pid comm script shell first arg
    for entry in "$PROC"/[0-9]*; do
        [[ -r "$entry/cmdline" && -r "$entry/comm" ]] || continue
        pid=${entry##*/}
        IFS= read -r comm < "$entry/comm" || fail unreadable-process-name
        argv "$pid"
        [[ ${#ARGV[@]} -gt 0 ]] || continue
        first=${ARGV[0]}
        if [[ $comm == mpx-main || $comm == misterplexd || $first == "$OLD_ARM" ]]; then
            daemon_identity "$pid" "$OLD_ARM" "$OLD_ARM_SHA" "$OLD_CONF"
            DAEMONS+=("$pid")
        elif [[ ${#ARGV[@]} -ge 2 ]]; then
            script=
            for arg in "${ARGV[@]:1}"; do
                case "$arg" in
                    *misterplex_core_watch.sh*|*misterplexd_supervise.sh*) script=$arg; break;;
                esac
            done
            [[ -n $script ]] || continue
            # Recognize option-bearing/wrapped helpers, but never silently exempt them.
            [[ ${#ARGV[@]} == 2 && $script == "${ARGV[1]}" ]] ||
                fail unapproved-respawn-helper
            case "${script##*/}" in
                misterplex_core_watch.sh|misterplexd_supervise.sh)
                    [[ $script == /* ]] || script="$(readlink -f "$entry/cwd")/$script"
                    script=$(readlink -f "$script")
                    case "$script" in
                        "$BASE/bin/misterplex_core_watch.sh"|"$BASE/scripts/misterplex_core_watch.sh"|\
                        "$BASE/bin/misterplexd_supervise.sh"|"$BASE/scripts/misterplexd_supervise.sh") ;;
                        *) fail unapproved-respawn-helper;;
                    esac
                    [[ ${#ARGV[@]} == 2 && -n ${HELPER_HASH[$script]:-} ]] ||
                        fail unapproved-respawn-helper
                    check_sha "$script" "${HELPER_HASH[$script]}"
                    shell=$(readlink -f "$first")
                    [[ $shell == "$(readlink -f /bin/sh)" ||
                       $shell == "$(readlink -f /bin/bash)" ]] || fail foreign-helper-interpreter
                    snapshot "$pid"
                    [[ "${EXE[$pid]}" == "$(stat -Lc '%d:%i:%u' "$shell")" ]] ||
                        fail foreign-helper-executable
                    case "${script##*/}" in
                        misterplex_core_watch.sh) WATCH+=("$pid");;
                        *) SUPERVISE+=("$pid");;
                    esac
                    ;;
                *) fail unapproved-respawn-helper;;
            esac
        fi
    done
    [[ ${#DAEMONS[@]} -le 1 ]] || fail duplicate-daemon
}
find_main() {
    local entry pid
    MAIN_PID=
    for entry in "$PROC"/[0-9]*; do
        [[ -L "$entry/exe" ]] || continue
        [[ "$(readlink "$entry/exe")" == "$MEDIA/MiSTer" ]] || continue
        [[ -z $MAIN_PID ]] || fail duplicate-main
        pid=${entry##*/}
        snapshot "$pid"
        [[ "${EXE[$pid]}" == "$(stat -Lc '%d:%i:%u' "$MEDIA/MiSTer")" ]] ||
            fail unapproved-main-executable
        MAIN_PID=$pid
    done
    [[ -n $MAIN_PID ]] || fail missing-main
}
retired_main_transition() {
    [[ $1 == transition && $MAIN_PID == "${MAIN_REQUEST_PID:-}" ]] || return 1
    if birth "$MAIN_PID" transition; then
        [[ $START == "$MAIN_REQUEST_BIRTH" ]] || fail main-pid-reused
        [[ $STATE == Z ]]
    else
        return 0
    fi
}
main_core() {
    local expected=$1 allow=${2:-} digest identity owner
    [[ $MAIN_PID == "${MAIN_REQUEST_PID:-}" ]] || allow=
    if ! birth "$MAIN_PID" "$allow"; then return 1; fi
    [[ $START == "${BIRTH[$MAIN_PID]}" ]] || fail main-identity-changed
    if [[ $STATE == Z && $allow == transition ]]; then return 1; fi
    [[ $STATE != Z && $STATE != T && $STATE != t ]] || fail main-identity-changed
    identity=$(stat -Lc '%d:%i:%u' "$PROC/$MAIN_PID/exe") || {
        retired_main_transition "$allow" && return 1
        fail unreadable-main-executable
    }
    [[ $identity == "${EXE[$MAIN_PID]}" ]] || fail main-identity-changed
    owner=$(awk '/^Uid:/ {print $2,$3,$4,$5}' "$PROC/$MAIN_PID/status") || {
        retired_main_transition "$allow" && return 1
        fail unreadable-main-owner
    }
    [[ $owner == "0 0 0 0" ]] || fail main-owner-changed
    digest=$(sha256sum "$PROC/$MAIN_PID/exe") || {
        retired_main_transition "$allow" && return 1
        fail unreadable-main-executable
    }
    [[ ${digest%% *} == "${EXEC_HASH[$MAIN_PID]}" ]] || fail main-executable-changed
    argv "$MAIN_PID" || {
        retired_main_transition "$allow" && return 1
        fail unreadable-main-arguments
    }
    [[ ${#ARGV[@]} == 2 && ${ARGV[0]} == "$MEDIA/MiSTer" && ${ARGV[1]} == "$expected" ]]
}
known_core() {
    if main_core "$OLD_RBF"; then return; fi
    main_core "$MEDIA/menu.rbf" || fail unknown-active-core
}
listeners() {
    local file
    [[ -r "$PROC/net/tcp" ]] || fail missing-socket-table
    for file in "$PROC/net/tcp" "$PROC/net/tcp6"; do
        [[ ! -r "$file" ]] ||
            awk '$2 ~ /:0BBD$/ && $4 == "0A" {print $10}' "$file"
    done
}
owns_port() {
    local pid=$1 inode fd found count=0 sockets
    sockets=$(listeners) || fail unreadable-socket-table
    [[ -n $sockets ]] || return 1
    while IFS= read -r inode; do
        [[ $inode =~ ^[0-9]+$ ]] || fail invalid-socket-identity
        found=0
        for fd in "$PROC/$pid/fd/"*; do
            [[ -L "$fd" ]] || continue
            if [[ "$(readlink "$fd")" == "socket:[$inode]" ]]; then found=1; break; fi
        done
        [[ $found == 1 ]] || fail foreign-port-owner
        count=$((count + 1))
    done <<< "$sockets"
    [[ $count -gt 0 ]]
}
preflight() {
    pidfd_action probe || fail pidfd-support-required
    if [[ $MODE == menu ]]; then
        [[ ! -L "$CMD" && ( -p "$CMD" || -c "$CMD" ) ]] || fail missing-command-endpoint
    fi
    check_sha "$FROZEN_RBF" "$FROZEN_RBF_SHA"
    check_sha "$FROZEN_ARM" "$FROZEN_ARM_SHA"
    check_sha "$OLD_RBF" "$OLD_RBF_SHA"
    check_sha "$OLD_ARM" "$OLD_ARM_SHA"
    regular "$OLD_CONF"; regular "$BASE/misterplex.conf"
    regular "$BASE/bin/misterplexd"; regular "$MEDIA/menu.rbf"; regular "$MEDIA/MiSTer"
    find_main; known_core; discover
    if [[ -n "$(listeners)" ]]; then
        [[ ${#DAEMONS[@]} == 1 ]] || fail occupied-companion-port
        owns_port "${DAEMONS[0]}" || fail missing-companion-listener
    fi
}
backup_file() {
    regular "$1"
    cp -p "$1" "$STAGE/rollback/$2"
    cmp -s "$1" "$STAGE/rollback/$2" || fail rollback-copy-mismatch
}
verify_preserved() {
    cmp -s "$FROZEN_RBF" "$BUNDLE/rollback/Plex_480p.rbf" || fail rollback-changed
    cmp -s "$FROZEN_ARM" "$BUNDLE/rollback/misterplexd.480p" || fail rollback-changed
    cmp -s "$OLD_RBF" "$BUNDLE/rollback/current.rbf" || fail current-core-changed
    cmp -s "$OLD_ARM" "$BUNDLE/rollback/current.arm" || fail current-daemon-changed
    cmp -s "$OLD_CONF" "$BUNDLE/rollback/current.conf" || fail current-config-changed
    cmp -s "$BASE/misterplex.conf" "$BUNDLE/rollback/generic.conf" || fail generic-config-changed
    cmp -s "$BASE/bin/misterplexd" "$BUNDLE/rollback/generic.arm" || fail generic-daemon-changed
}
collect_children() {
    local entry pid parent changed=1
    declare -A owned=()
    for pid in "${DAEMONS[@]}" "${WATCH[@]}" "${SUPERVISE[@]}"; do owned[$pid]=1; done
    for pid in "${CHILDREN[@]}"; do owned[$pid]=1; done
    while [[ $changed == 1 ]]; do
        changed=0
        for entry in "$PROC"/[0-9]*; do
            [[ -r "$entry/stat" ]] || continue
            pid=${entry##*/}
            [[ -z ${owned[$pid]:-} ]] || continue
            birth "$pid"; parent=$PPID_VALUE
            if [[ -n ${owned[$parent]:-} && $STATE != Z ]]; then
                snapshot "$pid"; CHILDREN+=("$pid"); owned[$pid]=1; changed=1
            fi
        done
    done
}
wait_children() {
    local pid i remaining deadline=$((SECONDS + 10))
    for ((i=0; i<40; i++)); do
        remaining=0
        for pid in "${CHILDREN[@]}"; do
            if same_process "$pid"; then remaining=1; fi
        done
        [[ $remaining == 0 ]] && return
        ((SECONDS < deadline)) || break
        sleep 0.25
    done
    fail owned-child-still-running
}
require_quiescent() {
    local entry pid fd target rc start retired
    discover
    [[ ${#WATCH[@]} == 0 && ${#SUPERVISE[@]} == 0 && ${#DAEMONS[@]} == 0 ]] ||
        fail companion-respawned
    [[ -z "$(listeners)" ]] || fail companion-port-not-quiescent
    # Refuse other hardware users; never signal them or unlink their SPI lock.
    for entry in "$PROC"/[0-9]*; do
        pid=${entry##*/}
        [[ $pid != "$MAIN_PID" ]] || continue
        if ! birth "$pid" transition; then continue; fi
        start=$START
        [[ $STATE != Z ]] || continue
        retired=0
        for fd in "$entry/fd/"*; do
            [[ -L "$fd" ]] || continue
            if ! target=$(readlink "$fd"); then
                if audit_process_retired "$pid" "$start"; then retired=1; break; fi
                [[ ! -L "$fd" ]] && continue
                fail unreadable-device-descriptor
            fi
            case "$target" in
                /dev/MrAudio|/dev/mem|/dev/uio[0-9]*|*/misterplex_spi.lock)
                    fail another-hardware-owner;;
            esac
        done
        [[ $retired == 0 ]] || continue
        if [[ -r "$entry/maps" ]]; then
            if grep -qE '[[:space:]]/dev/(mem|MrAudio|uio[0-9]+)( \(deleted\))?$' "$entry/maps"; then
                fail another-hardware-mapping
            else
                rc=$?
                if [[ $rc != 1 ]]; then
                    audit_process_retired "$pid" "$start" && continue
                    fail unreadable-hardware-mappings
                fi
            fi
        elif [[ -e "$entry" ]]; then
            audit_process_retired "$pid" "$start" || fail unreadable-hardware-mappings
        fi
        if audit_process_retired "$pid" "$start"; then continue; fi
    done
}
audit_process_retired() {
    if birth "$1" transition; then
        [[ $START == "$2" ]] || fail process-reused-during-hardware-audit
        [[ $STATE == Z ]]
    else
        return 0
    fi
}
send_core() {
    [[ ! -L "$CMD" && ( -p "$CMD" || -c "$CMD" ) ]] || fail missing-command-endpoint
    # A FIFO with a wedged/missing reader must not block before the observation timeout.
    timeout 3 bash -c 'printf "load_core %s\n" "$2" > "$1"' \
        mpx-core-command "$CMD" "$1" || fail core-command-write-failed
}
begin_main_transition() {
    known_core
    MAIN_REQUEST_PID=$MAIN_PID
    MAIN_REQUEST_BIRTH=${BIRTH[$MAIN_PID]}
    MAIN_REQUEST_EXE=${EXE[$MAIN_PID]}
    MAIN_REQUEST_HASH=${EXEC_HASH[$MAIN_PID]}
    MAIN_REQUEST_CORE=$1
    MAIN_REQUEST_TICKS=$(python3 - "$PROC/uptime" <<'MPX_MAIN_CLOCK'
from pathlib import Path
import os
import sys
value = Path(sys.argv[1]).read_text().split()[0]
seconds, _, fraction = value.partition(".")
if not seconds.isdigit() or (fraction and not fraction.isdigit()):
    raise SystemExit("invalid-boot-clock")
hz = os.sysconf("SC_CLK_TCK")
ticks = int(seconds) * hz
if fraction:
    ticks += int(fraction) * hz // (10 ** len(fraction))
print(ticks)
MPX_MAIN_CLOCK
    ) || fail unreadable-boot-clock
    [[ $MAIN_REQUEST_TICKS =~ ^[0-9]+$ ]] || fail invalid-boot-clock
}
main_handoff() {
    local entry pid candidate=
    for entry in "$PROC"/[0-9]*; do
        [[ -L "$entry/exe" ]] || continue
        [[ "$(readlink "$entry/exe")" == "$MEDIA/MiSTer" ]] || continue
        [[ -z $candidate ]] || fail duplicate-main
        candidate=${entry##*/}
    done
    [[ -n $candidate ]] || return 1
    pid=$candidate
    [[ $pid != "$MAIN_REQUEST_PID" ]] || fail main-pid-reused
    snapshot "$pid"
    [[ ${EXE[$pid]} == "$MAIN_REQUEST_EXE" && ${EXEC_HASH[$pid]} == "$MAIN_REQUEST_HASH" ]] ||
        fail main-handoff-executable-changed
    [[ ${BIRTH[$pid]} -ge $MAIN_REQUEST_TICKS &&
       ( ${PARENT[$pid]} == "$MAIN_REQUEST_PID" || ${PARENT[$pid]} == 1 ) ]] ||
        fail unapproved-main-handoff
    argv "$pid"
    [[ ${#ARGV[@]} == 2 && ${ARGV[0]} == "$MEDIA/MiSTer" && ${ARGV[1]} == "$MAIN_REQUEST_CORE" ]] ||
        fail unexpected-main-handoff-core
    MAIN_PID=$pid
}
wait_main() {
    local expected=$1 i
    [[ ${MAIN_REQUEST_CORE:-} == "$expected" ]] || fail unapproved-main-transition
    for ((i=0; i<20; i++)); do
        # Main can retire and start a fresh process for this one requested core.
        # Only that bounded handoff may replace the original process pin.
        if birth "$MAIN_REQUEST_PID" transition; then
            [[ $START == "$MAIN_REQUEST_BIRTH" ]] || fail main-pid-reused
            if [[ $STATE == Z ]]; then
                main_handoff || { sleep 0.5; continue; }
            fi
        else
            main_handoff || { sleep 0.5; continue; }
        fi
        if main_core "$expected" transition; then
            MAIN_REQUEST_CORE=
            return
        fi
        sleep 0.5
    done
    fail core-transition-timeout
}
launch_candidate() {
    # Do not start the historical supervisor: its recovery path uses SIGKILL.
    # No startup edits; reboot/autostart remains the operator's existing rollback policy.
    nohup env -i HOME=/root PATH=/usr/bin:/bin:/usr/sbin:/sbin \
        TMPDIR="$BUNDLE/run" LANG=C \
        MPX_VIDEO_BACKEND=fpga-h264 MPX_H264_PROTOTYPE=idr MPX_H264_FILTER=off \
        "$BUNDLE/misterplexd" --name MiSTerPlex --id misterplex-dev --port 3005 \
        --conf "$BUNDLE/misterplex.conf" 9>&- >&"$DAEMON_LOG_FD" 2>&1 < /dev/null &
    LAUNCHED_PID=$!
}
private_log_directory() {
    if [[ ! -e "$1" && ! -L "$1" ]]; then mkdir -m 700 "$1"; fi
    [[ -d "$1" && ! -L "$1" && "$(readlink -f "$1")" == "$1" &&
       "$(stat -c '%a:%u' "$1")" == "700:$EUID" ]] || fail unsafe-log-directory
}
prepare_private_log() {
    private_log_directory "$LOG_BASE"
    [[ ! -e "$LOG_DIR" && ! -L "$LOG_DIR" ]] || fail reused-log-directory
    private_log_directory "$LOG_DIR"
    (set -o noclobber; : > "$DAEMON_LOG") || fail log-create-failed
    chmod 600 "$DAEMON_LOG"
    regular "$DAEMON_LOG"
    [[ "$(stat -c '%a:%u:%h' "$DAEMON_LOG")" == "600:$EUID:1" ]] || fail unsafe-log-file
    exec {DAEMON_LOG_FD}>> "$DAEMON_LOG"
    [[ "$(stat -Lc '%a:%u:%h' "/proc/$$/fd/$DAEMON_LOG_FD")" == "600:$EUID:1" &&
       "$(stat -Lc '%d:%i' "/proc/$$/fd/$DAEMON_LOG_FD")" == \
       "$(stat -c '%d:%i' "$DAEMON_LOG")" ]] || fail log-descriptor-changed
    printf 'CANDIDATE_LOG %s\n' "$DAEMON_LOG"
}
ready() {
    local i body start=
    for ((i=0; i<20; i++)); do
        [[ -r "$PROC/$LAUNCHED_PID/stat" ]] || fail launched-daemon-exited
        birth "$LAUNCHED_PID"
        [[ -z $start || $START == "$start" ]] || fail launched-pid-reused
        start=$START
        # nohup/env may not have exec'd yet. Only accept the actual candidate inode.
        if [[ "$(readlink "$PROC/$LAUNCHED_PID/exe")" == "$BUNDLE/misterplexd" ]]; then
            daemon_identity "$LAUNCHED_PID" "$BUNDLE/misterplexd" "$ARM_SHA" "$BUNDLE/misterplex.conf"
            if owns_port "$LAUNCHED_PID"; then
                if body=$(wget -q -t 1 -T 2 -O - http://127.0.0.1:3005/resources); then
                    if [[ $body == *"<Player "* && $body == *'machineIdentifier="misterplex-dev"'* ]]; then
                        same_process "$LAUNCHED_PID" || fail ready-daemon-exited
                        owns_port "$LAUNCHED_PID" || fail ready-listener-disappeared
                        main_core "$BUNDLE/Plex.rbf" || fail ready-core-changed
                        verify_preserved
                        printf 'CANDIDATE_READY pid=%s\n' "$LAUNCHED_PID"
                        return
                    fi
                fi
            fi
        fi
        sleep 0.5
    done
    fail daemon-readiness-timeout
}

# Transaction begins here.
[[ "$(id -u)" == 0 ]] || fail root-required
for tool in sha256sum readlink stat awk grep cmp cp mv base64 flock wget nohup env sync timeout python3; do
    command -v "$tool" >/dev/null || fail missing-device-tool
done
[[ -d "$BASE" && ! -L "$BASE" ]] || fail missing-device-layout
preflight
mkdir -p "$BASE/run" "$BASE/candidates"
[[ "$(readlink -f "$BASE/run")" == "$BASE/run" &&
   "$(readlink -f "$BASE/candidates")" == "$BASE/candidates" ]] || fail noncanonical-directory
[[ ! -L "$BASE/run/candidate-deploy.lock" ]] || fail noncanonical-lock
exec 9>> "$BASE/run/candidate-deploy.lock"
flock -n 9 || fail deployment-lease-busy
preflight
[[ ! -e "$STAGE" && ! -e "$BUNDLE" ]] || fail transaction-already-exists
mkdir "$STAGE"
phase staging
# INSERT_VERIFIED_UPLOADS
mkdir "$STAGE/rollback" "$STAGE/run"
backup_file "$FROZEN_RBF" Plex_480p.rbf
backup_file "$FROZEN_ARM" misterplexd.480p
backup_file "$OLD_RBF" current.rbf
backup_file "$OLD_ARM" current.arm
backup_file "$OLD_CONF" current.conf
backup_file "$BASE/misterplex.conf" generic.conf
backup_file "$BASE/bin/misterplexd" generic.arm
# The existing config is data, including literal shell metacharacters and secrets.
awk 'NR==FNR {p=index($0,"="); key=substr($0,1,p-1); replacement[key]=$0; order[++n]=key; next}
     {line=$0; sub(/^[ \t]*/,"",line); p=index(line,"="); key=substr(line,1,p-1);
      sub(/[ \t]*$/,"",key); if (p && key in replacement) next; print}
     END {for(i=1;i<=n;i++) print replacement[order[i]]}' \
    "$STAGE/overlay.conf" "$STAGE/rollback/current.conf" > "$STAGE/misterplex.conf"
CONFIG_DIGEST=$(sha256sum "$STAGE/misterplex.conf")
readonly CONFIG_SHA=${CONFIG_DIGEST%% *}
chmod 700 "$STAGE/misterplexd"
chmod 600 "$STAGE/misterplex.conf"
printf '%s\n' "$BUILD_ID" "$RBF_SHA" "$ARM_SHA" > "$STAGE/approved-identity"
sync
mv "$STAGE" "$BUNDLE"
verify_preserved
printf 'CANDIDATE_STAGED %s\n' "$BUNDLE"
[[ $MODE == menu ]] || exit 0
prepare_private_log
phase quiescing
preflight
collect_children
for pid in "${WATCH[@]}"; do stop_one "$pid"; done
# A watcher can spawn a supervisor while retiring; rediscover before stopping it.
discover
[[ ${#WATCH[@]} == 0 ]] || fail watcher-respawned
collect_children
for pid in "${SUPERVISE[@]}"; do stop_one "$pid"; done
discover
[[ ${#WATCH[@]} == 0 && ${#SUPERVISE[@]} == 0 ]] || fail supervisor-respawned
collect_children
for pid in "${DAEMONS[@]}"; do
    [[ ${PARENT[$pid]} == 1 || ! -e "$PROC/${PARENT[$pid]}/stat" ]] ||
        fail unapproved-live-daemon-parent
    stop_one "$pid"
done
wait_children
require_quiescent
verify_preserved
check_sha "$BUNDLE/Plex.rbf" "$RBF_SHA"
check_sha "$BUNDLE/misterplexd" "$ARM_SHA"
check_sha "$BUNDLE/misterplex.conf" "$CONFIG_SHA"
known_core
if ! main_core "$MEDIA/menu.rbf"; then
    phase menu
    begin_main_transition "$MEDIA/menu.rbf"
    send_core "$MEDIA/menu.rbf"
    wait_main "$MEDIA/menu.rbf"
    require_quiescent
fi
phase plex
begin_main_transition "$BUNDLE/Plex.rbf"
send_core "$BUNDLE/Plex.rbf"
wait_main "$BUNDLE/Plex.rbf"
require_quiescent
main_core "$BUNDLE/Plex.rbf" || fail unexpected-active-core
phase starting
launch_candidate
ready
