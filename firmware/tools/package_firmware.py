#!/usr/bin/env python3
"""Build REV1FW.UPD and a ROM-DFU factory image from the current ELF."""

from __future__ import annotations

import argparse
import re
import struct
import tempfile
import zlib
from pathlib import Path

from validation_support import ROOT, read_version, require_fresh_build, run, stm32_tool

FORMAT = ROOT / "Core" / "Inc" / "fw_update_format.h"


def flashable_elf(binary: Path, base: int, entry: int) -> Path:
    """Wrap an exact flash image in one loadable ELF segment for CubeIDE."""
    result = binary.with_suffix(".elf")
    with tempfile.TemporaryDirectory(prefix="rev1-elf-") as scratch:
        obj = Path(scratch) / "image.o"
        check = Path(scratch) / "image.bin"
        run([stm32_tool("objcopy"), "-I", "binary", "-O", "elf32-littlearm", "-B", "arm",
             "--rename-section", ".data=.firmware,alloc,load,readonly,code,contents", binary, obj])
        run([stm32_tool("ld"), f"--section-start=.firmware=0x{base:08X}",
             "-e", f"0x{entry:08X}", obj, "-o", result])
        run([stm32_tool("objcopy"), "-O", "binary", result, check])
        if check.read_bytes() != binary.read_bytes():
            raise ValueError(f"flashable ELF differs from {binary.name}")
    data = result.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        raise ValueError(f"{result.name} is not a 32-bit ELF")
    elf_type, _, _, elf_entry, phoff = struct.unpack_from("<HHIII", data, 16)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    if elf_type != 2 or elf_entry != entry or phnum != 1 or phentsize < 32:
        raise ValueError(f"{result.name} has invalid ELF entry or program headers")
    segment = struct.unpack_from("<8I", data, phoff)
    ptype, offset, vaddr, paddr, filesz, memsz, flags, _ = segment
    if (ptype != 1 or vaddr != base or paddr != base or
            filesz != binary.stat().st_size or memsz != filesz or
            offset + filesz > len(data) or (flags & 1) == 0):
        raise ValueError(f"{result.name} does not load the exact flash range")
    return result


def constant(name: str) -> int:
    source = FORMAT.read_text(encoding="utf-8")
    match = re.search(rf"^#define\s+{name}\s+(0x[0-9A-Fa-f]+|[0-9]+)(?:U|UL)?\b", source, re.MULTILINE)
    if match is None:
        raise ValueError(f"missing numeric layout constant {name}")
    return int(match.group(1), 0)


IMAGE_NAMES = ("REV1FW.UPD", "rev1_factory.bin", "rev1_recovery.bin",
               "rev1_factory.elf", "rev1_recovery.elf", "HELP.BIN", "CWPACK.BIN")


def invalidate_images(output: Path) -> Path:
    # CubeIDE runs its pre/post-build steps from Debug/, but run() uses ROOT.
    # Resolve before passing file paths to either process. Pre-build removal
    # also covers a compiler/linker failure that never reaches post-build.
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for name in IMAGE_NAMES:
        (output / name).unlink(missing_ok=True)
    return output


def make_images(build: str, output: Path) -> tuple[Path, Path, Path]:
    output = invalidate_images(output)
    elf = require_fresh_build(build)
    run(["make", "-C", ROOT / "Bootloader", "all"])
    loader_file = ROOT / "Bootloader" / "bootloader.bin"
    if not loader_file.is_file() or loader_file.stat().st_mtime_ns < max(
        (ROOT / "Bootloader" / "bootloader.c").stat().st_mtime_ns,
        (ROOT / "Core" / "Inc" / "fw_update_format.h").stat().st_mtime_ns,
    ):
        raise ValueError("build Bootloader before packaging")

    loader_base = constant("FW_LOADER_BASE")
    loader_bytes = constant("FW_LOADER_BYTES")
    app_base = constant("FW_APP_BASE")
    footer_addr = constant("FW_FOOTER_ADDR")
    journal_addr = constant("FW_JOURNAL_ADDR")
    flash_end = constant("FW_FLASH_END")
    layout_abi = constant("FW_LAYOUT_ABI")
    board = constant("FW_BOARD_ID")
    abi = constant("FW_LOADER_ABI")
    loader = loader_file.read_bytes()
    if len(loader) != loader_bytes or app_base != loader_base + loader_bytes:
        raise ValueError("loader binary does not match flash layout")
    if abi < layout_abi:
        raise ValueError("loader ABI predates this flash layout")
    info = struct.unpack_from("<5I", loader, loader_bytes - 32)
    if info[:4] != (constant("FW_LOADER_INFO_MAGIC"), abi, app_base, loader_bytes) or \
            zlib.crc32(loader[-32:-16]) != info[4]:
        raise ValueError("loader ABI record is invalid")
    with tempfile.TemporaryDirectory(prefix="rev1-pack-") as scratch:
        binary = Path(scratch) / "app.bin"
        run([stm32_tool("objcopy"), "-O", "binary", elf, binary])
        app = binary.read_bytes()
    if not 512 <= len(app) <= footer_addr - app_base:
        raise ValueError("application exceeds its reserved flash region")
    stack, reset = struct.unpack_from("<II", app)
    if not (0x20000000 <= stack <= 0x20018000 and stack % 8 == 0 and
            reset & 1 and app_base <= (reset & ~1) < app_base + len(app)):
        raise ValueError("application vectors do not point into the relocated image")
    version, major, minor = read_version()
    if version.encode() not in app:
        raise ValueError("ELF does not contain the current VERSION; rebuild it")
    firmware_version = major * 100 + minor
    app_crc = zlib.crc32(app)
    header = bytearray(512)
    struct.pack_into("<9I", header, 0, constant("FW_PACKAGE_MAGIC"),
                     constant("FW_FORMAT_VERSION"), board, abi, app_base,
                     len(app), firmware_version, app_crc, 0)
    struct.pack_into("<I", header, 32, zlib.crc32(header))
    package = bytes(header) + app + bytes((-len(app)) % 512)
    if len(package) // 512 > constant("FW_MAP_ENTRIES"):
        raise ValueError("package is too large for the recovery map")

    footer_prefix = struct.pack("<7I", constant("FW_FOOTER_MAGIC"), board,
                                app_base, len(app), firmware_version, app_crc, abi)
    footer = footer_prefix + struct.pack("<I", zlib.crc32(footer_prefix))
    if len(footer) != journal_addr - footer_addr:
        raise ValueError("footer layout mismatch")
    # Layout 2: the image covers the whole flash, the journal (last page)
    # erased. Nothing past it any more: settings and calibration are on the
    # SD card since v8.09.
    factory = (loader + app + b"\xFF" * (footer_addr - app_base - len(app)) + footer +
               b"\xFF" * (flash_end - journal_addr))
    if len(factory) != flash_end - loader_base:
        raise ValueError("factory image does not cover the flash exactly")
    if factory[journal_addr - loader_base:] != b"\xFF" * (flash_end - journal_addr):
        raise ValueError("factory journal page must be erased")

    loader_reset = struct.unpack_from("<I", loader, 4)[0]
    with tempfile.TemporaryDirectory(prefix="rev1-publish-", dir=output) as scratch:
        stage = Path(scratch)
        (stage / "REV1FW.UPD").write_bytes(package)
        # Since v8.08 the settings help lives on the SD card (tools/gen_help_text.py):
        # it goes onto the card beside REV1FW.UPD, or through rev1_diag.py helpinstall.
        (stage / "HELP.BIN").write_bytes((ROOT / "sd" / "HELP.BIN").read_bytes())
        # The CW trainer's pack likewise (tools/gen_cw_pack.py).
        (stage / "CWPACK.BIN").write_bytes((ROOT / "sd" / "CWPACK.BIN").read_bytes())
        factory_file = stage / "rev1_factory.bin"
        recovery_file = stage / "rev1_recovery.bin"
        factory_file.write_bytes(factory)
        recovery_file.write_bytes(factory[loader_bytes:])
        flashable_elf(factory_file, loader_base, loader_reset)
        # The recovery ELF leaves protected loader pages untouched, but its
        # entry still starts the existing loader. Debugging must exercise reset.
        flashable_elf(recovery_file, app_base, loader_reset)
        for name in IMAGE_NAMES:
            (stage / name).replace(output / name)
    update_file = output / "REV1FW.UPD"
    factory_file = output / "rev1_factory.bin"
    recovery_file = output / "rev1_recovery.bin"
    factory_elf = output / "rev1_factory.elf"
    recovery_elf = output / "rev1_recovery.elf"
    print(f"{version}: app {len(app)} bytes, CRC32 {app_crc:08X}; loader {len(loader)} bytes")
    print(f"SD package: {update_file} ({len(package)} bytes), {output / 'HELP.BIN'} and "
          f"{output / 'CWPACK.BIN'} (copy all three to the card)")
    print(f"ROM DFU image: {factory_file} ({len(factory)} bytes, loader ABI {abi}, the whole flash)")
    print(f"ROM DFU recovery: {recovery_file} ({len(factory) - loader_bytes} bytes, starts at 0x{app_base:08X})")
    print(f"CubeIDE flash images: {factory_elf}, {recovery_elf}")
    return update_file, factory_file, recovery_file


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="Debug", choices=("Debug", "Release-Speed", "Diagnostic"))
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--invalidate-only", action="store_true")
    args = parser.parse_args()
    try:
        if args.invalidate_only:
            invalidate_images(args.output_dir)
        else:
            make_images(args.build_dir, args.output_dir)
    except (OSError, ValueError, RuntimeError) as exc:
        parser.exit(1, f"FAIL  {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
