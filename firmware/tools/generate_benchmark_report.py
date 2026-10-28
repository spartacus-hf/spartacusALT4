#!/usr/bin/env python3
"""Print reproducible build metrics; never writes a report into the repository."""

from __future__ import annotations

import argparse
import json
import sys

sys.dont_write_bytecode = True

from validation_support import ROOT, ValidationError, read_version, require_fresh_build, run, stm32_tool

COUNTERS = [
    ("USB submit", "MOD_CPU_DIAG_USB_SUBMIT"),
    ("External render", "MOD_CPU_DIAG_RENDER_EXT"),
    ("DMA refill", "MOD_CPU_DIAG_DMA_POP"),
    ("LCD service", "MOD_CPU_DIAG_LCD_SERVICE"),
    ("UI build", "MOD_CPU_DIAG_UI_BUILD"),
    ("Storage save", "MOD_CPU_DIAG_STORAGE_SAVE"),
    ("Carrier retune", "MOD_CPU_DIAG_RETUNE"),
]


def build_size(build: str) -> dict[str, int | str]:
    elf = require_fresh_build(build)
    output = run([stm32_tool("size"), elf]).stdout.splitlines()
    if len(output) < 2:
        raise ValidationError(f"cannot parse size output for {build}")
    fields = output[-1].split()
    if len(fields) < 4:
        raise ValidationError(f"cannot parse size row for {build}")
    return {"build": build, "text": int(fields[0]), "data": int(fields[1]),
            "bss": int(fields[2]), "total": int(fields[3])}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", action="append", dest="builds",
                        help="fresh build to include; repeatable (default: Debug)")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    builds = args.builds or ["Debug"]
    try:
        version, _, _ = read_version()
        sizes = [build_size(build) for build in builds]
    except ValidationError as exc:
        print(f"FAIL  {exc}", file=sys.stderr)
        return 1
    report = {"version": version, "builds": sizes,
              "hardware_counters": [{"path": label, "counter": counter}
                                    for label, counter in COUNTERS]}
    if args.json:
        print(json.dumps(report, indent=2))
        return 0
    print(f"# {version} reproducible build metrics\n")
    print("| Build | text | data | bss | total |")
    print("|---|---:|---:|---:|---:|")
    for row in sizes:
        print(f"| {row['build']} | {row['text']} | {row['data']} | {row['bss']} | {row['total']} |")
    print("\nOn-device timing is intentionally not fabricated by a host script. Enable the Diagnostic build, "
          "exercise the path, and record these DWT counters:\n")
    for item in report["hardware_counters"]:
        print(f"- {item['path']}: `{item['counter']}`")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
