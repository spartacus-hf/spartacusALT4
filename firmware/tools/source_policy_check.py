#!/usr/bin/env python3
"""Check source/build metadata and validation hygiene without modifying files."""

from __future__ import annotations

import argparse
import ast
import json
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True

from validation_support import ROOT, ValidationError, format_list, metadata_sources, policy, source_files


def python_write_calls(path: Path) -> list[str]:
    try:
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    except SyntaxError as exc:
        raise ValidationError(f"cannot parse {path.relative_to(ROOT)}: {exc}") from exc
    findings: list[str] = []
    write_methods = {"write_text", "write_bytes", "unlink", "rename", "rmdir"}
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        if isinstance(node.func, ast.Attribute) and node.func.attr in write_methods:
            findings.append(f"line {node.lineno}: {node.func.attr}()")
        if isinstance(node.func, ast.Name) and node.func.id == "open" and len(node.args) >= 2:
            mode = node.args[1]
            if isinstance(mode, ast.Constant) and isinstance(mode.value, str):
                if any(flag in mode.value for flag in "wax+"):
                    findings.append(f"line {node.lineno}: open({mode.value!r})")
    return findings


def analyze() -> dict[str, object]:
    actual = source_files()
    metadata = metadata_sources()
    missing = actual - metadata
    stale = {item for item in metadata - actual if item.startswith(("Core/Src/", "USB_Device/"))}
    if missing:
        raise ValidationError(f"C sources missing from Debug metadata: {format_list(missing)}")
    if stale:
        raise ValidationError(f"stale C sources in Debug metadata: {format_list(stale)}")

    scripts = sorted(path for path in (ROOT / "tools").rglob("*.py")
                     if "__pycache__" not in path.parts)
    # flash_rev1.py and rev1_diag.py are bench instruments: they save whole-
    # flash and settings-store backups under backups/ (git-ignored) or where
    # the operator says, and stage images and card files.
    allowed_writers = {"update_version.py", "package_firmware.py", "gen_rx_coeff.py",
                       "gen_rx_channel.py", "gen_ui_fonts.py", "gen_help_text.py", "gen_cw_pack.py", "ui_preview.py",
                       "flash_rev1.py", "rev1_diag.py"}
    temporary_probe_writers = {"storage_layout_report.py", "version_tool.py"}
    violations: dict[str, list[str]] = {}
    for script in scripts:
        relative_name = script.relative_to(ROOT / "tools").as_posix()
        if relative_name in allowed_writers:
            continue
        # tools/bench/ is the radio bench harness: its scripts write results
        # under tools/bench/results/ (git-ignored) and instrument scratch
        # copies of the tree, never the checkout.
        if relative_name.startswith("bench/"):
            continue
        calls = python_write_calls(script)
        # Compiler probes create sources/binaries only under TemporaryDirectory;
        # validate_current's full-tree snapshot still proves that they cannot
        # alter repository inputs. Keep that exception explicit and narrow.
        if calls and relative_name not in temporary_probe_writers:
            violations[relative_name] = calls
    if violations:
        detail = "; ".join(f"{name}: {', '.join(calls)}" for name, calls in violations.items())
        raise ValidationError(f"read-only validation scripts contain write operations: {detail}")

    archived = [path for path in actual if "/_archive/" in f"/{path}/"]
    if archived:
        raise ValidationError(f"archived sources entered active build scan: {format_list(archived)}")
    pipeline = (ROOT / "Core" / "Src" / "mod_tx_pipeline.c").read_text(encoding="utf-8")
    render_targets = set(re.findall(r"\bactive_render_fn\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*;", pipeline))
    covered_roots = {str(item) for item in policy()["realtime_roots"]}
    uncovered = render_targets - covered_roots
    if uncovered:
        raise ValidationError(
            "render dispatch targets missing from ELF real-time policy: " + ", ".join(sorted(uncovered)))
    # The CW paddle status used to be drawn as PEDAL; the landscape UI spreads
    # its pages over several ui_*.c files, so all of them are checked.
    for ui_source in sorted((ROOT / "Core" / "Src").glob("ui_*.c")):
        if '"PEDAL ' in ui_source.read_text(encoding="utf-8"):
            raise ValidationError(
                f"CW paddle status is mislabeled as PEDAL in {ui_source.relative_to(ROOT)}")
    # TIM2 and TIM5 are 32-bit timers on APB, which ignores the byte lanes: a
    # halfword DMA write arrives duplicated in both halves of the register, so
    # a headphone code of 232 became CCR = 0x00E800E8, far past ARR, and held
    # PA0/PA1 at DC. A DMA channel feeding either timer must write words.
    wide_timer_feeds = 0
    for c_source in sorted((ROOT / "Core" / "Src").glob("*.c")):
        text = c_source.read_text(encoding="utf-8")
        for channel in sorted(set(re.findall(
                r"\b(DMA\d_Channel\d)->CPAR\s*=\s*\(uint32_t\)\s*&\s*TIM[25]->\w+\s*;", text))):
            wide_timer_feeds += 1
            widths = [setting for setting in re.findall(
                rf"\b{channel}->CCR\s*=\s*([^;]*);", text, flags=re.DOTALL) if "PSIZE" in setting]
            if not widths or any("DMA_CCR_PSIZE_1" not in setting or "DMA_CCR_PSIZE_0" in setting
                                 for setting in widths):
                raise ValidationError(
                    f"{channel} in {c_source.relative_to(ROOT)} feeds a 32-bit timer without "
                    "a 32-bit peripheral size (DMA_CCR_PSIZE_1)")
    if not wide_timer_feeds:
        raise ValidationError("cannot locate the headphone TIM2 DMA channel")
    interrupts = (ROOT / "Core" / "Src" / "stm32g4xx_it.c").read_text(encoding="utf-8")
    usb_handler = re.search(
        r"void\s+USB_LP_IRQHandler\s*\(void\)\s*\{(.*?)HAL_PCD_IRQHandler",
        interrupts,
        flags=re.DOTALL,
    )
    if usb_handler is None:
        raise ValidationError("cannot locate USB_LP_IRQHandler hardware-fault policy")
    usb_fault_assignment = re.search(
        r"uint16_t\s+usb_fault\s*=\s*(.*?);",
        usb_handler.group(1),
        flags=re.DOTALL,
    )
    if usb_fault_assignment is None:
        raise ValidationError("USB_LP_IRQHandler does not classify USB hardware faults")
    usb_fault_expression = usb_fault_assignment.group(1)
    if "USB_ISTR_PMAOVR" not in usb_fault_expression:
        raise ValidationError("USB PMA overrun no longer enters the hard pipeline lockout")
    nonfatal_usb_flags = {"USB_ISTR_ERR", "USB_ISTR_ESOF"}
    misclassified = sorted(flag for flag in nonfatal_usb_flags if flag in usb_fault_expression)
    if misclassified:
        raise ValidationError(
            "recoverable USB flags enter the hard pipeline lockout: " + ", ".join(misclassified))
    return {
        "source_count": len(actual),
        "metadata_source_count": len(metadata),
        "python_script_count": len(scripts),
        "render_dispatch_targets": len(render_targets),
        "intentional_writers": sorted(allowed_writers),
    }


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
        print(f"PASS  all {result['source_count']} active C sources are represented in Debug metadata")
        print("PASS  validation/report Python scripts are source-tree read-only by policy")
        print("PASS  DMA into the 32-bit TIM2/TIM5 writes whole words")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
