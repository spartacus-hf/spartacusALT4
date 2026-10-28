#!/usr/bin/env python3
"""Enforce linked stack-frame, call-chain, and complexity budgets."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True

from elf_analysis import call_graph, elf_blocks
from validation_support import ROOT, ValidationError, parse_nm_symbols, policy, require_fresh_build


def usage_records(build: Path) -> tuple[dict[str, int], list[str]]:
    frames: dict[str, int] = {}
    dynamic: list[str] = []
    for path in build.rglob("*.su"):
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            fields = line.split("\t")
            if len(fields) < 3:
                continue
            location, amount, qualifier = fields[0], fields[1], fields[2]
            try:
                stack = int(amount)
            except ValueError:
                continue
            name = compiler_base(location.rsplit(":", 1)[-1])
            frames[name] = max(frames.get(name, 0), stack)
            if "dynamic" in qualifier:
                dynamic.append(name)
    return frames, dynamic


def binary_stack_frames(elf: Path, frames: dict[str, int]) -> None:
    """Conservative allocation sum for library functions without GCC .su.

    Count stack reservations on all paths without subtracting restores. This
    overestimates mutually exclusive paths and includes the precompiled ST
    PD archive and libm; compiler-produced usage remains authoritative.
    """
    for name, lines in elf_blocks(elf).items():
        if compiler_base(name) in frames:
            continue
        amount = 0
        for line in lines:
            match = re.search(r"\b(?:push(?:\.w)?|stmdb\s+sp!,)\s+\{([^}]+)\}", line)
            if match:
                amount += 4 * len(match[1].split(","))
            match = re.search(r"\bsub(?:s|\.w)?\s+sp(?:,\s*sp)?,\s*#([0-9]+)", line)
            if match:
                amount += int(match[1])
            match = re.search(r"\bvpush\s+\{d([0-9]+)-d([0-9]+)\}", line)
            if match:
                amount += 8 * (int(match[2]) - int(match[1]) + 1)
            match = re.search(r"\bstr(?:\.w)?\s+\w+,\s*\[sp,\s*#-([0-9]+)\]!", line)
            if match:
                amount += int(match[1])
        frames[name] = amount


def complexity_records(build: Path) -> dict[str, int]:
    values: dict[str, int] = {}
    first_party = [build / "Core", build / "USB_Device"]
    for base in first_party:
        for path in base.rglob("*.cyclo"):
            for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                fields = line.split("\t")
                if len(fields) != 2:
                    continue
                try:
                    value = int(fields[1])
                except ValueError:
                    continue
                name = fields[0].rsplit(":", 1)[-1]
                values[name] = max(values.get(name, 0), value)
    return values


def compiler_base(name: str) -> str:
    return re.split(r"\.(?:constprop|isra|part|cold)(?:\.[0-9]+)?", name, maxsplit=1)[0]


def frame_for(name: str, frames: dict[str, int]) -> int:
    if name in frames:
        return frames[name]
    base = compiler_base(name)
    if base in frames:
        return frames[base]
    if name.startswith("__") and name.endswith("_veneer"):
        return 0
    return 0


def longest_chain(root: str, graph: dict[str, set[str]], frames: dict[str, int],
                  cycle_exemptions: set[str]) -> tuple[int, list[str]]:
    memo: dict[str, tuple[int, list[str]]] = {}

    def visit(node: str, active: tuple[str, ...]) -> tuple[int, list[str]]:
        if node in active:
            if node in cycle_exemptions:
                return 0, []
            cycle = [*active[active.index(node):], node]
            raise ValidationError(
                "recursive call cycle prevents a bounded stack proof: " + " -> ".join(cycle))
        if node in memo:
            return memo[node]
        own = frame_for(node, frames)
        best_amount = 0
        best_path: list[str] = []
        for child in graph.get(node, ()):
            amount, path = visit(child, (*active, node))
            if amount > best_amount:
                best_amount, best_path = amount, path
        result = own + best_amount, [node, *best_path]
        memo[node] = result
        return result

    return visit(root, ())


def linker_stack_reserve() -> int:
    text = (ROOT / "STM32G474VETX_FLASH.ld").read_text(encoding="utf-8", errors="replace")
    match = re.search(r"^\s*_Min_Stack_Size\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)\s*;", text, re.MULTILINE)
    if match is None:
        raise ValidationError("_Min_Stack_Size is missing from linker script")
    return int(match.group(1), 0)


def analyze(build_dir: str, top: int) -> dict[str, object]:
    elf = require_fresh_build(build_dir)
    build = ROOT / build_dir
    symbols = parse_nm_symbols(elf)
    frames, dynamic = usage_records(build)
    if not frames:
        raise ValidationError(f"no .su records under {build_dir}; enable -fstack-usage")
    linked_frames = {name: amount for name, amount in frames.items()
                     if name in symbols or any(symbol.startswith(name + ".") for symbol in symbols)}
    linked_dynamic = sorted(name for name in dynamic if name in linked_frames)
    if linked_dynamic:
        raise ValidationError("linked functions have dynamic stack usage: " + ", ".join(linked_dynamic))
    binary_stack_frames(elf, frames)
    graph = call_graph(elf)
    settings = policy(build_dir)
    for caller, targets in settings.get("reviewed_indirect_targets", {}).items():
        if caller in graph:
            graph[caller].update(targets)
    budgets = settings["budgets"]
    cycle_exemptions = {str(name) for name in settings.get("stack_cycle_exemptions", {})}
    missing_exemptions = cycle_exemptions - symbols
    if missing_exemptions:
        raise ValidationError(
            "stack cycle exemption symbols missing from ELF: " + ", ".join(sorted(missing_exemptions)))
    max_frame_name, max_frame = max(linked_frames.items(), key=lambda item: item[1])
    if max_frame > int(budgets["stack_frame_max"]):
        raise ValidationError(f"{max_frame_name} frame {max_frame} exceeds {budgets['stack_frame_max']}")

    chain_groups: dict[str, list[dict[str, object]]] = {}
    for group, roots_key, budget_key in (
        ("realtime", "realtime_stack_roots", "stack_realtime_chain_max"),
        ("foreground", "foreground_stack_roots", "stack_foreground_chain_max"),
    ):
        rows: list[dict[str, object]] = []
        for root in settings[roots_key]:
            if root not in graph and root not in symbols:
                raise ValidationError(f"stack root {root} is missing from linked ELF")
            amount, path = longest_chain(str(root), graph, frames, cycle_exemptions)
            rows.append({"root": root, "bytes": amount, "path": path})
            if amount > int(budgets[budget_key]):
                raise ValidationError(
                    f"{group} chain {root} uses {amount} bytes, exceeds {budgets[budget_key]}: "
                    + " -> ".join(path))
        chain_groups[group] = rows

    complexity = complexity_records(build)
    linked_complexity = {name: value for name, value in complexity.items()
                         if (name in symbols or any(symbol.startswith(name + ".") for symbol in symbols))
                         and not any(name.startswith(prefix)
                                     for prefix in settings["complexity_exempt_prefixes"])}
    if not linked_complexity:
        raise ValidationError(f"no linked .cyclo records under {build_dir}; enable -fcyclomatic-complexity")
    complex_name, complex_value = max(linked_complexity.items(), key=lambda item: item[1])
    default_complexity = int(budgets["cyclomatic_function_max"])
    baselines = {str(name): int(value) for name, value in settings["complexity_baselines"].items()}
    regressions = []
    for name, value in linked_complexity.items():
        allowed = baselines.get(name, default_complexity)
        if value > allowed:
            regressions.append(f"{name} {value}>{allowed}")
    if regressions:
        raise ValidationError("cyclomatic complexity regression(s): " + ", ".join(sorted(regressions)))
    reserve = linker_stack_reserve()
    if reserve < int(budgets["stack_linker_min"]):
        raise ValidationError(
            f"linker stack reserve {reserve} is below nesting policy {budgets['stack_linker_min']}")
    nesting = max(row["bytes"] for row in chain_groups["foreground"])
    for roots in settings.get("stack_interrupt_priority_groups", []):
        level = max(longest_chain(root, graph, frames, cycle_exemptions)[0] for root in roots)
        nesting += level + 104  # core + lazy FPU exception frame, conservatively always present
    if nesting > reserve:
        raise ValidationError(f"nested foreground/IRQ bound {nesting} exceeds reserved stack {reserve}")
    largest = sorted(linked_frames.items(), key=lambda item: item[1], reverse=True)[:top]
    return {
        "build": build_dir,
        "linked_frame_records": len(linked_frames),
        "max_frame": {"name": max_frame_name, "bytes": max_frame},
        "chains": chain_groups,
        "max_complexity": {"name": complex_name, "value": complex_value},
        "linker_stack_reserve": reserve,
        "nested_stack_bound": nesting,
        "stack_cycle_exemptions": sorted(cycle_exemptions),
        "largest_frames": [{"name": name, "bytes": amount} for name, amount in largest],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="Debug")
    parser.add_argument("--top", type=int, default=15)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        result = analyze(args.build_dir, args.top)
    except ValidationError as exc:
        if args.json:
            print(json.dumps({"ok": False, "error": str(exc)}))
        else:
            print(f"FAIL  {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps({"ok": True, **result}, indent=2))
    else:
        maximum = result["max_frame"]
        print(f"Max linked frame: {maximum['bytes']} bytes in {maximum['name']}")
        for group, rows in result["chains"].items():
            worst = max(rows, key=lambda row: row["bytes"])
            print(f"{group.capitalize()} worst chain: {worst['bytes']} bytes from {worst['root']}")
            print("  " + " -> ".join(worst["path"]))
        complexity = result["max_complexity"]
        print(f"Max linked complexity: {complexity['value']} in {complexity['name']}")
        print(f"Nested foreground/IRQ bound: {result['nested_stack_bound']} bytes")
        print(f"PASS  stack frames/chains fit policy and linker reserves {result['linker_stack_reserve']} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
