#!/usr/bin/env bash
# Mutants for the SPS-gate layer 2 inspect (arm/misterplexd/av_inproc_decode.cpp; T-SPS-GATE-FALLBACK pre-reg v1.4.3
# b57a87fb, H264 p1-p3, review 03:04; p3 kind OUT per review 04:16 n1). Each mutated copy of av_inproc_decode.cpp is compiled with the base's CXX and
# CXXFLAGS into tests/unit/test_compressed_syntax.cpp (the mutant dir comes first on -I, so its copy wins) and run
# against the gatefix corpus. Grammar (as test_gate_caps_mutants.sh, H264 c1 / review 02:56, SUPP-2):
#   KILLED        rc 1 and the FIRST "FAIL compressed syntax:" line equals the mutant's pinned line
#   KILLED-OTHER  rc 1 but another first FAIL line: a FINDING, the run fails
#   SURVIVED      rc 0: the run fails
#   INVALID       rc >= 128 (signal/crash), a timeout (rc 124), or any other rc: the run fails
# A mutation that does not apply (sed leaves the file unchanged) is a vacuous mutant: the run fails.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$ROOT/arm/misterplexd/av_inproc_decode.cpp
CXX=${CXX:-g++}
CXXFLAGS=${CXXFLAGS:--std=c++17 -O2 -Wall -Wextra -I$ROOT/host}
AV="libavformat libavcodec libavutil libswresample"
AVCFLAGS=$(pkg-config --cflags $AV)
AVLIBS=$(pkg-config --libs $AV)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
n=0
mut() {
    local name=$1 script=$2 want=$3 rc=0 first
    mkdir -p "$T/$name"
    sed "$script" "$SRC" > "$T/$name/av_inproc_decode.cpp"
    if cmp -s "$SRC" "$T/$name/av_inproc_decode.cpp"; then
        echo "FAIL mutant $name: the edit did not apply (vacuous)" >&2
        exit 1
    fi
    # shellcheck disable=SC2086
    "$CXX" -I"$T/$name" $CXXFLAGS -DMPX_HAVE_LIBAV -I"$ROOT/arm/misterplexd" $AVCFLAGS -o "$T/$name/t" \
        "$ROOT/tests/unit/test_compressed_syntax.cpp" $AVLIBS -pthread 2> "$T/$name/build.log"
    timeout 60 "$T/$name/t" "$ROOT" > "$T/$name/out" 2> "$T/$name/err" || rc=$?
    first=$(grep -m1 '^FAIL compressed syntax:' "$T/$name/err" || true)
    if [ "$rc" = 0 ]; then
        echo "FAIL mutant $name: SURVIVED (rc 0)" >&2
        exit 1
    elif [ "$rc" != 1 ]; then
        echo "FAIL mutant $name: INVALID (rc $rc)" >&2
        exit 1
    elif [ "$first" != "$want" ]; then
        echo "FAIL mutant $name: KILLED-OTHER (FINDING): first '$first', pinned '$want'" >&2
        exit 1
    fi
    echo "  mutant $name: KILLED by its pinned check: $first"
    n=$((n + 1))
}
# G9: the SPS refs limit hard-coded back to 1.
mut M-G9-refs-hardcoded-1 \
    's/if (refs > refsLimit) return out("max_num_ref_frames", refs, refsLimit, kSpsErr);/if (refs > 1) return out("max_num_ref_frames", refs, 1, kSpsErr);/' \
    'FAIL compressed syntax: G9_acc_refs2 refs2: want IN, got AU0 OUT field=max_num_ref_frames value=2 limit=1'
# G18: the PPS l0 default check dropped.
mut M-G18-l0-default-dropped \
    's/if (l0Active > capRefs)$/if (false)/' \
    'FAIL compressed syntax: G18_ref_l0d1 v1: want AU0 OUT field=num_ref_idx_l0_default_active value=2 limit=1, got AU1 OUT field=num_ref_idx_l0_default_active value=2 limit=1 why=active > max_num_ref_frames'
# G24 / H264 p1 (M-P-REFS0): the == 0 leg dropped. p3 still refuses the P slice, under another field and err.
mut M-P-REFS0 \
    's/if (kind == 0 && maxReferences == 0)$/if (false)/' \
    'FAIL compressed syntax: G24_ref_P_refs0 v1: want AU1 MALFORMED field=max_num_ref_frames value=0 limit=1 why=P without references, got AU1 OUT field=num_ref_idx_l0_default_active value=1 limit=0 why=active > max_num_ref_frames'
# H264 p3 / review 03:04: the active > max_num_ref_frames check dropped.
mut M-p3-active-dropped \
    's/if (active > maxReferences)$/if (false)/' \
    'FAIL compressed syntax: G18_ref_l0d1 refs2: want AU1 OUT field=num_ref_idx_l0_default_active value=2 limit=1 why=active > max_num_ref_frames, got IN'
# p3 moved to the PPS (default count against max_num_ref_frames): refuses the all-intra refs-0 shape.
mut M-p3-at-pps \
    '/const uint64_t l0Active = uint64_t(l0) + 1;/a\
                if (l0Active > maxReferences) return refuse(gate::StreamOutcome::Out, "num_ref_idx_l0_default_active", static_cast<int64_t>(l0Active), maxReferences, "active > max_num_ref_frames", kPpsErr);' \
    'FAIL compressed syntax: G18_ref_l0d1 v1: want AU0 OUT field=num_ref_idx_l0_default_active value=2 limit=1, got AU0 OUT field=num_ref_idx_l0_default_active value=2 limit=1 why=active > max_num_ref_frames'
# G26: the override limit hard-coded back to 1.
mut M-G26-override-hardcoded-1 \
    's/if (active > capRefs)$/if (active > 1)/' \
    'FAIL compressed syntax: G26_ref_ovr1 refs2: want AU1 OUT field=num_ref_idx_l0_active_override value=2 limit=1 why=active > max_num_ref_frames, got AU1 OUT field=num_ref_idx_l0_active_override value=2 limit=2'
# H264 p2, order: max_num_ref_frames checked after gaps_in_frame_num_value_allowed_flag.
mut M-p2-order-refs-after-gaps \
    '/if (refs > refsLimit) return out("max_num_ref_frames", refs, refsLimit, kSpsErr);/d
/if (gaps) return out("gaps_in_frame_num_value_allowed_flag", 1, 0, kSpsErr);/a\
                if (refs > refsLimit) return out("max_num_ref_frames", refs, refsLimit, kSpsErr);' \
    'FAIL compressed syntax: SPS refs 3 + gaps, v1: want AU0 OUT field=max_num_ref_frames value=3 limit=1, got AU0 OUT field=gaps_in_frame_num_value_allowed_flag value=1 limit=0'
# H264 p2, naming: the PPS l0 default refusal names another field.
mut M-p2-name-l0-default \
    's/return out("num_ref_idx_l0_default_active", static_cast<int64_t>(l0Active), capRefs,/return out("max_num_ref_frames", static_cast<int64_t>(l0Active), capRefs,/' \
    'FAIL compressed syntax: G18_ref_l0d1 v1: want AU0 OUT field=num_ref_idx_l0_default_active value=2 limit=1, got AU0 OUT field=max_num_ref_frames value=2 limit=1'
# review 04:16 n1: p3 is legal H.264, so its kind is OUT; flipping it back to MALFORMED must be caught.
mut M-p3-kind-malformed \
    's/return refuse(gate::StreamOutcome::Out, activeField, static_cast<int64_t>(active),/return refuse(gate::StreamOutcome::Malformed, activeField, static_cast<int64_t>(active),/' \
    'FAIL compressed syntax: G18_ref_l0d1 refs2: want AU1 OUT field=num_ref_idx_l0_default_active value=2 limit=1 why=active > max_num_ref_frames, got AU1 MALFORMED field=num_ref_idx_l0_default_active value=2 limit=1 why=active > max_num_ref_frames'
# v1 byte-identity: an override ue cut inside its suffix loses d42a4460's :354 err.
mut M-ovr-cut-err \
    's/minus1 != 0 ? "multiple active references are unsupported"/false ? "multiple active references are unsupported"/' \
    "FAIL compressed syntax: P override ue cut in its suffix: err 'unsupported/truncated slice header or unnegotiated deblocking', want 'multiple active references are unsupported'"
# NVENC refs (1011): the prod sps_parser refuses max_num_ref_frames > 1, so a "declared-only second reference"
# relaxation on a one-reference core must be caught (the corpus G9 row pins it before the NV rows).
mut M-refs2-declared-relaxed \
    's/if (refs > refsLimit) return out("max_num_ref_frames", refs, refsLimit, kSpsErr);/if (refs > (capRefs == 1 ? 2u : capRefs)) return out("max_num_ref_frames", refs, capRefs, kSpsErr);/' \
    'FAIL compressed syntax: G9_acc_refs2 v1: want AU0 OUT field=max_num_ref_frames value=2 limit=1, got AU1 OUT field=max_num_ref_frames value=2 limit=1'
echo "test_compressed_syntax_mutants: OK ($n mutants KILLED by their pinned check)"
