#!/usr/bin/env python3
"""Render the landscape LCD UI on the host, as PNG screenshots.

Compiles the production UI sources (navigation, menu model, settings map,
renderers, graphics core) with the host compiler, links them against the
simulated radio in tools/tests/ui_preview_main.c plus zero stubs generated
from the firmware headers for everything else the UI calls, runs the
scripted knob/button session, and converts each frame to a PNG (2x, with a
contact sheet). The pixels are exactly what the panel is sent: the same
framebuffer, tile colours and RGB565 palette.

It writes only to --out (default: a new temporary directory). Pillow is
needed for PNG output; without it the PPM frames are kept.

Usage:
  ui_preview.py [--out DIR] [--scale N]
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parent.parent
UI_SOURCES = [
    "ui_gfx", "ui_render", "ui_render_menu", "ui_render_pages", "ui_menu", "ui_settings_map",
    "ui_labels", "ui_help", "ui_format", "ui_tune", "ui_nav", "ui_diag", "ui_sd", "ui_app", "ui_input", "radio_config",
    "ui_waterfall", "radio_spectrum", "mod_selftest_logic", "ui_trainer", "mod_morse_tables",
]
STUB_HEADERS = [
    "ui_state.h", "ui_lcd.h", "ui_sd.h", "ui_app.h", "radio.h", "radio_audio.h", "radio_bpf_sweep.h",
    "radio_af_sweep.h", "radio_iq_fit.h", "radio_pd.h", "radio_rx_cal.h", "mod_selftest.h", "mod_sdcard.h",
    "mod_sdcard_service.h", "sd_file_browser.h", "sd_audio_player.h", "fw_update.h", "usb_disk_mode.h",
    "usb_diag_mode.h", "board_rev1.h", "mod_priv.h", "mod_morse_trainer.h", "modulator.h",
    "radio_spectrum.h", "radio_clocks.h", "mod_sd_store.h", "radio_analog.h", "radio_digi.h",
]
CFLAGS = [
    "-std=gnu11", "-O1", "-g", "-w", "-DSTM32G474xx", "-DUSE_HAL_DRIVER",
    "-I", str(ROOT / "Core/Inc"),
    "-I", str(ROOT / "Drivers/STM32G4xx_HAL_Driver/Inc"),
    "-I", str(ROOT / "Drivers/CMSIS/Device/ST/STM32G4xx/Include"),
    "-I", str(ROOT / "Drivers/CMSIS/Include"),
    "-I", str(ROOT / "Middlewares/Third_Party/FatFs/src"),
    "-I", str(ROOT / "USB_Device/App"),
    "-I", str(ROOT / "Middlewares/ST/STM32_USB_Device_Library/Core/Inc"),
    f'-DHELP_BIN_PATH="{ROOT / "sd" / "HELP.BIN"}"',
]


def run(cmd: list[str]) -> str:
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit(f"failed: {' '.join(cmd[:3])} ...")
    return result.stdout


def symbols(objects: list[Path], defined: bool) -> set[str]:
    out = run(["nm", "--defined-only" if defined else "-u", *map(str, objects)])
    names = set()
    for line in out.splitlines():
        parts = line.split()
        if parts and not line.endswith(":"):
            names.add(parts[-1])
    return names


def header_text() -> str:
    text = "\n".join(h.read_text(errors="replace") for h in sorted((ROOT / "Core/Inc").glob("*.h")))
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return re.sub(r"^\s*#[^\n]*", " ", text, flags=re.M)


def stub_for(name: str, text: str) -> str | None:
    fn = re.search(r"(?:^|[;}\n])\s*((?:[A-Za-z_][\w]*[\s\*]+)+?)" + re.escape(name) + r"\s*\(([^()]*)\)\s*;", text)
    if fn:
        ret = re.sub(r"\b(extern|static|inline)\b", "", fn.group(1)).strip()
        params, body = [], []
        raw = fn.group(2).strip()
        if raw and raw != "void":
            for i, p in enumerate(raw.split(",")):
                p = p.strip()
                if p == "...":
                    params.append(p)
                    continue
                m = re.match(r"(.*?[\s\*])([A-Za-z_]\w*)(\s*\[[^\]]*\])?$", p)
                typ_words = re.sub(r"[\*\[\]\d]", " ", p).split()
                if m and len(typ_words) >= 2 and m.group(2) not in ("int", "char", "long", "short", "unsigned"):
                    typ, pname = m.group(1).strip(), m.group(2)
                    params.append(p)
                else:
                    typ, pname = p, f"p{i}"
                    params.append(f"{p} {pname}")
                if "*" in typ and "const" not in typ.split("*")[0] and "void" not in typ:
                    if re.search(r"\bchar\b", typ):
                        body.append(f"if ({pname}) {pname}[0] = 0;")
                    else:
                        body.append(f"if ({pname}) memset({pname}, 0, sizeof(*{pname}));")
        if ret == "void":
            tail = ""
        elif "*" in ret and re.search(r"\bchar\b", ret):
            tail = 'return "";'
        elif "*" in ret:
            tail = f"return ({ret})(void *)stub_zero;"
        else:
            tail = f"{{ {ret} r; memset(&r, 0, sizeof(r)); return r; }}"
        return f"{ret} {name}({', '.join(params) or 'void'}) {{ {' '.join(body)} {tail} }}"
    var = re.search(r"\bextern\s+([^;()]*?)\b" + re.escape(name) + r"\b(\s*\[[^\]]*\])?\s*;", text)
    if var:
        return f"{var.group(1).strip()} {name}{var.group(2) or ''};"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--scale", type=int, default=2)
    args = parser.parse_args()
    out = args.out or Path(tempfile.mkdtemp(prefix="alt2-ui-preview-"))
    build = out / "build"
    build.mkdir(parents=True, exist_ok=True)

    objects = []
    for name in UI_SOURCES:
        obj = build / f"{name}.o"
        run(["gcc", *CFLAGS, "-c", str(ROOT / "Core/Src" / f"{name}.c"), "-o", str(obj)])
        objects.append(obj)
    driver = build / "ui_preview_main.o"
    run(["gcc", *CFLAGS, "-c", str(ROOT / "tools/tests/ui_preview_main.c"), "-o", str(driver)])
    objects.append(driver)

    need = symbols(objects, False) - symbols(objects, True)
    text = header_text()
    # Everything left that no firmware header declares is the C library's.
    need.discard("_GLOBAL_OFFSET_TABLE_")
    stubs, unknown = [], []
    for name in sorted(need):
        stub = stub_for(name, text)
        if stub:
            stubs.append(stub)
        elif not re.match(r"(__|str|mem|f[a-z]|s?n?printf|printf|perror|exit|log|pow|round|lround|lrint|sqrt|sin|cos)", name):
            unknown.append(name)
    stub_c = build / "stubs.c"
    includes = "".join(f'#include "{h}"\n' for h in STUB_HEADERS)
    stub_c.write_text(f"{includes}#include <string.h>\nstatic unsigned long long stub_zero[1024];\n"
                      + "\n".join(stubs) + "\n")
    run(["gcc", *CFLAGS, "-c", str(stub_c), "-o", str(build / "stubs.o")])
    binary = build / "ui_preview"
    run(["gcc", "-o", str(binary), *map(str, objects), str(build / "stubs.o"), "-lm"])
    frames = out / "frames"
    frames.mkdir(exist_ok=True)
    print(run([str(binary), str(frames)]).strip())

    try:
        from PIL import Image
    except ImportError:
        print(f"PPM frames in {frames} (install Pillow for PNG)")
        return 0
    pngs = []
    for ppm in sorted(frames.glob("*.ppm")):
        img = Image.open(ppm).convert("RGB")
        img = img.resize((img.width * args.scale, img.height * args.scale), Image.NEAREST)
        png = out / (ppm.stem + ".png")
        img.save(png)
        pngs.append(img)
    cols = 4
    if pngs:
        w, h = pngs[0].size
        sheet = Image.new("RGB", (cols * (w + 8), ((len(pngs) + cols - 1) // cols) * (h + 8)), (40, 40, 40))
        for i, img in enumerate(pngs):
            sheet.paste(img, ((i % cols) * (w + 8), (i // cols) * (h + 8)))
        sheet.save(out / "contact_sheet.png")
    print(f"{len(pngs)} PNGs in {out}" + (f"; unresolved: {', '.join(unknown)}" if unknown else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
