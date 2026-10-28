#!/usr/bin/env python3
"""Report actual linked memory use and enforce centralized budgets."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True

from validation_support import ROOT, ValidationError, policy, require_fresh_build, run, stm32_tool

LINKER = ROOT / "STM32G474VETX_FLASH.ld"


def numeric_length(value: str) -> int:
    match = re.fullmatch(r"\s*([0-9]+)\s*[Kk]\s*", value)
    if match is not None:
        return int(match.group(1)) * 1024
    return int(value.strip(), 0)


def memory_regions() -> dict[str, tuple[int, int]]:
    text = LINKER.read_text(encoding="utf-8", errors="replace")
    regions: dict[str, tuple[int, int]] = {}
    pattern = re.compile(
        r"^\s*([A-Z0-9_]+)\s+\([^)]*\)\s*:\s*ORIGIN\s*=\s*([^,]+),"
        r"\s*LENGTH\s*=\s*([^\n]+)", re.MULTILINE)
    for name, origin, length in pattern.findall(text):
        regions[name] = (int(origin.strip(), 0), numeric_length(length))
    return regions


def map_sections(map_path: Path) -> list[tuple[str, int, int]]:
    sections: list[tuple[str, int, int]] = []
    pattern = re.compile(r"^(\.[A-Za-z0-9_.$]+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)")
    for line in map_path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = pattern.match(line)
        if match is not None:
            sections.append((match.group(1), int(match.group(2), 16), int(match.group(3), 16)))
    return sections


def region_high_water(sections: list[tuple[str, int, int]], origin: int, length: int,
                      *, exclude: set[str] | None = None) -> int:
    end = origin
    limit = origin + length
    for name, address, size in sections:
        if name in (exclude or set()) or size == 0:
            continue
        if origin <= address < limit and address + size <= limit:
            end = max(end, address + size)
    return end - origin


def flash_programmed_bytes(elf: Path, origin: int, length: int) -> tuple[int, list[str]]:
    readelf = stm32_tool("readelf")
    output = run([readelf, "-lW", elf]).stdout
    end = origin
    bad_rwe: list[str] = []
    pattern = re.compile(
        r"^\s*LOAD\s+0x[0-9a-fA-F]+\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)"
        r"\s+0x([0-9a-fA-F]+)\s+0x[0-9a-fA-F]+\s+(.+?)\s+0x[0-9a-fA-F]+\s*$")
    for line in output.splitlines():
        match = pattern.match(line)
        if match is None:
            continue
        physical, file_size, flags = int(match.group(1), 16), int(match.group(2), 16), match.group(3)
        if "RWE" in flags.replace(" ", ""):
            bad_rwe.append(line.strip())
        if file_size and origin <= physical < origin + length:
            end = max(end, physical + file_size)
    return end - origin, bad_rwe


def largest_symbols(elf: Path, origin: int, length: int, limit: int = 10) -> list[dict[str, object]]:
    nm = stm32_tool("nm")
    output = run([nm, "-S", "--size-sort", "--defined-only", elf]).stdout
    symbols: list[dict[str, object]] = []
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 4:
            continue
        try:
            address, size = int(fields[0], 16), int(fields[1], 16)
        except ValueError:
            continue
        if size and origin <= address < origin + length:
            symbols.append({"name": fields[-1], "bytes": size})
    return list(reversed(symbols[-limit:]))


def analyze(build_dir: str) -> dict[str, object]:
    elf = require_fresh_build(build_dir)
    map_path = ROOT / build_dir / "alt2.map"
    regions = memory_regions()
    required = {"FLASH_LOADER", "FLASH_APP", "IMAGE_FOOTER", "UPDATE_JOURNAL",
                "RAM", "CCMRAM"}
    missing = required - regions.keys()
    if missing:
        raise ValidationError("linker regions missing: " + ", ".join(sorted(missing)))
    flash_origin, flash_size = regions["FLASH_APP"]
    loader_origin, loader_size = regions["FLASH_LOADER"]
    footer_origin, footer_size = regions["IMAGE_FOOTER"]
    journal_origin, journal_size = regions["UPDATE_JOURNAL"]
    ram_origin, ram_size = regions["RAM"]
    ccm_origin, ccm_size = regions["CCMRAM"]
    # Layout 2 (v8.09): loader, application, footer, journal -- the journal
    # the last page of the 512 KiB; no settings or calibration in flash.
    if (loader_origin != 0x08000000 or loader_size != 4096 or
            flash_origin != loader_origin + loader_size or
            footer_origin != flash_origin + flash_size or footer_size != 32 or
            journal_origin != footer_origin + footer_size or journal_size != 2048 or
            journal_origin + journal_size != 0x08080000):
        raise ValidationError("loader/application/footer/journal flash layout is inconsistent")

    sections = map_sections(map_path)
    flash_used, rwe_segments = flash_programmed_bytes(elf, flash_origin, flash_size)
    if rwe_segments:
        raise ValidationError("writable/executable LOAD segment: " + "; ".join(rwe_segments))
    # Heap/stack reservation is a linker assertion inside the static high-water
    # mark. Exclude it here so RAM headroom describes the actual space available
    # for heap + all foreground/interrupt stacks.
    ram_static = region_high_water(sections, ram_origin, ram_size,
                                   exclude={"._user_heap_stack"})
    ccm_used = region_high_water(sections, ccm_origin, ccm_size)
    values = {
        "flash": {"used": flash_used, "capacity": flash_size, "free": flash_size - flash_used},
        "ram": {"used": ram_static, "capacity": ram_size, "free": ram_size - ram_static},
        "ccmram": {"used": ccm_used, "capacity": ccm_size, "free": ccm_size - ccm_used},
    }
    budgets = policy()["budgets"]
    if flash_used > int(budgets["flash_app_max_used"]):
        raise ValidationError(f"programmed flash {flash_used} exceeds {budgets['flash_app_max_used']}")
    if ram_size - ram_static < int(budgets["ram_min_unallocated"]):
        raise ValidationError(f"RAM headroom {ram_size - ram_static} is below {budgets['ram_min_unallocated']}")
    if ccm_size - ccm_used < int(budgets["ccmram_min_unallocated"]):
        raise ValidationError(f"CCMRAM headroom {ccm_size - ccm_used} is below {budgets['ccmram_min_unallocated']}")
    return {
        "build": build_dir,
        "regions": values,
        "largest_ram_symbols": largest_symbols(elf, ram_origin, ram_size),
        "largest_ccmram_symbols": largest_symbols(elf, ccm_origin, ccm_size),
        "rwe_segments": 0,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="Debug")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        result = analyze(args.build_dir)
    except ValidationError as exc:
        if args.json:
            print(json.dumps({"ok": False, "error": str(exc)}))
        else:
            print(f"FAIL  {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps({"ok": True, **result}, indent=2))
    else:
        for label in ("flash", "ram", "ccmram"):
            row = result["regions"][label]
            print(f"{label.upper():7s} {row['used']:7d} / {row['capacity']:7d} bytes; headroom {row['free']:7d}")
        print("PASS  linked image fits flash/RAM/CCMRAM policy and has no RWE LOAD segment")
        print("Largest RAM symbols:")
        for row in result["largest_ram_symbols"]:
            print(f"  {row['bytes']:6d}  {row['name']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
