#!/usr/bin/env python3
"""Transitive RTL reachability from the design top through the Quartus file list.

WHY THIS EXISTS
---------------
The parent orchestrator produced THREE successive wrong answers to the question
"is the fabric H.264 decoder connected?", each with an ad-hoc grep:

  1. Searched for a module whose name equals its FILENAME. No module in this
     codebase is named after its file, so it could only ever return NOBODY.
  2. Re-ran with real module names but with the regex
         ^\\s*MODULE\\s*(#|\\()
     which only matches `foo #(` and `foo (`. It MISSES the ordinary
         h264_idct4x4 u_h264_idct4x4 (
     form, i.e. the most common way to instantiate anything. That silently
     reported 11 live modules in decode_stub.sv as dead.
  3. Even corrected, a flat "who mentions X" grep is not reachability: a module
     instantiated only by an orphan root is still dead.

The fix for a bad probe is a better probe, so this walks the graph properly:
parse the .qip file list, index module declarations, extract instantiations, then
BFS from the top module. Anything not reached is dead regardless of who mentions it.

LIMITATIONS (stated, not hidden):
  - Does not evaluate `generate`/`ifdef` or QSF macros such as PRODUCT_NO_STUB, so
    a module inside a disabled branch is reported reachable here but may not
    survive synthesis. Post-fit hierarchy is the authority for what actually built.
  - Text-based, not a real elaborator.
  - Source-graph LIVE != modules present in a deployed RBF (bitstream may predate
    sources; nostub/PRODUCT_NO_STUB can drop the whole decode_stub branch).
  - Edges are taken from module..endmodule bodies only (not whole multi-module files).
  - Positive/negative controls must pass or the tool exits 3 (no LIVE/DEAD table).
"""
import re
import sys
from pathlib import Path

QIP_FILE_RE = re.compile(r'set_global_assignment\s+-name\s+(?:SYSTEMVERILOG|VERILOG)_FILE\s+(\S+)', re.I)
MODULE_DECL_RE = re.compile(r'^\s*module\s+([A-Za-z_][A-Za-z0-9_$]*)', re.M)
LINE_COMMENT_RE = re.compile(r'//[^\n]*')
BLOCK_COMMENT_RE = re.compile(r'/\*.*?\*/', re.S)

# An instantiation is: MODULE_NAME [#(params)] INSTANCE_NAME (
# The instance name is mandatory in Verilog for module instances, but the
# parameter form can push it past a #(...) block, so allow both shapes.
def instantiations(text, known_modules):
    """An instantiation is a line that BEGINS with a known module name, followed by
    either a parameter block (`#`) or an instance identifier.

    Deliberately does NOT try to regex-match the parameter list. A `#( ... )` block
    contains nested parens (`.WIDTH(FRAME_W)`), which a non-greedy `\\([^;]*?\\)`
    terminates at the first inner `)`. That bug made this tool report reachable=1,
    caught only because `decode_stub` was a known-positive control.

    A module DECLARATION cannot false-positive here: those lines begin with the
    keyword `module`, not with the module's own name.
    """
    found = set()
    for m in known_modules:
        pat = re.compile(r'^[ \t]*' + re.escape(m) + r'(?=[ \t]*(?:#|[A-Za-z_]))', re.M)
        if pat.search(text):
            found.add(m)
    return found


def strip_comments(t):
    return LINE_COMMENT_RE.sub('', BLOCK_COMMENT_RE.sub('', t))


def module_bodies(text):
    """Map module_name -> body text between module and matching endmodule.

    Multi-module files must NOT share one file-wide instantiation set: each
    module only owns instantiations that appear in its own body. (rd-duck NACK
    on file-union edges: leaf h264_dpb_i420_addr was falsely given siblings'
    children.)
    """
    bodies = {}
    # Iterate declarations in order; body runs until the next endmodule at depth 1
    decl_iter = list(MODULE_DECL_RE.finditer(text))
    for i, m in enumerate(decl_iter):
        name = m.group(1)
        start = m.end()
        # find endmodule after start; handle nested module rarely — scan linear
        end_m = re.search(r'\bendmodule\b', text[start:])
        if not end_m:
            bodies[name] = text[start:]
            continue
        end = start + end_m.start()
        # If another module decl appears before this endmodule, clamp (nested rare)
        if i + 1 < len(decl_iter) and decl_iter[i + 1].start() < end:
            end = decl_iter[i + 1].start()
        bodies[name] = text[start:end]
    return bodies


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else '.')
    top = sys.argv[2] if len(sys.argv) > 2 else 'Plex'
    qip = root / 'files.qip'
    if not qip.exists():
        print(f'FATAL: no files.qip at {qip}')
        return 2

    qip_files = []
    for line in qip.read_text().splitlines():
        mm = QIP_FILE_RE.search(line)
        if mm:
            qip_files.append(mm.group(1))

    # Index every module declaration across the whole tree, and note which are in the qip.
    decl_file = {}
    text_of = {}
    all_sv = sorted(list(root.rglob('*.sv')) + list(root.rglob('*.v')))
    for f in all_sv:
        try:
            raw = strip_comments(f.read_text(errors='ignore'))
        except OSError:
            continue
        text_of[f] = raw
        for name in MODULE_DECL_RE.findall(raw):
            decl_file.setdefault(name, f)

    known = set(decl_file)
    in_qip = set()
    for rel in qip_files:
        p = (root / rel).resolve()
        for name, f in decl_file.items():
            if f.resolve() == p:
                in_qip.add(name)
    # the top file itself is compiled via the .qsf, not the .qip
    in_qip.add(top)

    # Build the instantiation graph from PER-MODULE bodies (not whole-file text).
    bodies_of = {}
    for f, raw in text_of.items():
        for name, body in module_bodies(raw).items():
            bodies_of[name] = body

    edges = {}
    for name in known:
        edges[name] = instantiations(bodies_of.get(name, ''), known)

    # ---- Positive / negative controls (fail closed if the instrument is broken) ----
    control_failures = []

    def require(cond, msg):
        if not cond:
            control_failures.append(msg)

    # Negative: pure address helper must not own child module instances.
    if 'h264_dpb_i420_addr' in edges:
        require(
            len(edges['h264_dpb_i420_addr']) == 0,
            f"NEG h264_dpb_i420_addr children={sorted(edges['h264_dpb_i420_addr'])} "
            f"(expected []); file-union edge bug?",
        )

    # Positive: decode_stub body must list known backend leaves (source intent).
    if 'decode_stub' in edges:
        stub_need = {
            'h264_dequant4x4', 'h264_idct4x4', 'h264_recon4x4', 'h264_dpb_one_ref',
        }
        missing = sorted(stub_need - edges['decode_stub'])
        require(not missing, f"POS decode_stub missing direct children {missing}")

    # Positive: emu (Plex.sv) must instantiate stream_path when present.
    if 'emu' in edges:
        require('stream_path' in edges['emu'], f"POS emu children missing stream_path: {sorted(edges['emu'])}")

    # Positive: stream_path must instantiate decode_stub and ddr_bitstream_reader.
    if 'stream_path' in edges:
        for need in ('decode_stub', 'ddr_bitstream_reader'):
            require(need in edges['stream_path'],
                    f"POS stream_path missing {need}: {sorted(edges['stream_path'])}")

    if control_failures:
        print('REACHABILITY_CONTROLS_FAILED:')
        for msg in control_failures:
            print(f'  - {msg}')
        print('Instrument untrusted; refusing LIVE/DEAD table (rd-duck NACK class).')
        return 3

    print('REACHABILITY_CONTROLS_OK: body-sliced edges; leaf-neg + stub/emu/stream pos')

    # BFS from top, but only traverse INTO modules that Quartus actually compiles.
    seen, queue = set(), [top]
    while queue:
        cur = queue.pop()
        if cur in seen:
            continue
        seen.add(cur)
        for child in sorted(edges.get(cur, ())):
            if child in in_qip and child not in seen:
                queue.append(child)

    h264 = sorted(n for n in known if n.startswith('h264_'))
    live = [n for n in h264 if n in seen]
    dead = [n for n in h264 if n not in seen]

    print(f'top={top}  modules_declared={len(known)}  in_qip={len(in_qip)}  reachable={len(seen)}')
    print(f'--- h264_* REACHABLE FROM TOP (source graph; NOT post-fit): {len(live)}')
    for n in live:
        print(f'    LIVE {n:34s} ({decl_file[n].name})')
    print(f'--- h264_* NOT REACHABLE: {len(dead)}')
    for n in dead:
        why = 'not in files.qip' if n not in in_qip else 'in qip but never instantiated on a path from top'
        print(f'    DEAD {n:34s} ({decl_file[n].name}) [{why}]')
    print('CAVEAT: ignores generate/ifdef/PRODUCT_NO_STUB; post-fit hierarchy is authority.')
    print('CAVEAT: source reachability != deployed RBF contents / area paid.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
