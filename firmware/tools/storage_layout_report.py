#!/usr/bin/env python3
"""Compile the persisted ABI, compare its layout manifest, and report capacity.

Since v8.09 settings and calibration are records in the SD card's store
(mod_sd_store.c); the probe checks they still fit its slots."""

from __future__ import annotations

import argparse
import json
import re
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

from validation_support import ROOT, ValidationError, policy, run, stm32_tool


def compile_probe() -> dict[str, int]:
    keys = [
        "settings_version", "settings_blob_size", "settings_record_size",
        "SETTINGS_V28_LENGTH", "SETTINGS_V27_LENGTH", "SETTINGS_V26_LENGTH", "SETTINGS_V20_LENGTH",
        "cal_record_size", "cal_keys", "store_slot0_max", "store_slot_max", "store_slots",
        "store_first_lba", "store_end_lba",
    ]
    source = r'''
#include "mod_storage_layout_priv.h"
#include "mod_sd_store.h"
#include <stdint.h>
const uint32_t alt2_storage_abi_probe[] = {
    SETTINGS_VERSION,
    sizeof(settings_blob_t),
    sizeof(settings_record_t),
    SETTINGS_V28_LENGTH,
    SETTINGS_V27_LENGTH,
    SETTINGS_V26_LENGTH,
    SETTINGS_V20_LENGTH,
    sizeof(ssb_cal_record_t),
    BAND_COUNT * 6U,
    SD_STORE_SLOT0_MAX,
    SD_STORE_SLOT_MAX,
    SD_STORE_SLOTS,
    SD_SYSTEM_FIRST_LBA,
    SD_SYSTEM_END_LBA
};
'''
    with tempfile.TemporaryDirectory(prefix="alt2-storage-abi-") as temporary:
        temp = Path(temporary)
        probe = temp / "probe.c"
        assembly = temp / "probe.s"
        probe.write_text(source, encoding="utf-8")
        run([
            stm32_tool("gcc"), "-std=c11", "-fshort-enums", "-O0", "-Wall", "-Wextra", "-Werror",
            "-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard",
            "-DSTM32G474xx", "-DUSE_HAL_DRIVER",
            "-I", ROOT / "Core" / "Inc",
            "-I", ROOT / "Drivers" / "STM32G4xx_HAL_Driver" / "Inc",
            "-I", ROOT / "Drivers" / "CMSIS" / "Device" / "ST" / "STM32G4xx" / "Include",
            "-I", ROOT / "Drivers" / "CMSIS" / "Include",
            "-S", probe, "-o", assembly,
        ])
        values: list[int] = []
        in_probe = False
        for line in assembly.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.strip() == "alt2_storage_abi_probe:":
                in_probe = True
                continue
            if in_probe and line.lstrip().startswith(".word"):
                values.append(int(line.split(None, 1)[1], 0))
            elif in_probe and values and not line.startswith((" ", "\t")):
                break
        if len(values) != len(keys):
            raise ValidationError(
                f"cannot parse target ABI probe: expected {len(keys)} values, found {len(values)}")
        return dict(zip(keys, values, strict=True))


def linker_nvm_regions() -> dict[str, dict[str, int]]:
    """Persisted regions left in the MCU's flash: none since v8.09."""
    text = (ROOT / "STM32G474VETX_FLASH.ld").read_text(encoding="utf-8", errors="replace")
    regions: dict[str, dict[str, int]] = {}
    for name, origin, value, suffix in re.findall(
        r"^\s*(NVM_[A-Z0-9_]+)\s+\([^)]*\)\s*:\s*ORIGIN\s*=\s*(0x[0-9a-fA-F]+),"
        r"\s*LENGTH\s*=\s*([0-9]+)\s*([Kk]?)", text, re.MULTILINE):
        length = int(value) * (1024 if suffix else 1)
        regions[name] = {"origin": int(origin, 0), "bytes": length}
    return regions


def analyze() -> dict[str, object]:
    actual = compile_probe()
    manifest = policy()["storage_abi"]
    expected = {
        "settings_version": int(manifest["settings_version"]),
        "settings_blob_size": int(manifest["settings_blob_size"]),
        "settings_record_size": int(manifest["settings_record_size"]),
        **{name: int(value) for name, value in manifest["migration_offsets"].items()},
    }
    differences = [f"{name}: actual {actual.get(name)}, manifest {value}"
                   for name, value in expected.items() if actual.get(name) != value]
    if differences:
        raise ValidationError(
            "persisted ABI changed; add an explicit migration and update validation_policy.json: "
            + "; ".join(differences))
    if actual["settings_record_size"] > actual["store_slot0_max"]:
        raise ValidationError("the settings record no longer fits the store's settings slot")
    if actual["cal_record_size"] > actual["store_slot_max"]:
        raise ValidationError("a calibration record no longer fits a store slot")
    if 1 + actual["cal_keys"] > actual["store_slots"]:
        raise ValidationError("the store has fewer slots than calibration keys")
    if actual["store_end_lba"] > 2048:
        raise ValidationError("the store's system area reaches the in-radio formatter's volume (LBA 2048)")
    regions = linker_nvm_regions()
    if regions:
        raise ValidationError("the linker reserves flash for settings or calibration again: "
                              + ", ".join(sorted(regions)))
    return {"abi": actual, "manifest": expected, "regions": regions}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        result = analyze()
    except ValidationError as exc:
        if args.json:
            print(json.dumps({"ok": False, "error": str(exc)}))
        else:
            print(f"FAIL  {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps({"ok": True, **result}, indent=2))
    else:
        abi = result["abi"]
        print(f"settings v{abi['settings_version']}: blob {abi['settings_blob_size']} bytes, "
              f"record {abi['settings_record_size']} of {abi['store_slot0_max']} bytes in store slot 0")
        print(f"calibration: {abi['cal_keys']} keys of {abi['cal_record_size']} bytes in "
              f"{abi['store_slots'] - 1} store slots, LBA {abi['store_first_lba']}-{abi['store_end_lba'] - 1}")
        print("PASS  compiled persisted ABI matches the migration manifest and the SD store's capacity")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
