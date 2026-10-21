#!/usr/bin/env bash
# R1 A1FB pairing mutants (release 05:53, review 05:53: no load until the daemon + PLXH writer are
# CLEARed). test_fpga_build_ids and test_av_sync are built against a mutated scratch copy of
# host/libmisterplex/fpga_build_ids.hpp (-I scratch ahead of -I host; every other libmisterplex
# header is a symlink to this tree's), so the product header carries no MUT_ code.
#   A1FB_DROPPED   the 0x4800A1FB row removed                     -> both tests must FAIL
#   5D47_ACCEPTED  a 0x48005D47 row (a copy of A1FB) accepted too -> test_fpga_build_ids must FAIL
#   A152_DROPPED   the 0x4800A152 row (R1 50 MHz refit) removed    -> test_fpga_build_ids must FAIL
#   A153_ACCEPTED  a 0x4800A153 copy of it, 1 bit away, without the two R1 capabilities, accepted
#   9E36_DROPPED   the 0x48009E36 row (the fixes refit) removed     -> test_fpga_build_ids must FAIL
#   9E37_ACCEPTED  a 0x48009E37 copy of it, 1 bit away, without the two R1 capabilities, accepted
#                                                                   -> test_fpga_build_ids must FAIL
#                  (only the min-Hamming check can see it: table neighbours are not 'refused')
# The unmutated builds must pass first.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
CXX=${CXX:-g++}
FLAGS=(-std=c++17 -O2 -Wall -Wextra)
HDR="$ROOT/host/libmisterplex/fpga_build_ids.hpp"
python3 - "$HDR" "$T" <<'EOF' || exit 1
import os, re, sys
hdr, tmp = sys.argv[1], sys.argv[2]
src = open(hdr).read()
row = re.compile(r"\n    \{0x4800A1FBu,.*?\},(?=\n)", re.S)
assert len(row.findall(src)) == 1, "want exactly one 0x4800A1FBu row"
row152 = re.compile(r"\n    \{0x4800A152u,.*?\},(?=\n)", re.S)
assert len(row152.findall(src)) == 1, "want exactly one 0x4800A152u row"
row9e36 = re.compile(r"\n    \{0x48009E36u,.*?\},(?=\n)", re.S)
assert len(row9e36.findall(src)) == 1, "want exactly one 0x48009E36u row"
muts = {
    "A1FB_DROPPED": row.sub("", src, count=1),
    "5D47_ACCEPTED": row.sub(lambda m: m.group(0) + m.group(0).replace("0x4800A1FBu", "0x48005D47u"), src, count=1),
    "A152_DROPPED": row152.sub("", src, count=1),
    "A153_ACCEPTED": row152.sub(lambda m: m.group(0) + m.group(0).replace("0x4800A152u", "0x4800A153u")
                               .replace("true, true},", "false, false},"), src, count=1),
    "9E36_DROPPED": row9e36.sub("", src, count=1),
    "9E37_ACCEPTED": row9e36.sub(lambda m: m.group(0) + m.group(0).replace("0x48009E36u", "0x48009E37u")
                                 .replace("true, true},", "false, false},"), src, count=1),
}
lib = os.path.dirname(hdr)
for name, text in muts.items():
    assert text != src and "0x48005D47u," not in src and "0x4800A153u," not in src and "0x48009E37u," not in src
    d = os.path.join(tmp, name, "libmisterplex")
    os.makedirs(d)
    for f in os.listdir(lib):
        if f != "fpga_build_ids.hpp":
            os.symlink(os.path.join(lib, f), os.path.join(d, f))
    open(os.path.join(d, "fpga_build_ids.hpp"), "w").write(text)
EOF
fail=0
run() { # variant test -> prints the test's rc, or BUILD_FAIL (never counted as a RED mutant)
    local inc=() log="$T/$2.$1.log"
    [ "$1" = base ] || inc=(-I"$T/$1")
    if ! "$CXX" "${FLAGS[@]}" "${inc[@]}" -I"$ROOT/host" -o "$T/$2.$1" "$ROOT/tests/unit/$2.cpp" > "$log" 2>&1; then
        echo BUILD_FAIL
        return
    fi
    "$T/$2.$1" >> "$log" 2>&1
    echo $?
}
built() { # rc variant test
    [ "$1" != BUILD_FAIL ] && return 0
    echo "BUILD_FAIL $2 $3:"; cat "$T/$3.$2.log"; exit 1
}
for t in test_fpga_build_ids test_av_sync; do
    rc=$(run base "$t")
    built "$rc" base "$t"
    [ "$rc" = 0 ] || { echo "baseline $t FAILED (rc $rc)"; cat "$T/$t.base.log"; exit 1; }
done
echo "baseline: test_fpga_build_ids and test_av_sync pass"
for m in A1FB_DROPPED 5D47_ACCEPTED A152_DROPPED A153_ACCEPTED 9E36_DROPPED 9E37_ACCEPTED; do
    line="$m:"
    for t in test_fpga_build_ids test_av_sync; do
        rc=$(run "$m" "$t")
        built "$rc" "$m" "$t"
        first=$(grep -m 1 -E 'FAIL' "$T/$t.$m.log" | cut -c1-150)
        line+=" $t rc=$rc${first:+ [$first]}"
        if [ "$t" = test_fpga_build_ids ] || [ "$m" = A1FB_DROPPED ]; then
            [ "$rc" != 0 ] || { line+=" SURVIVED"; fail=1; }
        fi
    done
    echo "$line"
done
[ "$fail" = 0 ] || { echo "test_fpga_build_ids_a1fb_mutants: a mutant SURVIVED"; exit 1; }
echo "test_fpga_build_ids_a1fb_mutants: A1FB_DROPPED, 5D47_ACCEPTED, A152_DROPPED, A153_ACCEPTED, 9E36_DROPPED and 9E37_ACCEPTED FAIL as required"
