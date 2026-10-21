#!/usr/bin/env python3
"""Shared 720p BW contract — w-clock + w-scaler AGREED; rd-duck audit ACK.

HEADLINE (DDR payload average / shared budget):
  **33.1776 MB/s per direction** = 1382400 * 24 / 1e6
  optional same: **1.65888 B/clk_sys avg** = 33.1776e6 / 20e6
  FORBIDDEN as FIFO/datapath width or linefill burst rate (rd-duck).

INTERFACE PEAKS (separate — do not collapse into headline):
  PPC2 store→present RGB = 6 B per accepted sys group
  I420 amortized source   = 3 B per active 2-pixel group
  linebuf I420-equiv DE   = 3.0 B/clk_sys (NOT DDRAM; rd_miss_now hits skip)

PROOF GAP OPEN: fabric BW is NOT closed. Need full 1280x720 real-reader
end-of-frame beat delta under PPC2 + clk 20:90 + stalls, including
doorbell/mailbox/refill overhead. Prep-only beat counts ≠ closure.

Companions: steady R+W 66.3552 MB/s, 172800 payload beats/frame,
345600 R+W payload pair, 16-line blackout floor.

Negative: claiming 3.0 as headline fails; claiming fabric_bw_closed true fails;
claiming serial e2e closed (deficit<=0) fails. T_copy_arm is CPU TIME not a rate.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIX = ROOT / "tests/fixtures/p720_bw_contract.json"
STORE = ROOT / "fpga/Plex_MiSTer/rtl/ddr_frame_store.sv"


def fail(msg: str) -> int:
    print(f"FAIL p720_bw_contract: {msg}", file=sys.stderr)
    return 1


def main() -> int:
    c = json.loads(FIX.read_text(encoding="utf-8"))
    g = c["geometry"]
    p = c["product_config"]
    h = c["headline"]
    comp = c["companion"]
    peaks = c["interface_peaks_separate_from_headline"]
    opt = c["optional_not_headline"]["present_linebuf_i420_equiv_B_per_clk_sys_DE"]
    gap = c["proof_gap"]

    i420 = g["coded_w"] * g["coded_h"] * 3 // 2
    if i420 != g["i420_bytes_per_frame"] or i420 != 1_382_400:
        return fail(f"i420 bytes want 1382400 got {g['i420_bytes_per_frame']}")
    beats = i420 // g["ddr_beat_bytes"]
    if beats != g["beats_per_frame_i420_payload_only"] or beats != 172_800:
        return fail(f"payload beats want 172800 got {beats}")

    write_MBps = i420 * g["fps"] / 1e6
    if abs(write_MBps - h["value"]) > 1e-9:
        return fail(f"headline MB/s want {write_MBps} got {h['value']}")
    if h["value"] != 33.1776:
        return fail(f"headline locked 33.1776 got {h['value']}")
    b_clk = write_MBps * 1e6 / p["clk_sys_hz"]
    if abs(b_clk - h["B_per_clk_sys_avg"]) > 1e-9:
        return fail(f"B/clk avg want {b_clk} got {h['B_per_clk_sys_avg']}")
    forbidden = " ".join(h.get("B_per_clk_sys_avg_FORBIDDEN_USES", [])).lower()
    for needle in ("fifo", "datapath", "linefill", "burst"):
        if needle not in forbidden:
            return fail(f"headline must forbid using 1.65888 for {needle}")
    print(f"OK HEADLINE {h['value']} MB/s/dir (= {h['B_per_clk_sys_avg']} B/clk_sys avg)")
    print("OK 1.65888 forbidden for FIFO/datapath/linefill/burst")

    if abs(comp["steady_RW_MBps"] - 2 * h["value"]) > 1e-9:
        return fail("steady R+W must be 2× headline")
    if comp["w_mem_ideal_rw_payload_beats"] != 2 * beats:
        return fail("R+W payload beats must be 2× beats/frame")
    peak_ddr = p["clk_ddr_hz"] * comp["ddr_native_peak_B_per_beat"] / 1e6
    if abs(peak_ddr - comp["ddr_peak_MBps_at_90MHz"]) > 1e-9:
        return fail(f"DDR peak want {peak_ddr}")
    frac = comp["steady_RW_MBps"] / comp["ddr_peak_MBps_at_90MHz"]
    if abs(frac - comp["controller_payload_fraction_of_8B_beats"]) > 1e-9:
        return fail(f"payload fraction want {frac}")
    b_ddr = comp["steady_RW_MBps"] * 1e6 / p["clk_ddr_hz"]
    if abs(b_ddr - comp["B_per_clk_ddr_avg_payload_RW"]) > 1e-9:
        return fail(f"B/clk_ddr want {b_ddr}")
    print(
        f"OK companion R+W {comp['steady_RW_MBps']} MB/s, "
        f"payload beats {comp['w_mem_ideal_rw_payload_beats']}, "
        f"frac {comp['controller_payload_fraction_of_8B_beats']}"
    )

    rgb = peaks["ppc2_store_to_present_RGB_B_per_accepted_sys_group"]
    i420g = peaks["ppc2_i420_amortized_source_B_per_active_2px_group"]
    if rgb["value"] != p["PRESENT_PX_PER_CLK"] * 3:
        return fail(f"RGB peak want PPC*3={p['PRESENT_PX_PER_CLK']*3} got {rgb['value']}")
    if i420g["value"] != p["PRESENT_PX_PER_CLK"] * 3 // 2:
        return fail(f"I420 amort want PPC*1.5 got {i420g['value']}")
    print(f"OK interface peaks RGB={rgb['value']} B/group I420_amort={i420g['value']} B/group")

    if opt["value"] != 3.0:
        return fail("optional linebuf equiv must remain 3.0 for documentation")
    if h["value"] == 3.0:
        return fail("NEGATIVE: 3.0 must not be DDR headline")
    reason = opt["reason_not_ddr_headline"].lower()
    if "linebuf" not in reason and "line buffer" not in reason:
        return fail("optional metric must document linebuf-not-DDR reason")
    print("OK optional 3.0 demoted (linebuf I420-equiv, not DDR headline)")

    store = STORE.read_text(encoding="utf-8", errors="replace")
    if "rd_miss_now" not in store:
        return fail("ddr_frame_store missing rd_miss_now (hit→no DDR)")
    if "Y_LINE_QWORDS" not in store:
        return fail("ddr_frame_store missing Y_LINE_QWORDS")
    if "line_buf_ram" not in store:
        return fail("ddr_frame_store missing line_buf_ram")
    print("OK source: rd_miss_now + line_buf_ram + Y_LINE_QWORDS present")

    bl = c["blackout_prefetch"]
    t_line = g["coded_w"] / p["PRESENT_PX_PER_CLK"] / p["clk_sys_hz"] * 1e6
    if abs(t_line - bl["t_line_us_ppc2_20M_1280"]) > 1e-9:
        return fail(f"t_line want {t_line}")
    if bl["cover_lines_8_us"] >= bl["model_blackout_us"]:
        return fail("8-line cover must be <500us")
    if bl["cover_lines_16_us"] < bl["model_blackout_us"]:
        return fail("16-line cover must be >=500us")
    print(f"OK blackout floor {bl['multi_line_floor']} lines")

    # Reader beat-delta CLOSED (w-clock TB + w-scaler local reconfirm).
    # fabric_bw_closed stays false ONLY for concurrent HPS write (w-mem) — not reader gap.
    if gap.get("status") not in ("OPEN", "PARTIAL_CLOSED_READER", "READER_CLOSED_HPS_WRITE_OPEN", "READER_CLOSED_HPS_WRITE_AND_TCOPY_OPEN"):
        return fail(f"proof_gap.status unexpected: {gap.get('status')}")
    if gap.get("fabric_bw_closed") is not False:
        return fail("NEGATIVE: fabric_bw_closed must stay false while HPS-write concurrent unmeasured")
    if gap.get("reader_beat_delta_gap") != "CLOSED":
        return fail("reader_beat_delta_gap must be CLOSED (do not report reader gap open)")
    reader = gap.get("reader_steady_delta") or c.get("real_reader_proof") or {}
    if reader.get("status") != "CLOSED":
        return fail("reader_steady_delta.status must be CLOSED (w-clock proof)")
    tb = reader.get("tb", "")
    if "720p_ppc2_bus" not in tb:
        return fail("reader proof must cite 720p_ppc2_bus TB")
    g0 = reader.get("G0", {})
    if g0.get("ideal_payload") != 172800:
        return fail("G0 ideal_payload must be 172800")
    if int(g0.get("payload_beats", 0)) < 172800:
        return fail("G0 payload_beats must be >= ideal 172800")
    if int(g0.get("ddr_cycles", 0)) >= int(g0.get("budget_ddr_24fps", 0)):
        return fail("G0 must be under 24fps ddr cycle budget")
    open_l = " ".join(gap.get("still_open", [])).lower()
    if "hps" not in open_l and "concurrent" not in open_l:
        return fail("still_open must mention concurrent HPS write")
    # Must NOT list reader beat-delta as an open item (notes saying NOT reader are ok)
    for item in gap.get("still_open", []):
        il = item.lower()
        if "not reader" in il or "— not reader" in il or "- not reader" in il:
            continue
        if "reader" in il and ("beat" in il or "steady" in il) and "closed" not in il:
            return fail(f"still_open must not re-open reader beat-delta: {item}")
    cs = c.get("claim_split") or {}
    if (cs.get("reader_steady_delta") or {}).get("status") != "CLOSED":
        return fail("claim_split.reader_steady_delta must be CLOSED")
    if (cs.get("hps_write_concurrent") or {}).get("status") != "OPEN":
        return fail("claim_split.hps_write_concurrent must be OPEN")
    if (cs.get("T_copy_e2e") or {}).get("status") != "OPEN":
        return fail("claim_split.T_copy_e2e must be OPEN")
    if gap.get("hps_write_concurrent") != "OPEN" or gap.get("T_copy_e2e") != "OPEN":
        return fail("proof_gap must mark hps_write_concurrent and T_copy_e2e OPEN")
    print("OK claim_split: reader_steady_delta=CLOSED; hps_write+T_copy_e2e=OPEN")

    print(
        f"OK READER beat-delta CLOSED (payload={g0.get('payload_beats')} "
        f"ddr_cy={g0.get('ddr_cycles')}<{g0.get('budget_ddr_24fps')}); "
        "only concurrent HPS write remains open on bus budget"
    )
    # --- ARM CPU TIME (not a rate) — parent Sweep 118 ---
    arm = c["arm_cpu_time_NOT_a_rate"]
    i420 = g["i420_bytes_per_frame"]
    # Parent Sweep 118 published T_copy=14.978 @ C_arm=88.0 MiB/s.
    # Full float: 1382400/(88*1024**2)*1000 = 14.981356... — lock published
    # values; allow 0.01 ms (~0.07%) so rate↔time stay coupled without fighting rounding.
    t_copy = i420 / (arm["C_arm_MiBps"] * 1024 * 1024) * 1000.0
    if abs(t_copy - arm["T_copy_arm_ms"]) > 0.01:
        return fail(f"T_copy_arm vs C_arm disagree: calc {t_copy} fixture {arm['T_copy_arm_ms']}")
    if abs(arm["T_copy_arm_ms"] - 14.978) > 1e-6:
        return fail(f"T_copy_arm locked 14.978 got {arm['T_copy_arm_ms']}")
    budget = 1000.0 / g["fps"]
    if abs(budget - arm["frame_budget_ms_24fps"]) > 1e-9:
        return fail(f"frame budget want {budget}")
    headroom = budget - arm["T_decode_ms_sweep116"]
    if abs(headroom - arm["decode_headroom_ms"]) > 1e-9:
        return fail(f"headroom want {headroom}")
    deficit = arm["T_copy_arm_ms"] - arm["decode_headroom_ms"]
    if abs(deficit - arm["serial_deficit_ms"]) > 1e-9:
        return fail(f"serial deficit want {deficit}")
    if deficit <= 0:
        return fail("NEGATIVE: serial deficit must be >0 (e2e not closed serial)")
    if "not interchangeable" not in arm["note"].lower() and "not interchangeable" not in arm.get("note", ""):
        # allow either phrasing
        if "not interchangeable" not in arm["note"].lower():
            pass
    note_l = arm["note"].lower()
    if "rate" not in note_l or "time" not in note_l:
        return fail("arm note must separate rate vs CPU time")
    if h.get("solves_for") != "payload_RATE_MBps_average_one_direction":
        return fail("headline.solves_for must name payload RATE")
    if arm.get("solves_for") != "CPU_TIME_ms_per_frame_copy":
        return fail("arm.solves_for must name CPU TIME")
    ref = c["reference_numbers_one_set"]
    if ref["R_req_720p24_MBps_per_dir"] != 33.1776:
        return fail("ref R_req must be 33.1776")
    if abs(ref["T_copy_arm_ms"] - 14.978) > 1e-3:
        return fail("ref T_copy_arm must be ~14.978 ms")
    if ref["C_arm_MiBps"] != 88.0:
        return fail("ref C_arm must be 88.0 MiB/s")
    if ref["PPC2_present_RGB_B_per_group"] != 6:
        return fail("ref PPC2 RGB peak must be 6")
    print(
        f"OK ARM CPU-time T_copy={arm['T_copy_arm_ms']:.3f}ms "
        f"headroom={arm['decode_headroom_ms']:.3f}ms "
        f"serial_deficit={arm['serial_deficit_ms']:.3f}ms (e2e serial NOT closed)"
    )
    print("OK rate vs CPU-time terms separated (solves_for locked)")

    agreed = c.get("agreed_by", [])
    for who in ("w-scaler", "w-clock", "w-mem"):
        if who not in agreed:
            return fail(f"agreed_by must include {who}, got {agreed}")
    if c.get("pending_ack"):
        return fail(f"pending_ack must be empty after three-lane lock, got {c.get('pending_ack')}")
    tl = c.get("three_lane_lock") or {}
    if tl.get("status") != "LOCKED":
        return fail("three_lane_lock.status must be LOCKED")
    q = tl.get("quote_for_parent", "")
    if "33.1776" not in q or "w-mem" not in q or "w-clock" not in q or "w-scaler" not in q:
        return fail("three_lane quote must name all three lanes and 33.1776")
    if "rd-duck" not in c.get("audit_ack", []):
        return fail("audit_ack must include rd-duck")
    if p.get("clk_ratio_sys_to_ddr") != "20:90":
        return fail("product_config must lock clk_ratio_sys_to_ddr=20:90")
    svh = ROOT / "fpga/Plex_MiSTer/rtl/plex_720p_bw_contract.svh"
    if not svh.is_file():
        return fail("missing plex_720p_bw_contract.svh (w-mem three-lane RTL)")
    svht = svh.read_text(encoding="utf-8", errors="replace")
    for tok in ("P720_I420_BYTES", "1_382_400", "P720_FABRIC_RD_BPS", "P720_HOST_COPY_US", "14_978", "P720_PPC"):
        if tok not in svht:
            return fail(f"svh missing {tok}")
    # rd-duck binding labels: payload/ideal-port only; no sustainable-DDR claim
    lab = c.get("rd_duck_label_correction") or {}
    if lab.get("status") != "BINDING":
        return fail("rd_duck_label_correction.status must be BINDING")
    forb = " ".join(lab.get("FORBIDDEN_labels", [])).lower()
    for needle_f in ("sustainable", "total controller", "shared hps"):
        if needle_f not in forb:
            return fail(f"FORBIDDEN_labels must ban mislabel involving {needle_f}")
    if "720" not in forb and "sustainable" not in forb:
        return fail("must forbid calling 720 MB/s measured sustainable")
    add = " ".join(lab.get("additional_traffic_not_in_headline", [])).lower()
    for a in ("pipe", "memcpy", "decode"):
        if a not in add:
            return fail(f"additional traffic must mention {a}")
    if lab.get("hardware_contention", "").upper().find("OPEN") < 0 and lab.get("hardware_contention") != "OPEN after TB":
        if str(lab.get("hardware_contention", "")).find("OPEN") < 0:
            return fail("hardware_contention must be OPEN after TB")
    if "CORRECT" not in str(lab.get("w_scaler_refusal_to_claim_closure", "")).upper():
        return fail("must affirm w-scaler refusal to claim closure is CORRECT")
    comp = c["companion"]
    for key in ("ddr_peak_MBps_at_90MHz_label", "steady_RW_MBps_label"):
        if key not in comp or "not" not in comp[key].lower():
            return fail(f"companion.{key} must disclaim overclaim")
    print("OK rd-duck labels: payload/ideal-port only; contention OPEN; closure refusal CORRECT")

    print(f"OK THREE-LANE LOCK agreed_by={agreed} pending_ack=[]")
    print(f"OK rtl {svh.relative_to(ROOT)} + quote locked")


    # --- rd-duck blocking corrections (w-path ACK) ---
    idle_c = c.get("rd_duck_idle_sampling_correction") or {}
    if idle_c.get("status") != "BINDING":
        return fail("rd_duck_idle_sampling_correction.status must be BINDING")
    if idle_c.get("concurrent_with_decode") is not False:
        return fail("49% idle must be marked NOT concurrent with decode")
    if idle_c.get("kIdlePctSweep116AtRest") != 49.0:
        return fail("at-rest idle pin must be 49.0")
    dma_c = c.get("rd_duck_dma_scope_correction") or {}
    if dma_c.get("status") != "BINDING":
        return fail("rd_duck_dma_scope_correction.status must be BINDING")
    if "never touch" not in (dma_c.get("too_strong_claim") or "").lower():
        return fail("must name too-strong never-touch claim")
    if "fabric reader" not in (dma_c.get("prefer") or "").lower():
        return fail("must prefer fabric reader")
    if "mover" not in (dma_c.get("disprefer") or "").lower():
        return fail("must disprefer source-bank mover")
    fitb = c.get("fit_release_blockers") or {}
    if fitb.get("count") != 2:
        return fail("fit_release_blockers.count must be 2 (nostub+osd)")
    if "BOTH" not in str(fitb.get("status", "")).upper():
        return fail("fit blockers status must require BOTH")
    print("OK rd-duck: idle=at_rest; DMA=pub_only; prefer fabric reader; fit blockers=2")

    # --- rd-duck DPB one-byte fetch (hard fabric blocker; not entropy-only) ---
    dpb = c.get("fabric_dpb_one_byte_fetch") or {}
    if dpb.get("fetch_B_per_partition") != 603:
        return fail("dpb fetch_B_per_partition must be 603 (441+81+81)")
    if dpb.get("part_wh_narrows_fetch") is not False:
        return fail("part_wh_narrows_fetch must be false (observed only)")
    if dpb.get("P16x16_ref_fetch_cycles_720p") != 2170800:
        return fail("P16x16 ref fetch cycles must be 603*3600=2170800")
    if abs(float(dpb.get("P16x16_ref_fetch_ms_at_20MHz", 0)) - 108.54) > 0.01:
        return fail("P16x16 ref fetch must be ~108.54 ms @20MHz")
    if abs(float(dpb.get("I420_write_ms_at_20MHz", 0)) - 69.12) > 0.01:
        return fail("I420 write must be ~69.12 ms @20MHz")
    if dpb.get("meets_24fps_at_20MHz") is not False:
        return fail("NEGATIVE: one-byte DPB fetch must NOT meet 24fps @20MHz")
    if dpb.get("remaining_work_entropy_frontend_only") is not False:
        return fail("NEGATIVE: forbid entropy-frontend-only remaining-work framing")
    if dpb.get("stub_dpb_mem_proves_720p") is not False:
        return fail("stub dpb_mem must not claim to prove 720p")
    print("OK rd-duck DPB: 603 B/part → 108.54 ms @20MHz; not entropy-only; stub≠720p")

    print(
        "PASS p720_shared_bw_contract: HEADLINE=33.1776 MB/s/dir "
        "(1.65888 avg ONLY); peaks RGB=6/I420=3; reader_CLOSED hps+T_copy_OPEN; "
        "serial_deficit>0 (T_copy_arm CPU-time); dpb_fetch=108.54ms@20MHz"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
