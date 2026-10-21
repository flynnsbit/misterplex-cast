#!/usr/bin/env python3
"""Fast source-level guards for MiSTerPlex RTL/host ABI invariants."""
from __future__ import annotations

import os
import re
import sys
import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PRESENT_CORE = Path(os.environ.get("PRESENT_CORE", ROOT / "fpga/Plex_MiSTer/rtl/present_core.sv"))
PLEX_SV = Path(os.environ.get("PLEX_SV", ROOT / "fpga/Plex_MiSTer/Plex.sv"))
PLEX_SDC = Path(os.environ.get("PLEX_SDC", ROOT / "fpga/Plex_MiSTer/Plex.sdc"))
SYS_TOP_SDC = Path(os.environ.get("SYS_TOP_SDC", ROOT / "fpga/Plex_MiSTer/sys/sys_top.sdc"))
PLEX_PRJ_DIR = Path(os.environ.get("PLEX_PRJ_DIR", ROOT / "fpga/Plex_MiSTer"))
CDC_AUDIT_TCL = Path(os.environ.get("CDC_AUDIT_TCL", ROOT / "tests/clocks/cdc_audit.tcl"))
PLEX_QSF = Path(os.environ.get("PLEX_QSF", ROOT / "fpga/Plex_MiSTer/Plex.qsf"))
# T-RTLINV-STALE (1) binding (b): ifdef branches are evaluated under the build under test's macros.
PLEX_BUILD_QSF = Path(os.environ.get("PLEX_BUILD_QSF", ROOT / "fpga/Plex_MiSTer/Plex_480p_r1.qsf"))
DDRAM_FRAME_RD = Path(
    os.environ.get("DDRAM_FRAME_RD", ROOT / "fpga/Plex_MiSTer/rtl/ddram_frame_rd.sv")
)
DDR_FRAME_LAYOUT_SVH = Path(
    os.environ.get(
        "DDR_FRAME_LAYOUT_SVH", ROOT / "fpga/Plex_MiSTer/rtl/ddr_frame_layout_params.svh"
    )
)
DDR_FRAME_STORE = Path(
    os.environ.get("DDR_FRAME_STORE", ROOT / "fpga/Plex_MiSTer/rtl/ddr_frame_store.sv")
)
H264_DEBLOCK = Path(
    os.environ.get("H264_DEBLOCK", ROOT / "fpga/Plex_MiSTer/rtl/h264_deblock.sv")
)
H264_DPB = Path(os.environ.get("H264_DPB", ROOT / "fpga/Plex_MiSTer/rtl/h264_dpb.sv"))
ASYNC_FIFO = Path(os.environ.get("ASYNC_FIFO", ROOT / "fpga/Plex_MiSTer/rtl/async_fifo.sv"))
FPGA_SPI_HPP = Path(os.environ.get("FPGA_SPI_HPP", ROOT / "arm/misterplexd/fpga_spi.hpp"))
FPGA_SPI_CPP = Path(os.environ.get("FPGA_SPI_CPP", ROOT / "arm/misterplexd/fpga_spi.cpp"))
DDR_FRAME_LAYOUT_HPP = Path(
    os.environ.get("DDR_FRAME_LAYOUT_HPP", ROOT / "host/libmisterplex/ddr_frame_layout.hpp")
)
MEDIA_PLAYER_CPP = Path(
    os.environ.get("MEDIA_PLAYER_CPP", ROOT / "arm/misterplexd/media_player.cpp")
)
FB_PRESENT_CPP = Path(os.environ.get("FB_PRESENT_CPP", ROOT / "arm/misterplexd/fb_present.cpp"))
MISTERPLEXD_MAIN_CPP = Path(
    os.environ.get("MISTERPLEXD_MAIN_CPP", ROOT / "arm/misterplexd/main.cpp")
)
H264_RECON_HPP = Path(
    os.environ.get("H264_RECON_HPP", ROOT / "host/libmisterplex/h264_recon.hpp")
)
DDR_BITSTREAM_READER = Path(
    os.environ.get(
        "DDR_BITSTREAM_READER", ROOT / "fpga/Plex_MiSTer/rtl/ddr_bitstream_reader.sv"
    )
)
DDR_BITSTREAM_RING_HPP = Path(
    os.environ.get(
        "DDR_BITSTREAM_RING_HPP", ROOT / "host/libmisterplex/ddr_bitstream_ring.hpp"
    )
)
INPUT_MAILBOX_HPP = Path(
    os.environ.get("INPUT_MAILBOX_HPP", ROOT / "host/libmisterplex/input_mailbox.hpp")
)
MAILBOX_ABI_SPEC = Path(
    os.environ.get("MAILBOX_ABI_SPEC", ROOT / "host/libmisterplex/mailbox_abi_spec.hpp")
)
PLX_PAGE_HPP = Path(os.environ.get("PLX_PAGE_HPP", ROOT / "host/libmisterplex/plx_page.hpp"))
SDRAM_MAILBOX_HPP = Path(
    os.environ.get("SDRAM_MAILBOX_HPP", ROOT / "host/libmisterplex/sdram_mailbox.hpp")
)
PRESENT_BANK_HPP = Path(
    os.environ.get("PRESENT_BANK_HPP", ROOT / "host/libmisterplex/present_bank.hpp")
)
INPUT_MAILBOX_PROBE_CPP = Path(
    os.environ.get("INPUT_MAILBOX_PROBE_CPP", ROOT / "tools/input_mailbox_probe.cpp")
)
STATUS_TELEMETRY_HPP = Path(
    os.environ.get("STATUS_TELEMETRY_HPP", ROOT / "host/libmisterplex/status_telemetry.hpp")
)
VALIDATE_PLAYBACK_HW_SH = Path(
    os.environ.get(
        "VALIDATE_PLAYBACK_HW_SH", ROOT / "scripts/validate_playback_controls_hw.sh"
    )
)
TEST_DDR_FRAME_SH = Path(
    os.environ.get("TEST_DDR_FRAME_SH", ROOT / "tests/hw/test_ddr_frame.sh")
)
TEST_FPGA_PUSH_SH = Path(
    os.environ.get("TEST_FPGA_PUSH_SH", ROOT / "tests/hw/test_fpga_push.sh")
)
HW_README_MD = Path(os.environ.get("HW_README_MD", ROOT / "tests/hw/README.md"))
PHASE3_DECODE_MD = Path(os.environ.get("PHASE3_DECODE_MD", ROOT / "docs/phase3-decode.md"))
PUSH_FRAME_CPP = Path(os.environ.get("PUSH_FRAME_CPP", ROOT / "tools/push_frame.cpp"))
SET_STATUS_CPP = Path(os.environ.get("SET_STATUS_CPP", ROOT / "tools/set_status.cpp"))
H264_DPB_RTL = Path(os.environ.get("H264_DPB_RTL", ROOT / "fpga/Plex_MiSTer/rtl/h264_dpb.sv"))
H264_DEBLOCK_RTL = Path(os.environ.get("H264_DEBLOCK_RTL", ROOT / "fpga/Plex_MiSTer/rtl/h264_deblock.sv"))
QUARTUS_SV_GUARD = ROOT / "scripts/check_quartus_sv_subset.py"
GEN_EDGE_MARKERS_PY = Path(
    os.environ.get("GEN_EDGE_MARKERS_PY", ROOT / "scripts/gen_edge_markers.py")
)


def read(path: Path) -> str:
    try:
        return path.read_text()
    except OSError as e:
        fail(f"could not read {path}: {e}")


def strip_comments(text: str) -> str:
    out: list[str] = []
    i = 0
    in_str = False
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if in_str:
            out.append(c)
            if c == "\\" and n:
                out.append(n)
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
        elif c == '"':
            in_str = True
            out.append(c)
            i += 1
        elif c == "/" and n == "/":
            while i < len(text) and text[i] != "\n":
                i += 1
            out.append("\n")
        elif c == "/" and n == "*":
            i += 2
            while i + 1 < len(text) and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def qsf_verilog_macros(qsf_text: str) -> set[str]:
    """Names set by uncommented VERILOG_MACRO assignments in a qsf ('NAME' or 'NAME=VALUE')."""
    return {m.group(1) for m in re.finditer(
        r'^\s*set_global_assignment\s+-name\s+VERILOG_MACRO\s+"?([A-Za-z_]\w*)', qsf_text, re.M)}


def sv_select_defines(text: str, defines: set[str]) -> str:
    """The text with every `ifdef/`ifndef/`elsif/`else/`endif resolved for `defines`, nested and
    line-based. Inactive branches and all directive lines are dropped; `define/`undef in active
    code update the set in order. Unbalanced directives raise ValueError (the caller fails)."""
    out: list[str] = []
    defs = set(defines)
    stack: list[list[bool]] = []  # [parent_active, a_branch_already_taken]
    active = True
    for line in text.splitlines(keepends=True):
        m = re.match(r"\s*`(ifdef|ifndef|elsif|else|endif|define|undef)\b[ \t]*(\w*)", line)
        if not m:
            if active:
                out.append(line)
            continue
        k, name = m.group(1), m.group(2)
        if k in ("ifdef", "ifndef"):
            cond = (name in defs) == (k == "ifdef")
            stack.append([active, cond])
            active = active and cond
        elif k in ("elsif", "else", "endif") and not stack:
            raise ValueError(f"unbalanced `{k}")
        elif k == "elsif":
            parent, taken = stack[-1]
            cond = not taken and name in defs
            stack[-1][1] = taken or cond
            active = parent and cond
        elif k == "else":
            parent, taken = stack[-1]
            active = parent and not taken
            stack[-1][1] = True
        elif k == "endif":
            active = stack.pop()[0]
        elif active and k == "define":
            defs.add(name)
        elif active and k == "undef":
            defs.discard(name)
    if stack:
        raise ValueError(f"{len(stack)} unclosed `ifdef")
    return "".join(out)


# ---- shared param-override helper v2 (wT; review 20:29 (a)/(b), 20:32, 20:36; wX 20:30 API; H264 H-1/H-2) ----
# Pure text in, no paths; the host file must `import re` above this block.
# rel ending .sv/.svh/.v/.vh is RTL (comments stripped); anything else is cfg
# (qsf/qip/tcl/sdc), parsed by tcl_commands() with Tcl word rules.
_POH_RTL_EXT = (".sv", ".svh", ".v", ".vh")
_POH_DEFPARAM_LHS_RE = re.compile(
    r"\s*(?:[A-Za-z_][\w$]*(?:\s*\[[^\]]*\])?\s*\.\s*)+([A-Za-z_][\w$]*)\s*=(?!=)"
)
_POH_TCL_NAME_RE = re.compile(r"-name\s+\"?([A-Za-z_]\w*)\"?(?=\s|$)")
_POH_UNTERMINATED = "<malformed tcl word>"
# Frozen census (review 20:36 H-2): every tcl command head in the qsf/qip/tcl/sdc
# files of 1adda905 and 7fbc835b, plus set_parameter. Any other head (eval,
# uplevel, interp, rename, namespace, a $ [ \ " { head, ...) is 'unparsed'.
POH_TCL_HEADS = frozenset((
    'clock', 'close', 'create_clock', 'derive_clock_uncertainty', 'derive_pll_clocks',
    'exit', 'file', 'generateBuildID_Verilog', 'generateCDF', 'get_clocks',
    'get_current_revision', 'get_global_assignment', 'get_pins', 'get_ports',
    'get_registers', 'if', 'info', 'is_project_open', 'join', 'lindex', 'list', 'open',
    'post_message', 'proc', 'project_close', 'project_exists', 'project_open', 'puts',
    'pwd', 'read', 'regexp', 'set', 'set_clock_groups', 'set_clock_uncertainty',
    'set_false_path', 'set_global_assignment', 'set_instance_assignment',
    'set_location_assignment', 'set_max_delay', 'set_min_delay', 'set_multicycle_path',
    'set_parameter', 'source', 'string',
))
# proc and open are allowed only in sys/build_id.tcl (proc: its two names; open:
# see _poh_open_ok). H264 PH-1, review 21:04; PH-3, review 21:08.
# file (PH-4, review 21:08): only these exact (rel, command) pairs, frozen from the
# same clean trees (lines listed in the census output). Anything else is
# 'file not in census'.
_POH_FILE_CMDS = frozenset((
    ('rtl/pll.qip', 'file join $::quartus(qip_path) "pll.cmp"'),
    ('rtl/pll.qip', 'file join $::quartus(qip_path) "pll.v"'),
    ('rtl/pll.qip', 'file join $::quartus(qip_path) "pll/pll_0002.qip"'),
    ('rtl/pll.qip', 'file join $::quartus(qip_path) "pll/pll_0002.v"'),
    ('sys/build_id.tcl', 'file exists $outputFileName'),
    ('sys/pll.13.qip', 'file join $::quartus(qip_path) "pll.cmp"'),
    ('sys/pll_audio.13.qip', 'file join $::quartus(qip_path) "pll_audio.cmp"'),
    ('sys/pll_audio.13.qip', 'file join $::quartus(qip_path) "pll_audio.v"'),
    ('sys/pll_audio.13.qip', 'file join $::quartus(qip_path) "pll_audio/pll_audio_0002.v"'),
    ('sys/pll_audio.qip', 'file join $::quartus(qip_path) "pll_audio.cmp"'),
    ('sys/pll_audio.qip', 'file join $::quartus(qip_path) "pll_audio.v"'),
    ('sys/pll_audio.qip', 'file join $::quartus(qip_path) "pll_audio/pll_audio_0002.qip"'),
    ('sys/pll_audio.qip', 'file join $::quartus(qip_path) "pll_audio/pll_audio_0002.v"'),
    ('sys/pll_cfg.qip', 'file join $::quartus(qip_path) "pll_cfg/altera_pll_reconfig_core.v"'),
    ('sys/pll_cfg.qip', 'file join $::quartus(qip_path) "pll_cfg/altera_pll_reconfig_top.v"'),
    ('sys/pll_cfg.qip', 'file join $::quartus(qip_path) "pll_cfg/pll_cfg.v"'),
    ('sys/pll_cfg.qip', 'file join $::quartus(qip_path) "pll_cfg/pll_cfg_hdmi.v"'),
    ('sys/pll_hdmi.13.qip', 'file join $::quartus(qip_path) "pll_hdmi.cmp"'),
    ('sys/pll_hdmi.13.qip', 'file join $::quartus(qip_path) "pll_hdmi.v"'),
    ('sys/pll_hdmi.13.qip', 'file join $::quartus(qip_path) "pll_hdmi/pll_hdmi_0002.v"'),
    ('sys/pll_hdmi.qip', 'file join $::quartus(qip_path) "pll_hdmi.cmp"'),
    ('sys/pll_hdmi.qip', 'file join $::quartus(qip_path) "pll_hdmi.v"'),
    ('sys/pll_hdmi.qip', 'file join $::quartus(qip_path) "pll_hdmi/pll_hdmi_0002.qip"'),
    ('sys/pll_hdmi.qip', 'file join $::quartus(qip_path) "pll_hdmi/pll_hdmi_0002.v"'),
    ('sys/pll_q13.qip', 'file join $::quartus(qip_path) pll.13.qip'),
    ('sys/pll_q13.qip', 'file join $::quartus(qip_path) pll_audio.13.qip'),
    ('sys/pll_q13.qip', 'file join $::quartus(qip_path) pll_cfg.qip'),
    ('sys/pll_q13.qip', 'file join $::quartus(qip_path) pll_hdmi.13.qip'),
    ('sys/pll_q17.qip', 'file join $::quartus(qip_path) pll_audio.qip'),
    ('sys/pll_q17.qip', 'file join $::quartus(qip_path) pll_cfg.qip'),
    ('sys/pll_q17.qip', 'file join $::quartus(qip_path) pll_hdmi.qip'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) alsa.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) arcade_video.v'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) ascal.vhd'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) audio_out.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) ddr_svc.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) emu_ports.vh'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) f2sdram_safe_terminator.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) gamma_corr.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) hps_io.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) hq2x.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) i2c.v'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) i2s.v'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) iir_filter.v'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) ltc2308.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) math.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) mcp23009.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) mt32pi.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) osd.v'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) pll_hdmi_adj.vhd'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) scandoubler.v'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) scanlines.v'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) sd_card.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) shadowmask.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) sigma_delta_dac.v'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) spdif.v'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) sys_top.sdc'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) sys_top.v'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) sysmem.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) vga_out.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) video_cleaner.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) video_freak.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) video_freezer.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) video_mixer.sv'),
    ('sys/sys.qip', 'file join $::quartus(qip_path) yc_out.sv'),
))
# PH-5 (review 21:14): sys/build_id.tcl generates RTL (build_id.v), so it is frozen
# WHOLE: md5:count of its tcl_commands() list joined by '\n'. Normalization
# drops only top-level comments and blank lines; proc/if body text (including
# comments in it) is kept verbatim. Any edit needs a re-census (H264 + Bug).
_POH_BUILD_ID = 'dca19673b923a99b822f22329543d7d6:59'


def _poh_build_id_digest(text: str) -> str:
    import hashlib
    cmds = tcl_commands(text)
    return f"{hashlib.md5(chr(10).join(cmds).encode()).hexdigest()}:{len(cmds)}"
# PH-2 (accepted, fail-closed): brace DATA whose text names set_parameter or
# '-name PARAMETER' is reported as 'unparsed parameter line'.
_POH_PROC_NAMES = frozenset(("generateBuildID_Verilog", "generateCDF"))
_POH_DENY_HEADS = frozenset(("eval", "uplevel", "interp", "rename", "namespace", "subst", "after", "apply"))


def poh_strip_rtl(text: str) -> str:
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] not in '"\n':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            seg = text[i:n if j < 0 else j + 2]
            out.append("\n" * seg.count("\n"))
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


_POH_INDEX_RE = re.compile(r"\[(\d+(?::\d+)?|\*)\]")


def _poh_bracket(t: str, i: int) -> tuple:
    # t[i] == '['. A bus index ([3], [7:0], [*]) is not a command (H264 21:01).
    m = _POH_INDEX_RE.match(t, i)
    if m:
        return [], m.end()
    return _poh_tcl_parse(t, i + 1, "]")


def _poh_tcl_subst(t: str) -> list:
    # Commands substituted by [..] anywhere in t (backslash escapes honoured).
    cmds, i, n = [], 0, len(t)
    while i < n:
        if t[i] == "\\":
            i += 2
        elif t[i] == "[":
            sub, i = _poh_bracket(t, i)
            cmds.extend(sub)
        else:
            i += 1
    return cmds


def _poh_tcl_parse(t: str, i: int, close: str) -> tuple:
    # Tcl word rules: '"' and '{' open a word only at word start; '\' escapes one
    # char; '[..]' is a nested script in bare and quoted words, never in braces;
    # '#' is a comment only where a command starts. A brace word is data, except
    # the expr/body words of 'if' and the body of 'proc' (the only script heads
    # in the census). Returns (commands, next index).
    n, cmds, ws = len(t), [], " \t\r"

    def at_sep(k: int) -> bool:
        return k >= n or t[k] in ws + "\n;" or t.startswith("\\\n", k) or (bool(close) and t[k] == close)

    while True:
        while i < n and (t[i] in ws + "\n;" or t.startswith("\\\n", i)):
            i += 2 if t[i] == "\\" else 1
        if i >= n:
            if close:
                cmds.append(_POH_UNTERMINATED)
            return cmds, i
        if close and t[i] == close:
            return cmds, i + 1
        if t[i] == "#":
            while i < n and t[i] != "\n":
                i += 2 if t[i] == "\\" else 1
            continue
        start, extra, words = i, [], []
        while i < n and t[i] not in "\n;" and not (close and t[i] == close):
            if t[i] in ws or t.startswith("\\\n", i):
                i += 2 if t[i] == "\\" else 1
                continue
            if t[i] == "{":
                depth, i, bs = 1, i + 1, i + 1
                while i < n and depth:
                    if t[i] == "\\":
                        i += 2
                        continue
                    depth += {"{": 1, "}": -1}.get(t[i], 0)
                    i += 1
                if depth:
                    extra.append(_POH_UNTERMINATED)
                words.append(("{", t[bs:i - 1]))
                if not at_sep(i):
                    extra.append(_POH_UNTERMINATED)
            elif t[i] == '"':
                i, bs = i + 1, i + 1
                while i < n and t[i] != '"':
                    if t[i] == "\\":
                        i += 2
                    elif t[i] == "[":
                        sub, i = _poh_bracket(t, i)
                        extra.extend(sub)
                    else:
                        i += 1
                if i >= n:
                    extra.append(_POH_UNTERMINATED)
                words.append(('"', t[bs:i]))
                i += 1
                if not at_sep(i):
                    extra.append(_POH_UNTERMINATED)
            else:
                bs = i
                while not at_sep(i):
                    if t[i] == "\\":
                        i += 2
                    elif t[i] == "[":
                        sub, i = _poh_bracket(t, i)
                        extra.extend(sub)
                    else:
                        i += 1
                words.append(("", t[bs:i]))
        cmds.append(re.sub(r"\\\n\s*", " ", t[start:i]).strip())
        cmds.extend(extra)
        head = words[0][1] if words and words[0][0] == "" else ""
        rest = words[1:]
        if head == "if":
            want = "expr"
            for kind, w in rest:
                if want == "expr":
                    cmds.extend(_poh_tcl_subst(w) if kind == "{" else [])
                    want = "body"
                elif kind == "" and w == "then" and want == "body":
                    pass
                elif kind == "" and w == "elseif" and want == "kw":
                    want = "expr"
                elif kind == "" and w == "else" and want == "kw":
                    want = "body"
                elif want == "body" and kind == "{":
                    cmds.extend(_poh_tcl_parse(w, 0, "")[0])
                    want = "kw"
                else:
                    cmds.append(_POH_UNTERMINATED)
        elif head == "proc":
            if len(rest) == 3 and rest[2][0] == "{":
                cmds.extend(_poh_tcl_parse(rest[2][1], 0, "")[0])
            else:
                cmds.append(_POH_UNTERMINATED)


def tcl_commands(text: str) -> list:
    """Every tcl command in a qsf/qip/tcl/sdc text, one string each, including
    commands nested in [..] substitutions and in if/proc bodies. A malformed
    word (unterminated quote/brace/bracket, extra characters after a close
    quote/brace) yields the command '<malformed tcl word>'."""
    return _poh_tcl_parse(text, 0, "")[0]


def tcl_is_dynamic(cmd: str) -> bool:
    """True if a value in cmd is computed by tcl ($var or [substitution]); fail closed."""
    return "$" in cmd or "[" in cmd


def tcl_head(cmd: str) -> str:
    return cmd.split(None, 1)[0] if cmd.strip() else ""


def _poh_static_path(w: str) -> bool:
    w = w[1:-1] if len(w) >= 2 and w[0] + w[-1] in ('""', "{}") else w
    return bool(w) and not any(c in w for c in "$[\\|\"{} \t")


_POH_OUTNAME_LINES = (
    'set outputFileName "build_id.v"',
    "if { [file exists $outputFileName]} {",
    'set outputFile [open $outputFileName "r"]',
    'set outputFile [open $outputFileName "w"]',
    'post_message "Generated: [pwd]/$outputFileName: $buildDate"',
    'set outputFileName "jtag.cdf"',
    'set outputFile [open $outputFileName "w"]',
)


def _poh_outname_problems(rel: str, text: str) -> list:
    # review 21:06: the open $outputFileName allowance holds only while the word
    # outputFileName appears exactly at its 7 frozen lines of sys/build_id.tcl,
    # all inside the two allowed proc bodies, and nowhere else.
    occ = [m.start() for m in re.finditer("outputFileName", text)]
    if not occ:
        return []
    bad = f"{rel}: unparsed parameter line (open not in census) outputFileName"
    if rel != "sys/build_id.tcl":
        return [f"{bad} outside sys/build_id.tcl"]
    lines = tuple(ln.strip() for ln in text.splitlines() if "outputFileName" in ln)
    if lines != _POH_OUTNAME_LINES or len(occ) != len(_POH_OUTNAME_LINES):
        return [f"{bad} occurrences differ from the frozen 7"]
    spans = []
    for name in sorted(_POH_PROC_NAMES):
        ms = list(re.finditer(rf"^proc {name} \{{[^{{}}]*\}} \{{", text, re.M))
        if len(ms) != 1:
            return [f"{bad} proc {name} not found exactly once"]
        i, depth = ms[0].end(), 1
        while i < len(text) and depth:
            if text[i] == "\\":
                i += 2
                continue
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        spans.append((ms[0].end(), i))
    if not all(any(a <= o < b for a, b in spans) for o in occ):
        return [f"{bad} outside the allowed proc bodies"]
    return []


def _poh_open_ok(cmd: str, rel: str) -> bool:
    # review 21:08 PH-3: only the exact commands below, only in sys/build_id.tcl.
    # The word outputFileName is frozen at 7 sites inside the two allowed proc
    # bodies by _poh_outname_problems() (review 21:06).
    return rel == "sys/build_id.tcl" and cmd in ('open $outputFileName "r"', 'open $outputFileName "w"')


def tcl_head_problem(cmd: str, rel: str = "") -> str:
    """'' if cmd's head is a static census head, else the reason (fail closed).
    rel is the project-relative cfg path (proc is allowed only in sys/build_id.tcl)."""
    head = tcl_head(cmd)
    if not head or any(c in head for c in "$[\\\"{"):
        return "dynamic command head"
    if head in _POH_DENY_HEADS or head not in POH_TCL_HEADS:
        return "command head not in census"
    if head == "source" and (tcl_is_dynamic(cmd) or "\\" in cmd):
        return "source with dynamic argument"
    if head == "proc" and (rel != "sys/build_id.tcl" or (cmd.split() + [""])[1] not in _POH_PROC_NAMES):
        return "proc not in census"
    if head == "open" and not _poh_open_ok(cmd, rel):
        return "open not in census"
    if head == "file" and (rel, cmd) not in _POH_FILE_CMDS:
        return "file not in census"
    if rel == "sys/build_id.tcl" and cmd.split()[:2] == ["set", "outputFileName"] \
            and not _poh_static_path(cmd.split(None, 2)[2] if len(cmd.split()) == 3 else ""):
        return "open not in census"
    return ""


def param_override_problems(params, rtl_files: dict, cfg_files: dict) -> list:
    """defparam (matched on the LAST path element; hierarchical, multi-line,
    comma-list), set_parameter and set_instance_assignment -name PARAMETER that
    override a name in params. Unparsable or dynamic override statements, and any
    cfg command failing tcl_head_problem(), are reported as 'unparsed parameter
    line' (fail closed)."""
    params = set(params)
    probs = []
    for rel in sorted(rtl_files):
        body = poh_strip_rtl(rtl_files[rel])
        for m in re.finditer(r"\bdefparam\b", body):
            end, q = m.end(), False
            while end < len(body) and (q or body[end] != ";"):
                if body[end] == '"':
                    q = not q
                end += 1
            if end >= len(body):
                probs.append(f"{rel}: unparsed parameter line defparam with no ';'")
                continue
            parts, cur, depth, q = [], "", 0, False
            for ch in body[m.end():end]:
                if ch == '"':
                    q = not q
                elif not q and ch in "([{":
                    depth += 1
                elif not q and ch in ")]}":
                    depth -= 1
                if ch == "," and depth == 0 and not q:
                    parts.append(cur)
                    cur = ""
                else:
                    cur += ch
            parts.append(cur)
            for part in parts:
                lm = _POH_DEFPARAM_LHS_RE.match(part)
                if lm is None:
                    probs.append(f"{rel}: unparsed parameter line defparam {' '.join(part.split())[:80]}")
                elif lm.group(1) in params:
                    probs.append(f"{rel}: defparam overrides {lm.group(1)}")
    for rel in sorted(cfg_files):
        if rel == "sys/build_id.tcl" and _poh_build_id_digest(cfg_files[rel]) != _POH_BUILD_ID:
            probs.append(f"{rel}: unparsed parameter line (build_id.tcl not frozen) "
                         f"{_poh_build_id_digest(cfg_files[rel])} != {_POH_BUILD_ID}")
        probs.extend(_poh_outname_problems(rel, cfg_files[rel]))
        for cmd in tcl_commands(cfg_files[rel]):
            why = tcl_head_problem(cmd, rel)
            if not why and tcl_head(cmd) == "source":
                # PH-W2 (review 21:08): a static source must name a file in cfg_files
                # by its exact project-relative path.
                w = cmd.split()
                if len(w) != 2 or w[1] not in cfg_files:
                    why = "source not in cfg set"
            if why:
                probs.append(f"{rel}: unparsed parameter line ({why}) {cmd[:80]}")
                continue
            head = tcl_head(cmd)
            if head == "set_parameter":
                kind = "set_parameter"
            elif head == "set_instance_assignment" and re.search(r"-name\s+\"?PARAMETER\b", cmd, re.I):
                kind = "set_instance_assignment PARAMETER"
            elif re.search(r"\bset_parameter\b|-name\s+\"?PARAMETER\b", cmd, re.I):
                if "[" not in cmd and head not in ("if", "proc"):  # else nested cmds are parsed on their own
                    probs.append(f"{rel}: unparsed parameter line {cmd[:80]}")
                continue
            else:
                continue
            if tcl_is_dynamic(cmd) or "\\" in cmd:
                probs.append(f"{rel}: unparsed parameter line {cmd[:80]}")
                continue
            if kind == "set_parameter":
                names = _POH_TCL_NAME_RE.findall(cmd)
                if len(names) != 1:
                    probs.append(f"{rel}: unparsed parameter line {cmd[:80]}")
                    continue
            else:
                names = [w for w in re.findall(r"[A-Za-z_]\w*", cmd) if w in params]
            for nm in names:
                if nm in params:
                    probs.append(f"{rel}: {kind} overrides {nm}")
    return probs


def files_naming(param: str, files: dict) -> set:
    """rels whose live text (RTL comments stripped; cfg '#' comments skipped via
    tcl_commands) names param as a whole word."""
    pat = re.compile(rf"(?<![\w$]){re.escape(param)}(?![\w$])")
    hit = set()
    for rel, text in files.items():
        live = poh_strip_rtl(text) if rel.endswith(_POH_RTL_EXT) else "\n".join(tcl_commands(text))
        if pat.search(live):
            hit.add(rel)
    return hit
# ---- end shared param-override helper ----


def select_default_sv_fault_branches(text: str) -> str:
    """Return the product/default view of small fault-injection `ifdef blocks."""
    return re.sub(
        r"`ifdef\s+DDR_FRAME_STORE_FAULT_SWAP_UV_READ\b.*?`else\s*(.*?)`endif",
        r"\1",
        text,
        flags=re.S,
    )


def norm(s: str) -> str:
    return re.sub(r"\s+", "", s)


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    print(f"rtl_invariants: FAIL, {len(SKIPS)} SKIP before it" + (f" ({', '.join(SKIPS)})" if SKIPS else ""))
    raise SystemExit(1)


def check(cond: bool, msg: str) -> None:
    if not cond:
        fail(msg)


# T-RTL-INVARIANTS-CANON (B): a 480p-lineage block is skipped only when one of its named build
# inputs is absent from the tree (canon is the daemon/host line and has no 480p build). Present
# inputs always run the block. Every skip is printed and counted in main()'s summary line.
SKIPS: list[str] = []


def skip_if_absent(block: str, files: list[Path]) -> bool:
    for f in files:
        if not f.is_file():
            rel = f.relative_to(ROOT).as_posix() if f.is_relative_to(ROOT) else str(f)
            print(f"SKIP {block}: {rel} absent from tree (canon has no 480p build)")
            SKIPS.append(block)
            return True
    return False


def parse_num(expr: str) -> int:
    expr = expr.strip().rstrip(";").replace("_", "")
    m = re.search(r"(\d+)?'([hHdD])([0-9a-fA-F]+)", expr)
    if m:
        return int(m.group(3), 16 if m.group(2).lower() == "h" else 10)
    m = re.search(r"0x([0-9a-fA-F]+)", expr, re.I)
    if m:
        return int(m.group(1), 16)
    m = re.search(r"([0-9]+)", expr)
    if m:
        return int(m.group(1), 10)
    raise ValueError(expr)


def sv_const(text: str, name: str) -> int:
    m = re.search(
        rf"(?:localparam|parameter)(?:\s+\w+)?(?:\s*\[[^\]]+\])?\s+{re.escape(name)}\s*=\s*([^,\n;)]+)",
        text,
    )
    if not m:
        fail(
            f"{name} is missing from RTL. This is part of the host/FPGA contract; "
            "restore the fixed mailbox/scanout parameter or update the matching host side and tests."
        )
    return parse_num(m.group(1))


def cpp_const_map(text: str) -> dict[str, str]:
    return {
        m.group(1): m.group(2).strip()
        for m in re.finditer(r"constexpr\s+(?:\w+\s+)+(\w+)\s*=\s*([^;]+);", text)
    }


def cpp_const(text: str, name: str) -> int:
    consts = cpp_const_map(text)
    if name not in consts:
        fail(
            f"{name} is missing from host constants. The ARM daemon and RTL must agree on "
            "DDR mailbox addresses/magic; restore the constant or update both sides together."
        )
    seen: set[str] = set()

    def resolve(expr: str) -> int:
        expr = expr.strip()
        if "|" in expr:
            value = 0
            for part in expr.split("|"):
                value |= resolve(part)
            return value
        # Handle namespace::symbol references (e.g. mailbox_abi::kPlxsAddr)
        if "::" in expr:
            bare = expr.rsplit("::", 1)[1]
            if bare in consts:
                if bare in seen:
                    fail(f"host constant alias cycle while resolving {name}")
                seen.add(bare)
                return resolve(consts[bare])
        if re.fullmatch(r"\w+", expr) and expr in consts:
            if expr in seen:
                fail(f"host constant alias cycle while resolving {name}")
            seen.add(expr)
            return resolve(consts[expr])
        return parse_num(expr)

    return resolve(consts[name])


def check_present_core() -> None:
    text = strip_comments(read(PRESENT_CORE))
    de_lag = sv_const(text, "DE_LAG")
    check(
        de_lag == 3,
        f"present_core DE_LAG is {de_lag}, expected 3. DE_LAG aligns delayed HBlank/HSync "
        "to the frame_store pixel pipeline; changing it can reintroduce the user-visible "
        "1-pixel edge wrap. If pipeline latency changed, re-run the G-VID1 edge-check "
        "simulation/hardware sweep and update this test with the new measured value.",
    )

    assigns = re.findall(r"\bstore_x\s*<=\s*([^;]+);", text)
    rhs = [norm(a) for a in assigns]
    check(
        "store_x_clamped" in rhs,
        "present_core no longer assigns store_x <= store_x_clamped on ce_pix. The edge-wrap "
        "fix depends on store_x following the clamped counter through blank/overhang pixels.",
    )
    zero_assigns = [x for x in rhs if x in {"10'd0", "0", "10'h0"}]
    check(
        len(zero_assigns) == 1 and all(x in {"10'd0", "0", "10'h0", "store_x_clamped"} for x in rhs),
        "present_core store_x has a non-reset or extra zero assignment. Do not reset store_x "
        "during blank time: that handed source column 0 to delayed right-edge pixels and caused "
        "the visible 1-pixel wrap. Keep only reset=>0 and ce_pix=>store_x_clamped.",
    )
    check(
        not any("in_content" in x or "?" in x for x in rhs),
        "present_core store_x assignment is conditional on content/blank. The G-VID1 fix requires "
        "no blank-time store_x reset; store_x must follow store_x_clamped on every ce_pix.",
    )

    nt = norm(text)
    # Template path: past_last_row/store_y_clamped derive from TPL_V_STORE (default 240).
    # Legacy form was py>=10'd240 / clamp 10'd239. Parameterized form must keep default=240.
    legacy_clamp = (
        "past_last_row=(py>=10'd240)" in nt
        and "store_y_clamped=past_last_row?10'd239:py" in nt
    )
    param_clamp = (
        "past_last_row=(py>=10'(TPL_V_STORE))" in nt
        and "TPL_V_STORE=240" in nt
        and (
            "store_y_clamped=past_last_row?10'(TPL_V_STORE>0?TPL_V_STORE-1:0):py" in nt
            or "store_y_clamped=past_last_row?10'(TPL_V_STORE-1):py" in nt
        )
    )
    check(
        legacy_clamp or param_clamp,
        "present_core past_last_row clamp is missing. It prevents fetching row 240 and stops the "
        "241st-row/bottom-edge artifact; restore past_last_row/store_y_clamped with TPL_V_STORE=240 "
        "default (or legacy 10'd240/10'd239).",
    )
    check(
        "TPL_H_DE=529" in nt or "H_DE=10'd529" in nt or "localparamH_DE=10'd529" in nt,
        "present_core Template H_DE default is not 529 (FBAR/G-VID1 contract).",
    )
    check(
        "vb_d=vb|past_last_row" in nt,
        "present_core VBlank no longer includes past_last_row. The bottom-edge fix blanks rows "
        "past source row 239; restore `vb_d = vb | past_last_row` or re-verify G-VID1.",
    )
    print("PASS present_core G-VID1 scanout invariants")


def check_phase_a_surface() -> None:
    text = strip_comments(read(PLEX_SV))
    strings = " ".join(re.findall(r'"([^"]*)"', text))
    for entry in ("F1,raw", "F2,raw", "F3,264"):
        check(
            entry in strings,
            f"Plex.sv CONF_STR missing `{entry}`. That drops a Phase A shipping file slot; "
            "restore F1 raw frame, F2 raw audio, and F3 H.264 annex-B menu entries.",
        )
    for label in ("Play/Pause", "Stop", "Skip Fwd", "Skip Back"):
        check(
            label in strings and "J1," in strings,
            f"Plex.sv J1 controller mapper missing `{label}`. Phase A playback controls must "
            "remain exposed to MiSTer's mapper; restore the J1 Play/Pause/Stop/Skip labels.",
        )
    for token, why in (
        (r"\bps2_key\b", "keyboard playback controls"),
        (r"\bjoystick_0\b", "controller playback controls"),
        (r"\.ps2_key\s*\(\s*ps2_key\s*\)", "hps_io PS/2 wiring"),
        (r"\.joystick_0\s*\(\s*joystick_0\s*\)", "hps_io joystick wiring"),
        (r"joy_rise\s*\[\s*4\s*\]", "J1 Play/Pause decode"),
        (r"joy_rise\s*\[\s*5\s*\]", "J1 Stop decode"),
        (r"joy_rise\s*\[\s*6\s*\]", "J1 Skip Fwd decode"),
        (r"joy_rise\s*\[\s*7\s*\]", "J1 Skip Back decode"),
        (r"ps2_code\s*==\s*8'h29", "Space Play/Pause decode"),
        (r"ps2_code\s*==\s*8'h76", "Esc Stop decode"),
        (r"ps2_code\s*==\s*8'h74", "Right-arrow Skip Fwd decode"),
        (r"ps2_code\s*==\s*8'h6B", "Left-arrow Skip Back decode"),
    ):
        check(
            re.search(token, text) is not None,
            f"Plex.sv missing {why}. Phase A requires joystick_0 and ps2_key command decode; "
            "restore the hps_io wiring and playback command decoder.",
        )
    print("PASS Plex.sv Phase A feature surface")


def check_plex_reset_domains() -> None:
    text = strip_comments(read(PLEX_SV))

    def missing_reset_requirements(src: str) -> list[str]:
        missing: list[str] = []
        if not re.search(
            r"`ifdef\s+DDR_FRAME_STORE\s+wire\s+present_reset\s*=\s*reset\s*;"
            r"\s*`else\s*wire\s+present_reset\s*=\s*reset\s*\|\s*sdram_startup_busy\s*;"
            r"\s*`endif",
            src,
            re.S,
        ):
            missing.append(
                "Plex.sv must keep DDR_FRAME_STORE present_core reset independent of "
                "sdram_startup_busy, while non-DDR builds still wait for SDRAM startup"
            )
        m = re.search(r"present_core\s*#\s*\(.*?\)\s+present\s*\((.*?)\n\s*\);", src, re.S)
        if not m or re.search(r"\.reset\s*\(\s*present_reset\s*\)", m.group(1)) is None:
            missing.append(
                "present_core must consume present_reset. Resetting DDR presentation with "
                "reset|sdram_startup_busy can leave accepted DDR/YUV doorbells unpublished "
                "with has_frame=0."
            )
        return missing

    missing = missing_reset_requirements(text)
    if missing:
        fail(f"Plex.sv reset-domain contract: {missing[0]}")

    faulted = re.sub(
        r"(`ifdef\s+DDR_FRAME_STORE\s+wire\s+present_reset\s*=\s*)reset(\s*;)",
        r"\1reset | sdram_startup_busy\2",
        text,
        count=1,
        flags=re.S,
    )
    if not missing_reset_requirements(faulted):
        fail("deliberately tying DDR present reset to sdram_startup_busy did not make the reset gate red")
    print("PASS Plex.sv DDR presenter reset is not held behind SDRAM startup")


def check_quartus_syntax_tripwires() -> None:
    deblock = strip_comments(read(H264_DEBLOCK))
    dpb = strip_comments(read(H264_DPB))

    def missing_quartus_requirements(deblock_text: str, dpb_text: str) -> list[str]:
        missing: list[str] = []
        m = re.search(r"module\s+h264_deblock_writeback_ctrl\s*#\s*\((.*?)\)\s*\(", deblock_text, re.S)
        if not m or re.search(r"\blocalparam\b", m.group(1)):
            missing.append(
                "h264_deblock_writeback_ctrl parameter port list contains localparam. "
                "Verilator accepts this, but Quartus rejected the fit source; keep "
                "derived port widths as parameters or move them out of the port list."
            )
        if re.search(r"\)\s*\[[^\]]+\]", dpb_text):
            missing.append(
                "h264_dpb.sv contains a part-select directly on a function/expression "
                "result. Quartus rejected this around qpel_at; assign through a sized "
                "helper/value instead."
            )
        return missing

    missing = missing_quartus_requirements(deblock, dpb)
    if missing:
        fail(f"Quartus syntax tripwire: {missing[0]}")

    fault_deblock = deblock.replace("parameter int MB_AW =", "localparam int MB_AW =", 1)
    if not missing_quartus_requirements(fault_deblock, dpb):
        fail("deliberate h264_deblock localparam-in-parameter-list fault did not go red")
    fault_dpb = dpb.replace("low8(pix(row, col))", "pix(row, col)[7:0]", 1)
    if not missing_quartus_requirements(deblock, fault_dpb):
        fail("deliberate h264_dpb function-call part-select fault did not go red")
    print("PASS Quartus syntax tripwires for known Verilator-clean fit failures")


def check_async_fifo_write_full_no_comb_loop() -> None:
    text = strip_comments(read(ASYNC_FIFO))
    nt = norm(text)
    check(
        "wr_full_now=(wr_gray==wr_gray_full)" in nt,
        "async_fifo wr_full must be based on the registered write pointer. The old "
        "wr_bin_next->wr_gray_next->wr_full->wr_bin_next dependency formed a Quartus "
        "combinational loop inside ddr_frame_store input_fifo.",
    )
    check(
        "wr_bin_next=wr_bin+(wr_accept?PTR_ONE:'0)" in nt
        and "wr_accept=wr_en&&!wr_full_now" in nt,
        "async_fifo write pointer advance must use a separate wr_accept from registered "
        "full state; do not feed wr_full back through wr_bin_next.",
    )
    fault = nt.replace(
        "wr_accept=wr_en&&!wr_full_now",
        "wr_bin_next=wr_bin+((wr_en&&!wr_full)?PTR_ONE:'0)",
    )
    check(
        "wr_bin_next=wr_bin+((wr_en&&!wr_full)?PTR_ONE:'0)" not in nt
        and "wr_bin_next=wr_bin+((wr_en&&!wr_full)?PTR_ONE:'0)" in fault,
        "async_fifo deliberate write-full feedback fault did not go red",
    )
    print("PASS async_fifo write-full path has no source-level combinational loop")


def check_frame_store_cdc_contract() -> None:
    fs = strip_comments(read(DDR_FRAME_STORE))
    afifo = strip_comments(read(ASYNC_FIFO))
    sdc = strip_comments(read(PLEX_SDC) + "\n" + read(SYS_TOP_SDC))
    nft = norm(fs)
    nt_afifo = norm(afifo)

    requirements = [
        (
            "always@(posedgeclk_ddr)beginif(reset)beginreset_ddr_s1<=1'b1;reset_ddr_s2<=1'b1;",
            "ddr_frame_store must synchronously assert and release reset into clk_ddr before first DDR command",
        ),
        (
            ".wr_clk(clk),.wr_reset(reset)",
            "input FIFO write side must stay in the clk domain",
        ),
        (
            ".rd_clk(clk_ddr),.rd_reset(reset_ddr)",
            "input FIFO read side must use the clk_ddr-synchronised reset",
        ),
        (
            "swap_req_s1<=swap_req_t_ddr;swap_req_s2<=swap_req_s1;",
            "DDR-to-present frame-swap request must be a two-flop toggle crossing",
        ),
        (
            "pending_ready_s1<=pending_ready_ddr;pending_ready_s2<=pending_ready_s1;",
            "DDR-to-present pending-ready flag must be two-flop synchronised",
        ),
        (
            "line_buf_ram#(.WIDTH(Y_LINE_QWORDS),.AW(Y_QW_AW),.DATA_W(64))yram(.wr_clk(clk_ddr)",
            "frame line-buffer RAM writes must remain in clk_ddr",
        ),
        (
            ".rd_clk(clk),.rd_addr(y_rd_addr)",
            "frame line-buffer RAM reads must remain in clk",
        ),
    ]
    for needle, msg in requirements:
        check(needle in nft, f"ddr_frame_store CDC contract: {msg}")

    check(
        "wr_gray" in nt_afifo
        and "rd_gray_w1<=rd_gray;rd_gray_w2<=rd_gray_w1;" in nt_afifo
        and "wr_gray_r1<=wr_gray;wr_gray_r2<=wr_gray_r1;" in nt_afifo,
        "async_fifo CDC contract: pointers must cross as gray-coded values through two flops",
    )
    check(
        not re.search(
            r"set_(?:false|multicycle)_path[^\n]*(?:fstore|ddr_frame_store|DDRAM|present_ddr|stream_ddr|ddr_arb|async_fifo)",
            sdc,
            re.I,
        ),
        "SDC must not hide frame-store/DDR/arbiter timing with false or multicycle paths",
    )
    print("PASS frame-store CDC contract (clk/clk_ddr crossings and DDR timing constraints)")


# Frame-store PLX mailboxes: name, ddr_frame_store parameter, spec offset symbol and
# value, RTL magic localparam, spec magic symbol and value.
PLX_PAGE_MAILBOXES = [
    ("PLXS", "MAILBOX_PHYS", "kPlxsOff", 0x100, "MAGIC_S", "kPlxsMagic", 0x504C5853),
    ("PLXI", "INPUT_MAILBOX_PHYS", "kPlxiOff", 0x108, "MAGIC_I", "kPlxiMagic", 0x504C5849),
    ("PLXM", "SDRAM_MAILBOX_PHYS", "kPlxmOff", 0x110, "MAGIC_M", "kPlxmMagic", 0x504C584D),
    ("PLXF", "FRAME_MAILBOX_PHYS", "kPlxfOff", 0x118, "MAGIC_F", "kPlxfMagic", 0x504C5846),
    ("PLXD", "BANK_MAILBOX_PHYS", "kPlxdOff", 0x128, "MAGIC_D", "kPlxdMagic", 0x504C5844),
    ("PLXJ", "ASPECT_ACK_PHYS", "kPlxjOff", 0x130, "MAGIC_J", "kPlxjMagic", 0x504C584A),
]
# present_core page class: (plx_page.hpp PlxPageClass, the macros that select the branch,
# its page). review 00:55: PRESENT_480I 0x301FF, 720P_L4 0x3047F, PRESENT_480P or
# META_DPB 0x300FF, none 0x3007F (no recognised build).
PLX_PAGE_CLASSES = [
    ("NoPresentMacro", (), 0x3007F000),
    ("Present480pOrMetaDpb", ("PLEX_PRESENT_480P",), 0x300FF000),
    ("Present480pOrMetaDpb", ("PLEX_META_DPB",), 0x300FF000),
    ("Present480i", ("PLEX_PRESENT_480I",), 0x301FF000),
    ("Present720pL4", ("PLEX_PRESENT_720P_L4",), 0x3047F000),
]
SDRAM_DIAG_PHYS = 0x3007F120


# ---------------------------------------------------------------------------------------------
# T-SDC-FSDDR G1 fold (review 18:12/18:30; classification review/s11/t-sdc-fsddr, H264 PASS
# d7352bf7; E6 re-ruled LOADED-BUT-OVERRIDDEN by review 18:35).
#
# The SDC set is derived per qsf from its uncommented SDC_FILE lines, following `source` and
# QIP_FILE includes. Every set_false_path / set_multicycle_path / set_max_delay / set_min_delay /
# set_clock_groups / set_disable_timing row of every sourced core .sdc must equal, exactly
# (continuations joined, whitespace collapsed, `if {...}` guard included), a reviewed
# allow-list row. Each allow-list class must keep its static evidence at this tree.
# - sys/sys_top.sdc rows are MiSTer framework: listed, never failed. The one exception is a row
#   naming a core frame-store/DDR/arbiter/DPB-ref instance, which belongs in a core file.
# - Rows of .sdc files that no qsf sources are listed as INFO.
# - Fit evidence (STA 332049 ignored rows, the PLEX_CDC_AUDIT run, IOE registers per SDRAM pin)
#   is post-fit: review/s11/t-sdc-fsddr/harness.py.
PLEX_SDC_VERBS = ("set_false_path", "set_multicycle_path", "set_max_delay", "set_min_delay",
                  "set_clock_groups", "set_disable_timing")
_PLEX_SDC_VERB_RE = re.compile(r"^\s*(%s)\b" % "|".join(PLEX_SDC_VERBS))
_PLEX_CORE_TOKENS = re.compile(r"ddr_frame_store|fstore|present_ddr|stream_ddr|DDRAM|ddr_bus_arbiter|"
                               r"ddr_arb|ddr_dpb|dpb_sector|sdram_ref|SDRAM_|decoder_clk_xfer|u_dec_pll")
_AUDIT_GUARD = "![info exists ::env(PLEX_CDC_AUDIT)]"
PLEX_SDC_ALLOW: list[tuple[str, str, str, str]] = [
    # (class, file, guard, exact row) as of ea4ca1e0 / 7cb897b7 (identical SDC files)
    ("E1", "Plex.sdc", "", "set_false_path -from [get_registers {*ddr_frame_store:*|async_fifo:input_fifo|g_logic_ram.mem*}] -to [get_registers {*ddr_frame_store:*|async_fifo:input_fifo|g_logic_ram.read_data*}]"),
    ("E2", "Plex.sdc", "", "set_false_path -from [get_registers {*decoder_clk_xfer:*|async_fifo:*|g_logic_ram.mem*}] -to [get_registers {*decoder_clk_xfer:*|async_fifo:*|g_logic_ram.read_data*}]"),
    ("E2", "Plex.sdc", "", "set_false_path -from [get_registers {*decoder_clk_xfer*g_block_ram.mem_rtl_0*}] -to [get_registers {*decoder_clk_xfer*g_block_ram.read_data*}]"),
    ("E3", "Plex.sdc", "", "set_max_delay -from [get_registers {*decoder_clk_xfer*wr_gray[*]}] -to [get_registers {*decoder_clk_xfer*wr_gray_r1[*]}] 35.000"),
    ("E3", "Plex.sdc", "", "set_max_delay -from [get_registers {*decoder_clk_xfer*rd_gray[*]}] -to [get_registers {*decoder_clk_xfer*rd_gray_w1[*]}] 35.000"),
    ("E3", "Plex.sdc", "", "set_max_delay -from [get_registers {*decoder_clk_xfer*reset_clk}] -to [get_registers {*decoder_clk_xfer*rst_meta}] 35.000"),
    ("E3", "Plex.sdc", "", "set_max_delay -from [get_clocks {emu|u_dec_pll|altera_pll_i|cyclonev_pll|counter[0].output_counter|divclk}] -to [get_registers {*decoder_clk_xfer*wr_gray_r1*}] 35.000"),
    ("E3", "Plex.sdc", "", "set_max_delay -from [get_clocks {emu|pll|pll_inst|altera_pll_i|cyclonev_pll|counter[0].output_counter|divclk}] -to [get_registers {*decoder_clk_xfer*rd_gray_w1*}] 35.000"),
    ("E3", "Plex.sdc", "", "set_max_delay -from [get_registers {*decoder_clk_xfer*rst_sync}] -to [get_registers {*decoder_clk_xfer*rst_back1}] 35.000"),
    ("E3", "Plex.sdc", "", "set_max_delay -from [get_clocks {emu|u_dec_pll|altera_pll_i|cyclonev_pll|counter[0].output_counter|divclk}] -to [get_registers {*decoder_clk_xfer*rsp_empty_meta}] 35.000"),
    ("E4", "Plex_clk_dec.sdc", _AUDIT_GUARD, "set_clock_groups -asynchronous -group [get_clocks {emu|u_dec_pll|altera_pll_i|*|divclk}]"),
    ("E5", "Plex_sdram_ref.sdc", "", "set_false_path -from [get_registers {*sdram_ref_port:*|async_fifo:*|g_logic_ram.mem*}] -to [get_registers {*sdram_ref_port:*|async_fifo:*|g_logic_ram.read_data*}]"),
    ("E6", "Plex_sdram_ref.sdc", "", "set_max_delay -from [get_registers {*sdram_ref_port:*|async_fifo:*|wr_gray[*]}] -to [get_registers {*sdram_ref_port:*|async_fifo:*|wr_gray_r1[*]}] 7.000"),
    ("E6", "Plex_sdram_ref.sdc", "", "set_max_delay -from [get_registers {*sdram_ref_port:*|async_fifo:*|rd_gray[*]}] -to [get_registers {*sdram_ref_port:*|async_fifo:*|rd_gray_w1[*]}] 7.000"),
    ("E6", "Plex_sdram_ref.sdc", "", "set_min_delay -from [get_registers {*sdram_ref_port:*|async_fifo:*|wr_gray[*]}] -to [get_registers {*sdram_ref_port:*|async_fifo:*|wr_gray_r1[*]}] -7.000"),
    ("E6", "Plex_sdram_ref.sdc", "", "set_min_delay -from [get_registers {*sdram_ref_port:*|async_fifo:*|rd_gray[*]}] -to [get_registers {*sdram_ref_port:*|async_fifo:*|rd_gray_w1[*]}] -7.000"),
    ("E6", "Plex_sdram_ref.sdc", "", "set_max_delay -from [get_registers {*sdram_ref_port:*|ret_gray[*]}] -to [get_registers {*sdram_ref_port:*|rg1[*]}] 7.000"),
    ("E6", "Plex_sdram_ref.sdc", "", "set_min_delay -from [get_registers {*sdram_ref_port:*|ret_gray[*]}] -to [get_registers {*sdram_ref_port:*|rg1[*]}] -7.000"),
    ("E7", "Plex_sdram_ref.sdc", "", "set_false_path -to [get_registers {*sdram_ref_port:*|rst_sd_sync[*]}]"),
    ("E8", "Plex_sdram_ref.sdc", "", "set_false_path -to [get_ports {SDRAM_A[*] SDRAM_BA[*] SDRAM_DQ[*] SDRAM_DQML SDRAM_DQMH SDRAM_nCS SDRAM_nRAS SDRAM_nCAS SDRAM_nWE SDRAM_CKE SDRAM_CLK}]"),
    ("E8", "Plex_sdram_ref.sdc", "", "set_false_path -from [get_ports {SDRAM_DQ[*]}]"),
]


# I-1 (H264 70c20a9c, review 18:51): the 50 reviewed MiSTer framework exception rows of
# sys/sys_top.sdc (md5 d7face00 at d42a4460, ea4ca1e0 and 7cb897b7), frozen by exact text.
PLEX_SYS_TOP_ROWS: tuple[str, ...] = (
    'set_clock_groups -exclusive -group [get_clocks { *|pll|pll_inst|altera_pll_i|*[*].*|divclk}] -group [get_clocks { pll_hdmi|pll_hdmi_inst|altera_pll_i|*[0].*|divclk}] -group [get_clocks { pll_audio|pll_audio_inst|altera_pll_i|*[0].*|divclk}] -group [get_clocks { spi_sck}] -group [get_clocks { hdmi_sck}] -group [get_clocks { *|h2f_user0_clk}] -group [get_clocks { FPGA_CLK1_50 }] -group [get_clocks { FPGA_CLK2_50 }] -group [get_clocks { FPGA_CLK3_50 }]',
    'set_false_path -from [get_ports {KEY*}]',
    'set_false_path -from [get_ports {BTN_*}]',
    'set_false_path -to [get_ports {LED_*}]',
    'set_false_path -to [get_ports {VGA_*}]',
    'set_false_path -from [get_ports {VGA_EN}]',
    'set_false_path -to [get_ports {AUDIO_SPDIF}]',
    'set_false_path -to [get_ports {AUDIO_L}]',
    'set_false_path -to [get_ports {AUDIO_R}]',
    'set_false_path -from {get_ports {SW[*]}}',
    'set_false_path -to {cfg[*]}',
    'set_false_path -from {cfg[*]}',
    'set_false_path -from {VSET[*]}',
    'set_false_path -to {wcalc[*] hcalc[*]}',
    'set_false_path -to {hdmi_width[*] hdmi_height[*]}',
    'set_false_path -to {deb_* btn_en btn_up}',
    'set_multicycle_path -to {*_osd|osd_vcnt*} -setup 2',
    'set_multicycle_path -to {*_osd|osd_vcnt*} -hold 1',
    'set_false_path -to {*_osd|v_cnt*}',
    'set_false_path -to {*_osd|v_osd_start*}',
    'set_false_path -to {*_osd|v_info_start*}',
    'set_false_path -to {*_osd|h_osd_start*}',
    'set_false_path -from {*_osd|v_osd_start*}',
    'set_false_path -from {*_osd|v_info_start*}',
    'set_false_path -from {*_osd|h_osd_start*}',
    'set_false_path -from {*_osd|rot*}',
    'set_false_path -from {*_osd|dsp_width*}',
    'set_false_path -to {*_osd|half}',
    'set_false_path -to {WIDTH[*] HFP[*] HS[*] HBP[*] HEIGHT[*] VFP[*] VS[*] VBP[*]}',
    'set_false_path -from {WIDTH[*] HFP[*] HS[*] HBP[*] HEIGHT[*] VFP[*] VS[*] VBP[*]}',
    'set_false_path -to {FB_BASE[*] FB_BASE[*] FB_WIDTH[*] FB_HEIGHT[*] LFB_HMIN[*] LFB_HMAX[*] LFB_VMIN[*] LFB_VMAX[*]}',
    'set_false_path -from {FB_BASE[*] FB_BASE[*] FB_WIDTH[*] FB_HEIGHT[*] LFB_HMIN[*] LFB_HMAX[*] LFB_VMIN[*] LFB_VMAX[*]}',
    'set_false_path -to {vol_att[*] scaler_flt[*] led_overtake[*] led_state[*]}',
    'set_false_path -from {vol_att[*] scaler_flt[*] led_overtake[*] led_state[*]}',
    'set_false_path -from {aflt_* acx* acy* areset* arc*}',
    'set_false_path -from {arx* ary*}',
    'set_false_path -from {vs_line*}',
    'set_false_path -from {ColorBurst_Range* PhaseInc* pal_en cvbs yc_en}',
    'set_false_path -from {ascal|o_ihsize*}',
    'set_false_path -from {ascal|o_ivsize*}',
    'set_false_path -from {ascal|o_format*}',
    'set_false_path -from {ascal|o_hdown}',
    'set_false_path -from {ascal|o_vdown}',
    'set_false_path -from {ascal|o_hmin* ascal|o_hmax* ascal|o_vmin* ascal|o_vmax* ascal|o_vrrmax* ascal|o_vrr}',
    'set_false_path -from {ascal|o_hdisp* ascal|o_vdisp*}',
    'set_false_path -from {ascal|o_htotal* ascal|o_vtotal*}',
    'set_false_path -from {ascal|o_hsstart* ascal|o_vsstart* ascal|o_hsend* ascal|o_vsend*}',
    'set_false_path -from {ascal|o_hsize* ascal|o_vsize*}',
    'set_false_path -from {mcp23009|flg_*}',
    'set_false_path -to {sysmem|fpga_interfaces|clocks_resets|f2h*}',
)
# F-3 (H264 55322640, review 19:03): ALL 58 non-comment logical lines of sys/sys_top.sdc (67 physical;
# '\' continuations joined, whitespace collapsed), frozen by exact text in order (d7face00 at
# d42a4460, ea4ca1e0 and 7cb897b7). This catches escaped or eval'd verbs that are not rows.
PLEX_SYS_TOP_LINES: tuple[str, ...] = (
    'create_clock -period "50.0 MHz" [get_ports FPGA_CLK1_50]',
    'create_clock -period "50.0 MHz" [get_ports FPGA_CLK2_50]',
    'create_clock -period "50.0 MHz" [get_ports FPGA_CLK3_50]',
    'create_clock -period "100.0 MHz" [get_pins -compatibility_mode *|h2f_user0_clk]',
    'create_clock -period "100.0 MHz" [get_pins -compatibility_mode spi|sclk_out] -name spi_sck',
    'create_clock -period "10.0 MHz" [get_pins -compatibility_mode hdmi_i2c|out_clk] -name hdmi_sck',
    'derive_pll_clocks',
    'derive_clock_uncertainty',
    'set_clock_groups -exclusive -group [get_clocks { *|pll|pll_inst|altera_pll_i|*[*].*|divclk}] -group [get_clocks { pll_hdmi|pll_hdmi_inst|altera_pll_i|*[0].*|divclk}] -group [get_clocks { pll_audio|pll_audio_inst|altera_pll_i|*[0].*|divclk}] -group [get_clocks { spi_sck}] -group [get_clocks { hdmi_sck}] -group [get_clocks { *|h2f_user0_clk}] -group [get_clocks { FPGA_CLK1_50 }] -group [get_clocks { FPGA_CLK2_50 }] -group [get_clocks { FPGA_CLK3_50 }]',
    'set_false_path -from [get_ports {KEY*}]',
    'set_false_path -from [get_ports {BTN_*}]',
    'set_false_path -to [get_ports {LED_*}]',
    'set_false_path -to [get_ports {VGA_*}]',
    'set_false_path -from [get_ports {VGA_EN}]',
    'set_false_path -to [get_ports {AUDIO_SPDIF}]',
    'set_false_path -to [get_ports {AUDIO_L}]',
    'set_false_path -to [get_ports {AUDIO_R}]',
    'set_false_path -from {get_ports {SW[*]}}',
    'set_false_path -to {cfg[*]}',
    'set_false_path -from {cfg[*]}',
    'set_false_path -from {VSET[*]}',
    'set_false_path -to {wcalc[*] hcalc[*]}',
    'set_false_path -to {hdmi_width[*] hdmi_height[*]}',
    'set_false_path -to {deb_* btn_en btn_up}',
    'set_multicycle_path -to {*_osd|osd_vcnt*} -setup 2',
    'set_multicycle_path -to {*_osd|osd_vcnt*} -hold 1',
    'set_false_path -to {*_osd|v_cnt*}',
    'set_false_path -to {*_osd|v_osd_start*}',
    'set_false_path -to {*_osd|v_info_start*}',
    'set_false_path -to {*_osd|h_osd_start*}',
    'set_false_path -from {*_osd|v_osd_start*}',
    'set_false_path -from {*_osd|v_info_start*}',
    'set_false_path -from {*_osd|h_osd_start*}',
    'set_false_path -from {*_osd|rot*}',
    'set_false_path -from {*_osd|dsp_width*}',
    'set_false_path -to {*_osd|half}',
    'set_false_path -to {WIDTH[*] HFP[*] HS[*] HBP[*] HEIGHT[*] VFP[*] VS[*] VBP[*]}',
    'set_false_path -from {WIDTH[*] HFP[*] HS[*] HBP[*] HEIGHT[*] VFP[*] VS[*] VBP[*]}',
    'set_false_path -to {FB_BASE[*] FB_BASE[*] FB_WIDTH[*] FB_HEIGHT[*] LFB_HMIN[*] LFB_HMAX[*] LFB_VMIN[*] LFB_VMAX[*]}',
    'set_false_path -from {FB_BASE[*] FB_BASE[*] FB_WIDTH[*] FB_HEIGHT[*] LFB_HMIN[*] LFB_HMAX[*] LFB_VMIN[*] LFB_VMAX[*]}',
    'set_false_path -to {vol_att[*] scaler_flt[*] led_overtake[*] led_state[*]}',
    'set_false_path -from {vol_att[*] scaler_flt[*] led_overtake[*] led_state[*]}',
    'set_false_path -from {aflt_* acx* acy* areset* arc*}',
    'set_false_path -from {arx* ary*}',
    'set_false_path -from {vs_line*}',
    'set_false_path -from {ColorBurst_Range* PhaseInc* pal_en cvbs yc_en}',
    'set_false_path -from {ascal|o_ihsize*}',
    'set_false_path -from {ascal|o_ivsize*}',
    'set_false_path -from {ascal|o_format*}',
    'set_false_path -from {ascal|o_hdown}',
    'set_false_path -from {ascal|o_vdown}',
    'set_false_path -from {ascal|o_hmin* ascal|o_hmax* ascal|o_vmin* ascal|o_vmax* ascal|o_vrrmax* ascal|o_vrr}',
    'set_false_path -from {ascal|o_hdisp* ascal|o_vdisp*}',
    'set_false_path -from {ascal|o_htotal* ascal|o_vtotal*}',
    'set_false_path -from {ascal|o_hsstart* ascal|o_vsstart* ascal|o_hsend* ascal|o_vsend*}',
    'set_false_path -from {ascal|o_hsize* ascal|o_vsize*}',
    'set_false_path -from {mcp23009|flg_*}',
    'set_false_path -to {sysmem|fpga_interfaces|clocks_resets|f2h*}',
)
PLEX_SDC_GUARDS = frozenset({_AUDIT_GUARD})  # F-4: the one reviewed if-condition
PLEX_SDC_CORE_OTHER = frozenset({
    # Non-exception commands a core .sdc may hold, by exact text (anything else there is red).
    "derive_pll_clocks",
    "derive_clock_uncertainty",
    "set_clock_uncertainty -from [get_clocks {emu|u_dec_pll|altera_pll_i|cyclonev_pll|counter[0].output_counter|divclk}] -to [get_clocks {emu|u_dec_pll|altera_pll_i|cyclonev_pll|counter[0].output_counter|divclk}] -setup -add 1.000",
})
_PLEX_SDC_VERB_ANY = re.compile(r"\b(%s)\b" % "|".join(PLEX_SDC_VERBS))


def plex_sdc_scan(text: str, core: bool) -> tuple[list[tuple[int, str, str]], list[str]]:
    """((first line, guard, row) per exception command, problems). `\\` continuations are joined,
    whitespace is collapsed, `#` comment lines are dropped, and `if {cond} {` guards nest. It
    fails closed (H264 70c20a9c F-1/F-2, review 18:51):
    - an exception verb anywhere except at the start of its own row (one-line if, `;`-joined,
      eval, ...) is red;
    - a nested source/read_sdc is red (not followed);
    - an else/elseif branch is red (its guard is not modelled; an E4 copy there would keep the
      clk_dec cut on during the PLEX_CDC_AUDIT run);
    - in a core file, every other command must be a pinned PLEX_SDC_CORE_OTHER line;
    - $variables in a row are red (the row text would not pin the value)."""
    rows: list[tuple[int, str, str]] = []
    problems: list[str] = []
    guards: list[str] = []
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        start, line = i + 1, lines[i]
        while line.rstrip().endswith("\\") and i + 1 < len(lines):
            i += 1
            line = line.rstrip()[:-1] + " " + lines[i]
        i += 1
        s = " ".join(line.split())
        if not s or s.startswith("#"):
            continue
        verbs = _PLEX_SDC_VERB_ANY.findall(s)
        is_row = bool(_PLEX_SDC_VERB_RE.match(s))
        if len(verbs) != (1 if is_row else 0):
            problems.append(f":{start} exception verb outside a parsed row ({len(verbs)} verb(s), "
                            f"{'row' if is_row else 'not a row'}): {s[:100]}")
        if re.match(r"(source|read_sdc)\b", s):
            problems.append(f":{start} nested {s.split()[0]} inside an .sdc is not followed: {s[:100]}")
        m = re.fullmatch(r"\}\s*(else|elseif\b.*?)\s*\{", s)
        if m:
            problems.append(f":{start} else/elseif branch: its guard is not modelled ({s[:60]})")
            if guards:
                guards[-1] = f"!({guards[-1]}) [{m.group(1)}]"
            continue
        m = re.fullmatch(r"if\s*\{(.*)\}\s*\{", s)
        if m:
            if m.group(1).strip() not in PLEX_SDC_GUARDS:
                problems.append(f":{start} unpinned if-condition '{m.group(1).strip()[:60]}' (only "
                                f"{sorted(PLEX_SDC_GUARDS)} is reviewed)")
            guards.append(m.group(1).strip())
            continue
        if s == "}":
            if guards:
                guards.pop()
            else:
                problems.append(f":{start} unbalanced '}}'")
            continue
        if is_row:
            if "$" in s:
                problems.append(f":{start} variable substitution in an exception row: {s[:100]}")
            rows.append((start, " && ".join(guards), s))
            continue
        if s.endswith("{"):
            guards.append(f"<block {s[:40]}>")
        if core and s not in PLEX_SDC_CORE_OTHER:
            problems.append(f":{start} unrecognised command in a core .sdc: {s[:100]}")
    if guards:
        problems.append(f"unclosed block(s) at EOF: {guards}")
    return rows, problems


_INCLUDE_KEY_RE = re.compile(r"\b(SDC_FILE|SDC_ENTITY_FILE|QIP_FILE|SOURCE_TCL_SCRIPT_FILE)\b|^\s*source\b")
_INCLUDE_ASSIGN_RE = re.compile(
    r'^\s*set_global_assignment\s+(?:-library\s+"?\w+"?\s+)?-name\s+(SDC_FILE|QIP_FILE)\s+(.+?)\s*$')


def plex_sdc_sources(prj: dict[str, str], qsf: str,
                     files: list[str] | None = None) -> tuple[list[tuple[str, str]], list[str]]:
    """((sdc path, origin) per uncommented SDC_FILE reachable from qsf, problems). Paths are
    relative to the project dir; `source` and QIP_FILE are followed, and `files` (if given)
    collects every walked file in walk order. Each file is read as tcl commands through the shared
    splitter tcl_commands (review 20:33; helper 95d49ae0), one command per line. So a ';'-chained
    include, or one in an if/proc body (the only brace words the splitter parses), is seen. Fails
    closed (H264 55322640 F-5):
    - any qsf/qip/tcl command naming SDC_FILE, SDC_ENTITY_FILE, QIP_FILE, SOURCE_TCL_SCRIPT_FILE
      or `source` that the strict forms below do not parse is red;
    - an include whose target is not in the project is red.
    Not seen here: brace data. A `catch {source x}` (catch/foreach/while bodies) yields no command
    line. So every caller must also run the shared census over the same dict (PH-W, review 21:04);
    plex_sdc_inventory does.
    Strict forms:
    - `source PATH`;
    - `set_global_assignment [-library "X"] -name SDC_FILE|QIP_FILE VALUE`, where VALUE is a
      plain path, `[file join $::quartus(qip_path) "PATH"]`, or the sys.qip pll_q<ver> join."""
    out: list[tuple[str, str]] = []
    problems: list[str] = []
    seen: set[str] = set()

    def resolve(val: str, base: str) -> str | None:
        v = val.strip()
        if re.fullmatch(r"[\w./-]+", v):
            return v
        m = re.fullmatch(r'\[\s*file join \$::quartus\(qip_path\)\s+"?([\w./-]+)"?\s*\]', v)
        if m:
            return f"{base}/{m.group(1)}" if base else m.group(1)
        m = re.fullmatch(r"\[join \[list \$::quartus\(qip_path\) (\w+) \[regexp -inline \{\[0-9\]\+\} "
                         r"\$quartus\(version\)\] (\.\w+)\] \{\}\]", v)
        if m:  # the fit image is Quartus 17.0.2
            return f"{base}/{m.group(1)}17{m.group(2)}" if base else f"{m.group(1)}17{m.group(2)}"
        return None

    def walk(path: str, origin: str) -> None:
        if path in seen:
            return
        if path not in prj:
            problems.append(f"include target missing: {path} (from {origin})")
            return
        seen.add(path)
        if files is not None:
            files.append(path)
        base = path.rsplit("/", 1)[0] if "/" in path else ""
        # review 20:33: one tcl command per line (shared splitter), so a ';'-chained `source` /
        # SDC_FILE / QIP_FILE, or one in an if/proc body, is seen. A multi-line command (an if/proc
        # body is kept verbatim) is also scanned line by line, which can only add red. Line numbers
        # count lines of this view.
        for ln, line in enumerate("\n".join(tcl_commands(prj[path])).splitlines(), 1):
            if line.lstrip().startswith("#") or not _INCLUDE_KEY_RE.search(line):
                continue
            where = f"{path}:{ln}"
            m = re.fullmatch(r"\s*source\s+([\w./-]+)\s*", line)
            if m:
                walk(m.group(1), where)
                continue
            m = _INCLUDE_ASSIGN_RE.match(line)
            target = resolve(m.group(2), base) if m else None
            if target is None:
                problems.append(f"unparsed include line {where}: {' '.join(line.split())[:100]}")
            elif m.group(1) == "SDC_FILE":
                out.append((target, where))
            else:
                walk(target, where)

    walk(qsf, "project")
    return out, problems


_PLEX_MACRO_CMD_RE = re.compile(r'set_global_assignment -name VERILOG_MACRO ("?)([A-Za-z_]\w*)(?:=[\w\']+)?\1')
_PLEX_FLOW_SCRIPT_CMD = 'set_global_assignment -name PRE_FLOW_SCRIPT_FILE "quartus_sh:sys/build_id.tcl"'


def plex_build_macros(prj: dict[str, str], qsf: str) -> tuple[set[str], list[str]]:
    """(VERILOG_MACRO names, problems) for ONE build: the qsf plus every file it reaches through
    `source` / QIP_FILE (the plex_sdc_sources walk). Problems mean the set cannot be trusted (red).
    F-5 parity (H264 f930ab2c F-5, review 20:32), self-finding on 45f441e7 (1) and b5220693 N2:
    qsf_verilog_macros reads only line-anchored qsf text, so it misses a ';'-chained macro, a
    dynamic value, and a macro in a sourced tcl/qip. qsf_verilog_macros itself stays byte-unchanged
    because wX #3b calls it. Here, every walked file is read through the shared splitter
    (tcl_commands, helper 95d49ae0), and the read fails closed when:
    - a command names VERILOG_MACRO (any case) but is not exactly
      `set_global_assignment -name VERILOG_MACRO NAME[=VALUE]` (optionally quoted, static);
      this covers -remove, other options, $ [ \\, and if/catch bodies;
    - a set_global_assignment has a -name word that is missing or not a static identifier;
    - a *_FLOW_SCRIPT_FILE command is anything but the frozen build_id.tcl one (sys/sys.tcl;
      build_id.tcl itself is frozen whole by the helper);
    - the shared census (param_override_problems) or the walk report a problem in a walked file."""
    files: list[str] = []
    _, problems = plex_sdc_sources(prj, qsf, files)
    names: set[str] = set()
    for rel in files:
        for cmd in tcl_commands(prj[rel]):
            c = " ".join(cmd.split())
            w = c.split(" ")
            if w[0] == "set_global_assignment":
                i = w.index("-name") + 1 if "-name" in w else len(w)
                if i >= len(w) or not re.fullmatch(r"[A-Za-z_]\w*", w[i]):
                    problems.append(f"{rel}: set_global_assignment -name word missing or not static: {c[:100]}")
                    continue
            if re.search(r"VERILOG_MACRO", c, re.I):
                m = _PLEX_MACRO_CMD_RE.fullmatch(c)
                if m:
                    names.add(m.group(2))
                else:
                    problems.append(f"{rel}: unparsed VERILOG_MACRO command: {c[:100]}")
            if re.search(r"_FLOW_SCRIPT_FILE", c, re.I) and c != _PLEX_FLOW_SCRIPT_CMD:
                problems.append(f"{rel}: flow script not frozen: {c[:100]}")
    problems += param_override_problems(set(), {}, {r: prj[r] for r in files if not r.endswith(_POH_RTL_EXT)})
    return names, problems


def _plex_sdc_static(prj: dict[str, str], cdc_tcl: str | None, classes: set[str],
                     rows: list[tuple[str, int, str, str]]) -> list[str]:
    """Static evidence per allow-list class present at this tree (cites re-verified, latent rows
    proven dead). A failure names the class and what changed."""

def sv_preprocess(text: str, macros: set[str]) -> str:
    """The lines active under `ifdef/`ifndef/`elsif/`else/`endif for a macro set."""
    out: list[str] = []
    stack: list[tuple[bool, bool]] = []
    active = True
    defined = set(macros)
    for line in text.splitlines():
        s = line.strip()
        m = re.match(r"`(ifdef|ifndef|elsif)\s+(\w+)", s)
        if s.startswith(("`ifdef", "`ifndef")):
            cond = (m.group(2) in defined) != (m.group(1) == "ifndef")
            stack.append((active, cond))
            active = active and cond
        elif s.startswith("`elsif"):
            parent, taken = stack[-1]
            cond = m.group(2) in defined and not taken
            stack[-1] = (parent, taken or cond)
            active = parent and cond
        elif s.startswith("`else"):
            parent, taken = stack[-1]
            stack[-1] = (parent, True)
            active = parent and not taken
        elif s.startswith("`endif"):
            active = stack.pop()[0]
        elif active and s.startswith("`define"):
            defined.add(s.split()[1])
        elif active:
            out.append(line)
    return "\n".join(out)


def plex_sdc_inventory(prj: dict[str, str], cdc_tcl: str | None) -> tuple[list[str], list[str], dict]:
    """Non-exiting: (problems, info, counts) for a project dict {path relative to the project
    dir: text}."""
    problems: list[str] = []
    info: list[str] = []
    qsfs = sorted(p for p in prj if p.endswith(".qsf") and "/" not in p)
    sourced: dict[str, list[str]] = {}
    for q in qsfs:
        srcs, inc_problems = plex_sdc_sources(prj, q)
        problems += [f"{q}: {p}" for p in inc_problems]
        for sdc, origin in srcs:
            sourced.setdefault(sdc, []).append(origin)
    # PH-W (review 21:04 / 21:06): the walker ALWAYS runs together with the shared helper's census
    # (param_override_problems) over the SAME cfg dict. Rels are exact and project-relative
    # (e.g. 'sys/build_id.tcl'). A per-command tcl_head_problem alone is not enough.
    #   - A source or SDC_FILE hidden in brace data (catch/foreach/while bodies) is invisible to the
    #     walker, and goes red here on the census message alone.
    #   - PH-W2: a static source naming a file outside the dict is red twice: the walker says
    #     'include target missing' and the census says 'source not in cfg set'.
    problems += param_override_problems(set(), {}, {r: t for r, t in prj.items() if not r.endswith(_POH_RTL_EXT)})
    for sdc, origins in sorted(sourced.items()):
        if sdc not in prj:
            problems.append(f"sourced SDC file missing: {sdc} (from {sorted(set(origins))})")
    allow = {(f, g, t): c for c, f, g, t in PLEX_SDC_ALLOW}
    e4_text = next(t for c, _, _, t in PLEX_SDC_ALLOW if c == "E4")
    core: list[tuple[str, int, str, str]] = []
    counts = {"sys": 0, "core": 0, "info": 0}
    classes: dict[str, int] = {}
    seen_keys: dict[tuple[str, str, str], int] = {}
    sys_rows: list[str] = []
    for path in sorted(p for p in prj if p.endswith(".sdc")):
        is_sys = path == "sys/sys_top.sdc"
        rows, scan_problems = plex_sdc_scan(prj[path], core=not is_sys)
        if path not in sourced:
            counts["info"] += len(rows)
            if rows:
                info.append(f"{path}: {len(rows)} rows, sourced by no qsf (not checked)")
            continue
        problems += [f"{path}{p}" for p in scan_problems]
        for ln, guard, row in rows:
            if is_sys:
                counts["sys"] += 1
                sys_rows.append(row)
                if _PLEX_CORE_TOKENS.search(row):
                    problems.append(f"{path}:{ln} names a core frame-store/DDR/arbiter path in the "
                                    f"framework file; put it in a core .sdc for review: {row[:90]}")
                continue
            counts["core"] += 1
            if row == e4_text and guard != _AUDIT_GUARD:
                problems.append(f"{path}:{ln} E4 (the clk_dec clock-groups cut) outside "
                                f"'if {{{_AUDIT_GUARD}}}' (guard '{guard}'): the PLEX_CDC_AUDIT run "
                                "would keep the cut and pass vacuously")
            key = (path.rsplit("/", 1)[-1], guard, row)
            c = allow.get(key)
            if c is None:
                problems.append(f"{path}:{ln} unreviewed exception"
                                + (f" [guard: if {{{guard}}}]" if guard else "") + f": {row[:110]}")
                continue
            seen_keys[key] = seen_keys.get(key, 0) + 1
            if seen_keys[key] > 1:
                problems.append(f"{path}:{ln} duplicate reviewed row ({c}); each may appear once: {row[:80]}")
            classes[c] = classes.get(c, 0) + 1
            core.append((path, ln, guard, row))
    if "sys/sys_top.sdc" in sourced and "sys/sys_top.sdc" in prj:
        # I-1 (H264 70c20a9c, review 18:51): the framework rows are frozen at the reviewed 50.
        added = [r for r in sys_rows if r not in PLEX_SYS_TOP_ROWS]
        removed = [r for r in PLEX_SYS_TOP_ROWS if r not in sys_rows]
        if added or removed or len(sys_rows) != len(PLEX_SYS_TOP_ROWS):
            problems.append(f"sys/sys_top.sdc exception rows differ from the reviewed "
                            f"{len(PLEX_SYS_TOP_ROWS)} (sys_top d7face00): {len(sys_rows)} rows, "
                            f"added {[r[:70] for r in added[:3]]}, removed {[r[:70] for r in removed[:3]]}")
        lines = plex_sdc_lines(prj["sys/sys_top.sdc"])
        if tuple(lines) != PLEX_SYS_TOP_LINES:
            i = next((k for k, (a, b) in enumerate(zip(lines, PLEX_SYS_TOP_LINES)) if a != b),
                     min(len(lines), len(PLEX_SYS_TOP_LINES)))
            got = lines[i][:70] if i < len(lines) else "<end of file>"
            want = PLEX_SYS_TOP_LINES[i][:70] if i < len(PLEX_SYS_TOP_LINES) else "<end of file>"
            problems.append(f"sys/sys_top.sdc non-comment lines differ from the reviewed {len(PLEX_SYS_TOP_LINES)} "
                            f"(d7face00): {len(lines)} lines, first difference at #{i + 1}: got '{got}', want '{want}'")
    problems = list(dict.fromkeys(problems))
    problems += _plex_sdc_static(prj, cdc_tcl, set(classes), core)
    if "E6" in classes:
        info.append("E6 (Plex_sdram_ref.sdc +-7 ns gray bounds) is LOADED-BUT-OVERRIDDEN in r1 by "
                    "E4's clk_dec group (review 18:35): T-CDC-GRAY-SKEW open")
    counts["classes"] = classes
    counts["sourced"] = sorted(sourced)
    return problems, info, counts

def sv_eval(expr: str, env: dict[str, int]) -> int:
    e = re.sub(r"(?:\d+)?'[sS]?[hH]([0-9a-fA-F_]+)",
               lambda m: str(int(m.group(1).replace("_", ""), 16)), expr)
    e = re.sub(r"(?:\d+)?'[sS]?[dD]([0-9_]+)", lambda m: str(int(m.group(1).replace("_", ""))), e)
    e = re.sub(r"(?:\d+)?'[sS]?[bB]([01_]+)",
               lambda m: str(int(m.group(1).replace("_", ""), 2)), e)
    missing = sorted(n for n in set(re.findall(r"[A-Za-z_]\w*", e)) if n not in env)
    if missing:
        raise KeyError(", ".join(missing))
    return int(eval(e, {"__builtins__": {}}, dict(env)))

def sv_param_env(text: str, env: dict[str, int]) -> dict[str, int]:
    for m in re.finditer(
        r"(?:localparam|parameter)\s+(?:int\s+|bit\s+|logic\s+)?(?:\[[^\]]+\]\s*)?(\w+)\s*=\s*([^;,]+)[;,]",
        text,
    ):
        try:
            env[m.group(1)] = sv_eval(m.group(2), env)
        except (KeyError, SyntaxError, ValueError, TypeError, NameError, ZeroDivisionError):
            pass
    return env


def plex_sdc_lines(text: str) -> list[str]:
    """Non-comment logical lines: '\\' continuations joined, whitespace collapsed."""
    out: list[str] = []
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        while line.rstrip().endswith("\\") and i + 1 < len(lines):
            i += 1
            line = line.rstrip()[:-1] + " " + lines[i]
        i += 1
        s = " ".join(line.split())
        if s and not s.startswith("#"):
            out.append(s)
    return out



def _plex_prj_files(prj_dir: Path) -> dict[str, str]:
    skip = {"remote_out", "output_files", "output_files_480p", "db", "incremental_db", "qdb"}
    out: dict[str, str] = {}
    for f in prj_dir.rglob("*"):
        rel = f.relative_to(prj_dir).as_posix()
        if f.is_file() and f.suffix in (".qsf", ".qip", ".sdc", ".tcl", ".sv", ".v", ".svh", ".vh") \
                and rel.split("/", 1)[0] not in skip:
            out[rel] = f.read_text(errors="replace")
    return out


def check_sdc_exception_inventory() -> None:
    prj = _plex_prj_files(PLEX_PRJ_DIR)
    if not any(p.endswith(".qsf") for p in prj):
        fail(f"SDC inventory: no qsf under {PLEX_PRJ_DIR}")
    cdc_tcl = read(CDC_AUDIT_TCL) if CDC_AUDIT_TCL.exists() else None
    problems, info, counts = plex_sdc_inventory(prj, cdc_tcl)
    for line in info:
        print(f"INFO {line}")
    if problems:
        fail("SDC exception inventory (T-SDC-FSDDR): " + " | ".join(problems))

    # NEGs on a synthetic project built from the allow-list itself, so they bite at every rev
    # (d42a4460 has no core rows). Each must give the stated colour, and the base must be green.
    def rows_of(f: str) -> str:
        return "".join(t + "\n" for _, ff, g, t in PLEX_SDC_ALLOW if ff == f and not g)

    e4 = next(t for c, _, _, t in PLEX_SDC_ALLOW if c == "E4")
    e7 = next(t for c, _, _, t in PLEX_SDC_ALLOW if c == "E7")
    afifo_txt = ("wire rd_has_entry = (rd_gray != wr_gray_r2);\nalways @(posedge rd_clk) if (rd_reset) "
                 "read_data <= '0; else if (rd_prefetch) read_data <= mem[rd_bin[AW-1:0]];\n"
                 "always @(posedge rd_clk) begin wr_gray_r1 <= wr_gray; wr_gray_r2 <= wr_gray_r1; end\n")
    srp_txt = ("async_fifo #(.WIDTH(32), .AW(DEPTH_AW), .USE_BLOCK_RAM(1)) u_rq (.clk(a));\n"
               "async_fifo #(.WIDTH(99), .AW(DEPTH_AW), .USE_BLOCK_RAM(1)) u_wq (.clk(a));\n"
               '(* ASYNC_REG = "true" *) reg [1:0] rst_sd_sync = 2\'b11;\n'
               "always @(posedge clk_sdram or posedge reset)\n if (reset) rst_sd_sync <= 2'b11; "
               "else rst_sd_sync <= {rst_sd_sync[0], 1'b0};\n")
    fast = ("set_instance_assignment -name FAST_OUTPUT_REGISTER ON -to SDRAM_*\n"
            "set_instance_assignment -name FAST_OUTPUT_ENABLE_REGISTER ON -to SDRAM_DQ[*]\n"
            "set_instance_assignment -name FAST_INPUT_REGISTER ON -to SDRAM_DQ[*]\n")
    qsf = ("source sys/sys.tcl\nsource files.qip\nset_global_assignment -name SDC_FILE Plex_sdram_ref.sdc\n"
           "set_global_assignment -name SDC_FILE Plex_clk_dec.sdc\n")
    syn = {
        "Plex_480p_r1.qsf": qsf,
        "sys/sys.tcl": "set_global_assignment -name QIP_FILE sys/sys.qip\n" + fast,
        "sys/sys.qip": "set_global_assignment -name SDC_FILE           [file join $::quartus(qip_path) sys_top.sdc ]\n",
        "sys/sys_top.sdc": "".join(line + "\n" for line in PLEX_SYS_TOP_LINES),
        "files.qip": "set_global_assignment -name SDC_FILE Plex.sdc\n",
        "Plex.sdc": "derive_pll_clocks\n# core specific constraints\n" + rows_of("Plex.sdc"),
        "Plex_sdram_ref.sdc": rows_of("Plex_sdram_ref.sdc"),
        "Plex_clk_dec.sdc": ("if {![info exists ::env(PLEX_CDC_AUDIT)]} {\n\tset_clock_groups -asynchronous \\\n"
                             "\t\t-group [get_clocks {emu|u_dec_pll|altera_pll_i|*|divclk}]\n}\n"),
        "Plex_clk_pix.sdc": "set_clock_groups -asynchronous -group [get_clocks a] -group [get_clocks b]\n",
        "rtl/ddr_frame_store.sv": "async_fifo #(.WIDTH(8), .AW(2)) input_fifo (.wr_clk(clk));\n",
        "rtl/async_fifo.sv": afifo_txt,
        "rtl/sdram_ref_port.sv": srp_txt,
        "rtl/sdram_ref_phy.sv": "assign SDRAM_CKE = 1'b1;\n",
        "rtl/decoder_clk_xfer.sv": "module decoder_clk_xfer #(parameter W = 1) (input wire clk);\nendmodule\n",
        "Plex.sv": "stream_path spath (.clk(clk));\n",
    }
    syn_cdc = "set allow {\n\t{ddr_bus_arbiter:.*\\|x_s1}\n\t{ddr_dpb_sector:.*\\|y_s1}\n}\n"
    base_p, _, base_c = plex_sdc_inventory(syn, syn_cdc)
    if base_p or base_c["core"] != len(PLEX_SDC_ALLOW):
        fail(f"SDC inventory NEG base is not green with all {len(PLEX_SDC_ALLOW)} rows: {base_p} {base_c}")

    def mut(**kw: str | None) -> dict[str, str]:
        d = dict(syn)
        for k, v in kw.items():
            path = k.replace("__", "/").replace("_dot_", ".")
            if v is None:
                d.pop(path, None)
            else:
                d[path] = v
        return d

    fs_cut = "set_false_path -to [get_registers {*ddr_frame_store:*|*}]\n"
    esc = "set_false_pat\\h -to [get_registers {*ddr_frame_store:*|*}]"  # Tcl reads 'set_false_pat\h' as set_false_path
    negs = [
        # (name, project, cdc tcl, expect_green, substring of the problem text)
        ("G1-1 SDC_FILE added in the qsf (scanned, unreviewed row)",
         {**syn, "Plex_480p_r1.qsf": qsf + "set_global_assignment -name SDC_FILE Plex_extra.sdc\n",
          "Plex_extra.sdc": fs_cut}, syn_cdc, False, "Plex_extra.sdc:1 unreviewed exception"),
        ("G1-1b SDC_FILE added in the qsf, file missing",
         {**syn, "Plex_480p_r1.qsf": qsf + "set_global_assignment -name SDC_FILE Plex_gone.sdc\n"},
         syn_cdc, False, "sourced SDC file missing: Plex_gone.sdc"),
        ("G1-1c SDC_FILE added through files.qip",
         {**syn, "files.qip": syn["files.qip"] + "set_global_assignment -name SDC_FILE Plex_qip.sdc\n",
          "Plex_qip.sdc": fs_cut}, syn_cdc, False, "Plex_qip.sdc:1 unreviewed exception"),
        ("G1-2a set_min_delay row added",
         {**syn, "Plex.sdc": syn["Plex.sdc"] + "set_min_delay -from [get_registers {*ddr_bus_arbiter:*|x*}] "
          "-to [get_registers {*y*}] -1.000\n"}, syn_cdc, False, "unreviewed exception: set_min_delay"),
        ("G1-2b set_disable_timing row added",
         {**syn, "Plex.sdc": syn["Plex.sdc"] + "set_disable_timing [get_cells {*ddr_frame_store:*|*}]\n"},
         syn_cdc, False, "unreviewed exception: set_disable_timing"),
        ("G1-3a continuation split hides an added clause",
         {**syn, "Plex_sdram_ref.sdc": syn["Plex_sdram_ref.sdc"].replace(
             e7 + "\n", e7 + " \\\n    -from [get_registers {*ddr_frame_store:*|*}]\n")},
         syn_cdc, False, "unreviewed exception: set_false_path -to [get_registers {*sdram_ref_port:*|rst_sd_sync[*]}] -from"),
        ("G1-3b pure continuation split of a reviewed row (join is exact)",
         {**syn, "Plex_sdram_ref.sdc": syn["Plex_sdram_ref.sdc"].replace(
             e7 + "\n", e7.replace(" -to ", " \\\n  -to ") + "\n")}, syn_cdc, True, ""),
        ("commented-out exception is not a row",
         {**syn, "Plex.sdc": syn["Plex.sdc"] + "# " + fs_cut}, syn_cdc, True, ""),
        ("E4 PLEX_CDC_AUDIT guard removed",
         {**syn, "Plex_clk_dec.sdc": e4 + "\n"}, syn_cdc, False, "unreviewed exception: set_clock_groups"),
        ("E4 audit allow-list loses the arbiter/sector names", syn, "set allow {\n}\n", False, "E4:"),
        ("E1 row widened",
         {**syn, "Plex.sdc": syn["Plex.sdc"].replace("async_fifo:input_fifo|g_logic_ram.mem*", "*")},
         syn_cdc, False, "unreviewed exception"),
        ("E1 pointer path cut",
         {**syn, "Plex.sdc": syn["Plex.sdc"] + "set_false_path -to [get_registers {*ddr_frame_store:*|async_fifo:input_fifo|wr_gray_r1*}]\n"},
         syn_cdc, False, "unreviewed exception"),
        ("E2/E3 resurrected: decoder_clk_xfer instantiated",
         {**syn, "Plex.sv": syn["Plex.sv"] + "decoder_clk_xfer #(.W(1)) u_xfer (.clk(clk));\n"},
         syn_cdc, False, "E2/E3 resurrected"),
        ("E5 resurrected: sdram_ref fifo in logic RAM",
         {**syn, "rtl/sdram_ref_port.sv": srp_txt.replace(".USE_BLOCK_RAM(1)) u_wq", ".USE_BLOCK_RAM(0)) u_wq")},
         syn_cdc, False, "E5 resurrected"),
        ("E7 reset synchronizer cite lost",
         {**syn, "rtl/sdram_ref_port.sv": srp_txt.replace("{rst_sd_sync[0], 1'b0}", "1'b0")},
         syn_cdc, False, "E7: cite missing"),
        ("E8 FAST_INPUT_REGISTER lost",
         {**syn, "sys/sys.tcl": syn["sys/sys.tcl"].replace("FAST_INPUT_REGISTER", "FAST_INPUT_REGISTERX")},
         syn_cdc, False, "E8: sys/sys.tcl lost"),
        ("E8 SDRAM_CKE no longer constant",
         {**syn, "rtl/sdram_ref_phy.sv": "assign SDRAM_CKE = cke_q;\n"}, syn_cdc, False, "E8: cite missing"),
        ("SDC_FILE commented out in the qsf: rows become not-sourced",
         {**syn, "Plex_480p_r1.qsf": qsf.replace("set_global_assignment -name SDC_FILE Plex_sdram_ref.sdc",
                                                 "# set_global_assignment -name SDC_FILE Plex_sdram_ref.sdc")},
         syn_cdc, True, ""),
        ("framework file smuggles a core frame-store cut",
         {**syn, "sys/sys_top.sdc": syn["sys/sys_top.sdc"] + fs_cut}, syn_cdc, False, "framework file"),
        # H264 70c20a9c probes S1-S9 (review 18:51: probes as NEG teeth), each red with its own reason.
        ("S1 one-line if {1} {row}", {**syn, "Plex.sdc": syn["Plex.sdc"] + "if {1} {" + fs_cut.strip() + "}\n"},
         syn_cdc, False, "exception verb outside a parsed row"),
        ("S2 ;-joined after another command",
         {**syn, "Plex.sdc": syn["Plex.sdc"] + "derive_clock_uncertainty; " + fs_cut}, syn_cdc, False,
         "exception verb outside a parsed row"),
        ("S3 nested source of an extra .sdc",
         {**syn, "Plex.sdc": syn["Plex.sdc"] + "source Plex_x.sdc\n", "Plex_x.sdc": fs_cut}, syn_cdc, False,
         "nested source inside an .sdc is not followed"),
        ("S4 nested read_sdc of an extra .sdc",
         {**syn, "Plex.sdc": syn["Plex.sdc"] + "read_sdc Plex_y.sdc\n", "Plex_y.sdc": fs_cut}, syn_cdc, False,
         "nested read_sdc inside an .sdc is not followed"),
        ("S5' E4 copied into an else of the audit guard",
         {**syn, "Plex_clk_dec.sdc": syn["Plex_clk_dec.sdc"].replace("divclk}]\n}\n", "divclk}]\n} else {\n" + e4 + "\n}\n", 1)},
         syn_cdc, False, "else/elseif branch"),
        ("S5b new row in an elseif of the audit guard",
         {**syn, "Plex_clk_dec.sdc": syn["Plex_clk_dec.sdc"].replace("divclk}]\n}\n", "divclk}]\n} elseif {1} {\n" + fs_cut + "}\n", 1)},
         syn_cdc, False, "else/elseif branch"),
        ("S6 E4 moved out of the audit guard", {**syn, "Plex_clk_dec.sdc": e4 + "\n"}, syn_cdc, False,
         "outside 'if {![info exists ::env(PLEX_CDC_AUDIT)]}'"),
        ("S6b E4 kept, plus a second copy unguarded",
         {**syn, "Plex_clk_dec.sdc": syn["Plex_clk_dec.sdc"] + e4 + "\n"}, syn_cdc, False,
         "outside 'if {![info exists ::env(PLEX_CDC_AUDIT)]}'"),
        ("S7 row inside a foreach loop", {**syn, "Plex.sdc": syn["Plex.sdc"] + "foreach r {a} {\n" + fs_cut + "}\n"},
         syn_cdc, False, "unrecognised command in a core .sdc: foreach"),
        ("S8 framework clk_dec cut via a divclk wildcard",
         {**syn, "sys/sys_top.sdc": syn["sys/sys_top.sdc"]
          + "set_clock_groups -asynchronous -group [get_clocks {emu|*|divclk}]\n"}, syn_cdc, False,
         "sys/sys_top.sdc exception rows differ from the reviewed"),
        ("S9 eval-wrapped row", {**syn, "Plex.sdc": syn["Plex.sdc"] + "eval " + fs_cut}, syn_cdc, False,
         "exception verb outside a parsed row"),
        ("reviewed row duplicated", {**syn, "Plex_sdram_ref.sdc": syn["Plex_sdram_ref.sdc"] + e7 + "\n"},
         syn_cdc, False, "duplicate reviewed row (E7)"),
        ("variable in a row",
         {**syn, "Plex.sdc": syn["Plex.sdc"] + "set g {*ddr_frame_store:*|*}\nset_false_path -to [get_registers $g]\n"},
         syn_cdc, False, "variable substitution in an exception row"),
        ("framework row removed",
         {**syn, "sys/sys_top.sdc": syn["sys/sys_top.sdc"].replace(PLEX_SYS_TOP_ROWS[-1] + "\n", "")},
         syn_cdc, False, "sys/sys_top.sdc exception rows differ from the reviewed"),
        # Round 3 (H264 55322640 probe2 T1-T9; review 19:03), each red with its own reason.
        ("T1 sys_top escaped verb", {**syn, "sys/sys_top.sdc": syn["sys/sys_top.sdc"] + esc + "\n"},
         syn_cdc, False, "sys/sys_top.sdc non-comment lines differ"),
        ("T2 sys_top eval of a string-mapped verb",
         {**syn, "sys/sys_top.sdc": syn["sys/sys_top.sdc"]
          + "eval [string map {X _} setXfalseXpath] -to [get_registers {*ddr_frame_store:*|*}]\n"},
         syn_cdc, False, "sys/sys_top.sdc non-comment lines differ"),
        ("T3 sys_top nested source", {**syn, "sys/sys_top.sdc": syn["sys/sys_top.sdc"] + "source sys/x.sdc\n",
                                      "sys/x.sdc": fs_cut}, syn_cdc, False, "nested source inside an .sdc"),
        ("T4 core source inside an if condition",
         {**syn, "Plex.sdc": syn["Plex.sdc"] + 'if {[source Plex_x.sdc] eq ""} {\n}\n', "Plex_x.sdc": fs_cut},
         syn_cdc, False, "unpinned if-condition"),
        ("T5 core escaped verb inside an if condition",
         {**syn, "Plex.sdc": syn["Plex.sdc"] + "if {[" + esc + '] eq ""} {\n}\n'}, syn_cdc, False,
         "unpinned if-condition"),
        ("T6 core escaped verb as a row", {**syn, "Plex.sdc": syn["Plex.sdc"] + esc + "\n"}, syn_cdc, False,
         "unrecognised command in a core .sdc"),
        ("T7 qsf SDC_FILE with -entity before -name",
         {**syn, "Plex_480p_r1.qsf": qsf + "set_global_assignment -entity Plex -name SDC_FILE Plex_x.sdc\n",
          "Plex_x.sdc": fs_cut}, syn_cdc, False, "unparsed include line Plex_480p_r1.qsf:5"),
        ("T8 qsf SDC_ENTITY_FILE",
         {**syn, "Plex_480p_r1.qsf": qsf + "set_global_assignment -name SDC_ENTITY_FILE Plex_x.sdc -entity Plex\n",
          "Plex_x.sdc": fs_cut}, syn_cdc, False, "unparsed include line Plex_480p_r1.qsf:5"),
        ("T9 qsf source [file join ...] of a tcl adding SDC_FILE",
         {**syn, "Plex_480p_r1.qsf": qsf + "source [file join . x.tcl]\n",
          "x.tcl": "set_global_assignment -name SDC_FILE Plex_x.sdc\n", "Plex_x.sdc": fs_cut},
         syn_cdc, False, "unparsed include line Plex_480p_r1.qsf:5"),
        ("include target missing (QIP_FILE)",
         {**syn, "files.qip": syn["files.qip"] + "set_global_assignment -name QIP_FILE rtl/gone.qip\n"},
         syn_cdc, False, "include target missing: rtl/gone.qip"),
        # review 20:33: ';'-chained includes are followed through the shared splitter view.
        ("qsf_source_after_semicolon",
         {**syn, "Plex_480p_r1.qsf": qsf + "set x 1; source x.tcl\n",
          "x.tcl": "set_global_assignment -name SDC_FILE Plex_x.sdc\n", "Plex_x.sdc": fs_cut},
         syn_cdc, False, "Plex_x.sdc:1 unreviewed exception"),
        ("sdc_after_semicolon",
         {**syn, "Plex_480p_r1.qsf": qsf + "set x 1; set_global_assignment -name SDC_FILE Plex_x.sdc\n",
          "Plex_x.sdc": fs_cut}, syn_cdc, False, "Plex_x.sdc:1 unreviewed exception"),
        # PH-W (review 21:04/21:06): a source in brace data is invisible to the walker; red on the census.
        ("catch_hidden_source", {**syn, "Plex_480p_r1.qsf": qsf + "catch {source sys/no_such_file.tcl}\n"},
         syn_cdc, False, "Plex_480p_r1.qsf: unparsed parameter line (command head not in census) catch"),
        # PH-W2 (review 21:08): a static source of a file outside the dict (../../Makefile exists on disk
        # relative to the project dir) is red in the walker itself.
        ("source_not_in_dict", {**syn, "sys/sys.tcl": syn["sys/sys.tcl"] + "source ../../Makefile\n"},
         syn_cdc, False, "include target missing: ../../Makefile (from sys/sys.tcl:"),
    ]
    for name, p, tcl, want_green, needle in negs:
        if p == syn and tcl == syn_cdc:
            fail(f"SDC inventory NEG '{name}' is vacuous")
        probs, _, _ = plex_sdc_inventory(p, tcl)
        text = " | ".join(probs)
        if os.environ.get("RTLINV_VERBOSE"):
            print(f"  NEG {name}: {'GREEN' if not probs else 'RED'} :: {text[:170]}")
        if want_green != (not probs) or (needle and needle not in text):
            fail(f"SDC inventory NEG '{name}': expected {'GREEN' if want_green else 'RED'}"
                 f"{' with ' + repr(needle) if needle else ''}, got {probs}")
    # PH-W / PH-W2 split:
    #   - the walker alone misses the brace-hidden source, and the census alone catches it with exactly
    #     one message;
    #   - a static source outside the dict is red in BOTH.
    nmap = {n: p for n, p, _, _, _ in negs}
    for name, walker_red, census_needle in (
            ("catch_hidden_source", False, "(command head not in census) catch {source sys/no_such_file.tcl}"),
            ("source_not_in_dict", True, "(source not in cfg set) source ../../Makefile")):
        p = nmap[name]
        w = [x for q in sorted(k for k in p if k.endswith(".qsf") and "/" not in k) for x in plex_sdc_sources(p, q)[1]]
        c = param_override_problems(set(), {}, {r: t for r, t in p.items() if not r.endswith(_POH_RTL_EXT)})
        if bool(w) != walker_red or len(c) != 1 or census_needle not in c[0]:
            fail(f"SDC inventory PH-W split '{name}': walker {w}, census {c}")
    cls = ", ".join(f"{k} {v}" for k, v in sorted(counts["classes"].items())) or "none"
    print(f"PASS SDC exception inventory (sourced {counts['sourced']}; core rows {counts['core']} "
          f"reviewed [{cls}]; sys rows {counts['sys']} listed; not-sourced rows {counts['info']}; "
          f"{len(negs)} NEGs; census over the same cfg dict, PH-W/PH-W2 split checked)")

def present_core_plx_map(macros: set[str]) -> dict[str, tuple[int, str]]:
    """fstore mailbox addresses on one present_core branch: the override, else the
    ddr_frame_store module default (the branch passes none)."""
    defs = set(macros) | {"DDR_FRAME_STORE"}
    env = sv_param_env(sv_preprocess(strip_comments(read(DDR_FRAME_LAYOUT_SVH)), defs), {})
    pc = sv_preprocess(strip_comments(read(PRESENT_CORE)), defs)
    i = pc.find("ddr_frame_store #(")
    check(i >= 0, "present_core no longer instantiates ddr_frame_store #(...) fstore")
    sv_param_env(pc[:i], env)
    block = pc[i : pc.find(") fstore", i)]
    overrides = {
        m.group(1): m.group(2)
        for m in re.finditer(r"\.(\w+)\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)", block)
    }
    store = sv_preprocess(strip_comments(read(DDR_FRAME_STORE)), defs)
    header = store[: store.find(");")]
    inst = dict(env)
    for name, expr in overrides.items():
        try:
            inst[name] = sv_eval(expr, env)
        except (KeyError, SyntaxError, ValueError, TypeError, NameError):
            pass
    out: dict[str, tuple[int, str]] = {}
    for m in re.finditer(
        r"parameter\s+(?:int\s+|bit\s+)?(?:\[[^\]]+\]\s*)?(\w+)\s*=\s*([^,\n]+)", header
    ):
        name = m.group(1)
        if name in overrides:
            if name in inst:
                out[name] = (inst[name], "override")
            continue
        try:
            inst[name] = sv_eval(m.group(2).strip().rstrip(","), inst)
            out[name] = (inst[name], "module default")
        except (KeyError, SyntaxError, ValueError, TypeError, NameError):
            pass
    return out

def check_mailboxes() -> None:
    # Single source of truth (review 00:55, release 01:05): mailbox_abi_spec.hpp holds the
    # frame-store mailboxes as OFFSETS on a per-build page, plx_page.hpp the page per
    # present_core class, fpga_build_ids.hpp the class per build row. DIAG and the
    # 0x30140000 family stay absolute. Every present_core branch this tree implements must
    # put PLXS..PLXJ at its class page + the spec offset; no host header or tool may carry
    # an absolute frame-store page address.
    spec_text = strip_comments(read(MAILBOX_ABI_SPEC))
    page_text = strip_comments(read(PLX_PAGE_HPP))
    layout_text = strip_comments(read(DDR_FRAME_LAYOUT_HPP))
    store_rtl = strip_comments(read(DDR_FRAME_STORE))

    for name, _param, off_sym, off, rtl_magic, magic_sym, magic in PLX_PAGE_MAILBOXES:
        got = cpp_const(spec_text, off_sym)
        check(got == off,
              f"mailbox_abi_spec.hpp {off_sym}=0x{got:03X}, expected 0x{off:03X}: the {name} "
              "offset on the PLX page. The spec is the single source of truth; fix it there.")
        sm = cpp_const(spec_text, magic_sym)
        rm = sv_const(store_rtl, rtl_magic)
        check(sm == rm == magic,
              f"{name} magic: spec {magic_sym}=0x{sm:08X}, ddr_frame_store {rtl_magic}=0x{rm:08X}, "
              f"expected 0x{magic:08X}.")
    check(cpp_const(spec_text, "kPlxkMagic") == 0x504C584B, "PLXK doorbell magic must be 'PLXK'")
    check(cpp_const(spec_text, "kSdramDiagAddr") == SDRAM_DIAG_PHYS,
          "kSdramDiagAddr must stay the absolute 0x3007F120: present_core never overrides the "
          "DIAG parameter, so it does not move with the PLX page.")
    check(cpp_const(spec_text, "kPlxbAddr") == 0x30140000, "PLXB ring CTRL must stay 0x30140000")
    # main.cpp spells the PLXJ log address as a literal on purpose (test_n2_eof_e2e
    # N2_EOF_BASE_REF builds this main.cpp in pre-kPlx trees): it must stay kPlxjOff.
    main_cpp = strip_comments(read(MISTERPLEXD_MAIN_CPP))
    plxj_lits = main_cpp.count("+ 0x130u")
    check(cpp_const(spec_text, "kPlxjOff") == 0x130 and plxj_lits == 2,
          f"main.cpp has {plxj_lits} '+ 0x130u' PLXJ log literals (want 2) and spec kPlxjOff="
          f"0x{cpp_const(spec_text, 'kPlxjOff'):03X} (want 0x130): the literals must equal "
          "kPlxjOff. Change both together.")

    absolute = re.compile(
        r"\bkPlx[ksimfdjKSIMFDJ]Addr\b|\bk(?:DdrStatus|Input|Memtest|Underrun|BankRelease|Ddr)MailboxPhys\b"
        r"|\bkDdrDoorbellPhys\b|\bkSummaryPhys\b"
    )
    page_literal = re.compile(r"0x3007[Ff]1[0-3][0-9A-Fa-f]")
    for path in (MAILBOX_ABI_SPEC, PLX_PAGE_HPP, INPUT_MAILBOX_HPP, SDRAM_MAILBOX_HPP,
                 FPGA_SPI_HPP, FPGA_SPI_CPP, PRESENT_BANK_HPP, INPUT_MAILBOX_PROBE_CPP):
        text = strip_comments(read(path))
        m = absolute.search(text)
        check(m is None,
              f"{path} uses {m.group(0) if m else ''}: frame-store mailboxes are offsets on the "
              "build's PLX page (mailbox_abi_spec.hpp + plx_page.hpp). An absolute page address "
              "reads a dead page on every recognised build.")
        lits = [x for x in page_literal.findall(text) if x.lower() != "0x3007f120"]
        check(not lits,
              f"{path} hard-codes {lits}: take the PLX page from the build row (plx_page.hpp).")
    check("mailbox_abi::" in read(INPUT_MAILBOX_HPP),
          "input_mailbox.hpp must re-export from mailbox_abi_spec.hpp, not hardcode offsets")

    host_consts = layout_text + "\n" + page_text
    for cls, page in {"Present480pOrMetaDpb": 0x300FF000, "Present480i": 0x301FF000,
                      "Present720pL4": 0x3047F000, "NoPresentMacro": 0x3007F000}.items():
        m = re.search(rf"case\s+PlxPageClass::{cls}\s*:\s*return\s+(\w+)\s*;", page_text)
        check(m is not None, f"plx_page.hpp plxPageForClass has no case for {cls}")
        got = cpp_const(host_consts, m.group(1))
        check(got == page,
              f"plx_page.hpp class {cls} page 0x{got:08X}, expected 0x{page:08X} "
              "(review 00:55 page rule).")

    pc_raw = strip_comments(read(PRESENT_CORE))
    seen: list[str] = []
    for cls, macros, page in PLX_PAGE_CLASSES:
        if macros and not all(re.search(rf"`(?:ifdef|ifndef|elsif)\s+{m}\b", pc_raw) for m in macros):
            continue
        label = "+".join(macros) or "no PRESENT/META_DPB macro"
        addrs = present_core_plx_map(set(macros))
        for name, param, _sym, off, *_ in PLX_PAGE_MAILBOXES:
            check(param in addrs, f"present_core {label}: no value for fstore {param}")
            got, how = addrs[param]
            check(got == page + off,
                  f"present_core {label}: {name} {param}=0x{got:08X} ({how}), expected the "
                  f"{cls} page 0x{page:08X} + 0x{off:03X}. Fix the RTL branch or the "
                  "plx_page.hpp class table together, never with an absolute host constant.")
        if "DIAG_MAILBOX_PHYS" in addrs:
            check(addrs["DIAG_MAILBOX_PHYS"] == (SDRAM_DIAG_PHYS, "module default"),
                  f"present_core {label}: DIAG_MAILBOX_PHYS {addrs['DIAG_MAILBOX_PHYS']}; "
                  "kSdramDiagAddr is the absolute 0x3007F120, never overridden.")
        seen.append(f"{label} 0x{page:08X}")
    check(any(s.startswith("no PRESENT/META_DPB macro") for s in seen),
          "the no-macro present_core branch was not evaluated")
    ddram = strip_comments(read(DDRAM_FRAME_RD))
    if "MEMTEST_DIAG_MAILBOX_PHYS" in ddram:
        check(sv_const(ddram, "MEMTEST_DIAG_MAILBOX_PHYS") == SDRAM_DIAG_PHYS,
              "ddram_frame_rd MEMTEST_DIAG_MAILBOX_PHYS must equal kSdramDiagAddr 0x3007F120")

    # PLXD bit-field positions in the spec are what the RTL packs.
    check(cpp_const(spec_text, "kPlxdFreeBankMaskBit") == 0,
          "PLXD free_bank_mask starts at bit 0 of upper word (bits [33:32])")
    check(cpp_const(spec_text, "kPlxdFreeBankMaskWidth") == 2,
          "PLXD free_bank_mask is 2 bits wide")
    check(cpp_const(spec_text, "kPlxdDispBankBit") == 2,
          "PLXD disp_bank at bit 2 of upper word (bit [34])")
    check(cpp_const(spec_text, "kPlxdSwapPendingBit") == 3,
          "PLXD swap_pending at bit 3 of upper word (bit [35])")
    check(cpp_const(spec_text, "kPlxdFramesDoneBit") == 16,
          "PLXD frames_done starts at bit 16 of upper word (bits [63:48])")
    check(cpp_const(spec_text, "kPlxdFramesDoneWidth") == 16,
          "PLXD frames_done is 16 bits wide")

    # Offset and magic collisions (frame-store page, ring in-band magics).
    offsets: dict[int, str] = {}
    magics: dict[int, str] = {0x504C584B: "PLXK", 0x504C5842: "PLXB"}
    for name, _param, _sym, off, _rm, _ms, magic in PLX_PAGE_MAILBOXES:
        check(off not in offsets, f"OFFSET COLLISION: {name} +0x{off:03X} overlaps {offsets.get(off)}")
        offsets[off] = name
        check(magic not in magics, f"MAGIC COLLISION: {name} 0x{magic:08X} overlaps {magics.get(magic)}")
        magics[magic] = name
    check(0x120 not in offsets, "page offset +0x120 is the DIAG slot of the no-macro page")
    ring_magics = {
        "PLXR": 0x504C5852, "PLXE": 0x504C5845, "PLXN": 0x504C584E,
        "PLXT": 0x504C5854, "PLXU": 0x504C5855, "PLXV": 0x504C5856,
        "PLXW": 0x504C5857, "PLXY": 0x504C5859, "PLXZ": 0x504C585A,
        "PLXQ": 0x504C5851,
    }
    for rname, rmagic in ring_magics.items():
        check(rmagic not in magics,
              f"MAGIC COLLISION: ring {rname} magic 0x{rmagic:08X} overlaps mailbox "
              f"{magics.get(rmagic)}. Check mailbox_abi_spec.hpp.")
        magics[rmagic] = f"ring:{rname}"

    print("PASS DDR mailbox ABI (spec offsets, no absolute PLX page constant, class pages; "
          f"present_core branches: {', '.join(seen)})")


def check_mailbox_map_collisions() -> None:
    """The mailbox registry (docs/plx_mailbox_map.json): no offset, address or magic
    collision; page mailboxes are offsets whose ddr_frame_store module defaults sit on the
    no-macro page 0x3007F000; absolute entries match their RTL default."""
    import json

    map_path = ROOT / "docs" / "plx_mailbox_map.json"
    check(map_path.exists(), f"Mailbox registry missing: {map_path}")
    registry = json.loads(map_path.read_text())
    mailboxes = registry["mailboxes"]
    ring = registry.get("bitstream_ring", {})

    all_offsets: dict[int, str] = {}
    all_addrs: dict[int, str] = {}
    all_magics: dict[int, str] = {}
    for mb in mailboxes:
        name = mb["name"]
        check(("page_offset" in mb) != ("address" in mb) or mb.get("per_build"),
              f"Mailbox {name} needs exactly one of page_offset (PLX page) or address (absolute)")
        if "page_offset" in mb:
            off = int(mb["page_offset"], 16)
            if off in all_offsets:
                fail(f"Mailbox offset collision: {name} and {all_offsets[off]} both at +0x{off:03X}")
            all_offsets[off] = name
        if "address" in mb:
            addr = int(mb["address"], 16)
            if addr in all_addrs:
                fail(f"Mailbox address collision: {name} and {all_addrs[addr]} both at 0x{addr:08X}")
            all_addrs[addr] = name
        magic_s = mb.get("magic")
        if magic_s is not None:
            magic = int(magic_s, 16)
            if magic in all_magics:
                fail(f"Mailbox magic collision: {name} and {all_magics[magic]} both use 0x{magic:08X}")
            all_magics[magic] = name

    for rname, rval in ring.items():
        if rname.startswith("_"):
            continue
        addr = int(rval["address"], 16)
        if addr in all_addrs:
            fail(f"Ring address collision: {rname} and {all_addrs[addr]} both at 0x{addr:08X}")
        all_addrs[addr] = rname
        magic = int(rval["magic"], 16)
        if magic in all_magics:
            fail(f"Ring magic collision: {rname} and {all_magics[magic]} both use 0x{magic:08X}")
        all_magics[magic] = rname

    frame_store_rtl = strip_comments(read(DDR_FRAME_STORE))
    ddram_frame_rd_rtl = strip_comments(read(DDRAM_FRAME_RD))

    def rtl_param(param: str):
        for rtl_src in (frame_store_rtl, ddram_frame_rd_rtl):
            m = re.search(
                rf"(?:localparam|parameter)(?:\s+\w+)?(?:\s*\[[^\]]+\]\s*)?\s+{re.escape(param)}\s*=\s*([^,\n;)]+)",
                rtl_src,
            )
            if m:
                return parse_num(m.group(1))
        return None

    for mb in mailboxes:
        name = mb["name"]
        param = mb.get("rtl_addr_param")
        if param is not None:
            ra = rtl_param(param)
            check(ra is not None,
                  f"Mailbox map references RTL param {param} for {name}, but it was not found in "
                  "ddr_frame_store.sv or ddram_frame_rd.sv.")
            if "page_offset" in mb:
                want = 0x3007F000 + int(mb["page_offset"], 16)
                check(ra == want,
                      f"Mailbox map drift: {name} module default {param}=0x{ra:08X} != the no-macro "
                      f"page 0x3007F000 + {mb['page_offset']}. Every other present_core class "
                      "overrides it; see test check_mailboxes.")
            else:
                want = int(mb["address"], 16)
                check(ra == want,
                      f"Mailbox map drift: {name} RTL {param}=0x{ra:08X} != registry 0x{want:08X}.")
        magic_param = mb.get("rtl_magic_param")
        if magic_param is not None:
            rm = rtl_param(magic_param)
            check(rm is not None and rm == int(mb["magic"], 16),
                  f"Mailbox map drift: {name} RTL {magic_param} != registry {mb['magic']}.")

    print(f"PASS mailbox map collision guard ({len(all_offsets)} page offsets, "
          f"{len(all_addrs)} addresses, {len(all_magics)} magics, 0 collisions)")


def check_ddr_bitstream_ring() -> None:
    rtl = strip_comments(read(DDR_BITSTREAM_READER))
    spec_text = strip_comments(read(MAILBOX_ABI_SPEC))
    host = spec_text + "\n" + strip_comments(read(DDR_BITSTREAM_RING_HPP))
    fpga_spi = strip_comments(read(FPGA_SPI_CPP))
    cases = [
        ("DATA_PHYS", "kDataPhys", 0x30100000),
        ("CTRL_PHYS", "kCtrlPhys", 0x30140000),
        ("READ_PHYS", "kReadPhys", 0x30140008),
        ("ERR_PHYS", "kErrPhys", 0x30140010),
        ("STAT0_PHYS", "kStat0Phys", 0x30140018),
        ("STAT1_PHYS", "kStat1Phys", 0x30140020),
        ("STAT2_PHYS", "kStat2Phys", 0x30140028),
        ("STAT3_PHYS", "kStat3Phys", 0x30140030),
        ("STAT4_PHYS", "kStat4Phys", 0x30140038),
        ("STAT5_PHYS", "kStat5Phys", 0x30140040),
        ("STAT6_PHYS", "kStat6Phys", 0x30140048),
        ("RING_BYTES", "kRingBytes", 262144),
        ("MAGIC_CTRL", "kCtrlMagic", 0x504C5842),
        ("MAGIC_READ", "kReadMagic", 0x504C5852),
        ("MAGIC_ERR", "kErrMagic", 0x504C5845),
        ("MAGIC_REC", "kRecordMagic", 0x504C584E),
        ("MAGIC_ST0", "kStat0Magic", 0x504C5854),
        ("MAGIC_ST1", "kStat1Magic", 0x504C5855),
        ("MAGIC_ST2", "kStat2Magic", 0x504C5856),
        ("MAGIC_ST3", "kStat3Magic", 0x504C5857),
        ("MAGIC_ST4", "kStat4Magic", 0x504C5859),
        ("MAGIC_ST5", "kStat5Magic", 0x504C585A),
        ("MAGIC_ST6", "kStat6Magic", 0x504C5851),
    ]
    for rtl_name, host_name, expected in cases:
        rv = sv_const(rtl, rtl_name)
        hv = cpp_const(host, host_name)
        check(
            rv == hv == expected,
            f"DDR bitstream ring ABI mismatch: RTL {rtl_name}=0x{rv:X}, "
            f"host {host_name}=0x{hv:X}, expected 0x{expected:X}.",
        )
    err_bits = {
        "kErrTelemetrySeqShift": 32,
        "kErrUnderrunStickyBit": 45,
        "kErrOverrunStickyBit": 46,
        "kErrActiveBit": 47,
        "kErrUnderrunCountShift": 48,
        "kErrOverrunCountShift": 56,
    }
    for name, expected in err_bits.items():
        got = cpp_const(host, name)
        check(
            got == expected,
            f"DDR bitstream PLXE {name}={got}, expected {expected}. The RTL packs "
            "PLXE as seq[39:32], flags[47:45], underrun_count[55:48], "
            "overrun_count[63:56]; do not parse the old [42:40] flag positions.",
        )
    rtl_nt = norm(rtl)
    check(
        "DDRAM_DIN<={overrun_count[7:0],underrun_count[7:0],active,overrun_sticky,underrun_sticky,5'd0,telem_seq+8'd1,MAGIC_ERR}" in rtl_nt,
        "ddr_bitstream_reader no longer packs PLXE as {overrun_count[7:0], "
        "underrun_count[7:0], active, overrun_sticky, underrun_sticky, 5'd0, "
        "telem_seq, MAGIC_ERR}. Update ddr_bitstream_ring.hpp decode constants and "
        "red tests with the new producer layout.",
    )
    check(
        "decodeErrStatusWord(errRaw,status)" in norm(fpga_spi),
        "FpgaSpi::readBitstreamStatus must decode the PLXE word through the shared "
        "ddr_bitstream_ring.hpp helper. Open-coded shifts already drifted from the RTL "
        "producer once and made active/underrun/overrun flags lie.",
    )
    print("PASS DDR bitstream ring host/RTL ABI constants")


# T-STATUS-NEEDLE (review 19:03/19:04; H264 pre-check e37f76cf @9a4f9c93). Plex.sv packs the
# residual/recon status bytes from the sys_ copies of the decoder stickies (:2176-2179), and the
# mask (:2184-2189) uses the same names. Each sys_ copy must derive from its sticky in both builds:
# - `ifndef PLEX_CLK_DEC: exact assigns (:2102-2104);
# - `else: the last three entries of .dec_status and of .sys_status, in the same order
#   (:2145-2156).
STATUS_STICKY_BYTES = ((103, 96, "sys_st_res_word_sticky[7:0]"), (111, 104, "sys_st_res_word_sticky[15:8]"),
                       (119, 112, "sys_st_recon_sig_sticky"), (127, 120, "sys_st_recon_dbg_sticky"))
STATUS_STICKIES = ("st_res_word_sticky", "st_recon_sig_sticky", "st_recon_dbg_sticky")
STATUS_MASK = ("wire[127:0]status_telem_masked={sys_st_recon_dbg_sticky,sys_st_recon_sig_sticky,"
               "sys_st_res_word_sticky[15:8],sys_st_res_word_sticky[7:0],status_telem_r[95:0]};")
# F-8 (H264 d950e4a8, review 19:22): every write of status_telem_r, in order, by normalised text
# (Plex.sv :2163, :2165-2174, :2176-2179 @cb718e0a). The register name may appear only 17 times:
# the declaration, these 15 writes, and the one mask read.
STATUS_TELEM_WRITES = (
    "status_telem_r<=128'd0;", "status_telem_r[15:0]<=status[15:0];", "status_telem_r[23:16]<=telem_flags;",
    "status_telem_r[31:24]<=sys_last_nal_type;", "status_telem_r[47:32]<=sys_nalu_count;",
    "status_telem_r[55:48]<=sys_first_mb_type;", "status_telem_r[63:56]<=sys_slice_type;",
    "status_telem_r[71:64]<={sys_residual_ok,sys_residual_tc,sys_residual_t1};",
    "status_telem_r[79:72]<={ddr_busy,swap_pending,sys_slice_qp};", "status_telem_r[87:80]<=sys_sps_mb_w;",
    "status_telem_r[95:88]<=sys_sps_mb_h;", "status_telem_r[103:96]<=sys_st_res_word_sticky[7:0];",
    "status_telem_r[111:104]<=sys_st_res_word_sticky[15:8];", "status_telem_r[119:112]<=sys_st_recon_sig_sticky;",
    "status_telem_r[127:120]<=sys_st_recon_dbg_sticky;",
)
STATUS_TELEM_TOKENS = 17
_STMT_START = r"(?:^|;|\bbegin\b|\bend\b|\belse\b|\bassign\b|\)|@\s*\*)"


def _sv_writes(text: str, name: str) -> list[str]:
    """Normalised statements whose left side starts with `name` (procedural <= / =, or assign),
    in source order. A '<=' inside an if-condition is not at statement start and is not taken."""
    out = []
    for m in re.finditer(rf"{_STMT_START}\s*(\b{name}\b\s*(?:\[[^\]]*\]\s*)?(?:<=|(?<![<>=!])=(?!=))[^;]*;)",
                         text, re.M):
        out.append(norm(m.group(1)))
    return out


def _sv_concat_items(text: str, port: str) -> list[list[str]]:
    """Top-level items of every `.port({...})` concatenation in text, normalised."""
    out = []
    for m in re.finditer(rf"\.{port}\s*\(\s*\{{", text):
        depth, i, cur, items = 1, m.end(), [], []
        while i < len(text) and depth:
            c = text[i]
            if c in "{([":
                depth += 1
            elif c in "})]":
                depth -= 1
                if depth == 0:
                    break
            if c == "," and depth == 1:
                items.append(norm("".join(cur)))
                cur = []
            else:
                cur.append(c)
            i += 1
        items.append(norm("".join(cur)))
        out.append(items)
    return out


def status_sticky_problems(plex: str) -> list[str]:
    """Non-exiting; plex = comment-stripped Plex.sv."""
    out: list[str] = []
    for hi, lo, rhs in STATUS_STICKY_BYTES:
        asg = re.findall(rf"status_telem_r\s*\[\s*{hi}\s*:\s*{lo}\s*\]\s*<=\s*([^;]*);", plex)
        if len(asg) != 1 or norm(asg[0]) != norm(rhs):
            out.append(f"status_telem_r[{hi}:{lo}] must be assigned exactly once, from {rhs} "
                       f"(found {[norm(a) for a in asg]}); never the live residual/recon or stream bytes")
    masks = re.findall(r"wire\s*\[\s*127\s*:\s*0\s*\]\s*status_telem_masked\s*=\s*\{[^;]*\}\s*;", plex)
    if len(masks) != 1 or norm(masks[0]) != STATUS_MASK:
        out.append(f"status_telem_masked must be exactly '{STATUS_MASK}' (found {[norm(m) for m in masks]})")
    # The `ifndef PLEX_CLK_DEC block whose first branch drives the sys_ stickies.
    toks = list(re.finditer(r"`(ifdef|ifndef|elsif|else|endif)\b[ \t]*(\w*)", plex))
    blocks, stack = [], []
    for m in toks:
        kind = m.group(1)
        if kind in ("ifdef", "ifndef"):
            stack.append({"kind": kind, "macro": m.group(2), "start": m.end(), "else": None})
        elif kind in ("else", "elsif") and stack:
            if stack[-1]["else"] is None:
                stack[-1]["else"] = (m.start(), m.end())
        elif kind == "endif" and stack:
            b = stack.pop()
            if b["kind"] == "ifndef" and b["macro"] == "PLEX_CLK_DEC" and b["else"]:
                then_t, else_t = plex[b["start"]:b["else"][0]], plex[b["else"][1]:m.start()]
                if "sys_st_res_word_sticky" in then_t:
                    blocks.append((then_t, else_t))
    if len(blocks) != 1:
        out.append(f"expected exactly one `ifndef PLEX_CLK_DEC block driving the sys_ stickies, found {len(blocks)}")
        return out
    then_t, else_t = blocks[0]
    whole = norm(plex)
    for s in STATUS_STICKIES:
        a = f"assignsys_{s}={s};"
        if norm(then_t).count(a) != 1 or whole.count(f"assignsys_{s}=") != 1:
            out.append(f"`ifndef PLEX_CLK_DEC must hold exactly one 'assign sys_{s} = {s};' "
                       "(and no other assign of it)")
    # F-9: exactly one driver per sys_ twin in each build: the assign (ifndef) or the .sys_status
    # port (else). Common code drives none. Drivers counted: assign, procedural <= / = (incl.
    # always @*), and any port connection naming the twin.
    outside = plex.replace(then_t, "", 1).replace(else_t, "", 1)
    for s in STATUS_STICKIES:
        tw = f"sys_{s}"
        def drv(t: str) -> int:
            return len(_sv_writes(t, tw))
        def ports(t: str) -> int:
            return sum(1 for m in re.finditer(r"\.\w+\s*\((\{[^;]*?\}|[^;()]*)\)", t)
                       if re.search(rf"\b{tw}\b", m.group(1)))
        got = (drv(then_t), ports(then_t), drv(else_t), ports(else_t), drv(outside), ports(outside))
        if got != (1, 0, 0, 1, 0, 0):
            out.append(f"{tw} must have exactly one driver per build (ifndef: 1 assign; else: the .sys_status "
                       f"port; common code: none). Found (then drv, then ports, else drv, else ports, "
                       f"common drv, common ports) = {got}")
    # F-8: all status_telem_r writes, in order, frozen; no other reference to the register.
    writes = _sv_writes(plex, "status_telem_r")
    if tuple(writes) != STATUS_TELEM_WRITES:
        extra = [w for w in writes if w not in STATUS_TELEM_WRITES]
        div = next((i for i, (a, b) in enumerate(zip(writes, STATUS_TELEM_WRITES)) if a != b),
                   min(len(writes), len(STATUS_TELEM_WRITES)))
        got = writes[div] if div < len(writes) else "<none>"
        want = STATUS_TELEM_WRITES[div] if div < len(STATUS_TELEM_WRITES) else "<none>"
        out.append(f"status_telem_r writes differ from the reviewed {len(STATUS_TELEM_WRITES)} (in order): "
                   f"{len(writes)} found, unreviewed {extra[:3]}, first divergent write #{div}: "
                   f"found {got!r}, reviewed {want!r}")
    toks = len(re.findall(r"\bstatus_telem_r\b", plex))
    if toks != STATUS_TELEM_TOKENS:
        out.append(f"status_telem_r is referenced {toks} times, reviewed {STATUS_TELEM_TOKENS} (decl + 15 writes + "
                   "the mask read): a write the statement scan cannot see, or a new reader")
    dec = _sv_concat_items(else_t, "dec_status")
    sys_ = _sv_concat_items(else_t, "sys_status")
    if len(dec) != 1 or dec[0][-3:] != list(STATUS_STICKIES):
        out.append(f".dec_status (`else, PLEX_CLK_DEC) must end with {list(STATUS_STICKIES)} "
                   f"(found {[d[-3:] for d in dec]})")
    if len(sys_) != 1 or sys_[0][-3:] != [f"sys_{s}" for s in STATUS_STICKIES]:
        out.append(f".sys_status (`else, PLEX_CLK_DEC) must end with the sys_ twins in the same order "
                   f"(found {[s[-3:] for s in sys_]})")
    return out


def check_status_telemetry() -> None:
    plex = strip_comments(read(PLEX_SV))
    fpga_spi = strip_comments(read(FPGA_SPI_CPP))
    status_h = strip_comments(read(STATUS_TELEMETRY_HPP))
    push_frame = strip_comments(read(PUSH_FRAME_CPP))
    set_status = strip_comments(read(SET_STATUS_CPP))
    nt = norm(plex)
    expected = {
        "kResidualDcBitLo": 96,
        "kResidualDcBitHi": 103,
        "kResidualCsumBitLo": 104,
        "kResidualCsumBitHi": 111,
        "kReconSigBitLo": 112,
        "kReconSigBitHi": 119,
        "kReconDbgBitLo": 120,
        "kReconDbgBitHi": 127,
        "kResidualDcByte": 12,
        "kResidualCsumByte": 13,
        "kReconSigByte": 14,
        "kReconDbgByte": 15,
        "kReconSigMb0Block0": 0x3B,
        "kReconDbgMb0Block0": 0xF9,
    }
    for name, value in expected.items():
        got = cpp_const(status_h, name)
        check(
            got == value,
            f"status_telemetry.hpp {name}={got}, expected {value}. This is a status telemetry "
            "ABI position; moving it requires updating Plex.sv, parseCoreStatus, "
            "tests/parse_res_csum_status.py, and a hardware re-gate note.",
        )

    checks = [
        (
            r"status_telem_r\s*\[\s*103\s*:\s*96\s*\]\s*<=\s*st_res_word_sticky\s*\[\s*7\s*:\s*0\s*\]",
            "Plex.sv no longer packs residual_dc sticky into status[103:96]/raw[12]. "
            "res_dc=-24 is the adjacent control proving the residual latch fired.",
        ),
        (
            r"status_telem_r\s*\[\s*111\s*:\s*104\s*\]\s*<=\s*st_res_word_sticky\s*\[\s*15\s*:\s*8\s*\]",
            "Plex.sv no longer packs residual_csum sticky into status[111:104]/raw[13]. "
            "If raw[13] is stream_bytes[7:0], repeated pushes walk by file_size%256 and "
            "P3-3l1 remains blocked. Restore csum at raw[13] or re-gate with a new ABI.",
        ),
        (
            r"status_telem_r\s*\[\s*119\s*:\s*112\s*\]\s*<=\s*st_recon_sig_sticky",
            "Plex.sv no longer packs reconstructed-pixel signature into status[119:112]/raw[14]. "
            "P3-3l2 hardware must publish an IDCT-sensitive signature; restore recon_sig at "
            "raw[14] or update the documented ABI and hardware gate.",
        ),
        (
            r"status_telem_r\s*\[\s*127\s*:\s*120\s*\]\s*<=\s*st_recon_dbg_sticky",
            "Plex.sv no longer keeps P3 recon RCA flags in status[127:120]/raw[15]. "
            "This byte distinguishes coefficient/dequant/IDCT/recon stages on silicon; "
            "restore it or update the documented RCA gate.",
        ),
    ]
    for pat, msg in checks:
        check(re.search(pat, plex) is not None, msg)
    check(
        "status_telem_r[127:104]<=stream_bytes_in" not in nt
        and "status_telem[127:104]=stream_bytes_in" not in nt,
        "Plex.sv appears to use the old alias layout status[127:104]=stream_bytes_in. "
        "That leaves residual_dc at raw[12] but makes raw[13] the byte-counter low byte, "
        "exactly the +file_size%256 failure signature.",
    )
    check(
        "status_telem_masked={st_recon_dbg_sticky,st_recon_sig_sticky,st_res_word_sticky[15:8],st_res_word_sticky[7:0],status_telem_r[95:0]}" in nt,
        "Plex.sv residual/recon mask no longer structurally forces {recon_dbg, recon_sig, csum, dc}. "
        "This mask prevents residual_csum/recon_sig from aliasing live stream bytes; restore it or "
        "add an equivalent guarded pack with a simulation proof.",
    )
    check(
        "raw[kResidualCsumByte]" in fpga_spi
        and "raw[kReconSigByte]" in fpga_spi
        and "raw[kReconDbgByte]" in fpga_spi,
        "parseCoreStatus is not using the shared status_telemetry byte constants. The host "
        "must decode raw[13] as residual_csum, raw[14] as recon_sig and raw[15] as recon debug; restore the "
        "shared constants to avoid silent host/RTL ABI drift.",
    )
    check(
        ".recon_sig(recon_sig)" in plex and ".recon_valid(recon_valid)" in plex,
        "Plex.sv/stream_path no longer wire decode_stub recon telemetry to status. "
        "P3-3l2 requires product RTL to publish an IDCT-sensitive signature.",
    )
    status_tools = push_frame + "\n" + set_status
    check(
        "bytes_in=%u" not in status_tools and "stream_nalus=%u" in status_tools,
        "status tools must not label the post-P3 NAL-count liveness alias as bytes_in. "
        "Expose stream_nalus and bytes_in_unavailable=1 so freshness gates cannot mistake "
        "nalu=4 for four delivered bytes again.",
    )
    check(
        "frame_debug=0x%02x" in status_tools and "frame_seq=%u" in status_tools,
        "status tools must surface the DDR frame-store debug byte and doorbell token. "
        "frame_debug=0xE1 distinguishes a rejected non-YUV doorbell/harness bug from "
        "a broken delivery path.",
    )
    print("PASS residual/recon status telemetry ABI (raw[12]=dc raw[13]=csum raw[14]=recon_sig)")


def check_ddr_frame_layout_contract() -> None:
    # T-RTL-INVARIANTS-CANON C1: canon has no r1 qsf; it then checks parity under canon's own Plex.qsf.
    build_qsf = PLEX_BUILD_QSF
    if not build_qsf.is_file():
        fallback = PLEX_PRJ_DIR / "Plex.qsf"
        print(f"NOTE ddr frame layout contract: {build_qsf.name} absent from tree; build qsf = {fallback.name}")
        build_qsf = fallback
    host = strip_comments(read(DDR_FRAME_LAYOUT_HPP))
    raw_svh = strip_comments(read(DDR_FRAME_LAYOUT_SVH))
    # F-5 parity: the build's macros come from the qsf AND every file it sources, read through the shared
    # splitter, fail closed (plex_build_macros).
    try:
        build_rel = build_qsf.resolve().relative_to(PLEX_PRJ_DIR.resolve()).as_posix()
    except ValueError:
        fail(f"ddr frame layout: PLEX_BUILD_QSF {build_qsf} is not under PLEX_PRJ_DIR {PLEX_PRJ_DIR}")
        return
    build_macros, macro_problems = plex_build_macros(_plex_prj_files(PLEX_PRJ_DIR), build_rel)
    if macro_problems:
        fail(f"ddr frame layout: build macros of {build_rel} cannot be read: " + " | ".join(macro_problems))
    try:
        rtl = sv_select_defines(raw_svh, build_macros)
    except ValueError as e:
        fail(f"ddr_frame_layout_params.svh: {e}")
    pairs = [
        ("kPlex480pCodedWidth", "DDR_FRAME_CODED_WIDTH"),
        ("kPlex480pCodedHeight", "DDR_FRAME_CODED_HEIGHT"),
        ("kPlex480pDisplayWidth", "DDR_FRAME_DISPLAY_WIDTH"),
        ("kPlex480pDisplayHeight", "DDR_FRAME_DISPLAY_HEIGHT"),
        ("kPlex480pPresentedWidth", "DDR_FRAME_PRESENTED_WIDTH"),
        ("kPlex480pPresentedHeight", "DDR_FRAME_PRESENTED_HEIGHT"),
        ("kPlex480pCropLeft", "DDR_FRAME_CROP_LEFT"),
        ("kPlex480pCropRight", "DDR_FRAME_CROP_RIGHT"),
        ("kPlex480pCropTop", "DDR_FRAME_CROP_TOP"),
        ("kPlex480pCropBottom", "DDR_FRAME_CROP_BOTTOM"),
        ("kPlex480pPillarboxLeft", "DDR_FRAME_PILLARBOX_LEFT"),
        ("kPlex480pPillarboxRight", "DDR_FRAME_PILLARBOX_RIGHT"),
        ("kPlex480pRgb565LineQwords", "DDR_FRAME_RGB565_LINE_QWORDS"),
        ("kPlex480pYuvLumaLineQwords", "DDR_FRAME_YUV_LUMA_LINE_QWORDS"),
        ("kPlex480pYuvChromaLineQwords", "DDR_FRAME_YUV_CHROMA_LINE_QWORDS"),
        ("kPlex480pRgb565Bytes", "DDR_FRAME_RGB565_BYTES"),
        ("kPlex480pYuv420pBytes", "DDR_FRAME_YUV420P_BYTES"),
        ("kPlex480pYPlaneOffset", "DDR_FRAME_Y_PLANE_OFFSET"),
        ("kPlex480pUPlaneOffset", "DDR_FRAME_U_PLANE_OFFSET"),
        ("kPlex480pVPlaneOffset", "DDR_FRAME_V_PLANE_OFFSET"),
        ("kPlex480pYStrideBytes", "DDR_FRAME_Y_STRIDE_BYTES"),
        ("kPlex480pChromaStrideBytes", "DDR_FRAME_CHROMA_STRIDE_BYTES"),
        ("kPlex480pRgb565BankStride", "DDR_FRAME_RGB565_BANK_STRIDE"),
        ("kPlex480pYuv420pBankStride", "DDR_FRAME_YUV420P_BANK_STRIDE"),
        ("kPlex480pRgb565DoorbellPhys", "DDR_FRAME_RGB565_DOORBELL_PHYS"),
        ("kPlex480pYuv420pDoorbellPhys", "DDR_FRAME_YUV420P_DOORBELL_PHYS"),
        ("kYuv420BlackY", "DDR_FRAME_YUV_BLACK_Y"),
        ("kYuv420BlackU", "DDR_FRAME_YUV_BLACK_U"),
        ("kYuv420BlackV", "DDR_FRAME_YUV_BLACK_V"),
        # 720p tier (present path land; opt-in)
        ("kPlex720pCodedWidth", "DDR_FRAME_720P_CODED_WIDTH"),
        ("kPlex720pCodedHeight", "DDR_FRAME_720P_CODED_HEIGHT"),
        ("kPlex720pPresentedWidth", "DDR_FRAME_720P_PRESENTED_WIDTH"),
        ("kPlex720pPresentedHeight", "DDR_FRAME_720P_PRESENTED_HEIGHT"),
        ("kPlex720pYuv420pBankStride", "DDR_FRAME_720P_YUV420P_BANK_STRIDE"),
        ("kPlex720pPhysBase", "DDR_FRAME_720P_PHYS_BASE"),
        ("kPlex720pYuv420pDoorbellPhys", "DDR_FRAME_720P_YUV420P_DOORBELL_PHYS"),
        ("kPlex720p24BeamHTotal", "DDR_FRAME_720P24_BEAM_H_TOTAL"),
        ("kPlex720p24BeamVTotal", "DDR_FRAME_720P24_BEAM_V_TOTAL"),
        ("kPlex720p24BeamHDe", "DDR_FRAME_720P24_BEAM_H_DE"),
        ("kPlex720p24BeamVActive", "DDR_FRAME_720P24_BEAM_V_ACTIVE"),
        ("kPlex720p24ClkPixHz", "DDR_FRAME_720P24_CLK_PIX_HZ"),
        ("kPlex960PresentedWidth", "DDR_FRAME_960_PRESENTED_WIDTH"),
        ("kPlex960PresentedHeight", "DDR_FRAME_960_PRESENTED_HEIGHT"),
    ]
    for host_name, rtl_name in pairs:
        hv = cpp_const(host, host_name)
        rv = sv_const(rtl, rtl_name)
        check(
            hv == rv,
            f"DDR frame layout mismatch: {host_name}={hv} but {rtl_name}={rv}. "
            "ARM writer and RTL reader must agree on coded/display/presented geometry, "
            "burst qwords, bank stride, and doorbell addresses.",
        )

    # T-RTLINV-STALE (1) teeth: the evaluation follows the build's macros, and value drift on either
    # side is seen. Getters here are non-exiting (a missing name counts as a mismatch).
    def mismatches(host_t: str, rtl_t: str) -> list[str]:
        out = []
        for hn, rn in pairs:
            hm = re.search(rf"constexpr\s+(?:\w+\s+)+{re.escape(hn)}\s*=\s*([^;]+);", host_t)
            rm = re.search(rf"(?:localparam|parameter)(?:\s+\w+)?(?:\s*\[[^\]]+\])?\s+{re.escape(rn)}\s*=\s*([^,\n;)]+)", rtl_t)
            hv = parse_num(hm.group(1)) if hm else None
            rv = parse_num(rm.group(1)) if rm else None
            if hv is None or rv is None or hv != rv:
                out.append(f"{hn}={hv} vs {rn}={rv}")
        return out

    live = "localparam int DDR_FRAME_720P24_BEAM_H_TOTAL = 1312;"
    teeth = [
        ("live-branch H_TOTAL drift", host,
         sv_select_defines(raw_svh.replace(live, live.replace("1312", "1313")), build_macros), "BEAM_H_TOTAL"),
        ("host CLK_PIX drift", host.replace("kPlex720p24ClkPixHz = 24000000", "kPlex720p24ClkPixHz = 24000001"),
         rtl, "CLK_PIX_HZ"),
    ]
    if "PLEX_PRESENT_720P_CEA" in raw_svh:
        teeth.insert(0, ("PLEX_PRESENT_720P_CEA defined for the build", host,
                         sv_select_defines(raw_svh, build_macros | {"PLEX_PRESENT_720P_CEA"}), "BEAM_H_TOTAL"))
    else:
        print("SKIP ddr frame layout CEA tooth: no PLEX_PRESENT_720P_CEA branch in "
              f"{DDR_FRAME_LAYOUT_SVH.name} (canon has no 720p CEA RTL)")
        SKIPS.append("ddr frame layout CEA tooth")
    for name, h_t, r_t, needle in teeth:
        if (h_t, r_t) == (host, rtl):
            fail(f"DDR frame layout tooth '{name}' is vacuous (mutation changed nothing)")
        if not any(needle in m for m in mismatches(h_t, r_t)):
            fail(f"DDR frame layout tooth '{name}' did not go red on {needle}: {mismatches(h_t, r_t)}")
    check(
        cpp_const(host, "kPlex480pPillarboxLeft")
        + cpp_const(host, "kPlex480pDisplayWidth")
        + cpp_const(host, "kPlex480pPillarboxRight")
        == cpp_const(host, "kPlex480pPresentedWidth"),
        "480p pillarbox math no longer lands display width exactly in presented width",
    )
    print("PASS DDR frame layout ARM/RTL contract")


def chroma_line_stride_problems(text_norm: str) -> list[str]:
    """T-RTLINV-STALE (2), on normalised ddr_frame_store text. Since f176804c, fill_cy_qword is a
    shift-add generate (:1538-1580), not one multiply. In the default (non-fault) branch every arm
    'if (C_LINE_QWORDS == N)' must sum to exactly N times fill_cy_ext, and the generic arm must
    multiply by C_LINE_QWORDS_W. The `ifdef DDR_FRAME_STORE_FAULT_CHROMA_LUMA_STRIDE branch is
    test-only (it strides by Y on purpose) and is skipped."""
    out: list[str] = []
    if "wire[28:0]fill_cy_ext={{(30-Y_W){1'b0}},fill_cy};" not in text_norm:
        out.append("fill_cy_ext must zero-extend fill_cy as {{(30-Y_W){1'b0}}, fill_cy}")
    m = re.search(r"`ifdefDDR_FRAME_STORE_FAULT_CHROMA_LUMA_STRIDE(.*?)`else(.*?)`endif", text_norm, re.S)
    body = m.group(2) if m else text_norm
    arms = re.findall(r"if\(C_LINE_QWORDS==(\d+)\)begin:(\w+)assignfill_cy_qword=([^;]*);end", body)
    gen = re.findall(r"elsebegin:(\w+)assignfill_cy_qword=([^;]*);end", body)
    if not arms or len(gen) != 1:
        out.append(f"fill_cy_qword generate not recognised ({len(arms)} sized arms, {len(gen)} generic)")
    for n, label, expr in arms:
        total = 0
        for term in expr.split("+"):
            t = term.strip("()")
            sm = re.fullmatch(r"fill_cy_ext<<(\d+)", t)
            if sm:
                total += 1 << int(sm.group(1))
            elif t == "fill_cy_ext":
                total += 1
            else:
                total = None
                break
        if total != int(n):
            out.append(f"{label}: fill_cy_qword = {expr} is not {n} x fill_cy_ext (chroma line stride)")
    if gen and gen[0][1] != "fill_cy_ext*C_LINE_QWORDS_W":
        out.append(f"{gen[0][0]}: generic fill_cy_qword must be fill_cy_ext*C_LINE_QWORDS_W, got {gen[0][1]}")
    if body.count("assignfill_cy_qword=") != len(arms) + len(gen):
        out.append("fill_cy_qword has a driver outside its generate arms")
    return out


def check_ddr_frame_store_yuv_read_contract() -> None:
    rtl = select_default_sv_fault_branches(strip_comments(read(DDR_FRAME_STORE)))
    layout = strip_comments(read(DDR_FRAME_LAYOUT_SVH))
    nt = norm(rtl)

    coded_w = sv_const(layout, "DDR_FRAME_CODED_WIDTH")
    coded_h = sv_const(layout, "DDR_FRAME_CODED_HEIGHT")
    expected_y_offset = sv_const(layout, "DDR_FRAME_Y_PLANE_OFFSET")
    expected_u_offset = sv_const(layout, "DDR_FRAME_U_PLANE_OFFSET")
    expected_v_offset = sv_const(layout, "DDR_FRAME_V_PLANE_OFFSET")
    expected_y_stride = sv_const(layout, "DDR_FRAME_Y_STRIDE_BYTES")
    expected_c_stride = sv_const(layout, "DDR_FRAME_CHROMA_STRIDE_BYTES")

    def missing_requirements(text_norm: str) -> list[str]:
        required = [
            (
                "Y_LINE_QWORDS=CODED_W/8",
                "luma line fetch stride must be CODED_W/8 qwords",
            ),
            (
                "C_LINE_QWORDS=CODED_W/16",
                "chroma line fetch stride must be CODED_W/16 qwords (half-width bytes)",
            ),
            (
                "Y_PLANE_QWORDS=29'((CODED_W*CODED_H)/8)",
                "Y plane size in qwords must be coded_width*coded_height/8",
            ),
            (
                "C_PLANE_QWORDS=29'((CODED_W*CODED_H)/32)",
                "U/V plane size in qwords must be coded_width*coded_height/32",
            ),
            ("U_PLANE_BASE=Y_PLANE_QWORDS", "U read base must immediately follow Y"),
            (
                "V_PLANE_BASE=Y_PLANE_QWORDS+C_PLANE_QWORDS",
                "V read base must immediately follow U",
            ),
            (
                "u_addr=fill_bank_base+U_PLANE_BASE+fill_cy_qword+fill_qword_c",
                "U fetch address must use U_PLANE_BASE",
            ),
            (
                "v_addr=fill_bank_base+V_PLANE_BASE+fill_cy_qword+fill_qword_c",
                "V fetch address must use V_PLANE_BASE",
            ),
            (
                "chroma_addr=fill_plane_v?v_addr:u_addr",
                "chroma read address must select U while fill_plane_v=0, then V",
            ),
            (
                "line_addr=fill_is_chroma?chroma_addr:y_addr",
                "chroma fill path must issue the selected chroma address",
            ),
            ("rd_cy=src_y[CODED_Y_W-1:1]", "chroma line lookup must halve the source Y"),
            (
                "c_line_v2[video_slot]==(Y_W-1)'(rd_cy)",
                "chroma line-buffer hit must compare against the halved source Y",
            ),
            ("c_sel_r<=src_x[3:1]", "chroma byte select must halve the source X"),
            # T-RTLINV-STALE (2b): since f176804c the converter input is two hops: the byte pick from
            # the U/V RAM word, then a chroma-miss mux to neutral 128 (product change disclosed in
            # the commit; T-CHROMA-MISS-NEUTRAL). Both hops are pinned for U and V.
            ("wire[7:0]u_raw=pick_byte(selected_u_q,c_sel_r);", "YUV converter U input must come from U RAM (U byte pick)"),
            ("wire[7:0]v_raw=pick_byte(selected_v_q,c_sel_r);", "YUV converter V input must come from V RAM (V byte pick)"),
            ("wire[7:0]u_pix=c_hit_r?u_raw:8'd128;", "YUV converter U input must be the U RAM pick, neutral 128 on a chroma miss"),
            ("wire[7:0]v_pix=c_hit_r?v_raw:8'd128;", "YUV converter V input must be the V RAM pick, neutral 128 on a chroma miss"),
            ("r_calc_w=(y_ext<<<8)+(21'sd359*v_s)", "red channel must derive from V"),
            ("b_calc_w=(y_ext<<<8)+(21'sd454*u_s)", "blue channel must derive from U"),
        ]
        # The chroma DDR line stride (fill_cy_qword) is checked structurally (T-RTLINV-STALE (2)).
        stride = chroma_line_stride_problems(text_norm)
        return [msg for needle, msg in required if needle not in text_norm] + [
            f"chroma DDR line address must stride by the chroma line width: {p}" for p in stride]

    def enforce(text_norm: str, label: str) -> None:
        missing = missing_requirements(text_norm)
        if missing:
            fail(f"{label}: {missing[0]}")

    enforce(nt, "ddr_frame_store.sv")

    # Deliberate-fault proof: swapping the read bases in memory must make this
    # gate go red. This catches the exact symptom class where the ARM writes
    # byte-exact I420 but the RTL reads U from the V plane and V from the U plane.
    swapped = (
        nt.replace(
            "u_addr=fill_bank_base+U_PLANE_BASE+fill_cy_qword+fill_qword_c",
            "u_addr=fill_bank_base+V_PLANE_BASE+fill_cy_qword+fill_qword_c",
        )
        .replace(
            "v_addr=fill_bank_base+V_PLANE_BASE+fill_cy_qword+fill_qword_c",
            "v_addr=fill_bank_base+U_PLANE_BASE+fill_cy_qword+fill_qword_c",
        )
    )
    if not missing_requirements(swapped):
        fail("deliberately swapped U/V read bases did not make the DDR frame-store gate red")
    swapped_coeffs = (
        nt.replace(
            "r_calc_w=(y_ext<<<8)+(21'sd359*v_s)",
            "r_calc_w=(y_ext<<<8)+(21'sd359*u_s)",
        ).replace(
            "b_calc_w=(y_ext<<<8)+(21'sd454*u_s)",
            "b_calc_w=(y_ext<<<8)+(21'sd454*v_s)",
        )
    )
    if not missing_requirements(swapped_coeffs):
        fail("deliberately swapped YUV→RGB R/B chroma inputs did not make the gate red")
    bad_geometry = (
        nt.replace("rd_cy=src_y[CODED_Y_W-1:1]", "rd_cy=src_y[CODED_Y_W-2:0]")
        .replace("c_sel_r<=src_x[3:1]", "c_sel_r<=src_x[2:0]")
    )
    if not missing_requirements(bad_geometry):
        fail("deliberately broken chroma half-resolution geometry did not make the gate red")
    # T-RTLINV-STALE (2) teeth: the old needle mutant was a no-op after f176804c. Break the stride
    # in the live generate arms instead.
    stride_teeth = [
        ("g_cyq_640 strides by the luma width (80)",
         nt.replace("assignfill_cy_qword=(fill_cy_ext<<5)+(fill_cy_ext<<3);", "assignfill_cy_qword=(fill_cy_ext<<6)+(fill_cy_ext<<4);", 1)),
        ("generic arm multiplies by Y_LINE_QWORDS_W",
         nt.replace("assignfill_cy_qword=fill_cy_ext*C_LINE_QWORDS_W;", "assignfill_cy_qword=fill_cy_ext*Y_LINE_QWORDS_W;", 1)),
        ("g_cyq_foar624 drops its +fill_cy_ext term (38, not 39)",
         nt.replace("+(fill_cy_ext<<1)+fill_cy_ext;", "+(fill_cy_ext<<1);", 1)),
    ]
    for name, mutant in stride_teeth:
        if mutant == nt:
            fail(f"chroma stride tooth '{name}' is vacuous (mutation changed nothing)")
        if not chroma_line_stride_problems(mutant):
            fail(f"chroma stride tooth '{name}' did not make the gate red")
    # T-RTLINV-STALE (2b) teeth (H264 c26f1946): each must add a NEW miss.
    base_missing = set(missing_requirements(nt))
    for name, old, new in (
        ("U picked from selected_v_q", "u_raw=pick_byte(selected_u_q,c_sel_r);", "u_raw=pick_byte(selected_v_q,c_sel_r);"),
        ("V picked from selected_u_q", "v_raw=pick_byte(selected_v_q,c_sel_r);", "v_raw=pick_byte(selected_u_q,c_sel_r);"),
        ("U miss arm not 8'd128", "u_pix=c_hit_r?u_raw:8'd128;", "u_pix=c_hit_r?u_raw:8'd0;"),
        ("V miss arm not 8'd128", "v_pix=c_hit_r?v_raw:8'd128;", "v_pix=c_hit_r?v_raw:8'd0;"),
        ("U byte select c_sel_r -> y_sel_r", "u_raw=pick_byte(selected_u_q,c_sel_r);", "u_raw=pick_byte(selected_u_q,y_sel_r);"),
        ("V byte select c_sel_r -> y_sel_r", "v_raw=pick_byte(selected_v_q,c_sel_r);", "v_raw=pick_byte(selected_v_q,y_sel_r);"),
    ):
        if old not in nt:
            fail(f"U/V converter tooth '{name}' source text is gone from ddr_frame_store.sv (vacuous tooth)")
        if not set(missing_requirements(nt.replace(old, new, 1))) - base_missing:
            fail(f"U/V converter tooth '{name}' did not make the gate red")

    y_stride = coded_w
    c_stride = coded_w // 2
    y_offset = 0
    u_offset = coded_w * coded_h
    v_offset = u_offset + (coded_w // 2) * (coded_h // 2)
    check(
        (y_offset, u_offset, v_offset, y_stride, c_stride)
        == (
            expected_y_offset,
            expected_u_offset,
            expected_v_offset,
            expected_y_stride,
            expected_c_stride,
        ),
        "computed DDR frame-store read geometry does not match ratified 480p I420 layout: "
        f"computed Y/U/V offsets {y_offset}/{u_offset}/{v_offset}, strides "
        f"{y_stride}/{c_stride}; expected {expected_y_offset}/{expected_u_offset}/"
        f"{expected_v_offset}, strides {expected_y_stride}/{expected_c_stride}",
    )
    print(
        "PASS DDR frame-store YUV read contract "
        f"(Y={y_offset} U={u_offset} V={v_offset} strides Y={y_stride} C={c_stride}; "
        "deliberate U/V, R/B coefficient, chroma-geometry, chroma-stride and U/V converter "
        "pick/miss-arm faults go red)"
    )


# T-CHROMA-MISS-NEUTRAL N2 (H264 c39c160e, review 19:46/19:48): the BLACK_ON_CHROMA_MISS value
# each shipped or fit qsf elaborates. 1 = a chroma miss is a hard (black) pixel and the converter's
# neutral-128 arm is masked; 0 (240p) = the 128 arm is LIVE (luma with grey chroma), an unreviewed
# product behaviour pending release/lead's ruling.
CHROMA_MISS_BLACK_BY_QSF = (
    ("Plex_480p_r1.qsf", True),
    ("Plex_480p.qsf", True),
    ("Plex.qsf", True),
    ("Plex_480i.qsf", True),
    ("Plex_240p.qsf", False),
)


def chroma_miss_black_for_build(present_core: str, plex: str, frame_store: str, macros: set[str]) -> tuple:
    """(value or None, how) of ddr_frame_store's BLACK_ON_CHROMA_MISS for one build. Inputs are the
    comment-stripped sources; the `ifdef arms are resolved with the build's VERILOG_MACROs
    (plex_build_macros: the qsf and every file it sources)."""
    pc = norm(sv_select_defines(present_core, macros))
    args = re.findall(r"\.BLACK_ON_CHROMA_MISS\(([^()]*)\)", pc)
    if len(args) > 1:
        return None, f"{len(args)} .BLACK_ON_CHROMA_MISS overrides in one build"
    if not args:
        m = re.search(r"parameterbitBLACK_ON_CHROMA_MISS=1'b([01])[,)]", norm(frame_store))
        return (m.group(1) == "1", "ddr_frame_store default") if m else (None, "ddr_frame_store default not found")
    if args[0] in ("1'b1", "1'b0"):
        return args[0] == "1'b1", f"present_core .BLACK_ON_CHROMA_MISS({args[0]})"
    if args[0] == "FPGA_DECODE_320":
        px = norm(sv_select_defines(plex, macros))
        if ".FPGA_DECODE_320(FPGA320_CONFIG)" not in px:
            return None, "Plex.sv does not bind present_core .FPGA_DECODE_320(FPGA320_CONFIG)"
        n_def = len(re.findall(r"localparam(?:bit)?FPGA320_CONFIG=", px))
        if n_def != 1:
            return None, f"FPGA320_CONFIG is defined {n_def} times in the selected Plex.sv (want 1)"
        # Under `ifdef FPGA_VIDEO_320, Plex.sv $fatal's unless the macro value is 1.
        if "localparambitFPGA320_CONFIG=`FPGA_VIDEO_320;" in px:
            return True, "FPGA_DECODE_320 = FPGA320_CONFIG = `FPGA_VIDEO_320 (1)"
        if "localparambitFPGA320_CONFIG=1'b0;" in px:
            return False, "FPGA_DECODE_320 = FPGA320_CONFIG = 1'b0"
        return None, "FPGA320_CONFIG definition not recognised"
    return None, f"unrecognised override {args[0]!r}"


def check_chroma_miss_black_contract() -> None:
    """T-CHROMA-MISS-NEUTRAL N1/N2. N1: a chroma miss is a hard miss only when BLACK_ON_CHROMA_MISS
    is set (ddr_frame_store rd_miss_now). N2: per qsf, the value the build elaborates
    (CHROMA_MISS_BLACK_BY_QSF). The neutral-128 arm itself is pinned by the YUV read check (2b),
    whose miss-arm teeth are this item's tooth (e): one tooth, not two."""
    if skip_if_absent("chroma-miss contract", [PLEX_PRJ_DIR / q for q, _ in CHROMA_MISS_BLACK_BY_QSF]):
        return
    fs = select_default_sv_fault_branches(strip_comments(read(DDR_FRAME_STORE)))
    pc = strip_comments(read(PRESENT_CORE))
    plex = strip_comments(read(PLEX_SV))
    prj = _plex_prj_files(PLEX_PRJ_DIR)
    n1 = "wirerd_miss_now=rd_active&&rd_visible&&pixel_live&&(!y_hit_now||(BLACK_ON_CHROMA_MISS&&!c_hit_now));"

    def macros_of(p: dict[str, str]) -> tuple[dict[str, set[str]], list[str]]:
        mac: dict[str, set[str]] = {}
        out: list[str] = []
        for name, _ in CHROMA_MISS_BLACK_BY_QSF:
            mac[name], mp = plex_build_macros(p, name)
            out += [f"{name}: {x}" for x in mp]
        return mac, out

    base_macros, base_mp = macros_of(prj)
    if base_mp:
        fail("chroma-miss contract (N2 build macros): " + " | ".join(base_mp))

    def problems(fs_t: str, pc_t: str, mac: dict[str, set[str]] = base_macros) -> list[str]:
        out = []
        if n1 not in norm(fs_t):
            out.append("N1: rd_miss_now must be a hard miss on !y_hit_now, and on !c_hit_now only "
                       "when BLACK_ON_CHROMA_MISS")
        for name, want in CHROMA_MISS_BLACK_BY_QSF:
            try:
                got, how = chroma_miss_black_for_build(pc_t, plex, fs_t, mac[name])
            except ValueError as e:
                got, how = None, f"unbalanced `ifdef: {e}"
            if got is not want:
                out.append(f"N2: {name} must elaborate BLACK_ON_CHROMA_MISS={int(want)}, got {got} ({how})")
        return out

    base = problems(fs, pc)
    if base:
        fail("chroma-miss contract: " + " | ".join(base))
    arm480i = re.compile(r"`ifdef[ \t]+PLEX_ANALOG_480I[ \t]*\n([ \t]*\.BLACK_ON_CHROMA_MISS\(FPGA_DECODE_320\),[ \t]*\n)`endif[ \t]*\n")
    teeth = (
        ("(a) rd_miss_now term flipped", "fs", "(BLACK_ON_CHROMA_MISS && !c_hit_now)",
         lambda t: t.replace("(BLACK_ON_CHROMA_MISS && !c_hit_now)", "(!BLACK_ON_CHROMA_MISS && !c_hit_now)", 1)),
        ("(b) rd_miss_now chroma term dropped", "fs", "(!y_hit_now || (BLACK_ON_CHROMA_MISS && !c_hit_now))",
         lambda t: t.replace("(!y_hit_now || (BLACK_ON_CHROMA_MISS && !c_hit_now))", "(!y_hit_now)", 1)),
        ("(c) 480P arm BLACK_ON_CHROMA_MISS 1'b0", "pc", ".BLACK_ON_CHROMA_MISS(1'b1),",
         lambda t: t.replace(".BLACK_ON_CHROMA_MISS(1'b1),", ".BLACK_ON_CHROMA_MISS(1'b0),", 1)),
        ("(d) 480i ifdef block removed (480i falls back to 0)", "pc", None,
         lambda t: arm480i.sub("", t, count=1)),
        ("(d') 480i guard dropped (240p would get 1)", "pc", None,
         lambda t: arm480i.sub(r"\1", t, count=1)),
    )
    for name, where, old, mutate in teeth:
        src = fs if where == "fs" else pc
        mutant = mutate(src)
        if mutant == src or (old is not None and old not in src):
            fail(f"chroma-miss tooth '{name}' is vacuous (source text gone)")
        got = problems(mutant, pc) if where == "fs" else problems(fs, mutant)
        if not got:
            fail(f"chroma-miss tooth '{name}' did not make the gate red")

    # F-5 parity teeth for the per-build macro read (plex_build_macros). They mutate the prj dict:
    #   - a macro that flips the 240p arm must reach the N2 table, whether ';'-chained in the qsf or set
    #     in a sourced qip;
    #   - every form the reader cannot evaluate statically fails closed.
    def plus_prj(rel: str, text: str) -> dict[str, str]:
        if rel not in prj:
            fail(f"chroma-miss macro tooth target {rel} is gone (vacuous tooth)")
        return {**prj, rel: prj[rel] + text}

    def build_problems(p: dict[str, str]) -> list[str]:
        mac, mp = macros_of(p)
        return mp or problems(fs, pc, mac)

    flip = 'set_global_assignment -name VERILOG_MACRO "PLEX_ANALOG_480I=1"'
    want240 = "N2: Plex_240p.qsf must elaborate BLACK_ON_CHROMA_MISS=0"
    chained = plus_prj("Plex_240p.qsf", f"\nset x 1; {flip}\n")
    if "PLEX_ANALOG_480I" in qsf_verilog_macros(chained["Plex_240p.qsf"]):
        fail("chroma-miss macro tooth 'chained' is vacuous: the line reader already sees it")
    macro_teeth = (
        ("';'-chained macro in the 240p qsf", chained, want240),
        ("macro in sourced files.qip", plus_prj("files.qip", f"\n{flip}\n"), want240),
        ("dynamic macro value", plus_prj("Plex_240p.qsf", '\nset_global_assignment -name VERILOG_MACRO "$m"\n'),
         "Plex_240p.qsf: Plex_240p.qsf: unparsed VERILOG_MACRO command"),
        ("macro inside an if body", plus_prj("Plex_240p.qsf", f"\nif {{1}} {{{flip}}}\n"),
         "unparsed VERILOG_MACRO command: if {1}"),
        ("macro -remove", plus_prj("Plex_240p.qsf", '\nset_global_assignment -name VERILOG_MACRO "FPGA_VIDEO_320=1" -remove\n'),
         "unparsed VERILOG_MACRO command"),
        ("lower-case assignment name", plus_prj("Plex_240p.qsf", '\nset_global_assignment -name verilog_macro "PLEX_ANALOG_480I=1"\n'),
         "unparsed VERILOG_MACRO command"),
        ("escaped -name word", plus_prj("Plex_240p.qsf", '\nset_global_assignment -name VERILOG_\\x4dACRO "PLEX_ANALOG_480I=1"\n'),
         "-name word missing or not static"),
        ("a second flow script", plus_prj("sys/sys.tcl", '\nset_global_assignment -name POST_FLOW_SCRIPT_FILE "quartus_sh:x.tcl"\n'),
         "flow script not frozen"),
    )
    for name, p, expect in macro_teeth:
        got = build_problems(p)
        if os.environ.get("RTLINV_VERBOSE"):
            print(f"  MACRO TOOTH {name}: {'RED' if got else 'GREEN'} :: {' | '.join(got)[:220]}")
        if not any(expect in g for g in got):
            fail(f"chroma-miss macro tooth '{name}' did not go red with '{expect}': {got}")
    if build_problems(plus_prj("Plex_240p.qsf", f"\n# {flip}\n")):
        fail("chroma-miss macro control: a commented VERILOG_MACRO went red (false positive)")

    # N2 override blind spot (H264 b352c87e, review 20:29): nothing may override BLACK_ON_CHROMA_MISS
    # around the per-qsf evaluation above:
    #   - no defparam (matched on the last path element);
    #   - no set_parameter or set_instance_assignment PARAMETER anywhere in the project scan set;
    #   - only the two reviewed files name it.
    # Uses the shared param-override helper; its md5 is cited in the commit.
    # PH-W (review 21:04/21:06): the per-qsf macro evaluation and this census read the SAME prj dict
    # (_plex_prj_files, rels exact and relative to fpga/Plex_MiSTer). So a command hidden in brace data
    # (catch/foreach/while), which the per-qsf readers never see, is red here on the census alone
    # (tooth 'catch-hidden source').
    rtl = {r: t for r, t in prj.items() if r.endswith(_POH_RTL_EXT)}
    cfg = {r: t for r, t in prj.items() if not r.endswith(_POH_RTL_EXT)}
    n2_files = {"rtl/ddr_frame_store.sv", "rtl/present_core.sv"}

    def override_problems(rtl_t: dict, cfg_t: dict) -> list[str]:
        out = param_override_problems({"BLACK_ON_CHROMA_MISS"}, rtl_t, cfg_t)
        named = files_naming("BLACK_ON_CHROMA_MISS", {**rtl_t, **cfg_t})
        if named != n2_files:
            out.append(f"files naming BLACK_ON_CHROMA_MISS {sorted(named)} != reviewed {sorted(n2_files)}")
        return out

    base_ov = override_problems(rtl, cfg)
    if base_ov:
        fail("chroma-miss contract (N2 overrides): " + " | ".join(base_ov))

    def plus(files: dict, rel: str, text: str) -> dict:
        if rel not in files:
            fail(f"chroma-miss override tooth target {rel} is gone (vacuous tooth)")
        return {**files, rel: files[rel] + text}

    ov_teeth = (
        ("defparam in Plex.sv", plus(rtl, "Plex.sv", "\ndefparam present.fstore.BLACK_ON_CHROMA_MISS = 1'b0;\n"), cfg,
         "Plex.sv: defparam overrides BLACK_ON_CHROMA_MISS"),
        ("multi-line comma-list defparam", plus(rtl, "Plex.sv", "\ndefparam\n\tpresent.fstore.DDR_BURST_MAX = 64,\n"
                                                "\tpresent.fstore.BLACK_ON_CHROMA_MISS = 1'b0;\n"), cfg,
         "Plex.sv: defparam overrides BLACK_ON_CHROMA_MISS"),
        ("defparam in an .svh header", plus(rtl, "rtl/ddr_frame_layout_params.svh",
                                            "\ndefparam present.fstore.BLACK_ON_CHROMA_MISS = 1'b0;\n"), cfg,
         "rtl/ddr_frame_layout_params.svh: defparam overrides BLACK_ON_CHROMA_MISS"),
        ("set_parameter in the 240p qsf", rtl, plus(cfg, "Plex_240p.qsf", "\nset_parameter -name BLACK_ON_CHROMA_MISS 1 "
                                                    "-to \"emu:emu|present_core:present|ddr_frame_store:fstore\"\n"),
         "Plex_240p.qsf: set_parameter overrides BLACK_ON_CHROMA_MISS"),
        ("set_instance_assignment PARAMETER in the r1 qsf", rtl,
         plus(cfg, "Plex_480p_r1.qsf", "\nset_instance_assignment -name PARAMETER -to fstore BLACK_ON_CHROMA_MISS=0\n"),
         "Plex_480p_r1.qsf: set_instance_assignment PARAMETER overrides BLACK_ON_CHROMA_MISS"),
        ("dynamic set_parameter in sys/sys.tcl", rtl,
         plus(cfg, "sys/sys.tcl", "\nset p BLACK_ON_CHROMA_MISS; set_parameter -name $p 0\n"),
         "sys/sys.tcl: unparsed parameter line"),
        ("a third file names it (instance override in Plex.sv)",
         plus(rtl, "Plex.sv", "\nddr_frame_store #(.BLACK_ON_CHROMA_MISS(1'b0)) u_extra ();\n"), cfg,
         "files naming BLACK_ON_CHROMA_MISS"),
        ("a cfg file names it (VERILOG_MACRO in the 240p qsf)", rtl,
         plus(cfg, "Plex_240p.qsf", "\nset_global_assignment -name VERILOG_MACRO \"BLACK_ON_CHROMA_MISS=1\"\n"),
         "files naming BLACK_ON_CHROMA_MISS"),
        ("catch-hidden source in the 240p qsf (PH-W)", rtl,
         plus(cfg, "Plex_240p.qsf", "\ncatch {source sys/no_such_file.tcl}\n"),
         "Plex_240p.qsf: unparsed parameter line (command head not in census) catch"),
    )
    for name, rtl_t, cfg_t, expect in ov_teeth:
        got = override_problems(rtl_t, cfg_t)
        if os.environ.get("RTLINV_VERBOSE"):
            print(f"  OVERRIDE TOOTH {name}: {'RED' if got else 'GREEN'} :: {' | '.join(got)[:220]}")
        if not any(expect in g for g in got):
            fail(f"chroma-miss override tooth '{name}' did not go red with '{expect}': {got}")
    if override_problems(plus(rtl, "Plex.sv", "\n// BLACK_ON_CHROMA_MISS is set per arm in present_core\n"), cfg):
        fail("chroma-miss override control: a comment naming BLACK_ON_CHROMA_MISS went red (false positive)")
    print(
        "PASS chroma-miss contract (N1 rd_miss_now; N2 BLACK_ON_CHROMA_MISS = "
        + ", ".join(f"{n.removesuffix('.qsf')} {int(w)}" for n, w in CHROMA_MISS_BLACK_BY_QSF)
        + "; teeth (a)-(d), (d') red; (e) is the YUV read check's miss-arm teeth; build macros from the qsf "
        f"and every file it sources, fail closed, {len(macro_teeth)} macro teeth red; no defparam/"
        f"set_parameter override, named only in {sorted(n2_files)}; {len(ov_teeth)} override teeth red, "
        "comment controls green)"
    )


def check_present_geometry_stride_contract() -> None:
    host = strip_comments(read(DDR_FRAME_LAYOUT_HPP))
    layout = strip_comments(read(DDR_FRAME_LAYOUT_SVH))
    media = strip_comments(read(MEDIA_PLAYER_CPP))
    fb_present = strip_comments(read(FB_PRESENT_CPP))
    frame_store = select_default_sv_fault_branches(strip_comments(read(DDR_FRAME_STORE)))
    present_core = strip_comments(read(PRESENT_CORE))
    qsf = read(PLEX_QSF)

    host_nt = norm(host)
    media_nt = norm(media)
    fb_nt = norm(fb_present)
    frame_nt = norm(frame_store)
    present_nt = norm(present_core)
    qsf_nt = norm(qsf)

    coded_w = cpp_const(host, "kPlex480pCodedWidth")
    coded_h = cpp_const(host, "kPlex480pCodedHeight")
    display_w = cpp_const(host, "kPlex480pDisplayWidth")
    presented_w = cpp_const(host, "kPlex480pPresentedWidth")
    crop_right = cpp_const(host, "kPlex480pCropRight")
    pillar_left = cpp_const(host, "kPlex480pPillarboxLeft")
    pillar_right = cpp_const(host, "kPlex480pPillarboxRight")
    y_stride = cpp_const(host, "kPlex480pYStrideBytes")
    c_stride = cpp_const(host, "kPlex480pChromaStrideBytes")
    y_offset = cpp_const(host, "kPlex480pYPlaneOffset")
    u_offset = cpp_const(host, "kPlex480pUPlaneOffset")
    v_offset = cpp_const(host, "kPlex480pVPlaneOffset")
    y_bytes = cpp_const(host, "kPlex480pYuv420pBytes")

    check(
        (coded_w, coded_h, display_w, presented_w, crop_right, pillar_left, pillar_right)
        == (624, 480, 618, 640, 6, 11, 11),
        "480p geometry contract changed unexpectedly. If the PMS/profile target changed, "
        "update the geometry table, visual provenance, and this stride gate together.",
    )
    check(
        (y_stride, c_stride, y_offset, u_offset, v_offset, y_bytes)
        == (624, 312, 0, 299520, 374400, 449280),
        "480p I420 byte layout changed unexpectedly. The product path must declare any "
        "stride/offset change before grading distorted-video reports.",
    )

    # T-RTLINV-STALE (3b): since 2affb815 the crop is runtime-latched (RUNTIME_GEOMETRY) and still
    # applied exactly once: crop_x has one definition and one use, in src_x.
    crop_needles = (
        "wire[15:0]crop_x=RUNTIME_GEOMETRY?active_crop_left:16'(CROP_LEFT);",
        "wire[CODED_X_W-1:0]src_x=rd_visible?(CODED_X_W'(display_x)+CODED_X_W'(crop_x)):'0;",
    )

    def missing_stride_requirements(host_norm: str, media_norm: str, frame_norm: str) -> list[str]:
        required = [
            (
                host_norm,
                "constuint64_tlineBytes=static_cast<uint64_t>(geom.coded_width)",
                "ARM layout must derive luma stride from coded_width (624), not display/presented width",
            ),
            (
                host_norm,
                "constuint64_tchromaLineBytes=static_cast<uint64_t>(geom.coded_width/2)",
                "ARM layout must derive chroma stride from coded_width/2 (312), not cropped width/2",
            ),
            (
                host_norm,
                "constuint32_tyBytes=static_cast<uint32_t>(geom.coded_width*geom.coded_height)",
                "ARM plane offsets must use coded_width*coded_height for Y bytes",
            ),
            (
                host_norm,
                "if(width==kPlex480pPresentedWidth&&height==kPlex480pPresentedHeight)returnplex480pDdrFrameGeometry();",
                "640x480 presentation must map to the declared 624x480 coded / 618x480 display geometry",
            ),
            (
                media_norm,
                "constintrawW=ddrGeometry.coded_width;",
                "FFmpeg rawvideo width must be the coded stride width (624) for FPGA-presented 480p",
            ),
            (
                media_norm,
                "constintrawDisplayW=ddrGeometry.display_width;",
                "FFmpeg visible scale width must be the cropped display width (618)",
            ),
            (
                media_norm,
                'vf+=std::string("scale=")+displayScale+":force_original_aspect_ratio=decrease:flags="+swsFlags+",pad="+scale+":"+std::to_string(ddrGeometry.crop_left)+":"+std::to_string(ddrGeometry.crop_top)+":color=black";',
                "FFmpeg must scale into display geometry then pad once into the coded 624-pixel stride",
            ),
            (
                media_norm,
                "clearYuv420pCropPadding(slotFrame,ddrGeometry)",
                "rawvideo DDR frames must blacken coded crop padding before the frame-store reads them",
            ),
            (
                media_norm,
                "clearPlane(yuv+yBytes,cW,cW,cH,g.crop_left/2,g.crop_right/2,g.crop_top/2,g.crop_bottom/2,kYuv420BlackU)",
                "U crop padding must use chroma stride coded_width/2 (312) and half-resolution crop",
            ),
            (
                media_norm,
                "clearPlane(yuv+yBytes+cW*cH,cW,cW,cH,g.crop_left/2,g.crop_right/2,g.crop_top/2,g.crop_bottom/2,kYuv420BlackV)",
                "V crop padding must use chroma stride coded_width/2 (312) and half-resolution crop",
            ),
            (
                media_norm,
                "ok=fpga_.sendYuv420pFrameDdr(txFrame,txBytes,ddrGeometry,ddrBank_)",
                "rawvideo present must send the same declared geometry that selected FFmpeg's coded stride",
            ),
            (
                frame_norm,
                "Y_LINE_QWORDS=CODED_W/8",
                "RTL luma DDR line stride must be CODED_W/8 qwords (624 bytes), not FRAME_W/8",
            ),
            (
                frame_norm,
                "C_LINE_QWORDS=CODED_W/16",
                "RTL chroma DDR line stride must be CODED_W/16 qwords (312 bytes), not display/FRAME width",
            ),
            (
                frame_norm,
                "PRESENT_END_X=X_W'(PRESENT_X+DISPLAY_W)",
                "RTL visible area must end at pillar_left + display_width, leaving the right pillar outside DDR",
            ),
            (
                frame_norm,
                crop_needles[0],
                "RTL crop_x must be the runtime-latched crop (active_crop_left), else the CROP_LEFT parameter",
            ),
            (
                frame_norm,
                crop_needles[1],
                "RTL source X must crop from display_x into coded coordinates exactly once",
            ),
        ]
        out = [msg for haystack, needle, msg in required if needle not in haystack]
        n_crop = len(re.findall(r"\bcrop_x\b", frame_norm))
        if n_crop != 2:
            out.append(f"RTL crop_x must have exactly one definition and one use (src_x), found {n_crop} references")
        # T-RTLINV-STALE (2): one helper, shared with the DDR frame-store YUV read check.
        out += [f"RTL chroma fetch address must stride by 312-byte chroma lines: {p}"
                for p in chroma_line_stride_problems(frame_norm)]
        return out

    missing = missing_stride_requirements(host_nt, media_nt, frame_nt)
    # T-RTLINV-STALE (3b) teeth run before the base verdict, so they bite while the held FOAR/DAR
    # needle (T-RTLINV-STALE (3)) keeps this check failing. Each must add a NEW miss. Each tooth's
    # source text is part of a pinned needle, so if it is gone that needle already reports the
    # change in the base verdict below (no silent vacuous tooth); the tooth is skipped.
    for name, old, new in (
        ("crop added twice",
         "src_x=rd_visible?(CODED_X_W'(display_x)+CODED_X_W'(crop_x)):'0;",
         "src_x=rd_visible?(CODED_X_W'(display_x)+CODED_X_W'(crop_x)+CODED_X_W'(crop_x)):'0;"),
        ("crop dropped",
         "src_x=rd_visible?(CODED_X_W'(display_x)+CODED_X_W'(crop_x)):'0;",
         "src_x=rd_visible?(CODED_X_W'(display_x)):'0;"),
        ("RUNTIME arm pinned to the CROP_LEFT constant",
         "crop_x=RUNTIME_GEOMETRY?active_crop_left:16'(CROP_LEFT);",
         "crop_x=RUNTIME_GEOMETRY?16'(CROP_LEFT):16'(CROP_LEFT);"),
    ):
        if not any(old in needle for needle in crop_needles):
            fail(f"crop tooth '{name}' source text is not part of a pinned needle (vacuous tooth)")
        if old not in frame_nt:
            continue
        if not set(missing_stride_requirements(host_nt, media_nt, frame_nt.replace(old, new, 1))) - set(missing):
            fail(f"crop tooth '{name}' did not make the present geometry/stride gate red")
    if missing:
        fail("present geometry/stride contract: " + " | ".join(missing))

    # Default (non-L4) arm must still bind the 480p layout symbols. L4 arm binds
    # DDR_FRAME_720P_* via FS_* localparams (see present_core.sv). Accept either
    # direct .CODED_W(DDR_FRAME_*) or FS_CODED_W = DDR_FRAME_* (parameterized form).
    default_480_bind = (
        (
            ".CODED_W(DDR_FRAME_CODED_WIDTH)" in present_nt
            and ".DISPLAY_W(DDR_FRAME_DISPLAY_WIDTH)" in present_nt
            and ".PRESENT_X(DDR_FRAME_PILLARBOX_LEFT)" in present_nt
            and ".HPS_BANK_STRIDE_BYTES(DDR_FRAME_YUV420P_BANK_STRIDE)" in present_nt
            and ".DOORBELL_PHYS(DDR_FRAME_YUV420P_DOORBELL_PHYS)" in present_nt
        )
        or (
            "FS_CODED_W=DDR_FRAME_CODED_WIDTH" in present_nt
            and "FS_DISPLAY_W=DDR_FRAME_DISPLAY_WIDTH" in present_nt
            and "FS_PRESENT_X=DDR_FRAME_PILLARBOX_LEFT" in present_nt
            and "FS_BANK_STRIDE=DDR_FRAME_YUV420P_BANK_STRIDE" in present_nt
            and "FS_DOORBELL=DDR_FRAME_YUV420P_DOORBELL_PHYS" in present_nt
            and ".CODED_W(FS_CODED_W)" in present_nt
            and ".HPS_BANK_STRIDE_BYTES(FS_BANK_STRIDE)" in present_nt
        )
    )
    check(
        ".FRAME_W(FRAME_W)" in present_nt
        and ".FRAME_H(FRAME_H)" in present_nt
        and default_480_bind,
        "present_core must instantiate ddr_frame_store from the shared layout params: "
        "FRAME_W=640 for scanout, CODED_W=624 for DDR stride, DISPLAY_W=618 for crop, "
        "PRESENT_X=11 for pillarbox.",
    )
    # L4 consumer wiring (must not leave 720p constants unreferenced).
    l4_720_bind = (
        "FS_CODED_W=DDR_FRAME_720P_CODED_WIDTH" in present_nt
        and "FS_CODED_H=DDR_FRAME_720P_CODED_HEIGHT" in present_nt
        and "FS_DISPLAY_W=DDR_FRAME_720P_DISPLAY_WIDTH" in present_nt
        and "FS_DISPLAY_H=DDR_FRAME_720P_DISPLAY_HEIGHT" in present_nt
        and "FS_PRESENT_X=DDR_FRAME_720P_PILLARBOX_LEFT" in present_nt
        and "FS_BANK_STRIDE=DDR_FRAME_720P_YUV420P_BANK_STRIDE" in present_nt
        and "FS_DOORBELL=DDR_FRAME_720P_YUV420P_DOORBELL_PHYS" in present_nt
        and "FS_PHYS_BASE=DDR_FRAME_720P_PHYS_BASE" in present_nt
        and "ifdefPLEX_PRESENT_720P_L4" in present_nt
    )
    check(
        l4_720_bind,
        "present_core L4 arm must wire DDR_FRAME_720P_* into ddr_frame_store "
        "(coded/display/pillar/stride/doorbell/phys_base) behind PLEX_PRESENT_720P_L4.",
    )
    check(
        'set_global_assignment-nameVERILOG_MACRO"DDR_FRAME_STORE=1"' in qsf_nt
        and 'set_global_assignment-nameVERILOG_MACRO"FRAME_W=640"' in qsf_nt
        and 'set_global_assignment-nameVERILOG_MACRO"FRAME_H=480"' in qsf_nt,
        "Quartus build must declare DDR_FRAME_STORE with 640x480 presented scanout. "
        "A missing/changed FRAME_W silently changes the frame-store scanout geometry.",
    )
    check(
        "uPlane=yPlane+static_cast<size_t>(w)*static_cast<size_t>(h)" in fb_nt
        and "vPlane=uPlane+static_cast<size_t>(w/2)*static_cast<size_t>(h/2)" in fb_nt,
        "fb0 YUV reference blit must use the supplied coded width as stride and "
        "coded_width/2 as chroma stride.",
    )

    bad_host_presented_stride = host_nt.replace(
        "constuint64_tlineBytes=static_cast<uint64_t>(geom.coded_width)",
        "constuint64_tlineBytes=static_cast<uint64_t>(geom.presented_width)",
    )
    if not missing_stride_requirements(bad_host_presented_stride, media_nt, frame_nt):
        fail("deliberately changed ARM luma stride 624→640 did not make the geometry gate red")
    bad_media_display_stride = media_nt.replace(
        "constintrawW=ddrGeometry.coded_width;",
        "constintrawW=ddrGeometry.display_width;",
    )
    if not missing_stride_requirements(host_nt, bad_media_display_stride, frame_nt):
        fail("deliberately changed FFmpeg raw stride 624→618 did not make the geometry gate red")
    bad_chroma_stride = frame_nt.replace("C_LINE_QWORDS=CODED_W/16", "C_LINE_QWORDS=FRAME_W/16")
    if not missing_stride_requirements(host_nt, media_nt, bad_chroma_stride):
        fail("deliberately changed RTL chroma stride 312→320 did not make the geometry gate red")

    print(
        "PASS present geometry/stride chain is declared end-to-end "
        "(coded 624 stride, display 618 crop, presented 640 with 11px pillars, chroma stride 312)"
    )


def check_ddr_bank_handoff_contract() -> None:
    fpga_cpp = strip_comments(read(FPGA_SPI_CPP))
    fpga_h = strip_comments(read(FPGA_SPI_HPP))
    media = strip_comments(read(MEDIA_PLAYER_CPP))
    frame_store = select_default_sv_fault_branches(strip_comments(read(DDR_FRAME_STORE)))
    fpga_nt = norm(fpga_cpp)
    fpga_h_nt = norm(fpga_h)
    media_nt = norm(media)
    frame_nt = norm(frame_store)

    def missing_handoff_requirements(
        cpp_norm: str, h_norm: str, media_norm: str, rtl_norm: str
    ) -> list[str]:
        required = [
            (
                cpp_norm,
                "constexprintkDdrBankReuseMinUs=16000;",
                "ARM DDR writer must enforce a same-bank reuse floor of at least one 60Hz vsync "
                "(16ms; 40ms half-rates 24fps and is banned on the product rate path)",
            ),
            (
                h_norm,
                "doublelastDdrBankDoorbellMs_[2]={-1.0,-1.0};",
                "ARM DDR writer must remember when each bank was last published",
            ),
            (
                h_norm,
                "int64_tbank_reuse_wait_us=0;",
                "DDR timing telemetry must expose any same-bank reuse wait",
            ),
            (
                media_norm,
                "ddr_bank_reuse_wait_us_p=",
                "present_profile logs must surface same-bank reuse waits by name",
            ),
            (
                media_norm,
                "prof.ddrBankReuseWaitUs+=dt.bank_reuse_wait_us;",
                "present_profile must accumulate writer-side same-bank reuse waits",
            ),
            (
                cpp_norm,
                "constdoublelastBankMs=lastDdrBankDoorbellMs_[bank];",
                "sendDdrFrame must check the selected bank's last doorbell time before copying",
            ),
            (
                cpp_norm,
                "if(sinceUs<kDdrBankReuseMinUs)",
                "sendDdrFrame must wait rather than reusing the same bank before the reuse floor",
            ),
            (
                cpp_norm,
                "timing.bank_reuse_wait_us=waitUs;",
                "same-bank reuse waits must be externally visible in DDR timing telemetry",
            ),
            (
                cpp_norm,
                "std::memcpy(ddrMap_+bankOff,payload,len);__sync_synchronize();",
                "payload writes must be fenced before any doorbell can publish the bank",
            ),
            (
                cpp_norm,
                "dw[1]=hi;__sync_synchronize();dw[0]=kDdrDoorbellMagic;__sync_synchronize();",
                "doorbell must publish the bank/format/seq token before PLXK magic so a cold "
                "mailbox cannot expose magic with a zero/old format",
            ),
            (
                cpp_norm,
                "lastDdrBankDoorbellMs_[bank]=std::chrono::duration<double,std::milli>",
                "sendDdrFrame must record the successful publish time for the bank it just rang",
            ),
            (
                rtl_norm,
                "db_token=DDRAM_DOUT[63:32]",
                "frame-store reader must capture bank/format/seq as one 32-bit token from one DDR read",
            ),
            (
                rtl_norm,
                "db_format=db_token[30:29]",
                "frame-store reader must decode format from the same token as seq/bank",
            ),
            (
                rtl_norm,
                "db_token_new=db_valid_token&&(!have_seq||(db_token!=last_seq))",
                "frame-store reader must compare the whole token, not bank and seq from separate reads",
            ),
            (
                rtl_norm,
                "pending_bank_ddr<=DDRAM_DOUT[63];",
                "frame-store reader must latch the bank bit from the same DDR word that supplied seq",
            ),
            (
                rtl_norm,
                "last_seq<=db_token;",
                "frame-store reader must store the full token it consumed, preventing bank/seq torn reads",
            ),
        ]
        return [msg for haystack, needle, msg in required if needle not in haystack]

    missing = missing_handoff_requirements(fpga_nt, fpga_h_nt, media_nt, frame_nt)
    if missing:
        fail(f"DDR bank handoff contract: {missing[0]}")

    no_reuse_wait = fpga_nt.replace(
        "if(sinceUs<kDdrBankReuseMinUs)",
        "if(false)",
    )
    if not missing_handoff_requirements(no_reuse_wait, fpga_h_nt, media_nt, frame_nt):
        fail("deliberately removed same-bank reuse wait did not make the handoff gate red")
    bad_doorbell_order = fpga_nt.replace(
        "dw[1]=hi;__sync_synchronize();dw[0]=kDdrDoorbellMagic;__sync_synchronize();",
        "dw[0]=kDdrDoorbellMagic;dw[1]=hi;__sync_synchronize();",
    )
    if not missing_handoff_requirements(bad_doorbell_order, fpga_h_nt, media_nt, frame_nt):
        fail("deliberately reordered doorbell magic before token did not make the handoff gate red")
    torn_token_reader = frame_nt.replace(
        "last_seq<=db_token;",
        "last_seq<=DDRAM_DOUT[62:32];",
    )
    if not missing_handoff_requirements(fpga_nt, fpga_h_nt, media_nt, torn_token_reader):
        fail("deliberately split bank/seq token in RTL reader did not make the handoff gate red")

    print(
        "PASS DDR bank handoff publishes fenced frames and guards same-bank reuse "
        "(PLXD bank-release ACK at PLX page +0x128)"
    )


def check_yuv_ddr_writer_contract() -> None:
    media = strip_comments(read(MEDIA_PLAYER_CPP))
    fb_present = strip_comments(read(FB_PRESENT_CPP))
    main_cpp = strip_comments(read(MISTERPLEXD_MAIN_CPP))
    fpga_h = strip_comments(read(FPGA_SPI_HPP))
    fpga_cpp = strip_comments(read(FPGA_SPI_CPP))
    layout_h = strip_comments(read(DDR_FRAME_LAYOUT_HPP))
    recon_h = strip_comments(read(H264_RECON_HPP))
    ddr_writer_sources = "\n".join([media, main_cpp, fpga_h, fpga_cpp, layout_h])
    check(
        "packRgb24ToRgb565Le" not in media,
        "media_player.cpp reintroduced RGB24→RGB565 packing in the rawvideo present path. "
        "The C3 DDR frame store consumes planar YUV420p directly; do not leave a dead RGB565 "
        "alternate hot path that can drift from the RTL reader.",
    )
    check(
        "DdrFrameFormat::Rgb565" not in ddr_writer_sources,
        "ARM DDR writer code still exposes DdrFrameFormat::Rgb565. C3 RTL interprets DDR as "
        "I420/YUV420p unconditionally; retaining an RGB565 DDR enum/config is a silent "
        "cross-module format mismatch waiting to render garbage.",
    )
    check(
        "sendRgb565FrameDdr" not in ddr_writer_sources
        and "sendRgb24FrameDdr" not in ddr_writer_sources,
        "ARM code can still write RGB payloads to the DDR frame-store doorbell. C3 RTL is "
        "YUV-only; use sendYuv420pFrameDdr for DDR or fail loudly rather than producing "
        "decoder-looking garbage.",
    )
    check(
        "index == 1" in fpga_cpp
        and "non-YUV frame send refused" in fpga_cpp
        and "push_frame --ddr --yuv420p" in fpga_cpp,
        "FpgaSpi::sendFileTx must refuse F1/SPI frame sends with a named non-YUV error. "
        "The YUV420p DDR contract is not enforced if callers can still silently push RGB565 "
        "through the legacy F1 ioctl path.",
    )
    check(
        "DDR_FRAME_FORMAT" in main_cpp
        and "fixed to yuv420p" in main_cpp
        and "DdrFrameFormat::Rgb565" not in main_cpp,
        "misterplexd main must not allow DDR_FRAME_FORMAT=rgb565 to reactivate the old "
        "writer. Keep any legacy config key as an ignored warning and leave production fixed "
        "to yuv420p.",
    )

    media_nt = norm(media)
    fb_nt = norm(fb_present)
    fpga_nt = norm(fpga_cpp)
    recon_nt = norm(recon_h)

    def missing_arm_yuv_requirements(
        media_norm: str, fb_norm: str, fpga_norm: str, recon_norm: str
    ) -> list[str]:
        required = [
            (
                media_norm,
                'caseRawVideoFormat::Yuv420p:return"yuv420p";',
                "FFmpeg rawvideo output must request yuv420p/I420 for DDR YUV mode",
            ),
            (
                media_norm,
                "wantYuvDdr=wantFpgaFrameStore&&ddrFrameFormat_==DdrFrameFormat::Yuv420p",
                "FPGA DDR presentation must select the YUV420p rawvideo decoder output mode",
            ),
            (
                media_norm,
                "if(wantYuvDdr){videoFmt=RawVideoFormat::Yuv420p;}",
                "DDR YUV mode must force the FFmpeg pipe format to yuv420p/I420",
            ),
            (
                media_norm,
                'args.push_back("-pix_fmt");args.push_back(ffmpegPixFmt(videoFmt));',
                "FFmpeg invocation must bind -pix_fmt to the selected yuv420p/I420 decoder output",
            ),
            (
                media_norm,
                "caseRawVideoFormat::Yuv420p:returnyuv420pFrameBytes(width,height);",
                "YUV420p frame byte count must be coded_width*coded_height*3/2",
            ),
            (
                media_norm,
                "n=::read(rfd,dst+got,frameBytes-got);",
                "decoded rawvideo bytes must be read contiguously into the frame/DDR destination",
            ),
            (
                media_norm,
                "constuint8_t*txFrame=cleanFrame;",
                "DDR send must use the clean frame pointer, not a remapped chroma scratch buffer",
            ),
            (
                media_norm,
                "size_ttxBytes=frameBytes;",
                "DDR send length must remain the full contiguous yuv420p frame size",
            ),
            (
                media_norm,
                "presentCleanFrame(frame.data(),true);",
                "product playback must present the contiguous FFmpeg yuv420p pipe buffer",
            ),
            (
                media_norm,
                "clearPlane(yuv+yBytes,cW,cW,cH,g.crop_left/2,g.crop_right/2,g.crop_top/2,g.crop_bottom/2,kYuv420BlackU)",
                "crop padding must treat offset yBytes as the U/Cb plane",
            ),
            (
                media_norm,
                "clearPlane(yuv+yBytes+cW*cH,cW,cW,cH,g.crop_left/2,g.crop_right/2,g.crop_top/2,g.crop_bottom/2,kYuv420BlackV)",
                "crop padding must treat offset yBytes+cBytes as the V/Cr plane",
            ),
            (
                media_norm,
                "std::memcpy(yuv420p.data()+yBytes,rec.u.data(),cBytes)",
                "host recon staging must copy rec.u to the U/Cb plane at offset yBytes",
            ),
            (
                media_norm,
                "std::memcpy(yuv420p.data()+yBytes+cBytes,rec.v.data(),cBytes)",
                "host recon staging must copy rec.v to the V/Cr plane after U",
            ),
            (
                media_norm,
                "ok=fpga_.sendYuv420pFrameDdr(txFrame,txBytes,ddrGeometry,ddrBank_)",
                "product rawvideo DDR send must pass the yuv420p frame buffer unchanged",
            ),
            (
                fpga_norm,
                "returnsendDdrFrame(yuv420p,len,bank,policy)",
                "sendYuv420pFrameDdr must forward the yuv420p pointer to the DDR copier unchanged",
            ),
            (
                fpga_norm,
                "std::memcpy(ddrMap_+bankOff,payload,len)",
                "DDR writer must copy the contiguous Y,U,V payload into the bank without plane remap",
            ),
            (
                fb_norm,
                "uPlane=yPlane+static_cast<size_t>(w)*static_cast<size_t>(h)",
                "fb0 reference blit must locate U immediately after Y",
            ),
            (
                fb_norm,
                "vPlane=uPlane+static_cast<size_t>(w/2)*static_cast<size_t>(h/2)",
                "fb0 reference blit must locate V immediately after U",
            ),
            (
                fb_norm,
                "pixel::yuvToRgb(yRow[x],uRow[x/2],vRow[x/2],r,g,b)",
                "fb0 reference blit must feed yuvToRgb as Y,U,V",
            ),
            (
                recon_norm,
                "uint8_t*planes[2]={out.u.data(),out.v.data()}",
                "host H.264 recon chroma component order must be U/Cb then V/Cr",
            ),
            (
                recon_norm,
                "int16_tdcU[2][2],dcV[2][2]",
                "host H.264 recon must keep first/second chroma DC blocks as U then V",
            ),
            (
                recon_norm,
                "int16_t(*dcs[2])[2][2]={&dcU,&dcV}",
                "host H.264 recon must add chroma residuals to U then V planes",
            ),
        ]
        return [msg for haystack, needle, msg in required if needle not in haystack]

    missing = missing_arm_yuv_requirements(media_nt, fb_nt, fpga_nt, recon_nt)
    if missing:
        fail(f"ARM YUV420 DDR plane contract: {missing[0]}")

    swapped_media = (
        media_nt.replace(
            "std::memcpy(yuv420p.data()+yBytes,rec.u.data(),cBytes)",
            "std::memcpy(yuv420p.data()+yBytes,rec.v.data(),cBytes)",
        )
        .replace(
            "std::memcpy(yuv420p.data()+yBytes+cBytes,rec.v.data(),cBytes)",
            "std::memcpy(yuv420p.data()+yBytes+cBytes,rec.u.data(),cBytes)",
        )
        .replace(
            "clearPlane(yuv+yBytes,cW,cW,cH,g.crop_left/2,g.crop_right/2,g.crop_top/2,g.crop_bottom/2,kYuv420BlackU)",
            "clearPlane(yuv+yBytes,cW,cW,cH,g.crop_left/2,g.crop_right/2,g.crop_top/2,g.crop_bottom/2,kYuv420BlackV)",
        )
        .replace(
            "clearPlane(yuv+yBytes+cW*cH,cW,cW,cH,g.crop_left/2,g.crop_right/2,g.crop_top/2,g.crop_bottom/2,kYuv420BlackV)",
            "clearPlane(yuv+yBytes+cW*cH,cW,cW,cH,g.crop_left/2,g.crop_right/2,g.crop_top/2,g.crop_bottom/2,kYuv420BlackU)",
        )
    )
    if not missing_arm_yuv_requirements(swapped_media, fb_nt, fpga_nt, recon_nt):
        fail("deliberately swapped ARM U/V staging did not make the YUV420 DDR plane gate red")
    base_arm_missing = set(missing_arm_yuv_requirements(media_nt, fb_nt, fpga_nt, recon_nt))
    for old, new in (
        (
            "returnsendDdrFrame(yuv420p,len,bank,policy)",
            "returnsendDdrFrame(remapYuv420pForDdr(yuv420p),len,bank,policy)",
        ),
        ("std::memcpy(ddrMap_+bankOff,payload,len)", "copyYv12PlanesToDdr(ddrMap_+bankOff,payload,len)"),
    ):
        if old not in fpga_nt:
            fail(f"DDR payload remap mutant source text is gone from fpga_spi.cpp (vacuous mutant): {old}")
        remapped = set(missing_arm_yuv_requirements(media_nt, fb_nt, fpga_nt.replace(old, new), recon_nt))
        if not remapped - base_arm_missing:
            fail(f"deliberately remapped rawvideo DDR payload ({new}) did not make the YUV420 DDR plane gate red")
    print(
        "PASS ARM DDR writer keeps FFmpeg yuv420p/I420 order "
        "(Y then U/Cb then V/Cr) through the product DDR path"
    )

    compact_media = norm(media)
    check(
        "renderIdleYuv420p" in media
        and (
            "sendYuv420pFrameDdr(yuv.data(),yuv.size(),g,ddrBank_)" in compact_media
            or (
                "sendYuv420pFrameDdr(yuv.data(),yuv.size(),g,0)" in compact_media
                and "sendYuv420pFrameDdr(yuv.data(),yuv.size(),g,1)" in compact_media
            )
        ),
        "MediaPlayer::paintIdle must send the rendered logo/screensaver through the YUV420p "
        "DDR path. A hard-coded black I420 payload clears stale video but makes the selectable "
        "FPGA idle logo/screensaver disappear under PRESENT=fpga.",
    )
    check(
        "sendRgb24Frame(buf.data(),w,h,/*F1*/1)" not in compact_media
        and "sendRgb24Frame(buf.data(),w,h,1)" not in compact_media
        and "sendRgb565Bytes(txFrame,txBytes,/*F1*/1)" not in compact_media
        and "sendRgb565Bytes(txFrame,txBytes,1)" not in compact_media
        and "useDdrF1_=false" not in compact_media
        and "RGB F1 fallback" not in media,
        "media_player.cpp still has an RGB/SPI F1 fallback or disables future DDR attempts "
        "after a failure. That can hide a DDR YUV420p refusal and re-create the frozen-screen "
        "measurement failure; F1 product presentation must fail loudly and keep reporting DDR "
        "failure instead.",
    )
    check(
        "memset(yuv.data(),kYuv420BlackY,yBytes)" not in compact_media,
        "MediaPlayer::paintIdle still constructs an all-black DDR idle payload. That is only "
        "valid for IDLE_SCREEN=black; logo/screensaver modes must preserve the idle renderer.",
    )
    tooling = "\n".join(
        [
            strip_comments(read(VALIDATE_PLAYBACK_HW_SH)),
            strip_comments(read(TEST_DDR_FRAME_SH)),
            strip_comments(read(TEST_FPGA_PUSH_SH)),
            read(HW_README_MD),
            read(PHASE3_DECODE_MD),
        ]
    )
    check(
        "--ddr --rgb24" not in tooling,
        "DDR helper tooling still invokes push_frame --ddr --rgb24. The DDR frame-store path "
        "is YUV420p only; RGB helpers must remain SPI-only or the hardware gate will now "
        "fail before presenting a frame.",
    )
    check(
        "push_frame --ddr" not in tooling or "file.rgb565" not in tooling,
        "DDR helper documentation still describes push_frame --ddr with RGB565 input. That "
        "call site was orphaned when 28c6c79 removed RGB DDR writes; use --yuv420p/I420.",
    )
    check(
        "push_frame --index 1" not in tooling and "plex_test_320x240.rgb565" not in tooling,
        "Hardware helpers/docs still exercise the retired RGB565 SPI F1 frame path. That path "
        "must not be used as a product fallback or hardware gate; use push_frame --ddr "
        "--yuv420p.",
    )
    push_frame = strip_comments(read(PUSH_FRAME_CPP))
    gen_edge = read(GEN_EDGE_MARKERS_PY)
    check(
        "non-YUV frame send refused" in push_frame
        and "F1 frame-store path is DDR YUV420p only" in push_frame
        and "usage: push_frame --ddr" in push_frame
        and "push_frame [--index 1]" not in push_frame,
        "push_frame must make the F1 format contract executable: default/legacy F1 sends and "
        "--rgb24 must fail with a named non-YUV refusal, not silently push RGB.",
    )
    check(
        'default="yuv420p"' in gen_edge and 'default="rgb24"' not in gen_edge,
        "gen_edge_markers.py must default to the frame-store-safe YUV420p fixture. A default "
        "RGB24 fixture is too easy to feed into the DDR push path during hardware triage.",
    )
    forbidden_repo_patterns = [
        re.compile(r"sendRgb(?:24|565)FrameDdr"),
        re.compile(r"sendRgb24Frame\(buf\.data\(\),[^\n;]*(?:/\*F1\*/\s*)?1\)"),
        re.compile(r"sendRgb565Bytes\(txFrame,[^\n;]*(?:/\*F1\*/\s*)?1\)"),
        re.compile(r"--ddr\s+--rgb24"),
        re.compile(r"push_frame\s+--ddr[^\n]*\.rgb565"),
        re.compile(r"push_frame\s+--index\s+1"),
        re.compile(r"SPI F1 fallback"),
        re.compile(r"FFmpeg RGB F1 fallback"),
        re.compile(r'default="rgb24"'),
        re.compile(r"--format\s+rgb565"),
        re.compile(r"DdrFrameFormat::Rgb565"),
    ]
    allowed_paths = {Path("tests/unit/test_rtl_invariants.py")}
    # Nested git worktrees, local agent session dirs, and pinned lab binaries are
    # not product sources; scanning them false-fails the YUV-only DDR migration gate.
    # "Memory" is the git-ignored permanent lab/evidence store (see AGENTS.md). It
    # holds historical records and verbatim quotations of past daemon output, so it
    # documents what the system DID, not what it must do. Redacting a true quotation
    # to satisfy a source invariant would falsify the record.
    skip_parts = {
        ".git",
        "build",
        "__pycache__",
        ".worktrees",
        "Memory",
        "artifacts",
        ".copilot-session",
        ".agent-work",
        ".scratch-archive",
        ".scratch-w-gate2",
        "avsync_hdmi_out",
        "captures",
        "dist",
        "output_files",
        "db",
        "incremental_db",
        # Accidental nested worktree checkouts inside the primary clone.
        "MisterPlex-wt-path-land",
        "MisterPlex-wt-main-pin",
        "MisterPlex-wt-clock-pin-035fae51",
        "MisterPlex-wt-plxd-audit",
    }
    for path in ROOT.rglob("*"):
        if not path.is_file():
            continue
        rel = path.relative_to(ROOT)
        if rel in allowed_paths or any(part in skip_parts for part in rel.parts):
            continue
        if any(part.startswith("MisterPlex-wt-") for part in rel.parts):
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for pat in forbidden_repo_patterns:
            check(
                not pat.search(text),
                f"{rel} still contains forbidden DDR RGB/YUV migration pattern {pat.pattern!r}. "
                "DDR frame-store entrypoints and docs must be YUV420p-only; RGB helpers are "
                "allowed only outside DDR contexts.",
            )
    set_status = strip_comments(read(SET_STATUS_CPP))
    input_mailbox = strip_comments(read(INPUT_MAILBOX_HPP))
    fpga_status_sources = "\n".join([fpga_h, fpga_cpp, push_frame, set_status, input_mailbox])
    check(
        "readFrameStoreStatus" in fpga_status_sources
        and "frame_debug=0x%02x" in fpga_status_sources
        and "frame_status=absent" in fpga_status_sources
        and "PLXF mailbox absent/unwritten" in fpga_status_sources
        and "frame store refused non-YUV doorbell (0xE1)" in fpga_status_sources,
        "Frame-store debug 0xE1 must be first-class in FpgaSpi status tooling. "
        "Status output must include frame_debug=0x.., name a missing/unwritten PLXF mailbox "
        "as frame_status=absent, and print the human-readable non-YUV doorbell refusal, "
        "matching the visual provenance gate fields.",
    )
    print("PASS ARM DDR writer uses product yuv420p frame-store path only")


def check_present_path_degradation_contract() -> None:
    media = strip_comments(read(MEDIA_PLAYER_CPP))
    fb_present = strip_comments(read(FB_PRESENT_CPP))
    status_sources = "\n".join(
        [
            strip_comments(read(FPGA_SPI_CPP)),
            strip_comments(read(PUSH_FRAME_CPP)),
            strip_comments(read(SET_STATUS_CPP)),
            strip_comments(read(INPUT_MAILBOX_HPP)),
        ]
    )

    media_nt = norm(media)
    fb_nt = norm(fb_present)
    status_nt = norm(status_sources)

    def present_degradation_violations(
        media_norm: str, fb_norm: str, fpga_norm: str
    ) -> list[str]:
        violations: list[str] = []
        media_joined = media_norm.replace('""', "")
        fpga_joined = fpga_norm.replace('""', "")
        forbidden = [
            (
                "useDdrF1_=false",
                "F1 DDR failures must not latch-disable future DDR attempts; keep retrying so "
                "PLXF/frame_status remains observable at the point of failure",
            ),
            (
                "if(!ok)ok=",
                "present path must not try X and quietly assign ok from an alternate sender; "
                "fallbacks must be removed or logged/statused before any alternate mechanism",
            ),
            (
                "sendRgb24Frame(buf.data(),w,h,1)",
                "idle paint must not fall back to legacy RGB/SPI F1; frame store F1 is DDR "
                "YUV420p-only",
            ),
            (
                "sendRgb24Frame(buf.data(),w,h,/*F1*/1)",
                "idle paint must not fall back to legacy RGB/SPI F1; frame store F1 is DDR "
                "YUV420p-only",
            ),
            (
                "sendRgb565Bytes(txFrame,txBytes,1)",
                "rawvideo presentation must not fall back to legacy RGB565/SPI F1 after a DDR "
                "failure",
            ),
            (
                "sendRgb565Bytes(txFrame,txBytes,/*F1*/1)",
                "rawvideo presentation must not fall back to legacy RGB565/SPI F1 after a DDR "
                "failure",
            ),
        ]
        violations.extend(msg for needle, msg in forbidden if needle in media_joined)

        required_media = [
            (
                'log("media:idlefb0blitfailed");',
                "idle fb0 blit failure must be logged; a silent idle-present failure looks like "
                "a screensaver regression",
            ),
            (
                'log("media:idlepaintDDRfailed(willretryonre-probe):"+fpga_.lastError());',
                "idle DDR failure must name DDR and the re-probe recovery path",
            ),
            (
                'log("media:reconYUV420DDRF1unavailable:"+fpga_.lastError());',
                "STREAM recon DDR failure must log the named DDR/YUV failure and keep retrying",
            ),
            (
                'log("media:non-YUVF1framerefusedbeforesend;framestorerequiresDDRYUV420p");',
                "rawvideo non-YUV F1 attempts must be refused before send with a named reason",
            ),
            (
                'log("media:DDRYUV420pF1unavailable:"+fpga_.lastError());',
                "rawvideo DDR failure must be externally visible instead of falling back to a "
                "different present path",
            ),
            (
                'log("media:fpgaframe_tx:"+fpga_.lastError());',
                "rawvideo frame_tx errors must surface the low-level frame-store status "
                "(including frame_status=absent or frame_debug=0xE1)",
            ),
            (
                'log("media:reconF1skipped:YUVDDRframe-storeexpectscoded"+',
                "geometry fallback/skip in STREAM recon must be a named skip, not a quiet "
                "absence of F1 frames",
            ),
            (
                'log("media:blitfailedfmt="+std::string(ffmpegPixFmt(videoFmt)));',
                "fb0 present failures must be logged; changing bpp/geometry must not silently "
                "erase the reference surface",
            ),
        ]
        violations.extend(msg for needle, msg in required_media if needle not in media_joined)

        for m in re.finditer(r"catch\s*\([^)]*\)\s*\{([^{}]*)\}", media_joined):
            if "log(" not in m.group(1):
                violations.append(
                    "present thread catch blocks must log before continuing or stopping; "
                    "exceptions cannot silently degrade playback"
                )

        required_fpga = [
            (
                'setErr("non-YUVframesendrefused:SPIF1RGBframepathisdisabled;useDDRYUV420p(sendYuv420pFrameDdr/push_frame--ddr--yuv420p)");',
                "low-level F1/SPI frame sends must fail at point of use with a named non-YUV "
                "refusal",
            ),
            (
                'frameStoreStatusSuffix()',
                "DDR send failures must append PLXF/frame_status details so absent/0xE1 is "
                "visible to operators",
            ),
            (
                'frame_status=absent',
                "unwritten PLXF mailbox must surface as frame_status=absent, not as a generic "
                "present failure",
            ),
            (
                'framestorestatusunavailable(PLXFmailboxabsent/unwritten)',
                "PLXF absent/unwritten must have a human-legible diagnostic string",
            ),
            (
                'framestorerefusednon-YUVdoorbell(0xE1)',
                "PLXF frame_debug 0xE1 must be named as non-YUV doorbell refusal",
            ),
        ]
        violations.extend(msg for needle, msg in required_fpga if needle not in fpga_joined)

        if "returnfalse;" not in fb_norm:
            violations.append("fb_present must return false on unsupported/failed blits")
        return violations

    missing = present_degradation_violations(media_nt, fb_nt, status_nt)
    if missing:
        fail(f"ARM present-path degradation contract: {missing[0]}")

    rgb_fallback_media = media_nt.replace(
        'log("media:idlepaintDDRfailed(willretryonre-probe):"+fpga_.lastError());',
        "ok=fpga_.sendRgb24Frame(buf.data(),w,h,1);",
    )
    if not present_degradation_violations(rgb_fallback_media, fb_nt, status_nt):
        fail("deliberately reintroduced idle RGB/SPI F1 fallback did not make the gate red")

    latched_disable_media = media_nt.replace(
        'log("media:reconYUV420DDRF1unavailable:"+fpga_.lastError());',
        'useDdrF1_=false;',
    )
    if not present_degradation_violations(latched_disable_media, fb_nt, status_nt):
        fail("deliberately reintroduced one-shot DDR disable did not make the gate red")

    silent_ddr_media = media_nt.replace(
        'log("media:DDRYUV420pF1unavailable:"+fpga_.lastError());',
        "",
    )
    if not present_degradation_violations(silent_ddr_media, fb_nt, status_nt):
        fail("deliberately silenced rawvideo DDR failure did not make the gate red")

    print("PASS ARM present path has no silent fallback/degradation without named status")



def check_ddr_bitstream_product_path() -> None:
    media = strip_comments(read(MEDIA_PLAYER_CPP))
    check(
        "beginBitstreamSession" in media
        and "pushBitstreamNal" in media
        and "endBitstreamSession" in media,
        "media_player.cpp must feed STREAM=1 through the DDR record transport "
        "with explicit begin/pushNal/end. Falling back to ioctl or byte chunks "
        "reintroduces the one-shot/mid-NAL splice path that blocks FPGA decode.",
    )
    check(
        "NalDispatcher" in media and "h264stream::IBitstreamProducer" in media,
        "media_player.cpp must keep the w-a4 NalDispatcher/IBitstreamProducer source "
        "contract as the product feed loop. Replacing it with ad-hoc direct pushes risks "
        "duplicating sequence/backpressure policy and drifting from the host source side.",
    )
    check(
        "sendBitstreamChunkDdr" not in media,
        "media_player.cpp still calls sendBitstreamChunkDdr from the product stream. "
        "That legacy shim can wrap arbitrary byte chunks as records; product playback "
        "must copy complete Annex-B NALs with sequence/session metadata.",
    )
    check(
        "frames=0 with STREAM=1" in media
        and "DDR bitstream zero/effectively-empty delivery" in media
        and "readBitstreamStatus" in media,
        "STREAM=1 zero-delivery must fail loudly with DDR ring telemetry. A frames=0 "
        "session without producer/consumer/desync/underrun counters is indistinguishable "
        "from a decoder bug.",
    )
    print("PASS product STREAM path uses DDR NAL records with zero-delivery telemetry")


def check_h264_quartus_subset() -> None:
    spec = importlib.util.spec_from_file_location("check_quartus_sv_subset", QUARTUS_SV_GUARD)
    check(spec is not None and spec.loader is not None, "Quartus SV subset guard script missing")
    guard = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(guard)
    errors = []
    for rtl in (H264_DPB_RTL, H264_DEBLOCK_RTL):
        errors.extend(guard.check_file(rtl))
    check(
        not errors,
        "H.264 RTL uses constructs Quartus rejected while Verilator accepted:\n"
        + "\n".join(f"  {err}" for err in errors),
    )
    print("PASS H.264 Quartus SV subset guard")


def main() -> int:
    check_present_core()
    check_phase_a_surface()
    check_plex_reset_domains()
    check_quartus_syntax_tripwires()
    check_async_fifo_write_full_no_comb_loop()
    check_frame_store_cdc_contract()
    check_mailboxes()
    check_mailbox_map_collisions()
    check_ddr_bitstream_ring()
    check_status_telemetry()
    check_ddr_frame_layout_contract()
    check_ddr_frame_store_yuv_read_contract()
    check_chroma_miss_black_contract()
    check_present_geometry_stride_contract()
    check_ddr_bank_handoff_contract()
    check_yuv_ddr_writer_contract()
    check_present_path_degradation_contract()
    check_ddr_bitstream_product_path()
    check_h264_quartus_subset()
    print(f"rtl_invariants: all checks passed, {len(SKIPS)} SKIP" + (f" ({', '.join(SKIPS)})" if SKIPS else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
