#!/usr/bin/env python3
"""Check every fpga_build_ids.hpp row's PLX page class in the tree its lineage cites.

For each recognised row: take the commits its lineage names (hex tokens that resolve in
git and whose Plex_<raster>.qsf carries the row's FPGA_VIDEO_BUILD_ID), read that qsf's
uncommented VERILOG_MACROs (plus QIP/TCL it names and Plex.sv `defines), preprocess
present_core.sv, evaluate every fstore mailbox parameter (override, else ddr_frame_store
module default) and compare the page with the row's PlxPageClass (plx_page.hpp).
Lab tool (needs the git history of each cited commit); run it before adding a row.
Usage: scripts/verify_plx_page_rows.py [REPO]   (default: this checkout)
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tests", "unit"))
import test_rtl_invariants as inv  # noqa: E402  (sv_preprocess / sv_eval / sv_param_env)

FP = "fpga/Plex_MiSTer"
OFFS = {p: off for _n, p, _s, off, *_ in inv.PLX_PAGE_MAILBOXES}
CLASS_PAGE = {"Present480pOrMetaDpb": 0x300FF000, "Present480i": 0x301FF000,
              "Present720pL4": 0x3047F000, "NoPresentMacro": 0x3007F000}


def git_show(commit, path):
    r = subprocess.run(["git", "-C", REPO, "show", f"{commit}:{path}"], capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def rows():
    text = inv.strip_comments(open(os.path.join(REPO, "host/libmisterplex/fpga_build_ids.hpp")).read())
    alias = dict(re.findall(r"inline constexpr PlxPageClass (\w+) = PlxPageClass::(\w+);", text))
    body = text[text.index("kRecognisedFpgaBuilds[] = {"): text.index("};", text.index("kRecognisedFpgaBuilds[] = {"))]
    for m in re.finditer(r'\{(0x[0-9A-Fa-f]+)u,\s*"(\w+)",\s*(?:PlxPageClass::)?(\w+),(.*?)\}(?=,\s*(?:\{|$))', body, re.S):
        cls = alias.get(m.group(3), m.group(3))
        lineage = " ".join(re.findall(r'"([^"]*)"', m.group(4)))
        yield int(m.group(1), 16), m.group(2), cls, lineage


def qsf_macros(commit, qsf):
    macros, todo, seen = {}, [qsf], set()
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        text = git_show(commit, f"{FP}/{f}") or ""
        for ln in text.splitlines():
            s = ln.strip()
            if s.startswith("#"):
                continue
            m = re.match(r'set_global_assignment\s+-name\s+VERILOG_MACRO\s+"([^"=]+)(?:=([^"]*))?"', s)
            if m:
                macros[m.group(1)] = m.group(2) if m.group(2) is not None else "1"
            m = re.match(r'set_global_assignment\s+-name\s+(?:QIP_FILE|SOURCE_TCL_SCRIPT_FILE)\s+"?([^"\s]+)', s)
            if m:
                todo.append(m.group(1))
    return macros


def page_in_tree(commit, macros):
    defs = set(macros)
    plex = inv.sv_preprocess(inv.strip_comments(git_show(commit, f"{FP}/Plex.sv") or ""), defs)
    defs |= set(re.findall(r"`define\s+(\w+)", plex))
    env = inv.sv_param_env(inv.sv_preprocess(inv.strip_comments(
        git_show(commit, f"{FP}/rtl/ddr_frame_layout_params.svh") or ""), defs), {})
    pc = inv.sv_preprocess(inv.strip_comments(git_show(commit, f"{FP}/rtl/present_core.sv") or ""), defs)
    i = pc.find("ddr_frame_store #(")
    inv.sv_param_env(pc[:i], env)
    block = pc[i: pc.find(") fstore", i)]
    ov = {m.group(1): m.group(2) for m in re.finditer(r"\.(\w+)\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)", block)}
    store = inv.sv_preprocess(inv.strip_comments(git_show(commit, f"{FP}/rtl/ddr_frame_store.sv") or ""), defs)
    inst = dict(env)
    for k, e in ov.items():
        try:
            inst[k] = inv.sv_eval(e, env)
        except (KeyError, SyntaxError, ValueError, TypeError, NameError):
            pass
    for m in re.finditer(r"parameter\s+(?:int\s+|bit\s+)?(?:\[[^\]]+\]\s*)?(\w+)\s*=\s*([^,\n]+)", store[: store.find(");")]):
        if m.group(1) not in ov:
            try:
                inst[m.group(1)] = inv.sv_eval(m.group(2).strip().rstrip(","), inst)
            except (KeyError, SyntaxError, ValueError, TypeError, NameError):
                pass
    page = inst["MAILBOX_PHYS"] - OFFS["MAILBOX_PHYS"]
    ok = all(inst.get(p) == page + off for p, off in OFFS.items())
    return page, ok


def main():
    bad = 0
    n = 0
    for rid, raster, cls, lineage in rows():
        n += 1
        trees = []
        for tok in dict.fromkeys(re.findall(r"\b[0-9a-f]{7,40}\b", lineage)):
            mac = qsf_macros(tok, f"Plex_{raster}.qsf") if git_show(tok, f"{FP}/Plex_{raster}.qsf") else {}
            bid = mac.get("FPGA_VIDEO_BUILD_ID", "")
            if bid and int(bid.split("'h")[-1].replace("_", ""), 16) == rid:
                trees.append((tok, mac))
        if not trees:
            print(f"0x{rid:08X} {raster} {cls}: NO TREE (no lineage commit has this ID in Plex_{raster}.qsf)")
            bad += 1
            continue
        for tok, mac in trees:
            page, ok = page_in_tree(tok, mac)
            sel = ",".join(sorted(k for k in mac if k in ("PLEX_PRESENT_480P", "PLEX_PRESENT_480I",
                                                         "PLEX_PRESENT_720P_L4", "PLEX_META_DPB")))
            good = ok and page == CLASS_PAGE[cls]
            bad += not good
            print(f"0x{rid:08X} {raster} @{tok}: macros={sel or 'none'} page=0x{page:08X} "
                  f"offsets_ok={ok} row={cls} {'OK' if good else 'MISMATCH'}")
    print(f"PLX_PAGE_ROWS rows={n} bad={bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
