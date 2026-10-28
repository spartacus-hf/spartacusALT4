#!/usr/bin/env python3
"""Enforce linked-firmware call-graph and build-flavor policies."""

from __future__ import annotations

import argparse
import json
import sys

sys.dont_write_bytecode = True

from elf_analysis import call_graph, indirect_callers, reachable, shortest_path
from validation_support import ValidationError, parse_nm_symbols, policy, require_fresh_build


def check(build_dir: str) -> dict[str, object]:
    elf = require_fresh_build(build_dir)
    settings = policy(build_dir)
    realtime_usb_class = str(settings.get("realtime_usb_class", ""))
    if realtime_usb_class != "AUDIO":
        raise ValidationError(
            f"unsupported or missing real-time USB class policy: {realtime_usb_class!r}")
    graph = call_graph(elf)
    symbols = parse_nm_symbols(elf)
    reviewed_targets = {
        str(caller): [str(target) for target in targets]
        for caller, targets in settings.get("reviewed_indirect_targets", {}).items()
    }
    known_functions = set(graph) | symbols
    for caller, targets in reviewed_targets.items():
        if caller not in known_functions:
            # Optimizers may fully inline a small dispatcher (notably
            # USBD_AUDIO_Notify). Its reviewed mapping remains policy for
            # builds where it is emitted as a standalone function.
            continue
        missing_targets = [target for target in targets if target not in known_functions]
        if missing_targets:
            raise ValidationError(
                f"reviewed targets for {caller} are missing from ELF: " + ", ".join(missing_targets))
        graph.setdefault(caller, set()).update(targets)
    roots = [str(item) for item in settings["realtime_roots"]]
    forbidden = [str(item) for item in settings["forbidden_realtime_symbols"]]
    missing = [root for root in roots if root not in symbols and root not in graph]
    if missing:
        raise ValidationError("real-time policy roots missing from ELF: " + ", ".join(missing))

    violations: list[list[str]] = []
    for root in roots:
        for target in forbidden:
            path = shortest_path(graph, root, target)
            if path is not None:
                violations.append(path)
    if violations:
        rendered = "; ".join(" -> ".join(path) for path in violations[:8])
        raise ValidationError(f"forbidden real-time call path(s): {rendered}")

    # The self test and the diagnostic console are allowed to energise the
    # PIN bias and the envelope DACs, but never to key the transmitter or to
    # drive the AUX outputs that key external equipment. Prove that from the
    # linked call graph rather than from a comment in the source.
    #
    # BOARD_WriteRf and BOARD_PermitRf are deliberately NOT on the forbidden
    # list: each carries both the enable and the disable, and the diagnostic
    # paths reach them only to force RF idle. service_tx_entry is the one
    # function that actually keys the transmitter, so that is what is banned.
    #
    # Scope, stated honestly: this covers direct calls. The console's
    # `set put` reaches every panel action through the settings table's
    # function pointers by design, which is an indirect call the graph does
    # not traverse; that surface is bounded by the settings table itself,
    # which contains no transmit action.
    diagnostic_roots = [str(item) for item in settings.get("diagnostic_roots", [])]
    forbidden_diagnostic = [str(item) for item in settings.get("forbidden_diagnostic_symbols", [])]
    missing_diagnostic = [root for root in diagnostic_roots
                          if root not in symbols and root not in graph]
    if missing_diagnostic:
        raise ValidationError(
            "diagnostic policy roots missing from ELF: " + ", ".join(missing_diagnostic))
    diagnostic_violations: list[list[str]] = []
    for root in diagnostic_roots:
        for target in forbidden_diagnostic:
            if target not in symbols and target not in graph:
                continue
            path = shortest_path(graph, root, target)
            if path is not None:
                diagnostic_violations.append(path)
    if diagnostic_violations:
        rendered = "; ".join(" -> ".join(path) for path in diagnostic_violations[:8])
        raise ValidationError(f"diagnostic code reaches a transmit path: {rendered}")

    realtime_reachable: set[str] = set()
    for root in roots:
        realtime_reachable.update(reachable(graph, root))
    allowed_indirect = {str(item) for item in settings["allowed_realtime_indirect_callers"]}
    unmapped_allowed = allowed_indirect - reviewed_targets.keys()
    if unmapped_allowed:
        raise ValidationError(
            "allowed indirect callers lack reviewed target sets: " + ", ".join(sorted(unmapped_allowed)))
    unexpected_indirect = (indirect_callers(elf) & realtime_reachable) - allowed_indirect
    if unexpected_indirect:
        raise ValidationError(
            "unreviewed indirect call in real-time graph: " + ", ".join(sorted(unexpected_indirect)))

    expected_deep = bool(settings["builds"].get(build_dir, {}).get("deep_diagnostics", False))
    deep_symbols = {name for name in symbols if name.startswith("MOD_CpuDiag")}
    if expected_deep and not deep_symbols:
        raise ValidationError(f"{build_dir} is configured as diagnostic but deep-diagnostic symbols are absent")
    if not expected_deep and deep_symbols:
        raise ValidationError(f"{build_dir} unexpectedly links deep-diagnostic symbols")
    return {
        "build": build_dir,
        "functions": len(graph),
        "roots": len(roots),
        "forbidden_symbols": len(forbidden),
        "reviewed_indirect_callers": len(allowed_indirect & realtime_reachable),
        "realtime_usb_class": realtime_usb_class,
        "deep_diagnostics": expected_deep,
        "diagnostic_roots": len(diagnostic_roots),
        "forbidden_diagnostic": len(forbidden_diagnostic),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="Debug")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        result = check(args.build_dir)
    except ValidationError as exc:
        if args.json:
            print(json.dumps({"ok": False, "error": str(exc)}))
        else:
            print(f"FAIL  {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps({"ok": True, **result}, indent=2))
    else:
        print(f"PASS  {result['roots']} real-time roots cannot reach {result['forbidden_symbols']} forbidden APIs")
        print(f"PASS  reviewed indirect targets model {result['realtime_usb_class']} mode")
        print(f"PASS  {result['diagnostic_roots']} diagnostic roots cannot reach "
              f"{result['forbidden_diagnostic']} transmit APIs")
        print(f"PASS  {result['build']} deep-diagnostic flavor is {str(result['deep_diagnostics']).lower()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
