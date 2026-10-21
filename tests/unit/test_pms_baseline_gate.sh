#!/usr/bin/env bash
# Offline proof that the PMS delivered-SPS gate fails red on each FPGA-breaking constraint.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROBE="$ROOT/build/pms_baseline_probe"
WORK="$ROOT/build/pms-baseline-gate"
mkdir -p "$WORK"

python3 - "$WORK" <<'PY'
from pathlib import Path
import sys

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)


def bits_u(v: int, n: int) -> str:
    return format(v, f"0{n}b")


def bits_ue(v: int) -> str:
    code = v + 1
    b = format(code, "b")
    return "0" * (len(b) - 1) + b


def bits_se(v: int) -> str:
    code = 2 * v - 1 if v > 0 else -2 * v
    return bits_ue(code)


def rbsp(bitstr: str) -> bytes:
    bitstr += "1"
    bitstr += "0" * ((8 - len(bitstr) % 8) % 8)
    return int(bitstr, 2).to_bytes(len(bitstr) // 8, "big")


def epb(data: bytes) -> bytes:
    out = bytearray()
    zeros = 0
    for b in data:
        if zeros >= 2 and b <= 3:
            out.append(3)
            zeros = 0
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def nal(nal_type: int, payload: bytes, nal_ref_idc: int = 3) -> bytes:
    return b"\x00\x00\x00\x01" + bytes([(nal_ref_idc << 5) | nal_type]) + epb(payload)


def sps(profile: int = 66, max_refs: int = 1) -> bytes:
    b = ""
    b += bits_u(profile, 8)
    b += bits_u(0xC0 if profile == 66 else 0, 8)
    b += bits_u(30, 8)
    b += bits_ue(0)  # seq_parameter_set_id
    if profile in {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135}:
        b += bits_ue(1)  # chroma_format_idc = 4:2:0
        b += bits_ue(0)  # bit_depth_luma_minus8
        b += bits_ue(0)  # bit_depth_chroma_minus8
        b += bits_u(0, 1)  # qpprime_y_zero_transform_bypass_flag
        b += bits_u(0, 1)  # seq_scaling_matrix_present_flag
    b += bits_ue(0)  # log2_max_frame_num_minus4 -> 4 bits
    b += bits_ue(2)  # pic_order_cnt_type
    b += bits_ue(max_refs)
    b += bits_u(0, 1)  # gaps_in_frame_num_value_allowed_flag
    b += bits_ue(38)  # pic_width_in_mbs_minus1 -> 39 mbs = 624px
    b += bits_ue(29)  # pic_height_in_map_units_minus1 -> 30 mbs = 480px
    b += bits_u(1, 1)  # frame_mbs_only_flag
    b += bits_u(1, 1)  # direct_8x8_inference_flag
    b += bits_u(1, 1)  # frame_cropping_flag
    b += bits_ue(0) + bits_ue(3) + bits_ue(0) + bits_ue(0)  # display 618x480
    b += bits_u(0, 1)  # vui_parameters_present_flag
    return rbsp(b)


def pps(cabac: int = 0) -> bytes:
    b = ""
    b += bits_ue(0)  # pic_parameter_set_id
    b += bits_ue(0)  # seq_parameter_set_id
    b += bits_u(cabac, 1)
    b += bits_u(0, 1)  # bottom_field_pic_order_in_frame_present_flag
    b += bits_ue(0)  # num_slice_groups_minus1
    b += bits_ue(0)  # num_ref_idx_l0_default_active_minus1
    b += bits_ue(0)  # num_ref_idx_l1_default_active_minus1
    b += bits_u(0, 1)  # weighted_pred_flag
    b += bits_u(0, 2)  # weighted_bipred_idc
    b += bits_se(0)  # pic_init_qp_minus26
    b += bits_se(0)  # pic_init_qs_minus26
    b += bits_se(0)  # chroma_qp_index_offset
    b += bits_u(1, 1)  # deblocking_filter_control_present_flag
    b += bits_u(0, 1)  # constrained_intra_pred_flag
    b += bits_u(0, 1)  # redundant_pic_cnt_present_flag
    return rbsp(b)


def slice_rbsp(slice_type: int = 2, idr: bool = True) -> bytes:
    b = ""
    b += bits_ue(0)  # first_mb_in_slice
    b += bits_ue(slice_type)
    b += bits_ue(0)  # pic_parameter_set_id
    b += bits_u(0, 4)  # frame_num
    if idr:
        b += bits_ue(0)  # idr_pic_id
        b += bits_u(0, 1)  # no_output_of_prior_pics_flag
        b += bits_u(0, 1)  # long_term_reference_flag
    b += bits_se(0)  # slice_qp_delta
    b += bits_ue(1)  # disable_deblocking_filter_idc
    b += bits_ue(0)  # first_mb_type; not needed by this probe but keeps parser happy
    return rbsp(b)


def stream(path: str, *, profile: int = 66, cabac: int = 0, max_refs: int = 1,
           b_slice: bool = False) -> None:
    data = bytearray()
    data += nal(7, sps(profile=profile, max_refs=max_refs))
    data += nal(8, pps(cabac=cabac))
    if b_slice:
        data += nal(1, slice_rbsp(slice_type=1, idr=False), nal_ref_idc=2)
    else:
        data += nal(5, slice_rbsp(slice_type=2, idr=True))
        data += nal(1, slice_rbsp(slice_type=0, idr=False), nal_ref_idc=2)
    (out / path).write_bytes(data)


stream("green.264")
stream("bad_profile_high.264", profile=100)
stream("bad_cabac.264", cabac=1)
stream("bad_max_ref.264", max_refs=4)
stream("bad_b_slice.264", b_slice=True)
PY

run_green() {
  local name="$1"
  local out
  out="$("$PROBE" --annexb "$WORK/$name" 2>&1)"
  printf '%s\n' "$out"
  [[ "$out" == *"profile_idc=66"* ]]
  [[ "$out" == *"entropy_cabac=0"* ]]
  [[ "$out" == *"max_num_ref_frames=1"* ]]
  [[ "$out" == *" b=0 "* ]]
  [[ "$out" == *"coded=624x480 display=618x480"* ]]
}

run_red() {
  local name="$1" needle="$2"
  local out rc
  set +e
  out="$("$PROBE" --annexb "$WORK/$name" 2>&1)"
  rc=$?
  set -e
  printf '%s\n' "$out"
  if [[ $rc -eq 0 ]]; then
    echo "FAIL: $name unexpectedly passed" >&2
    return 1
  fi
  [[ "$out" == *"$needle"* ]]
}

run_green green.264
run_red bad_profile_high.264 "profile_idc=100, expected 66"
run_red bad_cabac.264 "entropy_cabac=1, expected 0"
run_red bad_max_ref.264 "max_num_ref_frames=4, expected 1"
run_red bad_b_slice.264 "b_slices=1, expected 0"

set +e
missing_out="$(env -u PLEX_BASE -u PLEX_TOKEN -u MISTERPLEX_BASELINE_KEY -u PLEX_KEY \
  MISTERPLEX_CONF="$WORK/missing.conf" MISTER_CONF= \
  "$ROOT/tests/hw/test_pms_baseline_profile.sh" 2>&1)"
missing_rc=$?
set -e
printf '%s\n' "$missing_out"
if [[ $missing_rc -ne 77 || "$missing_out" != *"SKIP-NOT-PASS"* ]]; then
  echo "FAIL: absent live PMS inputs must be SKIP-NOT-PASS rc=77, got rc=$missing_rc" >&2
  exit 1
fi

echo "test_pms_baseline_gate: OK green plus red proofs for profile/cabac/ref/B and absent deps"
