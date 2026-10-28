#!/usr/bin/env python3
"""Prove that a change left the receive and transmit code alone.

    tools/rt_identity_check.py --base <old build dir> --new <new build dir>

Compares the objects of two builds section by section (the code is built
with -ffunction-sections -fdata-sections, so a section is one function or
one object): the bytes, and every relocation by offset, type and the name of
the symbol or section it points at. Relocations in an object are symbolic,
so a function that compiled to the same instructions compares equal no matter
where the linker later puts it.

Every object on the real-time list -- the receiver, the transmit pipeline,
the RF transport, CW keying, USB audio, USB-PD, the SD audio player and
decoder, FatFs, the HAL and the interrupt handlers -- must be identical, apart
from sections named with --allow OBJECT:SECTION (each one a change made on
purpose). So must every function reachable from an interrupt vector (the
call graph of each build's alt2.elf: direct branches and the function
addresses its code loads), whichever object it lives in -- main.o's
MOD_GetVrefMillivolts or mod_storage.o's MOD_EventLog, say. Other objects are
listed when they changed, for information.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from elftools.elf.constants import SH_FLAGS

# Real-time objects by stem: every one of them runs in, or feeds, the receive
# and transmit paths (interrupts, PendSV, DMA, USB audio, USB-PD, SD audio).
RT_CORE = (
    "audio_resample", "clock_plan", "hrtim_phase", "mod_api_audio", "mod_cw",
    "mod_dsp_tables", "mod_morse_tables", "mod_outphase", "mod_state",
    "mod_transport", "mod_transport_logic", "mod_tx_pipeline", "modulator",
    "radio", "radio_analog", "radio_audio", "radio_clocks", "radio_cw_decoder",
    "radio_dsp", "radio_pd", "radio_pd_policy", "radio_pd_port",
    "radio_rf_meter", "radio_rx_front", "radio_spectrum", "sd_audio_decoder", "sd_audio_math",
    "sd_audio_player", "stm32g4xx_it", "system_stm32g4xx",
)
RT_OTHER = (
    "ff", "ffunicode", "usbd_audio", "usbd_audio_if", "usbd_core",
    "usbd_ctlreq", "usbd_ioreq", "usbd_desc", "usbd_cdc_min", "usbd_cat", "usb_device",
    "usbd_conf", "startup_stm32g474vetx",
)
RT_DIRS = ("Drivers/STM32G4xx_HAL_Driver/Src", "Middlewares/ST/STM32_USBPD_Library")


def is_rt(rel: str) -> bool:
    stem = Path(rel).stem
    if rel.startswith("Core/Src/"):
        return stem in RT_CORE
    if any(rel.startswith(d) for d in RT_DIRS):
        return True
    return stem in RT_OTHER


def tool(name: str) -> str:
    found = shutil.which(name)
    if found:
        return found
    hits = sorted(Path("/opt/stm32cubeide/plugins").glob(f"*gnu-tools-for-stm32*/tools/bin/{name}"))
    if not hits:
        raise SystemExit(f"rt_identity_check: {name} not found")
    return str(hits[-1])


def isr_reachable(elf: Path) -> set[str]:
    """Functions reachable from the vector table, Reset_Handler excepted."""
    dis = subprocess.run([tool("arm-none-eabi-objdump"), "-d", "--no-show-raw-insn", str(elf)],
                         capture_output=True, text=True, check=True).stdout
    start: dict[str, int] = {}
    edges: dict[str, set] = {}
    words: dict[str, set] = {}
    cur = None
    branch = re.compile(r"\s(?:bl|blx|b|b\.w|b[a-z]{2}\.w|b[a-z]{2}|cbz|cbnz)\s+[0-9a-f]+\s+<([^>+]+)")
    for line in dis.splitlines():
        m = re.match(r"^([0-9a-f]+) <([^>]+)>:", line)
        if m:
            cur = m.group(2)
            start[cur] = int(m.group(1), 16)
            continue
        if cur is None:
            continue
        m = branch.search(line)
        if m and m.group(1) != cur:
            edges.setdefault(cur, set()).add(m.group(1))
        m = re.search(r"\.word\s+0x([0-9a-f]+)", line)
        if m:
            words.setdefault(cur, set()).add(int(m.group(1), 16) & ~1)
    at = {a: f for f, a in start.items()}
    for f, ws in words.items():  # function addresses loaded as callbacks
        for w in ws:
            g = at.get(w)
            if g and g != f:
                edges.setdefault(f, set()).add(g)
    vec = subprocess.run([tool("arm-none-eabi-objcopy"), "-O", "binary", "-j", ".isr_vector",
                          str(elf), "/dev/stdout"], capture_output=True, check=True).stdout
    roots = set()
    for i in range(4, len(vec) - 3, 4):
        f = at.get(int.from_bytes(vec[i:i + 4], "little") & ~1)
        if f and f not in ("Reset_Handler", "Default_Handler"):
            roots.add(f)
    seen: set[str] = set()
    todo = list(roots)
    while todo:
        f = todo.pop()
        if f not in seen:
            seen.add(f)
            todo.extend(edges.get(f, ()))
    return seen


LOCAL_STATIC = re.compile(r"^(\.(?:rodata|data|bss)\..+?)\.\d+$")


def plain(name: str) -> str:
    """A function-local static's section without GCC's per-object counter
    (.rodata.g_win.20 is .rodata.g_win.0 once another static came or went)."""
    m = LOCAL_STATIC.match(name)
    return m.group(1) if m else name


def sections(path: Path) -> dict[str, tuple[bytes | int, tuple]]:
    """Allocated sections: name -> (bytes, or size for NOBITS; relocations)."""
    out: dict[str, tuple[bytes | int, tuple]] = {}
    with path.open("rb") as f:
        elf = ELFFile(f)
        all_secs = list(elf.iter_sections())
        names = [s.name for s in all_secs]
        symbols = None
        relocs: dict[str, list] = {}
        for sec in all_secs:
            if not isinstance(sec, RelocationSection):
                continue
            # Debug-info relocations are most of an object and say nothing
            # about the code: only relocations of allocated sections count.
            if not (all_secs[sec["sh_info"]]["sh_flags"] & SH_FLAGS.SHF_ALLOC):
                continue
            if symbols is None:
                symbols = list(elf.get_section_by_name(".symtab").iter_symbols())
            target = names[sec["sh_info"]]
            rows = []
            for r in sec.iter_relocations():
                sym = symbols[r["r_info_sym"]]
                name = sym.name
                if sym["st_info"]["type"] == "STT_SECTION":
                    name = names[sym["st_shndx"]] if isinstance(sym["st_shndx"], int) else str(sym["st_shndx"])
                addend = r["r_addend"] if r.is_RELA() else None
                rows.append((r["r_offset"], r["r_info_type"], plain(name), addend))
            relocs[target] = rows
        for sec in all_secs:
            if not (sec["sh_flags"] & SH_FLAGS.SHF_ALLOC):
                continue
            body = sec["sh_size"] if sec["sh_type"] == "SHT_NOBITS" else sec.data()
            key = plain(sec.name)
            n = 2
            while key in out:  # COMDAT groups or same-named statics repeat a name
                key = f"{plain(sec.name)}#{n}"
                n += 1
            out[key] = (body, tuple(sorted(relocs.get(sec.name, ()))))
    return out


def objects(build: Path) -> dict[str, Path]:
    return {str(p.relative_to(build)): p for p in build.rglob("*.o")}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", required=True, type=Path, help="build directory of the reference image")
    ap.add_argument("--new", required=True, type=Path, help="build directory of the changed image")
    ap.add_argument("--allow", action="append", default=[],
                    help="OBJECT_STEM:SECTION changed on purpose (repeatable; SECTION may be *)")
    ap.add_argument("--quiet", action="store_true", help="only the verdict and real-time differences")
    args = ap.parse_args()

    allowed = set()
    for item in args.allow:
        stem, _, sec = item.partition(":")
        allowed.add((stem, sec or "*"))

    base, new = objects(args.base), objects(args.new)
    reach: set[str] = set()
    for build in (args.base, args.new):
        if (build / "alt2.elf").is_file():
            reach |= isr_reachable(build / "alt2.elf")
    rt_sections = {f".text.{f}" for f in reach}
    failures = 0
    info = []
    for rel in sorted(set(base) | set(new)):
        rt = is_rt(rel)
        stem = Path(rel).stem
        if rel not in base or rel not in new:
            what = "added" if rel not in base else "removed"
            if rt:
                print(f"FAIL {rel}: real-time object {what}")
                failures += 1
            else:
                info.append(f"{rel}: {what}")
            continue
        a, b = sections(base[rel]), sections(new[rel])
        changed = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))
        if not changed:
            continue
        hot = changed if rt else [sec for sec in changed if sec.split("#")[0] in rt_sections]
        if len(hot) < len(changed):
            info.append(f"{rel}: {len(changed) - len(hot)} section(s) changed")
        for sec in hot:
            if (stem, "*") in allowed or (stem, sec) in allowed:
                print(f"ALLOW {rel} {sec}")
                continue
            kind = "added" if sec not in a else "removed" if sec not in b else "differs"
            print(f"FAIL {rel} {sec}: {kind}")
            failures += 1
    if not args.quiet:
        for line in info:
            print(f"info {line}")
    print(f"{len(reach)} functions reachable from interrupts")
    if failures:
        print(f"FAIL {failures} real-time section(s) changed")
        return 1
    print("PASS every real-time object is identical (allowed changes listed above)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
