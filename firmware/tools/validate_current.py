#!/usr/bin/env python3
"""Stable firmware validation entry point.

The firmware ELFs a tier checks are built first when missing or stale
(Debug before the two flavors derived from its make files); the checks
themselves are read-only, proven by the tracked and source-tree snapshots
taken after the builds. --no-build fails on a stale ELF instead."""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.dont_write_bytecode = True

from validation_support import (
    ROOT,
    ValidationError,
    assert_snapshot_unchanged,
    assert_source_tree_unchanged,
    build_artifacts,
    run,
    source_tree_snapshot,
    tracked_snapshot,
)


def commands(mode: str) -> list[tuple[str, list[str]]]:
    python = sys.executable
    base = [
        ("version", [python, "-B", "tools/version_tool.py", "check"]),
        ("source policy", [python, "-B", "tools/source_policy_check.py"]),
        ("strict target compile", [python, "-B", "tools/strict_compile_check.py"]),
        ("validation parser tests", [python, "-B", "tools/tests/validation_logic_test.py"]),
        ("analytic design oracle", [python, "-B", "tools/analytic_design_oracle.py"]),
        ("production host tests", [python, "-B", "tools/host_tests.py"]),
        ("storage ABI", [python, "-B", "tools/storage_layout_report.py"]),
        ("factory defaults", [python, "-B", "tools/defaults_consistency_check.py"]),
        ("console pin table", [python, "-B", "tools/pin_table_check.py"]),
        ("UI font table", [python, "-B", "tools/gen_ui_fonts.py", "--check"]),
        ("RX decimation table", [python, "-B", "tools/gen_rx_coeff.py", "--check"]),
        ("settings help text", [python, "-B", "tools/gen_help_text.py", "--check"]),
        ("CW trainer pack", [python, "-B", "tools/gen_cw_pack.py", "--check"]),
    ]
    if mode == "quick":
        return base
    base.append(("protected loader", [python, "-B", "tools/bootloader_policy_check.py"]))
    builds = ["Debug"] if mode == "default" else ["Debug", "Release-Speed", "Diagnostic"]
    for build in builds:
        base.extend([
            (f"{build} embedded version", [python, "-B", "tools/version_tool.py", "check", "--build-dir", build]),
            (f"{build} memory", [python, "-B", "tools/memory_budget_check.py", "--build-dir", build]),
            (f"{build} stack/complexity", [python, "-B", "tools/stack_usage_report.py", "--build-dir", build]),
            (f"{build} real-time ELF policy", [python, "-B", "tools/elf_policy_check.py", "--build-dir", build]),
        ])
    return base


BUILDERS = {
    "Debug": ["make", "-C", "Debug", f"-j{os.cpu_count() or 4}", "all"],
    "Release-Speed": ["bash", "tools/build_release_speed.sh"],
    "Diagnostic": ["bash", "tools/build_diagnostic.sh"],
}
DERIVED_MARKERS = {"Release-Speed": ".generated_release_speed", "Diagnostic": ".generated_diagnostic"}
DEBUG_METADATA = ("makefile", "objects.mk", "sources.mk", "objects.list", "subdir.mk")


def needs_build(build: str) -> str | None:
    """Why `build` must be built before validation, or None when it is fresh."""
    elf, map_path = build_artifacts(build)
    if not elf.is_file() or not map_path.is_file():
        return "missing"
    if run(["make", "-q", "-C", build, "alt2.elf"], check=False).returncode != 0:
        return "stale"
    marker = DERIVED_MARKERS.get(build)
    if marker:
        # A derived flavor copies Debug's make files when it is built.
        built = (ROOT / build / marker).stat().st_mtime
        if any(path.stat().st_mtime > built for path in (ROOT / "Debug").rglob("*")
               if path.is_file() and path.name in DEBUG_METADATA):
            return "older than Debug's make files"
    return None


def build_stale(builds: list[str], quiet: bool) -> list[dict[str, object]]:
    built: list[dict[str, object]] = []
    for build in builds:
        why = needs_build(build)
        if why is None:
            continue
        if not quiet:
            print(f"BUILD {build} ({why})", flush=True)
        started = time.monotonic()
        result = run(BUILDERS[build], check=False)
        row = {"name": build, "why": why, "ok": result.returncode == 0,
               "seconds": round(time.monotonic() - started, 3)}
        built.append(row)
        if result.returncode != 0:
            tail = "\n".join((result.stdout + result.stderr).strip().splitlines()[-40:])
            raise ValidationError(f"building {build} failed ({result.returncode}):\n{tail}")
    return built


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--quick", action="store_true", help="compiler-backed host checks; no firmware ELF required")
    modes.add_argument("--release", action="store_true", help="validate fresh Debug, Release-Speed, and Diagnostic ELFs")
    parser.add_argument("--no-build", action="store_true", help="fail on a missing or stale ELF instead of building it")
    parser.add_argument("--json", action="store_true", help="emit one machine-readable result")
    args = parser.parse_args()
    mode = "quick" if args.quick else "release" if args.release else "default"
    builds = [] if mode == "quick" or args.no_build else ["Debug"] if mode == "default" else list(BUILDERS)
    results: list[dict[str, object]] = []
    built: list[dict[str, object]] = []
    failure: str | None = None
    started_all = time.monotonic()
    try:
        built = build_stale(builds, args.json)
    except ValidationError as exc:
        failure = str(exc)
    tracked_before = tracked_snapshot()
    source_before = source_tree_snapshot()
    try:
        for name, command in ([] if failure else commands(mode)):
            started = time.monotonic()
            result = run(command, check=False)
            elapsed = time.monotonic() - started
            row = {
                "name": name,
                "ok": result.returncode == 0,
                "seconds": round(elapsed, 3),
                "output": (result.stdout + result.stderr).strip(),
            }
            results.append(row)
            if result.returncode != 0:
                failure = f"{name} failed: {row['output']}"
                break
    except ValidationError as exc:
        failure = str(exc)
    try:
        assert_snapshot_unchanged(tracked_before)
        assert_source_tree_unchanged(source_before)
    except ValidationError as exc:
        failure = f"{failure}; {exc}" if failure else str(exc)
    elapsed_all = round(time.monotonic() - started_all, 3)
    ok = failure is None
    if args.json:
        print(json.dumps({"ok": ok, "mode": mode, "seconds": elapsed_all,
                          "error": failure, "built": built, "steps": results}, indent=2))
    else:
        for row in results:
            prefix = "PASS" if row["ok"] else "FAIL"
            print(f"{prefix}  {row['name']} ({row['seconds']:.3f}s)")
            if row["output"]:
                for line in str(row["output"]).splitlines():
                    print(f"      {line}")
        if ok:
            note = f" after building {', '.join(str(b['name']) for b in built)}" if built else ""
            print(f"PASS  {mode} validation completed read-only{note} in {elapsed_all:.3f}s")
        else:
            print(f"FAIL  {failure}", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
