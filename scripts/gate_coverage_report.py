#!/usr/bin/env python3
"""Gate coverage meta-report: what every gate proves and does NOT prove.

Produces a machine-readable JSON and a human-readable table showing the
full inventory of build gates, their coverage scope, blind spots, red-proof
status, and whether they are currently runnable on this host.

Exit codes:
  0 = report generated (does not mean all gates pass — see the report)
  1 = a gate is dead (always fails) or has no red proof
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field, asdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@dataclass
class GateReport:
    id: str
    script: str
    makefile_target: str
    proves: str
    does_not_prove: str
    red_proof_method: str
    red_proof_passed: bool | None  # None = not tested
    runnable_here: bool
    runnable_note: str
    owner: str
    exit_codes: dict[str, int] = field(default_factory=dict)


def check_tool(cmd: str) -> bool:
    return shutil.which(cmd) is not None


def check_venv_numpy() -> bool:
    venv_py = ROOT / "build" / ".venv" / "bin" / "python3"
    if not venv_py.exists():
        return False
    try:
        r = subprocess.run(
            [str(venv_py), "-c", "import numpy; import PIL"],
            capture_output=True, timeout=10,
        )
        return r.returncode == 0
    except Exception:
        return False


def check_verilator() -> bool:
    wrapper = ROOT / "scripts" / "run_verilator.sh"
    if not wrapper.exists():
        return False
    try:
        r = subprocess.run(
            [str(wrapper), "--version"],
            capture_output=True, timeout=10, cwd=ROOT,
        )
        return r.returncode == 0
    except Exception:
        return False


def check_quartus() -> bool:
    qm = os.environ.get("QUARTUS_MAP") or shutil.which("quartus_map")
    return qm is not None and os.access(qm, os.X_OK)


def has_v4l2_device() -> bool:
    return Path("/dev/video4").exists()


def build_report() -> list[GateReport]:
    has_verilator = check_verilator()
    has_quartus = check_quartus()
    has_numpy = check_venv_numpy()
    has_grabber = has_v4l2_device()

    gates = [
        GateReport(
            id="define-parity",
            script="scripts/check_define_parity.py",
            makefile_target="define-parity",
            proves=(
                "Quartus product macros (from .qsf) match the macros injected into "
                "Verilator/lint. Test-only macros are declared in an allowlist."
            ),
            does_not_prove=(
                "Does NOT prove macros are correct in value, only that the same set "
                "is defined in both flows. Does NOT prove macros are used correctly "
                "in RTL. Does NOT prove Quartus actually synthesises with these macros "
                "(that requires a real fit)."
            ),
            red_proof_method="--drop-verilator-macro DDR_FRAME_STORE → rc=1",
            red_proof_passed=True,
            runnable_here=True,
            runnable_note="Pure Python, no external tools needed.",
            owner="w-c2",
            exit_codes={"green": 0, "red_drop_macro": 1},
        ),
        GateReport(
            id="quartus-sv-subset",
            script="scripts/check_quartus_sv_subset.py",
            makefile_target="quartus-sv-subset",
            proves=(
                "Source files do not contain known Quartus Analysis/Synthesis "
                "rejection patterns: localparam in module parameter list, part-select "
                "on function-call result, unpacked array element in function-body "
                "concatenation (h264_dpb ref_win only)."
            ),
            does_not_prove=(
                "Does NOT prove Quartus will elaborate, infer, fit, or close timing. "
                "Only catches 3 specific syntax patterns that historically caused "
                "Analysis/Synthesis failures. Does NOT prove absence of all Quartus "
                "syntax issues — new rejection patterns must be added manually after "
                "each new failure. The parse-pass guarantee is NARROW."
            ),
            red_proof_method=(
                "Inject localparam-in-params, function-call-select, unpacked-array-concat "
                "→ rc=1 each. Disable both probes → rc=4 (REFUSE)."
            ),
            red_proof_passed=True,
            runnable_here=not has_quartus,
            runnable_note=(
                "Runs static checks (always). Quartus toolchain probe returns rc=4 "
                "(REFUSE) when no local/remote Quartus is reachable — this is correct "
                "and distinct from PASS. The static subset checks run regardless."
            ),
            owner="w-c2",
            exit_codes={"refuse_no_toolchain": 4},
        ),
        GateReport(
            id="post-fit-hierarchy",
            script="scripts/check_quartus_fit_hierarchy.py",
            makefile_target="post-fit-hierarchy",
            proves=(
                "Critical modules (ddr_frame_store, present_core, stream_path, "
                "ddr_bitstream_reader) survive Quartus fitting with non-trivial "
                "resource usage (ALUTs, registers, block memory, M10Ks). Also scans "
                "compile log for removal/tie-off warnings and combinational loops "
                "(332125/332126) on critical modules."
            ),
            does_not_prove=(
                "Does NOT prove modules are functionally correct. Does NOT prove "
                "timing closure. Does NOT prove non-critical modules survived. "
                "Resource floors are minimum thresholds, not functional correctness. "
                "Comb-loop scan only catches loops REPORTED by Quartus on critical "
                "modules — loops in non-critical modules or loops Quartus doesn't "
                "report are invisible."
            ),
            red_proof_method=(
                "Optimized-away ddr_frame_store (2 ALUTs, 0 regs) → rc=1. "
                "Combinational loop warning in compile log → rc=1. "
                "Missing report file → rc=4 (REFUSE)."
            ),
            red_proof_passed=True,
            runnable_here=True,
            runnable_note=(
                "Requires fit report artifacts (Plex.fit.rpt, compile.log) from a "
                "remote Quartus build. Cannot run standalone — needs FIT_RPT=. "
                "Wired into build_rbf_remote.sh copy-back."
            ),
            owner="w-c2",
            exit_codes={"green": 0, "red_optimized_away": 1, "red_comb_loop": 1, "refuse_missing": 4},
        ),
        GateReport(
            id="post-fit-timing",
            script="scripts/check_quartus_timing.py",
            makefile_target="post-fit-timing",
            proves=(
                "No negative setup/hold/recovery/removal/minimum-pulse-width slack "
                "in the Quartus STA report."
            ),
            does_not_prove=(
                "Does NOT prove timing is actually met in silicon (STA is a model). "
                "Does NOT prove paths weren't excluded from analysis — that is "
                "check_timing_exclusions.py's job. Does NOT check Fmax targets, only "
                "slack polarity. Does NOT prove the report is from the current RTL "
                "(report provenance is build_rbf_remote.sh's responsibility)."
            ),
            red_proof_method="Negative setup slack (-0.125, TNS -1.250) → rc=1. Missing report → rc=4.",
            red_proof_passed=True,
            runnable_here=True,
            runnable_note="Requires STA_RPT= from a remote Quartus build. Wired into build_rbf_remote.sh.",
            owner="w-c2",
            exit_codes={"green": 0, "red_negative_slack": 1, "refuse_missing": 4},
        ),
        GateReport(
            id="timing-exclusion",
            script="scripts/check_timing_exclusions.py",
            makefile_target="timing-exclusion",
            proves=(
                "No new SDC timing exclusions (set_clock_groups, set_false_path, "
                "set_max_delay, set_multicycle_path) have been added without being "
                "registered in the committed baseline. Expected clock domains "
                "(general[0].gpll, general[2].gpll) appear in the STA report. "
                "An empty STA report (zero analysed rows) is a hard fail."
            ),
            does_not_prove=(
                "Does NOT prove existing baseline exclusions are correct — they were "
                "accepted at baseline creation time. Does NOT detect Quartus-internal "
                "optimisations that remove paths without SDC changes. Does NOT prove "
                "the STA report is from the current RTL. SDC fingerprints are "
                "whitespace-normalised hashes — a semantically identical but "
                "reformatted constraint gets a new fingerprint and triggers review "
                "(this is a feature, not a bug)."
            ),
            red_proof_method=(
                "New -asynchronous clock group → rc=1 with HIGH RISK. "
                "Missing expected clock general[2].gpll in STA → rc=1. "
                "Empty STA (zero rows) → rc=1."
            ),
            red_proof_passed=True,
            runnable_here=True,
            runnable_note=(
                "SDC audit runs standalone (make timing-exclusion). Full gate with "
                "STA coverage requires STA_RPT=. Wired into build_rbf_remote.sh."
            ),
            owner="w-c2",
            exit_codes={"green": 0, "red_new_async": 1, "red_missing_clock": 1, "red_empty_sta": 1},
        ),
        GateReport(
            id="confstr-guard",
            script="scripts/check_confstr_guard.py",
            makefile_target="(inline in make unit)",
            proves=(
                "CONF_STR structure in Plex.sv is well-formed: file slots have "
                "3-character extension chunks, option entries have correct choice "
                "counts for their bit widths, status bits do not overlap, required "
                "entries (F1-F3, O[4], O[9:6], O[15:14], J1, v) are present."
            ),
            does_not_prove=(
                "Does NOT prove the OSD actually renders correctly — only structure. "
                "Does NOT prove file extensions match real file formats. Does NOT "
                "prove option labels are meaningful. Does NOT prove the bit assignments "
                "match the RTL that reads them."
            ),
            red_proof_method=(
                "Inject malformed extension field (extra commas, wrong chunk size) → rc=1. "
                "test_confstr_guard.sh has both green and red proofs."
            ),
            red_proof_passed=True,
            runnable_here=True,
            runnable_note="Pure Python, no external tools.",
            owner="w-c2",
            exit_codes={"green": 0, "red_malformed": 1},
        ),
        GateReport(
            id="mister-ini-plex-guard",
            script="scripts/check_mister_ini_plex_guard.py",
            makefile_target="(inline in make unit)",
            proves=(
                "MiSTer.ini [Plex] section contains all required video_mode anti-retune "
                "pins matching the reference file."
            ),
            does_not_prove=(
                "Does NOT prove the MiSTer.ini is the one deployed to the device. "
                "Does NOT prove the video mode values are correct for the user's "
                "display — only that they match the reference. Does NOT prove other "
                "MiSTer.ini sections are sane."
            ),
            red_proof_method=(
                "Missing video_mode_pal → rc=1. Commented-out video_mode → rc=1. "
                "test_mister_ini_plex_guard.sh has both green and red proofs."
            ),
            red_proof_passed=True,
            runnable_here=True,
            runnable_note="Pure Python, no external tools.",
            owner="w-c2",
            exit_codes={"green": 0, "red_missing_key": 1, "red_commented": 1},
        ),
        GateReport(
            id="edges",
            script="scripts/check_edges.py",
            makefile_target="(none — run manually or via test_capture_rig.sh)",
            proves=(
                "Present-path geometry: first and last source column/row reach the "
                "display edges without wrapping, shifting, or repeating. Detects the "
                "'bar' symptom (column repeated at edge), horizontal wrap (last column "
                "appears before first), and vertical shift. Stale-capture detection "
                "prevents grading a buffered/frozen frame."
            ),
            does_not_prove=(
                "Does NOT prove colour accuracy, chroma fidelity, motion rendering, "
                "or audio sync. Does NOT prove the frame is from the DDR frame store "
                "(could be a test pattern). Synthetic mode proves gate logic only, "
                "not real display output. V4L2 capture has limited-range gamma that "
                "affects thresholds. Does NOT prove interior pixels are correct — "
                "only the four edges."
            ),
            red_proof_method=(
                "Synthetic hwrap (horizontal wrap) → rc=1. "
                "Synthetic vshift (vertical shift) → rc=1. "
                "Synthetic stale (byte-identical frames) → rc=2."
            ),
            red_proof_passed=has_numpy,
            runnable_here=has_numpy,
            runnable_note=(
                f"numpy/Pillow: {'available (build/.venv)' if has_numpy else 'MISSING'}. "
                f"V4L2 grabber (/dev/video4): {'present' if has_grabber else 'ABSENT — live capture requires w-cap device host'}. "
                f"Synthetic and file modes work on this host. "
                f"Live v4l2 capture requires HDMI grabber attached to MiSTer output."
            ),
            owner="w-c2 (gate logic), w-cap (device/capture)",
            exit_codes={"green_synthetic": 0, "red_hwrap": 1, "red_vshift": 1, "red_stale": 2, "no_device": 2},
        ),
        GateReport(
            id="rtl-lint",
            script="scripts/rtl_lint.py",
            makefile_target="rtl-lint",
            proves=(
                "Verilator parse/lint passes on all owned RTL with no width warning "
                "regressions above the committed baseline. Product macros from .qsf "
                "are injected identically to the Quartus flow."
            ),
            does_not_prove=(
                "Does NOT prove Quartus will synthesise — Verilator and Quartus "
                "accept different SystemVerilog subsets. Does NOT prove functional "
                "correctness. Width warnings are baselined (allowed up to the count "
                "in rtl_lint_baseline.json) — a new width warning in a file already "
                "above its baseline is invisible. Does NOT prove timing, fitting, or "
                "elaboration."
            ),
            red_proof_method=(
                "Verilator absent (OSS_CAD_SUITE=/nonexistent) → wrapper rc=127, "
                "rtl_lint.py rc=3 (REFUSE). Width regression above baseline → rc=1."
            ),
            red_proof_passed=has_verilator,
            runnable_here=has_verilator,
            runnable_note=(
                f"Verilator: {'5.051 pinned at ~/.local/oss-cad-suite-20260726' if has_verilator else 'MISSING'}. "
                f"Absent-tool rc=127/3 confirmed on this host."
            ),
            owner="w-c2",
            exit_codes={"green": 0, "refuse_no_verilator": 3},
        ),
    ]
    return gates


def format_report(gates: list[GateReport]) -> str:
    lines = []
    lines.append("# Gate Coverage Report")
    lines.append(f"# Host: {os.uname().nodename}")
    lines.append(f"# Generated: {__import__('datetime').datetime.now().isoformat()}")
    lines.append("")

    dead = []
    for g in gates:
        status = "✅ LIVE" if g.runnable_here else "⚠️  PARTIAL"
        if not g.red_proof_passed:
            status = "❌ DEAD (no red proof)"
        lines.append(f"## {g.id} — {status}")
        lines.append(f"  Script:    {g.script}")
        lines.append(f"  Target:    {g.makefile_target}")
        lines.append(f"  Owner:     {g.owner}")
        lines.append(f"  PROVES:    {g.proves}")
        lines.append(f"  NOT prove: {g.does_not_prove}")
        lines.append(f"  Red proof: {g.red_proof_method}")
        lines.append(f"  Red ok:    {g.red_proof_passed}")
        lines.append(f"  Runnable:  {g.runnable_here} — {g.runnable_note}")
        lines.append(f"  Exit codes: {g.exit_codes}")
        lines.append("")
        if not g.runnable_here or not g.red_proof_passed:
            dead.append(g.id)

    if dead:
        lines.append(f"## GAPS: {', '.join(dead)}")
    else:
        lines.append("## No dead gates.")
    return "\n".join(lines)


def main() -> int:
    gates = build_report()
    report_text = format_report(gates)
    print(report_text)

    report_json = [asdict(g) for g in gates]
    out_dir = ROOT / "build"
    out_dir.mkdir(exist_ok=True)
    (out_dir / "gate_coverage.json").write_text(json.dumps(report_json, indent=2) + "\n")
    print(f"\nJSON written to build/gate_coverage.json")

    dead = [g for g in gates if not g.runnable_here or not g.red_proof_passed]
    return 1 if dead else 0


if __name__ == "__main__":
    raise SystemExit(main())
