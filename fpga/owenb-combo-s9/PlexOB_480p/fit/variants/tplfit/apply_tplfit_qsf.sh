#!/usr/bin/env bash
# review A-logiclock-qsf.md (md5 a5606be9): Block A (Template.qsf:28-50 + ROUTER MAX), optional Block B pins.
# Block C (LogicLock) NOT applied: Lite 17.0.2. Usage: apply_tplfit_qsf.sh <H264.qsf> A|AB
set -euo pipefail
Q="${1:?qsf}"; V="${2:?A|AB}"
grep -q '^# TPLFIT' "$Q" && { echo "already applied"; exit 1; }
for k in FITTER_AGGRESSIVE_ROUTABILITY_OPTIMIZATION ROUTER_TIMING_OPTIMIZATION_LEVEL FINAL_PLACEMENT_OPTIMIZATION \
  FITTER_EFFORT OPTIMIZATION_MODE OPTIMIZATION_TECHNIQUE PHYSICAL_SYNTHESIS_ ROUTER_LCELL_INSERTION_AND_LOGIC_DUPLICATION \
  ROUTER_CLOCKING_TOPOLOGY_ANALYSIS PERIPHERY_TO_CORE_PLACEMENT_AND_ROUTING_OPTIMIZATION OPTIMIZE_POWER_DURING_FITTING \
  AUTO_DELAY_CHAINS_FOR_HIGH_FANOUT_INPUT_PINS ADV_NETLIST_OPT_SYNTH_WYSIWYG_REMAP SYNTH_GATED_CLOCK_CONVERSION \
  ECO_OPTIMIZE_TIMING ALM_REGISTER_PACKING_EFFORT; do
  sed -i "/^set_global_assignment -name ${k}/d" "$Q"
done
{
echo "# TPLFIT Block A (Template.qsf:28-50 + ROUTER MAX), review 2026-10-17"
for l in 'OPTIMIZE_POWER_DURING_FITTING OFF' 'FINAL_PLACEMENT_OPTIMIZATION ALWAYS' 'FITTER_EFFORT "STANDARD FIT"' \
 'OPTIMIZATION_MODE "HIGH PERFORMANCE EFFORT"' 'ROUTER_LCELL_INSERTION_AND_LOGIC_DUPLICATION ON' \
 'PHYSICAL_SYNTHESIS_COMBO_LOGIC ON' 'PHYSICAL_SYNTHESIS_REGISTER_DUPLICATION ON' 'PHYSICAL_SYNTHESIS_REGISTER_RETIMING ON' \
 'OPTIMIZATION_TECHNIQUE SPEED' 'AUTO_DELAY_CHAINS_FOR_HIGH_FANOUT_INPUT_PINS ON' 'PHYSICAL_SYNTHESIS_COMBO_LOGIC_FOR_AREA ON' \
 'ADV_NETLIST_OPT_SYNTH_WYSIWYG_REMAP ON' 'SYNTH_GATED_CLOCK_CONVERSION ON' 'ROUTER_CLOCKING_TOPOLOGY_ANALYSIS ON' \
 'ECO_OPTIMIZE_TIMING ON' 'PERIPHERY_TO_CORE_PLACEMENT_AND_ROUTING_OPTIMIZATION ON' \
 'PHYSICAL_SYNTHESIS_ASYNCHRONOUS_SIGNAL_PIPELINING ON' 'ALM_REGISTER_PACKING_EFFORT MEDIUM' 'ROUTER_TIMING_OPTIMIZATION_LEVEL MAXIMUM'; do
  echo "set_global_assignment -name $l"; done
if [ "$V" = AB ]; then
echo "# TPLFIT Block B: poly_pix sinks next to DSP cols X20 (h) / X32 (v)"
echo 'set_location_assignment LAB_X21_Y14 -to "ascal:ascal|o_h_poly_pix.r*"'
echo 'set_location_assignment LAB_X21_Y16 -to "ascal:ascal|o_h_poly_pix.g*"'
echo 'set_location_assignment LAB_X21_Y18 -to "ascal:ascal|o_h_poly_pix.b*"'
echo 'set_location_assignment LAB_X33_Y14 -to "ascal:ascal|o_v_poly_pix.r*"'
echo 'set_location_assignment LAB_X33_Y16 -to "ascal:ascal|o_v_poly_pix.g*"'
echo 'set_location_assignment LAB_X33_Y18 -to "ascal:ascal|o_v_poly_pix.b*"'
fi
} >> "$Q"
echo "apply_tplfit_qsf $V: $(md5sum "$Q" | cut -c1-32)"
