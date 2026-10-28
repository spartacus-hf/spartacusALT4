#!/usr/bin/env python3
"""Read-only gate for the protected REV1 first-stage recovery loader."""

from __future__ import annotations

import argparse
import json
import struct
import sys
import xml.etree.ElementTree as ET
import zlib

sys.dont_write_bytecode = True

from package_firmware import constant
from validation_support import ROOT, ValidationError, run, stm32_tool


def check_cubeide_flash_paths() -> None:
    project = ET.parse(ROOT / ".cproject").getroot()
    debug = [item for item in project.iter("configuration")
             if item.get("name") == "Debug" and item.get("postbuildStep")]
    if len(debug) != 1 or not all(part in debug[0].get("postbuildStep", "") for part in
                                  ("package_firmware.py", "dist/debug")):
        raise ValidationError("CubeIDE Debug build does not package the loader and image footer")
    if not all(part in debug[0].get("prebuildStep", "") for part in
               ("package_firmware.py", "--invalidate-only", "dist/debug")):
        raise ValidationError("CubeIDE Debug build can flash stale images after a compile failure")
    key = "com.st.stm32cube.ide.mcu.debug.launch.loadList"
    for launch, image in (("alt2 Debug.launch", "rev1_recovery.elf"),
                          ("alt2 Factory Debug.launch", "rev1_factory.elf")):
        config = ET.parse(ROOT / launch).getroot()
        attributes = [item.get("value") for item in config.iter("stringAttribute")
                      if item.get("key") == key]
        if len(attributes) != 1:
            raise ValidationError(f"{launch} has no unique image load list")
        items = json.loads(attributes[0])["fItems"]
        if (len(items) != 2 or items[0].get("fPath") != "Debug/alt2.elf" or
                items[0].get("fDownload") is not False or
                items[0].get("fLoadSymbols") is not True or
                items[1].get("fPath") != f"dist/debug/{image}" or
                items[1].get("fDownload") is not True or
                items[1].get("fLoadSymbols") is not False):
            raise ValidationError(f"{launch} could launch an app without its reset image")


def check() -> dict[str, int]:
    check_cubeide_flash_paths()
    loader_dir = ROOT / "Bootloader"
    binary = loader_dir / "bootloader.bin"
    elf = loader_dir / "bootloader.elf"
    inputs = [loader_dir / name for name in ("bootloader.c", "startup.s", "bootloader.ld", "Makefile")]
    inputs.append(ROOT / "Core/Inc/fw_update_format.h")
    if not binary.is_file() or not elf.is_file():
        raise ValidationError("build the first stage with make -C Bootloader all")
    newest_input = max(path.stat().st_mtime_ns for path in inputs)
    if min(binary.stat().st_mtime_ns, elf.stat().st_mtime_ns) < newest_input:
        raise ValidationError("loader binary/ELF is stale; rebuild Bootloader")
    data = binary.read_bytes()
    base = constant("FW_LOADER_BASE")
    size = constant("FW_LOADER_BYTES")
    app_base = constant("FW_APP_BASE")
    if len(data) != size or app_base != base + size:
        raise ValidationError("loader binary does not occupy exactly the protected pages")
    sp, reset = struct.unpack_from("<II", data)
    if sp != 0x20018000 or not (reset & 1 and base <= (reset & ~1) < app_base):
        raise ValidationError("loader reset vectors are invalid")
    info = struct.unpack_from("<5I", data, size - 32)
    if info[:4] != (constant("FW_LOADER_INFO_MAGIC"), constant("FW_LOADER_ABI"), app_base, size):
        raise ValidationError("loader ABI marker disagrees with application layout")
    if zlib.crc32(data[-32:-16]) != info[4]:
        raise ValidationError("loader ABI marker CRC is wrong")
    if run([stm32_tool("nm"), "-u", elf]).stdout.strip():
        raise ValidationError("first stage has unresolved library dependencies")
    return {"bytes": size, "reset": reset, "abi": info[1]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    try:
        result = check()
    except (OSError, ValidationError) as exc:
        print(f"FAIL  {exc}", file=sys.stderr)
        return 1
    print(f"PASS  {result['bytes']} byte independent first stage, ABI {result['abi']}, reset 0x{result['reset']:08X}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
