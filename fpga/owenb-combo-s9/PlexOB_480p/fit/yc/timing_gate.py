#!/usr/bin/env python3
"""Read a Quartus Multicorner Timing Analysis Summary and apply the Y/C bar.

Bar (HDMI-EXP): pll_hdmi setup >= -0.3 ns, and every other clock's setup,
hold, recovery and removal >= 0. N/A cells are paths that do not exist.
Minimum pulse width is not part of this bar.

Prints RESULT.env lines on stdout. Exit 0 when the bar holds, 2 when it
does not (FIT MARGINAL), 1 when the summary cannot be read.

`--map` is the Quartus map report (a sibling H264.fit.rpt is included when
it sits next to that file). The Analysis & Synthesis or Fitter "Resource
Utilization by Entity" table must contain a `ring_in:ring_in_inst` row
whose ALM count is greater than zero. A mention only under Registers
Removed, or a row with zero ALMs, prints MAP_RING_IN=0 and does not print
TIMING_PASS=1. GATE_PASS=1 only when the timing bar and that row both hold.
A map miss exits 1, so the fit does not report FIT done.
"""
from __future__ import annotations

import argparse
import re
import sys
from decimal import Decimal
from pathlib import Path

PLL_BAR = Decimal("-0.3")
CHECKS = ("setup", "hold", "recovery", "removal")


def cells(line: str) -> list[str] | None:
    if ";" not in line:
        return None
    parts = [p.strip() for p in line.split(";")]
    if parts and parts[0] == "":
        parts = parts[1:]
    if parts and parts[-1] == "":
        parts = parts[:-1]
    return parts or None


def parse_num(token: str) -> Decimal | None:
    if token.upper() in {"N/A", "NA", "-", "--"}:
        return None
    try:
        return Decimal(token)
    except Exception:
        return None


_ENTITY_TITLE = re.compile(r"resource utilization by entity|entity resource utilization", re.I)
_REMOVED_TITLE = re.compile(r"registers removed", re.I)
_ALM_COL = re.compile(r"\bALMs?\b", re.I)
_RING_NODE = "ring_in:ring_in_inst"


def _section_title(raw: str) -> str | None:
    row = cells(raw)
    if row is None:
        text = raw.strip()
        if text and not text.startswith("+") and ";" not in raw:
            return text
        return None
    if len(row) == 1 and row[0]:
        return row[0]
    return None


def _alm_index(row: list[str]) -> int | None:
    for index, cell in enumerate(row):
        if _ALM_COL.search(cell):
            return index
    return None


def _alm_value(token: str) -> Decimal | None:
    match = re.search(r"-?\d{1,3}(?:,\d{3})+(?:\.\d+)?|-?\d+(?:\.\d+)?", token)
    if not match:
        return None
    return Decimal(match.group(0).replace(",", ""))


def entity_ring_in_alive(text: str) -> bool:
    """True when an Entity utilization row names ring_in with a non-zero ALM count.

    Rows under Registers Removed do not count, and neither does a zero-ALM row.
    """
    in_entity = False
    alm_idx: int | None = None
    width = 0
    for raw in text.splitlines():
        title = _section_title(raw)
        if title is not None:
            in_entity = _ENTITY_TITLE.search(title) is not None
            alm_idx = None
            width = 0
            continue
        if not in_entity:
            continue
        row = cells(raw)
        if not row:
            continue
        if any(_REMOVED_TITLE.search(cell) for cell in row):
            in_entity = False
            alm_idx = None
            width = 0
            continue
        found = _alm_index(row)
        if found is not None and not any(_RING_NODE in cell for cell in row):
            alm_idx = found
            width = len(row)
            continue
        if alm_idx is None or len(row) != width or alm_idx >= len(row):
            continue
        if not any(_RING_NODE in cell for cell in row):
            continue
        value = _alm_value(row[alm_idx])
        if value is not None and value > 0:
            return True
    return False


def parse_summary(text: str) -> tuple[dict[str, list[Decimal | None]], dict[str, list[Decimal | None]]]:
    """Return (worst-case slack, design-wide TNS) keyed by clock name."""
    slack: dict[str, list[Decimal | None]] = {}
    tns: dict[str, list[Decimal | None]] = {}
    mode: str | None = None
    in_summary = False
    for raw in text.splitlines():
        if "Multicorner Timing Analysis Summary" in raw and raw.strip().startswith(";"):
            in_summary = True
            mode = None
            continue
        if not in_summary:
            continue
        if raw.strip().startswith("+") and mode == "tns" and tns:
            break
        row = cells(raw)
        if not row:
            continue
        head = row[0].lower()
        if head == "clock":
            continue
        if head == "worst-case slack":
            mode = "slack"
            continue
        if head == "design-wide tns":
            mode = "tns"
            continue
        if mode is None:
            continue
        nums = [parse_num(tok) for tok in row[1:5]]
        while len(nums) < 4:
            nums.append(None)
        (slack if mode == "slack" else tns)[row[0]] = nums[:4]
    if not slack:
        raise ValueError("Multicorner Timing Analysis Summary has no clock rows")
    return slack, tns


def worst(rows: dict[str, list[Decimal | None]], index: int) -> tuple[Decimal | None, str]:
    found: Decimal | None = None
    name = ""
    for clock, nums in rows.items():
        value = nums[index]
        if value is None:
            continue
        if found is None or value < found:
            found = value
            name = clock
    return found, name


def classify(slack: dict[str, list[Decimal | None]]) -> tuple[dict[str, list[Decimal | None]], dict[str, list[Decimal | None]]]:
    pll = {k: v for k, v in slack.items() if "pll_hdmi" in k.lower()}
    other = {k: v for k, v in slack.items() if "pll_hdmi" not in k.lower()}
    if not pll:
        raise ValueError("no pll_hdmi clock in the multicorner summary")
    return pll, other


def hdmi_pll_from_log(text: str) -> str:
    match = re.search(r"hdmi_pll_check:\s*(PASS|FAIL|WARNING)", text, re.I)
    if not match:
        return "absent"
    return match.group(1).upper()


def env_lines(values: dict[str, str]) -> str:
    keys = (
        "PLL_HDMI_SETUP",
        "PLL_HDMI_HOLD",
        "PLL_HDMI_RECOVERY",
        "PLL_HDMI_REMOVAL",
        "PLL_HDMI_TNS",
        "OTHER_SETUP",
        "OTHER_HOLD",
        "OTHER_RECOVERY",
        "OTHER_REMOVAL",
        "OTHER_SETUP_CLOCK",
        "OTHER_HOLD_CLOCK",
        "HDMI_PLL_CHECK",
        "RBF_MARGINAL",
        "TIMING_PASS",
    )
    return "\n".join(f"{k}={values[k]}" for k in keys if k in values)


def evaluate(sta_text: str, log_text: str = "", rbf_path: str = "") -> tuple[dict[str, str], bool]:
    slack, tns = parse_summary(sta_text)
    pll, other = classify(slack)
    values: dict[str, str] = {}
    reasons: list[str] = []
    pll_setup, _ = worst(pll, 0)
    if pll_setup is None:
        raise ValueError("pll_hdmi setup slack is N/A")
    for index, name in enumerate(CHECKS):
        value, _ = worst(pll, index)
        values[f"PLL_HDMI_{name.upper()}"] = "N/A" if value is None else format(value, "f")
    pll_tns_rows = {k: v for k, v in tns.items() if "pll_hdmi" in k.lower()}
    tns_setup, _ = worst(pll_tns_rows, 0) if pll_tns_rows else (None, "")
    values["PLL_HDMI_TNS"] = "N/A" if tns_setup is None else format(tns_setup, "f")
    if pll_setup < PLL_BAR:
        reasons.append(f"pll_hdmi setup {pll_setup} < {PLL_BAR}")
    for index, name in enumerate(CHECKS):
        value, clock = worst(other, index)
        key = f"OTHER_{name.upper()}"
        values[key] = "N/A" if value is None else format(value, "f")
        if name in ("setup", "hold"):
            values[f"{key}_CLOCK"] = clock
        if value is not None and value < 0:
            reasons.append(f"{clock} {name} {value} < 0")
    hdmi = hdmi_pll_from_log(log_text) if log_text else "absent"
    values["HDMI_PLL_CHECK"] = hdmi
    marginal_name = "_MARGINAL" in Path(rbf_path).name
    values["RBF_MARGINAL"] = "1" if marginal_name else "0"
    if log_text and hdmi != "PASS":
        reasons.append(f"hdmi_pll_check {hdmi}")
    if marginal_name:
        reasons.append("RBF name contains _MARGINAL")
    ok = not reasons
    values["TIMING_PASS"] = "1" if ok else "0"
    if reasons:
        values["TIMING_WHY"] = "; ".join(reasons)
    return values, ok


def emit(values: dict[str, str], timing_ok: bool, map_state: bool | None) -> int:
    """Print RESULT.env lines. map_state is None when --map was not passed."""
    if map_state is False and values.get("TIMING_PASS") == "1":
        del values["TIMING_PASS"]
    print(env_lines(values))
    if "TIMING_WHY" in values:
        print(f"TIMING_WHY={values['TIMING_WHY']}")
    if map_state is not None:
        print(f"MAP_RING_IN={'1' if map_state else '0'}")
    gate_ok = timing_ok and map_state is not False
    print(f"GATE_PASS={'1' if gate_ok else '0'}")
    if map_state is False:
        print(
            "timing_gate: entity resource table has no ring_in:ring_in_inst row with non-zero ALMs",
            file=sys.stderr,
        )
        return 1
    return 0 if timing_ok else 2


def map_report_text(path: Path) -> str:
    text = path.read_text(errors="replace")
    if ".map.rpt" not in path.name:
        return text
    fit = path.with_name(path.name.replace(".map.rpt", ".fit.rpt"))
    if fit.is_file():
        text += "\n" + fit.read_text(errors="replace")
    return text


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sta", required=True)
    parser.add_argument("--log", default="")
    parser.add_argument("--rbf", default="")
    parser.add_argument("--map", default="")
    args = parser.parse_args()
    try:
        sta = Path(args.sta).read_text(errors="replace")
        log = Path(args.log).read_text(errors="replace") if args.log else ""
        values, ok = evaluate(sta, log, args.rbf)
    except (OSError, ValueError) as exc:
        print("TIMING_PARSE=0", file=sys.stdout)
        print("GATE_PASS=0", file=sys.stdout)
        print(f"timing_gate: {exc}", file=sys.stderr)
        return 1
    if not args.map:
        return emit(values, ok, None)
    try:
        map_text = map_report_text(Path(args.map))
    except OSError as exc:
        if values.get("TIMING_PASS") == "1":
            del values["TIMING_PASS"]
        print(env_lines(values))
        if "TIMING_WHY" in values:
            print(f"TIMING_WHY={values['TIMING_WHY']}")
        print("MAP_RING_IN=0")
        print("GATE_PASS=0")
        print(f"timing_gate: {exc}", file=sys.stderr)
        return 1
    return emit(values, ok, entity_ring_in_alive(map_text))


if __name__ == "__main__":
    sys.exit(main())
