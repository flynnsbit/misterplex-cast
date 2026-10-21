#!/usr/bin/env python3
"""PlexOB Y/C wiring on the shipping owenb-combo-s9 tree."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
TREE = ROOT / "fpga/owenb-combo-s9/PlexOB_480p"
EMU = TREE / "vendor/h264/emu.sv"
QSF = TREE / "vendor/H264.qsf"
HDR = ROOT / "host/libmisterplex/plexob_yc.hpp"
SECOND = ROOT / "fpga/PlexOB_480p/vendor/h264/emu.sv"
RING = TREE / "vendor/h264/ring_stat.sv"
SYNC = TREE / "sync_vendor.sh"
RECIPE = TREE / "fit/yc/launch_yc.sh"
HISTORICAL = (
    TREE / "fit/variants/combo/H264.qsf.A_E2",
    TREE / "fit/variants/tplfit/H264.qsf.A",
    TREE / "fit/variants/tplfit/H264.qsf.AB",
    TREE / "fit/variants/tplfit/H264.qsf.E2",
    TREE / "fit/variants/combo/launch_combo.sh",
)


def fail(msg: str) -> None:
    print(f"FAIL {msg}", file=sys.stderr)
    sys.exit(1)


def strip_sv_comments(text: str) -> str:
    """Drop // and /* */ comments. Strings keep any comment-like text inside them."""
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("//", i):
            nl = text.find("\n", i)
            if nl < 0:
                break
            i = nl
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            out.append(" ")
            if end < 0:
                break
            i = end + 2
            continue
        if text[i] == '"':
            out.append('"')
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


_RING_TOKEN = re.compile(
    r"`(ifdef|ifndef|endif)\b"
    r"|\bif\s*\(\s*(?:0|1'[bBhH]0)\s*\)"
    r"|\bbegin\b"
    r"|\bend\b"
    r"|\bring_in\s+ring_in_inst\b"
    r"|\bassign\s+(in_req|in_ix|in_word)\s*="
)


def ring_wiring_errors(text: str) -> list[str]:
    """Tie-offs, a missing instance, or an instance hidden from elaboration.

    `assign in_req=1'b0` matches with or without whitespace around `=`.
    `ring_in_inst` inside `ifdef` / `ifndef` (including the else arm) or an
    `if (0)` / `if (1'b0)` block is not a live instance.
    """
    code = strip_sv_comments(text)
    errors: list[str] = []
    if not re.search(r"\bring_in\s+ring_in_inst\b", code):
        errors.append("missing ring_in ring_in_inst")
    ifdef_depth = 0
    begin_depth = 0
    pending_if0 = 0
    if0_stack: list[int] = []
    for match in _RING_TOKEN.finditer(code):
        directive = match.group(1)
        tie = match.group(2)
        token = match.group(0)
        if directive in ("ifdef", "ifndef"):
            ifdef_depth += 1
        elif directive == "endif":
            ifdef_depth = max(0, ifdef_depth - 1)
        elif tie:
            errors.append(f"assign {tie}")
        elif token.startswith("if"):
            pending_if0 += 1
        elif token == "begin":
            begin_depth += 1
            if pending_if0:
                if0_stack.append(begin_depth)
                pending_if0 -= 1
        elif token == "end":
            if if0_stack and if0_stack[-1] == begin_depth:
                if0_stack.pop()
            begin_depth = max(0, begin_depth - 1)
        elif "ring_in_inst" in token:
            if ifdef_depth > 0:
                errors.append("ring_in_inst inside ifdef")
            if if0_stack or pending_if0:
                errors.append("ring_in_inst inside if 0")
                if not if0_stack:
                    pending_if0 = 0
    return errors


def occupied_bits(text: str) -> set[int]:
    used: set[int] = set()
    for match in re.finditer(r"[OR]\[(\d+)(?::(\d+))?\]", text):
        hi = int(match.group(1))
        lo = int(match.group(2)) if match.group(2) else hi
        if hi < lo:
            hi, lo = lo, hi
        for bit in range(lo, hi + 1):
            if bit in used:
                raise ValueError(f"bit {bit} overlaps")
            used.add(bit)
    return used


def conf_str_body(emu: str) -> str:
    start = emu.find("localparam CONF_STR = {")
    if start < 0:
        raise ValueError("no CONF_STR")
    end = emu.find("};", start)
    if end < 0:
        raise ValueError("CONF_STR does not end")
    return emu[start:end]


def reject_word82(text: str) -> None:
    if "st_sys_ix == 7'd82" in text and "status[23:20]" in text:
        raise ValueError("word 82 carries Y/C")
    if "st_sys_ix == 7'd87" not in text:
        raise ValueError("word 87 arm missing")


def main() -> None:
    if SECOND.exists():
        fail("a second fpga/PlexOB_480p emu.sv must not be added")
    emu = EMU.read_text()
    qsf = QSF.read_text()
    hdr = HDR.read_text()
    if 'VERILOG_MACRO "MISTER_DISABLE_YC' in qsf:
        fail("vendor/H264.qsf still sets MISTER_DISABLE_YC")
    if 'MISTER_DISABLE_ADAPTIVE=1' not in qsf:
        fail("MISTER_DISABLE_ADAPTIVE must stay set")
    if "32'h0B59C11A" not in qsf:
        fail("qsf BUILD_ID is not 0x0B59C11A")
    if "32'h0B59C119" in qsf:
        fail("qsf still stamps the dead BUILD_ID 0x0B59C119")
    for needle in (
        "O[21:20],Analog Output,RGB,S-Video,Composite;",
        "O[23:22],NTSC Color,NTSC,PAL60,PAL-M;",
        "status[23:20]",
        "O[6:5],Video Output,Auto,Interlaced,Progressive,Native (HDMI);",
        "yc_analog_out",
        "status[19] & ~yc_analog_out",
        "st_sys_ix == 7'd87",
        "st_sys_ix == 7'd88",
        "32'h4D505842",
    ):
        if needle not in emu:
            fail(f"emu.sv missing {needle}")
    if "O[24]," in emu or "O[24:" in emu:
        fail("O[24] is reserved for the later 240p entry and must not be used")
    if "status[24]" in emu.split("mpx_ring_sys_word")[-1].split("st_sys_word;")[0]:
        fail("word 87 must not drive status[24] in this PR")
    if "st_sys_ix == 7'd82" in emu:
        fail("word 82 (rumble) must not be overwritten")
    wiring = ring_wiring_errors(emu)
    if wiring:
        fail(f"emu.sv ring_in wiring: {', '.join(wiring)}")
    commented_inst = "\n".join((
        "// ring_in ring_in_inst (",
        "/* ring_in ring_in_inst (",
        "assign in_req = 1'b0;",
        "*/",
        "assign in_go = in_go;",
    ))
    if not ring_wiring_errors(commented_inst):
        fail("commented-out ring_in instance was accepted")
    live_inst = "\n".join((
        'wire [8*16-1:0] url = "http://x";',
        "ring_in ring_in_inst (",
        "// assign in_req = 1'b0;",
        "/* assign in_ix = 5'd0; assign in_word = 64'd0; */",
        ");",
    ))
    live_errors = ring_wiring_errors(live_inst)
    if live_errors:
        fail(f"commented tie-off was treated as live: {live_errors}")
    tight_tie = "\n".join((
        "ring_in ring_in_inst (",
        "assign in_req=1'b0;",
        "assign in_ix=5'd0;",
        "assign in_word=64'd0;",
        ");",
    ))
    tight_errors = ring_wiring_errors(tight_tie)
    for signal in ("in_req", "in_ix", "in_word"):
        if not any(signal in err for err in tight_errors):
            fail(f"assign {signal}= tie-off with no spaces was accepted ({tight_errors})")
    commented_tight = "\n".join((
        "ring_in ring_in_inst (",
        "// assign in_req=1'b0;",
        ");",
    ))
    if ring_wiring_errors(commented_tight):
        fail(f"commented tight tie-off was treated as live: {ring_wiring_errors(commented_tight)}")
    hidden_ifdef = "\n".join((
        "`ifdef NEVER",
        "ring_in ring_in_inst (",
        ");",
        "`endif",
    ))
    if not any("ifdef" in err for err in ring_wiring_errors(hidden_ifdef)):
        fail("ring_in_inst inside ifdef NEVER was accepted")
    hidden_ifndef = "\n".join((
        "`ifndef RING_IN",
        "ring_in ring_in_inst (",
        ");",
        "`endif",
    ))
    if not any("ifdef" in err for err in ring_wiring_errors(hidden_ifndef)):
        fail("ring_in_inst inside ifndef was accepted")
    hidden_else = "\n".join((
        "`ifdef NEVER",
        "wire unused = 1'b0;",
        "`else",
        "ring_in ring_in_inst (",
        ");",
        "`endif",
    ))
    if not any("ifdef" in err for err in ring_wiring_errors(hidden_else)):
        fail("ring_in_inst inside an ifdef else arm was accepted")
    hidden_if0 = "\n".join((
        "if (0) begin",
        "ring_in ring_in_inst (",
        ");",
        "end",
    ))
    if not any("if 0" in err for err in ring_wiring_errors(hidden_if0)):
        fail("ring_in_inst inside if (0) was accepted")
    hidden_if0_tight = "\n".join((
        "if(1'b0) begin",
        "ring_in ring_in_inst (",
        ");",
        "end",
    ))
    if not any("if 0" in err for err in ring_wiring_errors(hidden_if0_tight)):
        fail("ring_in_inst inside if(1'b0) was accepted")
    live_after_hidden = "\n".join((
        "`ifdef MPX_PLEXOB_BUILD_ID",
        "wire [31:0] id = 32'h0;",
        "`else",
        "wire [31:0] id = 32'h0B59C11A;",
        "`endif",
        "if (0) begin",
        "wire unused = 1'b0;",
        "end",
        "ring_in ring_in_inst (",
        ");",
    ))
    if ring_wiring_errors(live_after_hidden):
        fail(f"ring_in_inst after a closed ifdef and if (0) was rejected: {ring_wiring_errors(live_after_hidden)}")
    if "32'h0B59C11A" not in emu:
        fail("emu.sv BUILD_ID fallback is not 0x0B59C11A")
    if "32'h0B59C119" in emu:
        fail("emu.sv still has the dead BUILD_ID 0x0B59C119")
    if "kRingWordYcMode = 87" not in hdr or "kPlexObYcBuildId = 0x0B59C11Au" not in hdr:
        fail("daemon header does not match the RTL map")
    for needle in (
        "cdc_snap #(.W(26)) ar_xing",
        "if (ar_tick && ar_prim)",
        "40..47 -> 88..95",
        "st_free >= 7'd56",
    ):
        blob = emu if "ar_" in needle or "cdc_snap" in needle else RING.read_text()
        if needle not in blob:
            fail(f"missing {needle}")
    if "FITTER_AGGRESSIVE_ROUTABILITY" in qsf:
        fail("qsf enables aggressive routability")
    if not re.search(r"^set_global_assignment -name SEED 10$", qsf, re.M):
        fail("qsf SEED is not 10")
    import hashlib
    qsf_md5 = hashlib.md5(QSF.read_bytes()).hexdigest()
    if qsf_md5 != "55648a1335bf640fae56a11ba10730d2":
        fail(f"qsf md5 {qsf_md5} is not the 0x0B59C11A seed-10 file")
    e2 = re.findall(
        r'^set_instance_assignment -name ALLOW_SYNCH_CTRL_USAGE OFF -to "ascal:ascal\|o_[vh]_poly_pix\*"',
        qsf,
        re.M,
    )
    if len(e2) != 2:
        fail(f"E2 ascal lines: {len(e2)}")
    if "OPTIMIZE_POWER_DURING_FITTING OFF" not in qsf:
        fail("TPLFIT Block A missing")
    if (TREE / "vendor/sys/ascal.vhd").exists():
        fail("vendor/sys/ascal.vhd must not be committed")
    sync = SYNC.read_text()
    for rel in (
        "H264.qsf",
        "h264/cdc.sv",
        "h264/dec_stub.sv",
        "h264/disp_reader.sv",
        "h264/emu.sv",
        "h264/h264.sdc",
        "h264/ring_stat.sv",
    ):
        if rel not in sync:
            fail(f"sync_vendor.sh does not exclude {rel}")
    tracked = subprocess.check_output(
        ["git", "ls-files", "fpga/owenb-combo-s9/PlexOB_480p/vendor"], cwd=ROOT, text=True
    )
    for line in tracked.splitlines():
        rel = line.split("vendor/", 1)[1]
        if rel not in sync:
            fail(f"sync_vendor.sh does not exclude {rel}")
    if "apply_sys_patches" in RECIPE.read_text() or "launch_combo" in RECIPE.read_text():
        fail("the Y/C recipe calls the hpoly step or the historical combo launcher")
    ver = re.search(r'"v,(\d+);"', emu)
    if not ver:
        fail("CONF_STR has no v,N")
    n = ver.group(1)
    if f"kPlexObConfVersion = {n}" not in hdr:
        fail(f"daemon CONF_STR version constant is not {n}")
    if '"/media/fat/config/PlexOB.CFG"' in hdr:
        fail("daemon still pins the unversioned PlexOB.CFG path")
    player = (ROOT / "arm/misterplexd/media_player.cpp").read_text()
    if "plexObCfgPath()" not in player:
        fail("the Y/C tick does not use the versioned CFG path")
    if "PlexOB.CFG" in player:
        fail("media_player.cpp still names PlexOB.CFG")
    recipe = RECIPE.read_text()
    docker = (TREE / "run_quartus_docker.sh").read_text()
    for needle in (
        "--compile",
        "--name",
        "plexob-yc-0b59c11a",
        "ring_in:ring_in_inst",
        "--map",
        "GATE_PASS=1",
        "yc_publish_rbf",
        "h264/ring_in.sv",
        "NOTIFY_SILENT",
        "sync_vendor.sh",
        "output_files",
        "RESULT.env",
        "eac10641c5af0b808d332bb61344ee2dfdba4271",
        "diff -rq",
        "rbf.json",
    ):
        if needle not in recipe:
            fail(f"launch_yc.sh fit leg missing {needle}")
    rc_at = recipe.find('[ "$gate_rc" = 0 ]')
    publish_at = recipe.find('yc_publish_rbf "$staged" "$rel" "$gate_out" "$gate_rc"')
    if rc_at < 0 or publish_at < 0 or rc_at > publish_at:
        fail("gate rc is not checked before publish")
    if "-e NOTIFY_SILENT" not in docker or "-v /build" not in docker:
        fail("run_quartus_docker.sh does not pass NOTIFY_SILENT and /build")
    if 'NOTIFY_SILENT="${NOTIFY_SILENT:-1}"' not in docker:
        fail("run_quartus_docker.sh does not default NOTIFY_SILENT")
    if "tools/.notify_url" not in sync:
        fail("sync_vendor.sh does not exclude tools/.notify_url")
    legacy = (TREE / "build_plexob_480p.sh").read_text()
    if "HISTORICAL" not in legacy[:500]:
        fail("build_plexob_480p.sh is not marked historical")
    if "apply_sys_patches" in legacy:
        fail("build_plexob_480p.sh still calls the T1b sys patches")
    for path in HISTORICAL:
        head = path.read_text()[:400]
        if "HISTORICAL" not in head:
            fail(f"{path.name} is not marked historical")
        if path.suffix != ".sh" and path.name != "launch_combo.sh":
            pass
    try:
        occupied_bits(conf_str_body(emu))
    except ValueError as exc:
        fail(f"CONF_STR collision: {exc}")
    used = occupied_bits(conf_str_body(emu))
    if not {20, 21, 22, 23} <= used or 24 in used:
        fail(f"CONF_STR bit map wrong: {sorted(used)}")
    mutant_o24 = (
        'O[21:20],Analog Output,RGB,S-Video,Composite;\n'
        'O[20],15 kHz Raster,480i,240p;\n'
    )
    try:
        occupied_bits(mutant_o24)
    except ValueError:
        pass
    else:
        fail("MUT_O24_AT_20 was accepted")
    mutant_w82 = "(st_sys_ix == 7'd82) ? {32'd0, 28'd0, status[23:20]} :"
    try:
        reject_word82(mutant_w82)
    except ValueError:
        pass
    else:
        fail("MUT_W87_ON_82 was accepted")
    reject_word82(emu)
    gate_timing()
    gate_release()
    subprocess.check_call(["bash", str(RECIPE), "check"], cwd=ROOT)
    print("PASS PlexOB Y/C emu wiring")


def gate_timing() -> None:
    import tempfile

    gate = TREE / "fit/yc/timing_gate.py"

    def report(clocks: list[tuple[str, str, str, str, str]], tns: list[tuple[str, str]]) -> str:
        lines = [
            "; Multicorner Timing Analysis Summary ;",
            "; Clock ; Setup ; Hold ; Recovery ; Removal ; Minimum Pulse Width ;",
            "; Worst-case Slack ; 0.000 ; 0.000 ; 0.000 ; 0.000 ; 1.000 ;",
        ]
        for name, setup, hold, rec, rem in clocks:
            lines.append(f"; {name} ; {setup} ; {hold} ; {rec} ; {rem} ; 1.000 ;")
        lines.append("; Design-wide TNS ; 0.000 ; 0.000 ; 0.000 ; 0.000 ; 0.000 ;")
        for name, setup_tns in tns:
            lines.append(f"; {name} ; {setup_tns} ; 0.000 ; 0.000 ; 0.000 ; 0.000 ;")
        lines.append("+------------------+")
        return "\n".join(lines) + "\n"

    def run(
        text: str,
        log: str = "hdmi_pll_check: PASS\n",
        rbf: str = "plexob-yc-0b59c11a.rbf",
        map_text: str | None = None,
    ) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as tmp:
            sta = Path(tmp) / "H264.sta.rpt"
            log_path = Path(tmp) / "fit.log"
            sta.write_text(text)
            log_path.write_text(log)
            cmd = ["python3", str(gate), "--sta", str(sta), "--log", str(log_path), "--rbf", str(Path(tmp) / rbf)]
            if map_text is not None:
                map_path = Path(tmp) / "H264.map.rpt"
                map_path.write_text(map_text)
                cmd.extend(["--map", str(map_path)])
            proc = subprocess.run(cmd, text=True, capture_output=True)
            return proc.returncode, proc.stdout

    seed9, out9 = run(report(
        [("pll_hdmi", "-0.386", "0.099", "3.795", "0.370"), ("clk_sys", "0.466", "0.085", "1.783", "0.254")],
        [("pll_hdmi", "-9.731")],
    ))
    if seed9 != 2 or "PLL_HDMI_SETUP=-0.386" not in out9 or "TIMING_PASS=0" not in out9:
        fail(f"seed 9 slack was not marked marginal ({seed9})")
    seed5, out5 = run(report(
        [("pll_hdmi", "0.008", "0.088", "4.232", "0.444"), ("clk_vid", "0.874", "0.112", "1.882", "0.257")],
        [("pll_hdmi", "0.000")],
    ))
    if seed5 != 0 or "TIMING_PASS=1" not in out5 or "OTHER_SETUP=0.874" not in out5:
        fail(f"seed 5 slack was not accepted ({seed5} {out5})")
    seed2, _ = run(report(
        [("pll_hdmi", "-0.077", "0.110", "3.866", "0.551"), ("clk_sys", "0.540", "0.073", "1.910", "0.260")],
        [("pll_hdmi", "-1.536")],
    ))
    if seed2 != 0:
        fail("pll_hdmi -0.077 should meet the -0.3 bar")
    edge, _ = run(report(
        [("pll_hdmi", "-0.300", "0.010", "1.000", "0.010"), ("clk_sys", "0.100", "0.010", "1.000", "0.010")],
        [("pll_hdmi", "-1.000")],
    ))
    if edge != 0:
        fail("pll_hdmi -0.300 should meet the bar")
    seed3, _ = run(report(
        [("pll_hdmi", "-1.098", "0.123", "2.602", "0.766"), ("sys_pll", "0.377", "-0.150", "2.318", "0.259")],
        [("pll_hdmi", "-31.739")],
    ))
    if seed3 != 2:
        fail("a negative non-hdmi hold was accepted")
    marginal, out_m = run(
        report(
            [("pll_hdmi", "0.008", "0.088", "4.232", "0.444"), ("clk_vid", "0.874", "0.112", "1.882", "0.257")],
            [("pll_hdmi", "0.000")],
        ),
        rbf="plexob-yc-0b59c11a_MARGINAL.rbf",
    )
    if marginal != 2 or "RBF_MARGINAL=1" not in out_m:
        fail("a _MARGINAL RBF was accepted")
    hdmi_fail, _ = run(
        report(
            [("pll_hdmi", "0.008", "0.088", "4.232", "0.444"), ("clk_vid", "0.874", "0.112", "1.882", "0.257")],
            [("pll_hdmi", "0.000")],
        ),
        log="hdmi_pll_check: FAIL\n",
    )
    if hdmi_fail != 2:
        fail("hdmi_pll_check FAIL was accepted")
    seed5_map = report(
        [("pll_hdmi", "0.008", "0.088", "4.232", "0.444"), ("clk_vid", "0.874", "0.112", "1.882", "0.257")],
        [("pll_hdmi", "0.000")],
    )

    def entity_rpt(
        rows: list[tuple[str, str, str]],
        removed: list[str] | None = None,
        title: str = "Analysis & Synthesis Resource Utilization by Entity",
    ) -> str:
        lines = [
            f"; {title} ;",
            "; Compilation Hierarchy Node ; Entity Name ; ALMs needed [=A-B+C] ;",
        ]
        for node, entity, alms in rows:
            lines.append(f"; {node} ; {entity} ; {alms} ;")
        lines.append("+----------------------------------+")
        if removed is not None:
            lines.append("; Registers Removed During Synthesis ;")
            lines.append("; Register name ; Reason for Removal ;")
            for name in removed:
                lines.append(f"; {name} ; Lost fanout ;")
        return "\n".join(lines) + "\n"

    mapped, out_map = run(seed5_map, map_text=entity_rpt([
        ("|emu:emu", "emu", "12000"),
        ("|emu:emu|ring_in:ring_in_inst", "ring_in", "180"),
    ]))
    if mapped != 0 or "MAP_RING_IN=1" not in out_map or "TIMING_PASS=1" not in out_map or "GATE_PASS=1" not in out_map:
        fail(f"map hierarchy was not accepted ({mapped} {out_map})")
    fitter, out_fit = run(seed5_map, map_text=entity_rpt(
        [("|emu:emu|ring_in:ring_in_inst", "ring_in", "0.80")],
        title="Fitter Resource Utilization by Entity",
    ))
    if fitter != 0 or "MAP_RING_IN=1" not in out_fit or "GATE_PASS=1" not in out_fit:
        fail(f"fitter entity row was not accepted ({fitter} {out_fit})")
    removed, out_removed = run(seed5_map, map_text=entity_rpt(
        [("|emu:emu", "emu", "12000"), ("|emu:emu|ring_stat:ring_stat_inst", "ring_stat", "40")],
        removed=["|emu:emu|ring_in:ring_in_inst|count[3]"],
    ))
    if (
        removed == 0
        or "MAP_RING_IN=0" not in out_removed
        or "GATE_PASS=1" in out_removed
        or "TIMING_PASS=1" in out_removed
    ):
        fail(f"ring_in only under Registers Removed was accepted ({removed} {out_removed})")
    zero, out_zero = run(seed5_map, map_text=entity_rpt([
        ("|emu:emu|ring_in:ring_in_inst", "ring_in", "0"),
    ]))
    if zero == 0 or "MAP_RING_IN=0" not in out_zero or "TIMING_PASS=1" in out_zero or "GATE_PASS=0" not in out_zero:
        fail(f"zero-ALM ring_in row was accepted ({zero} {out_zero})")
    bare, out_bare = run(seed5_map, map_text="emu:emu|ring_in:ring_in_inst\n")
    if bare == 0 or "MAP_RING_IN=0" not in out_bare or "TIMING_PASS=1" in out_bare:
        fail(f"bare ring_in mention was accepted ({bare} {out_bare})")
    marginal_map, out_mm = run(
        report(
            [("pll_hdmi", "-0.386", "0.099", "3.795", "0.370"), ("clk_sys", "0.466", "0.085", "1.783", "0.254")],
            [("pll_hdmi", "-9.731")],
        ),
        map_text=entity_rpt([("|emu:emu|ring_in:ring_in_inst", "ring_in", "180")]),
    )
    if marginal_map != 2 or "TIMING_PASS=0" not in out_mm or "MAP_RING_IN=1" not in out_mm or "GATE_PASS=0" not in out_mm:
        fail(f"timing miss with a live ring_in was not marginal ({marginal_map} {out_mm})")


def gate_release() -> None:
    import tempfile

    recipe = ["bash", str(RECIPE), "publish"]
    payload = b"bitstream"
    manifest_body = '{"name":"plexob"}\n'

    def invoke(staged: Path, rel: Path, gate: str, rc: str | None = None) -> subprocess.CompletedProcess[str]:
        cmd = [*recipe, str(staged), str(rel)]
        if rc is not None:
            cmd.append(rc)
        return subprocess.run(cmd, input=gate, text=True, capture_output=True)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        staged = root / "output_files" / "plexob-yc-0b59c11a.rbf"
        staged.parent.mkdir()
        staged.write_bytes(payload)
        rel = root / "releases"
        rel.mkdir()
        stale = rel / staged.name
        manifest = rel / (staged.name + ".json")
        stale.write_bytes(b"stale")
        manifest.write_text(manifest_body)
        bad = invoke(
            staged,
            rel,
            "PLL_HDMI_SETUP=0.008\nTIMING_PASS=1\nMAP_RING_IN=0\nGATE_PASS=0\n",
        )
        if bad.returncode == 0 or stale.exists():
            fail(f"map failure published the rbf ({bad.returncode} {bad.stdout} {bad.stderr})")
        if manifest.exists():
            fail("failed gate left the rbf.json manifest in releases/")
        if staged.read_bytes() != payload:
            fail("publish removed the staged rbf on a map miss")
        manifest.write_text(manifest_body)
        good = invoke(staged, rel, "TIMING_PASS=1\nMAP_RING_IN=1\nGATE_PASS=1\n", "0")
        published = rel / staged.name
        if good.returncode != 0 or not published.is_file() or published.read_bytes() != payload:
            fail(f"GATE_PASS=1 did not copy the rbf ({good.returncode} {good.stdout} {good.stderr})")
        if "RBF_PATH=" not in good.stdout:
            fail("GATE_PASS=1 did not report RBF_PATH")
        if manifest.read_text() != manifest_body:
            fail("GATE_PASS=1 with rc 0 removed the rbf.json manifest")
        published.write_bytes(b"stale")
        manifest.write_text(manifest_body)
        bad_rc = invoke(staged, rel, "TIMING_PASS=1\nMAP_RING_IN=1\nGATE_PASS=1\n", "2")
        if bad_rc.returncode == 0 or published.exists() or manifest.exists():
            fail(
                "GATE_PASS=1 with rc 2 published the rbf "
                f"({bad_rc.returncode} rbf={published.exists()} json={manifest.exists()})"
            )
        if staged.read_bytes() != payload:
            fail("non-zero rc removed the staged rbf")
        published.write_bytes(b"stale")
        manifest.write_text(manifest_body)
        for gate in ("GATE_PASS=10\n", "GATE_PASS=1 extra\n", "GATE_PASS=1 \n"):
            published.write_bytes(b"stale")
            manifest.write_text(manifest_body)
            inexact = invoke(staged, rel, gate, "0")
            if inexact.returncode == 0 or published.exists() or manifest.exists():
                fail(
                    f"inexact gate {gate!r} published the rbf "
                    f"({inexact.returncode} rbf={published.exists()} json={manifest.exists()})"
                )


if __name__ == "__main__":
    main()
