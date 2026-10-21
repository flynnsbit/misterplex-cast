#!/bin/sh
# load_core guard v2 (mister-loadcore-guard wrap-script): every load_core below runs only when no daemon runs
# mister-loadcore-guard v2 remote functions (POSIX sh: busybox ash, bash, bash --posix)
_mlc_save(){
  _mlc_sf=; case $- in *f*) _mlc_sf=1; set +f ;; esac
  if [ -n "${IFS+x}" ]; then _mlc_si=1; _mlc_sv=$IFS; else _mlc_si=; fi
  IFS=$(printf ' \t\nx'); IFS=${IFS%x}
}
_mlc_restore(){
  if [ -n "$_mlc_si" ]; then IFS=$_mlc_sv; else unset IFS; fi
  if [ -n "$_mlc_sf" ]; then set -f; fi
  return 0
}
_mlc_flag(){ if [ -n "${MLC_FLAG_FILE:-}" ]; then echo "$1" > "$MLC_FLAG_FILE" 2>/dev/null; fi; return 0; }
_mlc_id(){
  _mlc_h=$(uname -n 2>/dev/null) || _mlc_h=?
  _mlc_fx=; if [ -n "$_mlc_c_fix" ]; then _mlc_fx=" FIXTURE[proc=$_mlc_c_R]"; fi
}
_mlc_abort(){ # rc reason...
  _mlc_ar=$1; shift
  _mlc_m="LOADCORE_GUARD_ABORT host=$_mlc_h$_mlc_fx refused load_core $_mlc_w: $*; nothing was loaded"
  echo "$_mlc_m" >&2; echo "$_mlc_m"; _mlc_flag "$_mlc_ar"; return "$_mlc_ar"
}
_mlc_hit(){
  case " $_mlc_pids " in *" $1 "*) ;; *) _mlc_pids="$_mlc_pids $1" ;; esac
  _mlc_sig="$_mlc_sig $1:$2"
}
_mlc_ismain(){
  _mlc_me=$(readlink "$_mlc_R/$1/exe" 2>/dev/null) || _mlc_me=
  for _mlc_mx in $_mlc_c_main; do
    case $_mlc_me in "$_mlc_mx"|"$_mlc_mx (deleted)") return 0 ;; esac
  done
  return 1
}
# udp :32412 is also Plex GDM discovery: a binder whose exe is one of the known Plex Media Server
# binaries, by full path (_mlc_c_gdm, ':'-separated, sealed), is not a daemon. Only the udp rule
# skips it; maps, fds, lock and tcp still count. A binary of that name elsewhere is counted.
_mlc_gdm(){
  _mlc_gb=$(readlink "$_mlc_R/$1/exe" 2>/dev/null) || return 1
  _mlc_gb=${_mlc_gb% (deleted)}
  _mlc_gl=$_mlc_c_gdm
  while [ -n "$_mlc_gl" ]; do
    [ "$_mlc_gb" = "${_mlc_gl%%:*}" ] && return 0
    case $_mlc_gl in *:*) _mlc_gl=${_mlc_gl#*:} ;; *) _mlc_gl= ;; esac
  done
  return 1
}
_mlc_net(){
  [ -r "$1" ] || return 0
  while read -r _mlc_n1 _mlc_la _mlc_n3 _mlc_st _mlc_n5 _mlc_n6 _mlc_n7 _mlc_n8 _mlc_n9 _mlc_in _mlc_rest; do
    _mlc_pt=${_mlc_la##*:}
    if [ "$2" = tcp ]; then
      [ "$_mlc_st" = 0A ] || continue
      _mlc_pl=$_mlc_tcpx
    else
      _mlc_pl=$_mlc_udpx
    fi
    for _mlc_px in $_mlc_pl; do
      if [ "$_mlc_pt" = "${_mlc_px%%=*}" ]; then _mlc_ino="$_mlc_ino $_mlc_in=$2/${_mlc_px#*=}"; fi
    done
  done < "$1"
  return 0
}
_mlc_guard_body(){ # WHAT MODE: guard (before a load), check / checkv (report), pre / prev (restore pre-check)
  _mlc_w=${1:-?}; _mlc_md=${2:-guard}; _mlc_R=$_mlc_c_R; _mlc_L=$_mlc_c_L; _mlc_S=$_mlc_c_S
  _mlc_id
  _mlc_pids=; _mlc_sig=; _mlc_ino=; _mlc_sock=; _mlc_bad=; _mlc_ign=; _mlc_oth=; _mlc_mainp=; _mlc_ld=; _mlc_li=
  for _mlc_t in readlink tr head grep ls; do
    command -v "$_mlc_t" >/dev/null 2>&1 || _mlc_bad="$_mlc_bad $_mlc_t"
  done
  [ -z "$_mlc_bad" ] || { _mlc_abort 98 "cannot verify, missing tool(s):$_mlc_bad"; return $?; }
  _mlc_np=0; _mlc_nm=0
  for _mlc_d in "$_mlc_R"/[0-9]*; do
    [ -d "$_mlc_d" ] || continue
    _mlc_p=${_mlc_d##*/}
    [ "$_mlc_p" = "$$" ] && continue
    _mlc_np=$((_mlc_np + 1))
    [ -r "$_mlc_d/maps" ] && _mlc_nm=$((_mlc_nm + 1))
    _mlc_e=$(readlink "$_mlc_d/exe" 2>/dev/null) || _mlc_e=
    _mlc_a=$(tr '\000' '\n' 2>/dev/null < "$_mlc_d/cmdline" | head -n 1) || _mlc_a=
    case ${_mlc_e##*/} in
      *misterplexd*) _mlc_hit "$_mlc_p" name ;;
      *) case ${_mlc_a##*/} in
           *misterplexd*) _mlc_hit "$_mlc_p" name ;;
           *) case ${_mlc_e##*/}/${_mlc_a##*/} in *misterplex*) _mlc_oth="$_mlc_oth pid=$_mlc_p exe=${_mlc_e:-unreadable} argv0=${_mlc_a:-none};" ;; esac ;;
         esac ;;
    esac
  done
  _mlc_miss=
  [ "$_mlc_np" -gt 0 ] || _mlc_miss="$_mlc_miss no processes under $_mlc_R;"
  [ "$_mlc_nm" -gt 0 ] || _mlc_miss="$_mlc_miss no readable $_mlc_R/PID/maps;"
  for _mlc_x in net/tcp net/udp locks; do [ -r "$_mlc_R/$_mlc_x" ] || _mlc_miss="$_mlc_miss no $_mlc_R/$_mlc_x;"; done
  [ -z "$_mlc_miss" ] || { _mlc_abort 98 "cannot verify:$_mlc_miss"; return $?; }
  for _mlc_f in $(grep -l -E '/dev/(mem|mplex_ddr)' "$_mlc_R"/[0-9]*/maps 2>/dev/null); do
    _mlc_p=${_mlc_f%/maps}; _mlc_p=${_mlc_p##*/}
    [ "$_mlc_p" = "$$" ] && continue
    if _mlc_ismain "$_mlc_p"; then _mlc_mainp="$_mlc_mainp $_mlc_p"; continue; fi
    if grep -q /dev/mem "$_mlc_f" 2>/dev/null; then _mlc_hit "$_mlc_p" maps:/dev/mem; fi
    if grep -q /dev/mplex_ddr "$_mlc_f" 2>/dev/null; then _mlc_hit "$_mlc_p" maps:/dev/mplex_ddr; fi
  done
  _mlc_fds=$(ls -l "$_mlc_R"/[0-9]*/fd/* 2>/dev/null) || :
  case $_mlc_fds in *' -> '*) ;; *) _mlc_abort 98 "cannot verify: no readable $_mlc_R/PID/fd entries"; return $? ;; esac
  while IFS= read -r _mlc_l; do
    case $_mlc_l in *' -> '*) ;; *) continue ;; esac
    _mlc_t=${_mlc_l#* -> }; _mlc_n=${_mlc_l%% -> *}; _mlc_n=${_mlc_n##* }
    _mlc_p=${_mlc_n%/fd/*}; _mlc_p=${_mlc_p##*/}
    [ "$_mlc_p" = "$$" ] && continue
    case $_mlc_t in
      "$_mlc_L"|"$_mlc_L (deleted)") _mlc_hit "$_mlc_p" "lock:$_mlc_L" ;;
      "$_mlc_S"|"$_mlc_S (deleted)") _mlc_hit "$_mlc_p" "spilock:$_mlc_S" ;;
      /dev/mem|/dev/mplex_ddr*) _mlc_ismain "$_mlc_p" || _mlc_hit "$_mlc_p" "fd:$_mlc_t" ;;
      socket:\[*\]) _mlc_t=${_mlc_t#socket:\[}; _mlc_sock="$_mlc_sock ${_mlc_t%\]}=$_mlc_p" ;;
    esac
  done <<_MLC_FDS_
$_mlc_fds
_MLC_FDS_
  if [ -e "$_mlc_L" ]; then
    set -- $(ls -di "$_mlc_L" 2>/dev/null)
    _mlc_li=${1:-}
    # /proc/locks names the file as MAJ:MIN:INODE (kernel %02x:%02x:%lu). Match the device too when
    # stat gives it; without stat (or an odd value) fall back to the inode alone (more matches, never fewer).
    _mlc_x=$(stat -c %d "$_mlc_L" 2>/dev/null) || _mlc_x=
    case $_mlc_x in
      ''|*[!0-9]*|??????????*) _mlc_ld= ;;
      *) _mlc_ld=$(printf '%02x:%02x' $(( (_mlc_x >> 8) & 4095 )) $(( (_mlc_x & 255) | ((_mlc_x >> 12) & 1048320) ))) || _mlc_ld= ;;
    esac
    if [ -n "$_mlc_li" ]; then
      while read -r _mlc_l; do
        _mlc_q=
        for _mlc_x in $_mlc_l; do
          case $_mlc_x in
            *:*:*) if [ "${_mlc_x##*:}" = "$_mlc_li" ] && { [ -z "$_mlc_ld" ] || [ "${_mlc_x%:*}" = "$_mlc_ld" ]; } \
                      && [ -n "$_mlc_q" ] && [ "$_mlc_q" != "$$" ]; then _mlc_hit "$_mlc_q" "flock:$_mlc_L"; fi; break ;;
          esac
          _mlc_q=$_mlc_x
        done
      done < "$_mlc_R/locks"
    fi
  fi
  _mlc_tcpx=; for _mlc_x in $_mlc_c_tcp; do _mlc_tcpx="$_mlc_tcpx $(printf '%04X' "$_mlc_x")=$_mlc_x"; done
  _mlc_udpx=; for _mlc_x in $_mlc_c_udp; do _mlc_udpx="$_mlc_udpx $(printf '%04X' "$_mlc_x")=$_mlc_x"; done
  _mlc_net "$_mlc_R/net/tcp" tcp; _mlc_net "$_mlc_R/net/tcp6" tcp
  _mlc_net "$_mlc_R/net/udp" udp; _mlc_net "$_mlc_R/net/udp6" udp
  for _mlc_x in $_mlc_ino; do
    _mlc_i=${_mlc_x%%=*}; _mlc_v=${_mlc_x#*=}; _mlc_o=
    for _mlc_y in $_mlc_sock; do
      if [ "${_mlc_y%%=*}" = "$_mlc_i" ]; then
        _mlc_o=1; _mlc_q=${_mlc_y#*=}
        case $_mlc_v in
          udp/*) if _mlc_gdm "$_mlc_q"; then _mlc_ign="$_mlc_ign pid=$_mlc_q exe=[$_mlc_gb] $_mlc_v (Plex discovery);"; continue; fi ;;
        esac
        _mlc_hit "$_mlc_q" "port:$_mlc_v"
      fi
    done
    [ -n "$_mlc_o" ] || _mlc_hit unknown "port:$_mlc_v,inode=$_mlc_i,no-owner-fd"
  done
  _mlc_igs=; [ -z "$_mlc_ign" ] || _mlc_igs="; not counted:$_mlc_ign"
  case $_mlc_md in checkv|prev) _mlc_info ;; esac
  if [ -z "$_mlc_pids" ]; then
    case $_mlc_md in
      pre*) echo "LOADCORE_GUARD_PRECHECK_OK host=$_mlc_h$_mlc_fx: no daemon and no other holder among $_mlc_np processes, nothing to stop$_mlc_igs" >&2 ;;
      *) echo "LOADCORE_GUARD_OK host=$_mlc_h$_mlc_fx before load_core $_mlc_w: no daemon among $_mlc_np processes (name, /dev/mem+/dev/mplex_ddr maps and fds, $_mlc_L and $_mlc_S holders, tcp $_mlc_c_tcp udp $_mlc_c_udp)$_mlc_igs" >&2 ;;
    esac
    return 0
  fi
  _mlc_n=0; _mlc_hits=; _mlc_nk=0; _mlc_kh=; _mlc_nb=0; _mlc_bh=
  for _mlc_p in $_mlc_pids; do
    _mlc_n=$((_mlc_n + 1)); _mlc_why=
    for _mlc_x in $_mlc_sig; do
      case $_mlc_x in "$_mlc_p":*) _mlc_why="$_mlc_why${_mlc_why:+,}${_mlc_x#*:}" ;; esac
    done
    _mlc_e=; _mlc_kl=
    if [ "$_mlc_p" = unknown ]; then
      _mlc_d="pid=unknown why=$_mlc_why"
    else
      _mlc_e=$(readlink "$_mlc_R/$_mlc_p/exe" 2>/dev/null) || _mlc_e=
      _mlc_a=$(tr '\000' '\n' 2>/dev/null < "$_mlc_R/$_mlc_p/cmdline" | head -n 1) || _mlc_a=
      _mlc_d="pid=$_mlc_p exe=${_mlc_e:-unreadable} argv0=${_mlc_a:-none} why=$_mlc_why"
      # exactly what prod_kill_strays all stops: a readable exe path containing misterplexd (deleted or not)
      case $_mlc_e in *misterplexd*) _mlc_kl=1 ;; esac
    fi
    _mlc_hits="$_mlc_hits $_mlc_d;"
    if [ -n "$_mlc_kl" ]; then _mlc_nk=$((_mlc_nk + 1)); _mlc_kh="$_mlc_kh $_mlc_d;"
    else _mlc_nb=$((_mlc_nb + 1)); _mlc_bh="$_mlc_bh $_mlc_d;"; fi
  done
  case $_mlc_md in
    pre*)
      if [ "$_mlc_nb" = 0 ]; then
        echo "LOADCORE_GUARD_PRECHECK_OK host=$_mlc_h$_mlc_fx: $_mlc_nk misterplexd process(es), all stopped by prod_kill_strays all (exe path contains misterplexd):$_mlc_kh no other holder$_mlc_igs" >&2
        return 0
      fi
      _mlc_m="LOADCORE_GUARD_PRECHECK_BLOCKED host=$_mlc_h$_mlc_fx: $_mlc_nb holder(s) that prod_kill_strays all will not stop:$_mlc_bh killable:${_mlc_kh:- none;} nothing was stopped or swapped"
      echo "$_mlc_m" >&2; echo "$_mlc_m"; return 97 ;;
  esac
  if [ "$_mlc_md" = guard ] && [ -n "$_mlc_c_allow" ]; then
    echo "LOADCORE_GUARD_OVERRIDE host=$_mlc_h$_mlc_fx reason=[$_mlc_c_allow] load_core $_mlc_w with $_mlc_n daemon process(es) running:$_mlc_hits" >&2
    return 0
  fi
  _mlc_abort 97 "$_mlc_n daemon process(es) running:$_mlc_hits stop every misterplexd and lab daemon first (stopd / prod_kill_strays all)$_mlc_igs"
}
# check --verbose / precheck --verbose: what the detectors saw but did not count
_mlc_info(){
  _mlc_m="LOADCORE_GUARD_INFO host=$_mlc_h$_mlc_fx"
  echo "$_mlc_m sources: $_mlc_np processes, $_mlc_nm readable maps, proc=$_mlc_R tcp=[$_mlc_c_tcp] udp=[$_mlc_c_udp] main=[$_mlc_c_main] override=$([ -n "$_mlc_c_allow" ] && echo "set, ignored by check" || echo none)" >&2
  if [ -z "$_mlc_li" ]; then echo "$_mlc_m lock $_mlc_L: absent or no inode" >&2
  elif [ -n "$_mlc_ld" ]; then echo "$_mlc_m lock $_mlc_L: /proc/locks match on dev:ino $_mlc_ld:$_mlc_li" >&2
  else echo "$_mlc_m lock $_mlc_L: no stat device, /proc/locks match on inode $_mlc_li only" >&2; fi
  echo "$_mlc_m not counted, Main mapping /dev/mem: pids=[${_mlc_mainp# }]" >&2
  echo "$_mlc_m not counted, udp Plex discovery binders:${_mlc_ign:- none} (known Plex Media Server exes: $_mlc_c_gdm)" >&2
  echo "$_mlc_m not counted, other misterplex processes (no daemon trait):${_mlc_oth:- none}" >&2
  return 0
}
mlc_guard(){ _mlc_save; _mlc_rc=0; _mlc_guard_body "${1:-?}" guard || _mlc_rc=$?; _mlc_restore; [ "$_mlc_rc" != 0 ] || _mlc_ok=1; return $_mlc_rc; }
# report only (never honours MLC_ALLOW_DAEMON): check | checkv | pre | prev
mlc_check(){ _mlc_save; _mlc_rc=0
  case ${1:-check} in check|checkv) _mlc_guard_body "(check)" "${1:-check}" || _mlc_rc=$? ;; *) _mlc_guard_body "(precheck)" "$1" || _mlc_rc=$? ;; esac
  _mlc_restore; return $_mlc_rc; }
mlc_rc(){ case $1 in 97|98) _mlc_flag "$1"; exit "$1" ;; esac; return "$1"; }
# after a pipeline, $(...) or child shell: a refusal recorded anywhere below stops this shell now
mlc_flagrc(){
  if [ -n "${MLC_FLAG_FILE:-}" ] && [ -s "$MLC_FLAG_FILE" ]; then
    _mlc_fr=; read -r _mlc_fr < "$MLC_FLAG_FILE" 2>/dev/null; exit "${_mlc_fr:-98}"
  fi
  return "$1"
}
_mlc_xset(){ _mlc_xv=$1; shift; _mlc_xwhy=$*; }
_mlc_xgrep(){ # verdict-on-error grep: rc 0 match, 1 none, anything else -> refuse (cannot verify)
  _mlc_xr=0; grep "$@" 2>/dev/null || _mlc_xr=$?
  case $_mlc_xr in 0|1) return $_mlc_xr ;; esac
  _mlc_xset refuse "$_mlc_xf cannot be read (grep rc $_mlc_xr)"; return 2
}
_mlc_xngrep(){ # ERE on $_mlc_xf with quotes and backslashes removed: 0 match, 1 none, 2 refuse (cannot verify)
  [ -r "$_mlc_xf" ] || { _mlc_xset refuse "$_mlc_xf cannot be read"; return 2; }
  _mlc_xr=0; tr -d "\"'\\\\" < "$_mlc_xf" 2>/dev/null | grep -q -E "$1" 2>/dev/null || _mlc_xr=$?
  case $_mlc_xr in 0|1) return $_mlc_xr ;; esac
  _mlc_xset refuse "$_mlc_xf cannot be read (grep rc $_mlc_xr)"; return 2
}
_mlc_xfile(){ # one file: sets _mlc_xv (ok|self|guard|refuse), may queue more files
  _mlc_xf=$1
  [ -e "$_mlc_xf" ] || return 0
  if [ ! -f "$_mlc_xf" ] || [ ! -r "$_mlc_xf" ]; then _mlc_xset refuse "$_mlc_xf is not a readable regular file"; return; fi
  _mlc_xe=$(dd if="$_mlc_xf" bs=4 count=1 2>/dev/null | tr -d '\000') || { _mlc_xset refuse "$_mlc_xf cannot be read"; return; }
  case $_mlc_xe in
    ?ELF)
      _mlc_xgrep -q load_core "$_mlc_xf"
      case $? in 0) _mlc_xset guard "$_mlc_xf (ELF) holds load_core" ;; esac
      return ;;
  esac
  _mlc_xgrep -q -F '# load_core guard v2 (mister-loadcore-guard wrap-script)' "$_mlc_xf"
  case $? in
    0)
      _mlc_xt=$(tail -n 1 "$_mlc_xf" 2>/dev/null) || _mlc_xt=
      case $_mlc_xt in
        '# mister-loadcore-guard v2 digest '*) ;;
        *) _mlc_xset refuse "$_mlc_xf carries the wrap-script marker but no digest"; return ;;
      esac
      _mlc_xd=$(sed '$d' "$_mlc_xf" 2>/dev/null | md5sum 2>/dev/null) || _mlc_xd=
      _mlc_xd=${_mlc_xd%% *}
      if [ -z "$_mlc_xd" ] || [ "$_mlc_xd" != "${_mlc_xt##* }" ]; then
        _mlc_xset refuse "$_mlc_xf was edited after wrap-script (digest mismatch)"; return
      fi
      [ "$_mlc_xv" = ok ] && _mlc_xset self "$_mlc_xf checks each of its own loads"
      return ;;
    2) return ;;
  esac
  if [ -n "$_mlc_xI" ]; then
    # Sourced code runs in this shell, next to the guard's functions. Quotes and backslashes are
    # removed before matching, so spliced names (m""lc_guard) count too. Keep SHADOW in sync with
    # 31-analyze.py.
    _mlc_xngrep '(^|[^A-Za-z0-9_])(_mlc_[A-Za-z0-9_]*|mlc_[A-Za-z0-9_]+|MLC_FLAG_FILE)'
    case $? in 0) _mlc_xset refuse "$_mlc_xf (sourced) touches the guard's own names"; return ;; 2) return ;; esac
    _mlc_xngrep '(^|[;&|({[:space:]])trap([[:space:]]|$)'
    case $? in 0) _mlc_xset refuse "$_mlc_xf (sourced) sets a trap; the guard needs the EXIT trap"; return ;; 2) return ;; esac
    _mlc_xngrep '(^|[^A-Za-z0-9_])(unset|alias|eval|readonly|declare|typeset)([^A-Za-z0-9_]|$)'
    case $? in 0) _mlc_xset refuse "$_mlc_xf (sourced) uses unset, alias, eval, readonly, declare or typeset (run it as a script instead)"; return ;; 2) return ;; esac
    _mlc_xgrep -q -F "\$'" "$_mlc_xf"
    case $? in 0) _mlc_xset refuse "$_mlc_xf (sourced) uses \$'...' quoting the guard cannot read"; return ;; 2) return ;; esac
    _mlc_xngrep '(^|[;&|({[:space:]])(function[[:space:]]+)?(\.|:|\[|alias|awk|basename|bash|break|builtin|cat|cd|command|continue|cut|dd|declare|dirname|echo|env|eval|exec|exit|export|expr|false|find|getopts|grep|hash|head|kill|ln|local|ls|md5sum|mkdir|pidof|printf|pwd|read|readlink|readonly|return|rm|sed|set|sh|shift|sleep|sort|source|stat|tee|test|timeout|tr|trap|true|type|typeset|umask|uname|unalias|unset|wait|wc|xargs)[[:space:]]*\([[:space:]]*\)|(^|[;&|({[:space:]])function[[:space:]]+(\.|:|\[|alias|awk|basename|bash|break|builtin|cat|cd|command|continue|cut|dd|declare|dirname|echo|env|eval|exec|exit|export|expr|false|find|getopts|grep|hash|head|kill|ln|local|ls|md5sum|mkdir|pidof|printf|pwd|read|readlink|readonly|return|rm|sed|set|sh|shift|sleep|sort|source|stat|tee|test|timeout|tr|trap|true|type|typeset|umask|uname|unalias|unset|wait|wc|xargs)([^A-Za-z0-9_]|$)'
    case $? in 0) _mlc_xset refuse "$_mlc_xf (sourced) defines a function named like a builtin or a tool the guard runs"; return ;; 2) return ;; esac
  fi
  _mlc_xr=0; grep -v '^[[:space:]]*#' "$_mlc_xf" 2>/dev/null | grep -q -E 'load_core[^#]*>[[:space:]]*/dev/MiSTer_cmd' || _mlc_xr=$?
  case $_mlc_xr in
    0) _mlc_xset refuse "$_mlc_xf writes load_core without the inline guard (run it through mister-loadcore-guard wrap-script)"; return ;;
    1) ;;
    *) _mlc_xset refuse "$_mlc_xf cannot be read"; return ;;
  esac
  _mlc_xgrep -q -E 'load_core|MiSTer_cmd|switch_misterplex_mode|plexctl\.sh|misterplex_release_f12|MiSTerPlex_Mode' "$_mlc_xf"
  case $? in
    0) _mlc_xset guard "$_mlc_xf mentions load_core, MiSTer_cmd or a MiSTer loader"; return ;;
    2) return ;;
  esac
  _mlc_xr=0
  grep -v '^[[:space:]]*#' "$_mlc_xf" 2>/dev/null | grep -q -E '(^|[;&|[:space:]])(sh|bash|ash|dash|source|\.)[[:space:]]+[^-/$[:space:]#]|(^|[;&|[:space:]])\./' || _mlc_xr=$?
  case $_mlc_xr in
    0) _mlc_xset guard "$_mlc_xf runs a script by a relative path"; return ;;
    1) ;;
    *) _mlc_xset refuse "$_mlc_xf cannot be read"; return ;;
  esac
  for _mlc_xa in $(grep -o -E '/[A-Za-z0-9_./+-]+' "$_mlc_xf" 2>/dev/null); do
    case $_mlc_xa in
      *.sh|*.py|*.pl|*.bash) _mlc_xq="$_mlc_xq $_mlc_xa" ;;
      /tmp/?*|/media/fat/?*)
        if [ -f "$_mlc_xa" ] && [ -x "$_mlc_xa" ]; then
          _mlc_xs=$(wc -c < "$_mlc_xa" 2>/dev/null) || _mlc_xs=0
          [ "${_mlc_xs:-0}" -le 1048576 ] && _mlc_xq="$_mlc_xq $_mlc_xa"
        fi ;;
    esac
  done
  return 0
}
_mlc_xcheck(){ # TYPE NAME: C command word, S script operand, I sourced script. Sets _mlc_xv, _mlc_xwhy.
  _mlc_xv=ok; _mlc_xwhy=; _mlc_xq=; _mlc_xseen=; _mlc_xn=0; _mlc_xI=; _mlc_xf=$2
  [ "$1" = I ] && _mlc_xI=1
  for _mlc_t in dd tr sed md5sum tail grep wc; do
    command -v "$_mlc_t" >/dev/null 2>&1 || { _mlc_xset refuse "cannot check $2: $_mlc_t missing"; return 0; }
  done
  case $2 in
    */*) _mlc_xq=$2 ;;
    *)
      _mlc_xp=$(command -v "$2" 2>/dev/null) || _mlc_xp=
      if [ "$1" = C ]; then
        case $_mlc_xp in
          ''|"$2") return 0 ;;
          /*) _mlc_xq=$_mlc_xp ;;
          *) _mlc_xset guard "$2 is an alias ($_mlc_xp)"; return 0 ;;
        esac
      else
        [ -e "./$2" ] && _mlc_xq="./$2"
        case $_mlc_xp in /*) _mlc_xq="$_mlc_xq $_mlc_xp" ;; esac
      fi ;;
  esac
  while [ -n "$_mlc_xq" ]; do
    set -- $_mlc_xq
    [ $# -gt 0 ] || break
    _mlc_xg=$1; shift; _mlc_xq=$*
    case " $_mlc_xseen " in *" $_mlc_xg "*) continue ;; esac
    _mlc_xseen="$_mlc_xseen $_mlc_xg"; _mlc_xn=$((_mlc_xn + 1))
    if [ "$_mlc_xn" -gt 32 ]; then _mlc_xset refuse "more than 32 nested scripts"; return 0; fi
    _mlc_xfile "$_mlc_xg"
    case $_mlc_xv in guard|refuse) return 0 ;; esac
  done
  return 0
}
_mlc_first(){ _mlc_f1=${1:-}; }
mlc_pre(){
  _mlc_save; _mlc_pw=; _mlc_pr=; _mlc_ph=
  for _mlc_px in "$@"; do
    _mlc_pt=; _mlc_pn=
    case $_mlc_px in
      -h) _mlc_ph=1 ;;
      G:*) _mlc_pw="$_mlc_pw ${_mlc_px#G:};" ;;
      S:*|C:*|I:*) _mlc_pt=${_mlc_px%%:*}; _mlc_pn=${_mlc_px#?:} ;;
      D:*|V:*|J:*) _mlc_first ${_mlc_px#?:}; _mlc_pn=$_mlc_f1
                   case $_mlc_px in D:*) _mlc_pt=C ;; V:*) _mlc_pt=S ;; *) _mlc_pt=I ;; esac ;;
    esac
    [ -n "$_mlc_pn" ] || continue
    _mlc_xcheck "$_mlc_pt" "$_mlc_pn"
    case $_mlc_xv in
      guard) _mlc_pw="$_mlc_pw $_mlc_pn ($_mlc_xwhy);" ;;
      self) [ -n "$_mlc_ph" ] && _mlc_pw="$_mlc_pw $_mlc_pn ($_mlc_xwhy; started in a pipeline or in the background);" ;;
      refuse) _mlc_pr="$_mlc_pr $_mlc_xwhy;" ;;
    esac
  done
  _mlc_rc=0
  if [ -n "$_mlc_pr" ]; then
    _mlc_id; _mlc_w="via a script"; _mlc_abort 98 "cannot place a check inside it:$_mlc_pr" || _mlc_rc=$?
  elif [ -n "$_mlc_pw" ]; then
    _mlc_guard_body "via$_mlc_pw" || _mlc_rc=$?
  fi
  _mlc_restore; [ "$_mlc_rc" != 0 ] || _mlc_ok=1; return $_mlc_rc
}
case $(readonly -p 2>/dev/null) in *_mlc_c_R=*) ;; *) _mlc_c_R='/proc'; _mlc_c_L='/tmp/misterplexd.lock'; _mlc_c_S='/tmp/misterplex_spi.lock'; _mlc_c_tcp='3005'; _mlc_c_udp='32412'; _mlc_c_main='/media/fat/MiSTer'; _mlc_c_gdm='/usr/lib/plexmediaserver/Plex Media Server:/usr/lib/plexmediaserver/Plex DLNA Server:/usr/lib/plexmediaserver/Plex Tuner Service'; _mlc_c_allow=''; _mlc_c_fix=''; readonly _mlc_c_R _mlc_c_L _mlc_c_S _mlc_c_tcp _mlc_c_udp _mlc_c_main _mlc_c_gdm _mlc_c_allow _mlc_c_fix ;; esac
if [ -z "${MLC_FLAG_FILE:-}" ]; then MLC_FLAG_FILE=/tmp/.mlc-refused.$$; export MLC_FLAG_FILE; rm -f "$MLC_FLAG_FILE"; trap '_mlc_x=$?; if [ -s "$MLC_FLAG_FILE" ]; then read -r _mlc_x < "$MLC_FLAG_FILE"; fi; rm -f "$MLC_FLAG_FILE"; exit "$_mlc_x"' EXIT; fi
# misterplexd-mast-load.sh RBF - the MAST-WEDGE hand-off's ONE core load (source; install the
# copy that `mister-loadcore-guard wrap-script` produces, which inlines the 0-misterplexd guard
# before the load and ends in its digest line). Called only by `misterplexd-boot.sh mast-reload`
# after the handing-off misterplexd has exited.
# P-a (review 01:19/01:25/01:28): right after the guard and right before load_core (the same
# guarded line below), withdraw any stale audio command from the DDR control page, so the new core
# never boots into a valid stale MACT/MACC (a candidate silent-core trigger, glass G2 01:03).
# Hygiene, never a second gate: the ONE load always follows.
# The write: busybox devmem with width 32 (O_SYNC /dev/mem, one mapped page) makes two stores to
# the 64-bit commit word only, 0x30140128-0x3014012F: 0x30140128 := 0 first (the 'MACC' magic half,
# which alone invalidates the command), then 0x3014012C := 0 (the publication half). Nothing else
# is written. Both halves are read back; a non-zero read-back logs ONE loud 'withdraw failed' line.
# R6 (review 02:08/02:17; H264 review/rca/r6-quiesce-input-1005.md §2-§5; RCA §11/§13): no DPB
# write traffic may be in flight when Main's fpga_core_reset(1) lands (fpga_io.cpp:474, before
# do_bridge :489 and the load :490). The feed is already stopped: the launcher runs this only after
# the handing-off misterplexd is gone (MR_EXIT_WAIT), and it holds its single-instance lock for its
# whole run, so no start launcher can start another daemon meanwhile. A daemon started by hand
# outside the launcher is caught by the guard's 0-misterplexd scan right before the load_core write
# (after the drain, the quiet wait and P-a): the guard then refuses the load (rc 97). The guard
# checks placed before the r6_quiesce and pa_withdraw calls are static (these are script
# functions, not loaders): they do not scan. Before P-a, the §5 raw path (reads only, 32-bit
# devmem): quiesced = PLXB 0x30140000 and
# PLXR 0x30140008 magics valid, written [62:32] == consumed [62:32], PLXT 0x30140018 magic valid
# with level [63:32] == 0, the same on 2 reads 50 ms apart. Then a 200 ms quiet wait timed from
# that second read (consumed is not decoded: the encoded-AU buffer can still hold an AU being
# decoded and committed), then P-a, then load_core. Not quiesced within MAST_LOAD_DRAIN_MS (3000):
# the load still happens, with ONE loud 'R6 NOT QUIESCED' line (boot log and FAILED file). No
# PLXB: nothing was ever fed, nothing to drain.
# R6 PORT SUSPECT (review 02:17): with CORENAME Plex and a CAPS row naming a known build (MPC4
# 0x30140070, the build at 0x30140074 in kRecognisedFpgaBuilds), a PLXR or PLXT magic that is
# wrong on 2 reads is the offset-port signature (RCA §12/§13). The loader then logs ONE loud line
# and the D10 0x300FF100-13F and 0x30140000-01F words, still withdraws (P-a), and does NOT load
# (rc 96: the launcher marks refused why=r6-port-suspect, reboot first). Each outcome goes to the
# reload marker as r6=... before the load, so a later silent core is attributed (the R5a line).
# Test-only overrides (unset on the box): MAST_LOAD_FAILED, MAST_LOAD_DRAIN_MS. Tests put a fake
# devmem first in PATH. From the launcher: MAST_LOAD_MARKER (its MARKER), MAST_LOAD_CORENAME.
RBF=$1
[ -n "$RBF" ] || { echo "misterplexd-mast-load: no RBF" >&2; exit 2; }
R6_DRAIN_MS=${MAST_LOAD_DRAIN_MS:-3000}
R6_MARKER=${MAST_LOAD_MARKER:-/tmp/misterplexd.mast-reload}
R6_CORENAME=${MAST_LOAD_CORENAME:-/tmp/CORENAME}
# kRecognisedFpgaBuilds (host/libmisterplex/fpga_build_ids.hpp); a launcher row keeps them equal.
R6_BUILDS="0x4800F36A 0x48004D57 0x48004D40 0x48005D40 0x48005D45 0x48005D50 0x48005D52 0x4800A1FB 0x4800A152 0x48009E36 0x48002731 0x4800C88F 0x48003ECB 0x480058BC 0x480094DD 0x48018AE0 0x48FF65F2 0x487A1BD1 0x48C5385C 0x48C51FF1 0x48CFEE31 0x48A64AED 0x48110015 0x48032F84 0x48038D9A 0x4800F40A 0x48006A7E 0x2400F36A 0x2400F40B 0x24003997 0x24F1A2C4 0x24A10026 0x24C07821 0x4801F40A 0x48F14A0B 0x48C078E2"
# One function of allowlisted commands only (devmem, read, cat, usleep, sleep, echo, printf, date):
# the guard wrap puts a check before every call of a script function, so nothing in here calls
# one. Returns 96 on R6 PORT SUSPECT (no load), else 0 after the 200 ms quiet wait.
r6_quiesce() {
  R6=
  if [ "$((0xFFFFFFFF))" != 4294967295 ]; then
    R6="not-quiesced (this shell's math is not 64-bit: nothing was judged)"
  fi
  cn=$(cat "$R6_CORENAME" 2>/dev/null)
  b0=$(devmem 0x30140070 32 2>/dev/null)
  b1=$(devmem 0x30140074 32 2>/dev/null)
  gate=0
  if [ "$cn" = Plex ] && [ "$b0" = 0x4D504334 ]; then
    case " $R6_BUILDS " in *" $b1 "*) gate=1 ;; esac
  fi
  read -r u _ < /proc/uptime
  cs=${u#*.}
  case $cs in 0?) cs=${cs#0} ;; esac
  t0=$((${u%.*} * 100 + cs))
  prev=
  bad=0
  while [ -z "$R6" ]; do
    c0=$(devmem 0x30140000 32 2>/dev/null)
    c1=$(devmem 0x30140004 32 2>/dev/null)
    r0=$(devmem 0x30140008 32 2>/dev/null)
    r1=$(devmem 0x3014000C 32 2>/dev/null)
    l0=$(devmem 0x30140018 32 2>/dev/null)
    l1=$(devmem 0x3014001C 32 2>/dev/null)
    read -r u _ < /proc/uptime
    cs=${u#*.}
    case $cs in 0?) cs=${cs#0} ;; esac
    ms=$(((${u%.*} * 100 + cs - t0) * 10))
    s="PLXB $c0:$c1 PLXR $r0:$r1 PLXT $l0:$l1"
    if [ "$c0" != 0x504C5842 ]; then
      R6="no-feed (CTRL [$c0] is not PLXB: nothing was fed, nothing to drain)"
      break
    fi
    if [ "$r0" != 0x504C5852 ] || [ "$l0" != 0x504C5854 ]; then
      bad=$((bad + 1))
      if [ "$gate" = 1 ] && [ "$bad" -ge 2 ]; then
        R6="port-suspect (PLXR/PLXT magic wrong on 2 reads, CORENAME Plex, CAPS build $b1: $s)"
        break
      fi
    else
      bad=0
    fi
    key=
    if [ "$r0" = 0x504C5852 ] && [ "$l0" = 0x504C5854 ] &&
      [ $((c1 & 0x7FFFFFFF)) -eq $((r1 & 0x7FFFFFFF)) ] && [ $((l1)) -eq 0 ]; then
      key="$c1:$r1"
    fi
    if [ -n "$key" ] && [ "$key" = "$prev" ]; then
      R6="quiesced (consumed == written, level 0: $s, on 2 reads after $ms ms)"
      break
    fi
    prev=$key
    if [ "$ms" -ge "$R6_DRAIN_MS" ]; then
      R6="not-quiesced (consumed != written or level != 0 within $R6_DRAIN_MS ms: $s)"
      break
    fi
    usleep 50000 2>/dev/null || sleep 0.05 2>/dev/null || sleep 1
  done
  if [ -f "$R6_MARKER" ]; then printf 'r6=%s\n' "$R6" >> "$R6_MARKER"; fi
  case $R6 in
    port-suspect*)
      m="MAST_LOAD R6 PORT SUSPECT: $R6 - NO core load: HPS reboot first (runbook; R6 cannot heal an offset port)"
      echo "!!! $m"
      echo "$(date '+%F %T') $m" >> "${MAST_LOAD_FAILED:-/tmp/misterplexd-boot.FAILED}"
      w=
      for a in 0x300FF100 0x300FF104 0x300FF108 0x300FF10C 0x300FF110 0x300FF114 0x300FF118 \
        0x300FF11C 0x300FF120 0x300FF124 0x300FF128 0x300FF12C 0x300FF130 0x300FF134 0x300FF138 \
        0x300FF13C; do
        w="$w $(devmem "$a" 32 2>/dev/null)"
      done
      echo "MAST_LOAD R6 D10 0x300FF100-13F:$w"
      w=
      for a in 0x30140000 0x30140004 0x30140008 0x3014000C 0x30140010 0x30140014 0x30140018 \
        0x3014001C; do
        w="$w $(devmem "$a" 32 2>/dev/null)"
      done
      echo "MAST_LOAD R6 0x30140000-01F:$w"
      return 96 ;;
    not-quiesced*)
      m="MAST_LOAD R6 NOT QUIESCED: $R6 - the ONE load proceeds"
      echo "!!! $m"
      echo "$(date '+%F %T') $m" >> "${MAST_LOAD_FAILED:-/tmp/misterplexd-boot.FAILED}" ;;
    *) echo "MAST_LOAD: R6 $R6; 200 ms quiet, then P-a and the ONE load" ;;
  esac
  usleep 200000 2>/dev/null || sleep 0.2 2>/dev/null || sleep 1
  return 0
}
pa_withdraw() {
  devmem 0x30140128 32 0
  devmem 0x3014012C 32 0
  lo=$(devmem 0x30140128 32 2>/dev/null)
  hi=$(devmem 0x3014012C 32 2>/dev/null)
  if [ "$lo" = 0x00000000 ] && [ "$hi" = 0x00000000 ]; then
    echo "MAST_LOAD: audio command withdrawn before load_core (commit 0x30140128-0x3014012F := 0, read back 0)"
  else
    m="MAST_LOAD withdraw failed: commit 0x30140128 read back lo=[$lo] hi=[$hi] - the ONE load proceeds"
    echo "!!! $m"
    echo "$(date '+%F %T') $m" >> "${MAST_LOAD_FAILED:-/tmp/misterplexd-boot.FAILED}"
  fi
  return 0
}
# One guarded call: the guard runs before pa_load (it writes load_core), so the R6 quiesce (drain
# and 200 ms quiet wait) and the withdraw follow the guard and precede the load, in that order. On
# R6 PORT SUSPECT the withdraw still runs and nothing is loaded (rc 96).
pa_load() {
  { _mlc_ok=; mlc_pre C:r6_quiesce || exit $?; [ "$_mlc_ok" = 1 ] || { echo "LOADCORE_GUARD_ABORT the guard function did not run (removed or replaced); nothing was loaded" >&2; exit 98; }; r6_quiesce; mlc_rc $?; mlc_flagrc $?; }
  r6rc=$?
  { _mlc_ok=; mlc_pre C:pa_withdraw || exit $?; [ "$_mlc_ok" = 1 ] || { echo "LOADCORE_GUARD_ABORT the guard function did not run (removed or replaced); nothing was loaded" >&2; exit 98; }; pa_withdraw; mlc_rc $?; mlc_flagrc $?; }
  if [ "$r6rc" = 96 ]; then return 96; fi
  { _mlc_ok=; mlc_guard "$1" || exit $?; [ "$_mlc_ok" = 1 ] || { echo "LOADCORE_GUARD_ABORT the guard function did not run (removed or replaced); nothing was loaded" >&2; exit 98; }; echo "load_core $1" > /dev/MiSTer_cmd; }
}
{ _mlc_ok=; mlc_pre C:pa_load || exit $?; [ "$_mlc_ok" = 1 ] || { echo "LOADCORE_GUARD_ABORT the guard function did not run (removed or replaced); nothing was loaded" >&2; exit 98; }; pa_load "$RBF"; mlc_rc $?; mlc_flagrc $?; }
# mister-loadcore-guard v2 digest d3540c25be7c3cf3ab6cec0c88ebfb07
