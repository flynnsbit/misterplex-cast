# Source after ROOT is set. Exit 0 with SKIP when bench RTL/TB paths are absent on canon.
verilator_bench_skip_if_absent() {
  local f
  for f in "$@"; do
    if [[ ! -f "$f" ]]; then
      printf 'SKIP: Verilator bench absent on canon: %s\n' "${f#"$ROOT"/}" >&2
      exit 0
    fi
  done
}
