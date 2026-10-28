#!/usr/bin/env python3
"""Read-only verification of all firmware-version representations."""

from __future__ import annotations

import argparse
import json
import re
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

from validation_support import REPO, ROOT, ValidationError, host_cc, read_version, run

HEADER = ROOT / "Core" / "Inc" / "modulator.h"
README = ROOT / "README.md"
REPO_README = REPO / "README.md"
def macro_integer(text: str, name: str) -> int:
    match = re.search(rf"^\s*#define\s+{re.escape(name)}\s+([0-9]+)\s*$", text, re.MULTILINE)
    if match is None:
        raise ValidationError(f"{name} must be a plain decimal macro in Core/Inc/modulator.h")
    return int(match.group(1))


def compiled_header_version() -> dict[str, str | int]:
    """Ask a C compiler to expand every firmware-facing version value."""
    source = r'''
#include "modulator.h"
#include <stdio.h>
int main(void)
{
    printf("%s\n%u\n%c.%c%c\n", FW_VERSION_STRING, (unsigned)FW_VERSION_BCD,
           (char)FW_VERSION_INQUIRY_MAJOR, (char)FW_VERSION_INQUIRY_MINOR_TENS,
           (char)FW_VERSION_INQUIRY_MINOR_ONES);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="alt2-version-") as temporary:
        temp = Path(temporary)
        probe = temp / "probe.c"
        binary = temp / "probe"
        probe.write_text(source, encoding="utf-8")
        run([host_cc(), "-std=c11", "-fshort-enums", "-Werror",
             "-I", ROOT / "Core" / "Inc", probe, "-o", binary])
        lines = run([binary]).stdout.splitlines()
        if len(lines) != 3:
            raise ValidationError("cannot parse compiled firmware version probe")
        return {"string": lines[0], "bcd": int(lines[1]), "inquiry": lines[2]}


def current_lines(path: Path) -> list[str]:
    if not path.is_file():
        return []
    return [line.strip() for line in path.read_text(encoding="utf-8", errors="replace").splitlines()
            if "current firmware" in line.lower()]


def collect_failures(build_dir: str | None = None) -> tuple[str, list[str], dict[str, object]]:
    version, major, minor = read_version()
    failures: list[str] = []
    details: dict[str, object] = {"version": version, "major": major, "minor": minor}
    header = HEADER.read_text(encoding="utf-8", errors="replace")
    header_major = macro_integer(header, "FW_VERSION_MAJOR")
    header_minor = macro_integer(header, "FW_VERSION_MINOR")
    details["header_components"] = [header_major, header_minor]
    if major > 9 or minor > 99:
        failures.append("firmware version exceeds the one-digit major/two-digit minor USB inquiry format")
    if header_major != major or header_minor != minor:
        failures.append(f"firmware macros expand from v{header_major}.{header_minor}, expected {version}")
    expanded = compiled_header_version()
    details["compiled"] = expanded
    expected_bcd = ((major // 10) << 12) | ((major % 10) << 8) | \
                   ((minor // 10) << 4) | (minor % 10)
    expected_inquiry = f"{major}.{minor:02d}"
    if expanded["string"] != version:
        failures.append(
            f"compiler expands FW_VERSION_STRING to {expanded['string']!r}, expected {version!r}")
    if expanded["bcd"] != expected_bcd:
        failures.append(
            f"compiler expands FW_VERSION_BCD to {expanded['bcd']:#06x}, expected {expected_bcd:#06x}")
    if expanded["inquiry"] != expected_inquiry:
        failures.append(
            f"compiler expands MSC revision to {expanded['inquiry']!r}, expected {expected_inquiry!r}")

    expected_lines = {
        README: re.compile(rf"^Current firmware:\s*`{re.escape(version)}`\.$"),
        REPO_README: re.compile(rf"^The current firmware version is\s*`{re.escape(version)}`\.$"),
    }
    for path, expected in expected_lines.items():
        lines = current_lines(path)
        if len(lines) != 1 or expected.fullmatch(lines[0]) is None:
            failures.append(f"{path.relative_to(REPO)} has no unique current-version line for {version}")

    inquiry = (ROOT / "Core" / "Src" / "usbd_msc_if.c").read_text(encoding="utf-8")
    for macro in ("FW_VERSION_INQUIRY_MAJOR", "FW_VERSION_INQUIRY_MINOR_TENS",
                  "FW_VERSION_INQUIRY_MINOR_ONES"):
        if inquiry.count(macro) != 1:
            failures.append(f"MSC inquiry revision does not use {macro} exactly once")
    for descriptor in ("usbd_desc.c", "usbd_msc_desc.c"):
        text = (ROOT / "USB_Device" / "App" / descriptor).read_text(encoding="utf-8")
        if text.count("FW_VERSION_BCD") != 2:
            failures.append(f"USB_Device/App/{descriptor} does not derive both bcdDevice bytes")

    if build_dir is not None:
        from validation_support import read_elf_symbol_bytes, require_fresh_build, stm32_tool

        elf = require_fresh_build(build_dir)
        strings = stm32_tool("strings")
        embedded_versions = sorted({
            line for line in run([strings, elf]).stdout.splitlines()
            if re.fullmatch(r"v[0-9]+\.[0-9]+", line)
        })
        expected_descriptor_revision = expected_bcd.to_bytes(2, "little")
        descriptor_revisions: dict[str, str] = {}
        for symbol in ("USBD_AUDIO_DeviceDesc", "USBD_MSC_DeviceDesc"):
            descriptor = read_elf_symbol_bytes(elf, symbol)
            if len(descriptor) != 18:
                failures.append(f"{symbol} is {len(descriptor)} bytes, expected USB device length 18")
                continue
            actual = descriptor[12:14]
            descriptor_revisions[symbol] = actual.hex()
            if actual != expected_descriptor_revision:
                failures.append(
                    f"{symbol} embeds bcdDevice 0x{int.from_bytes(actual, 'little'):04x}, "
                    f"expected 0x{expected_bcd:04x}")
        inquiry_data = read_elf_symbol_bytes(elf, "STORAGE_Inquirydata_FS")
        embedded_inquiry = inquiry_data[32:36].decode("ascii", errors="replace")
        if len(inquiry_data) != 36:
            failures.append(f"MSC inquiry data is {len(inquiry_data)} bytes, expected 36")
        elif embedded_inquiry != expected_inquiry:
            failures.append(
                f"MSC inquiry ELF revision is {embedded_inquiry!r}, expected {expected_inquiry!r}")
        details["elf"] = str(elf.relative_to(ROOT))
        details["elf_versions"] = embedded_versions
        details["elf_descriptor_revisions"] = descriptor_revisions
        details["elf_inquiry_revision"] = embedded_inquiry
        if embedded_versions != [version]:
            failures.append(
                f"{elf.relative_to(ROOT)} embeds version strings {embedded_versions}, expected only {version}")
    return version, failures, details


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command")
    check = subparsers.add_parser("check", help="consistency and compiler-expansion check")
    check.add_argument("--build-dir", help="also verify a fresh ELF embeds the current version")
    check.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        version, failures, details = collect_failures(getattr(args, "build_dir", None))
    except (OSError, ValidationError) as exc:
        print(f"FAIL  {exc}", file=sys.stderr)
        return 1
    if getattr(args, "json", False):
        print(json.dumps({"ok": not failures, "failures": failures, **details}, indent=2))
    elif failures:
        for failure in failures:
            print(f"FAIL  {failure}")
    else:
        print(f"PASS  VERSION, compiled firmware string, USB revisions, and docs agree on {version}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
