#!/usr/bin/env python3
"""Type-check safety-critical first-party firmware with strict target warnings."""

from __future__ import annotations

import argparse
import json
import sys

sys.dont_write_bytecode = True

from validation_support import ROOT, ValidationError, policy, run, stm32_tool


def command_prefix() -> list[object]:
    include = [
        ("-I", ROOT / "Core" / "Inc"),
        ("-isystem", ROOT / "Drivers" / "STM32G4xx_HAL_Driver" / "Inc"),
        ("-isystem", ROOT / "Drivers" / "STM32G4xx_HAL_Driver" / "Inc" / "Legacy"),
        ("-isystem", ROOT / "Drivers" / "CMSIS" / "Device" / "ST" / "STM32G4xx" / "Include"),
        ("-isystem", ROOT / "Drivers" / "CMSIS" / "Include"),
        ("-isystem", ROOT / "USB_Device" / "App"),
        ("-isystem", ROOT / "USB_Device" / "Target"),
        ("-isystem", ROOT / "Middlewares" / "ST" / "STM32_USB_Device_Library" / "Core" / "Inc"),
        ("-isystem", ROOT / "Middlewares" / "ST" / "STM32_USB_Device_Library" / "Class" / "AUDIO" / "Inc"),
        ("-isystem", ROOT / "Middlewares" / "ST" / "STM32_USB_Device_Library" / "Class" / "MSC" / "Inc"),
    ]
    include += [("-isystem", ROOT / "Middlewares/ST/STM32_USBPD_Library" / x) for x in ("Core/inc", "Devices/STM32G4XX/inc")]
    flattened = [item for pair in include for item in pair]
    return [
        stm32_tool("gcc"), "-fsyntax-only", "-std=gnu11", "-O2",
        "-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard",
        "-DDEBUG", "-DUSE_HAL_DRIVER", "-DSTM32G474xx",
        "-DUSE_FULL_LL_DRIVER", "-DUSBPD_PORT_COUNT=1", "-D_SNK", "-DUSBPDCORE_LIB_PD3_CONFIG_1",
        "-Wall", "-Wextra", "-Werror", "-Wshadow", "-Wconversion",
        "-Wsign-conversion", "-Wundef", "-Wdouble-promotion", "-Wformat=2",
        *flattened,
    ]


def check() -> dict[str, object]:
    configured = [str(item) for item in policy().get("strict_compile_sources", [])]
    if not configured:
        raise ValidationError("strict_compile_sources is empty")
    missing = [relative for relative in configured if not (ROOT / relative).is_file()]
    if missing:
        raise ValidationError("strict compile sources missing: " + ", ".join(missing))
    prefix = command_prefix()
    for relative in configured:
        run([*prefix, ROOT / relative])
    return {"sources": configured, "warning_classes": [
        "conversion", "sign-conversion", "shadow", "undef", "double-promotion", "format"
    ]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        result = check()
    except ValidationError as exc:
        if args.json:
            print(json.dumps({"ok": False, "error": str(exc)}))
        else:
            print(f"FAIL  {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps({"ok": True, **result}, indent=2))
    else:
        print(f"PASS  {len(result['sources'])} safety-critical sources pass strict target type checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
