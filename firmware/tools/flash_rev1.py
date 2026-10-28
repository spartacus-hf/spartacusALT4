#!/usr/bin/env python3
"""Flash a REV1 over SWD and make sure its SD card holds what the image needs.

    tools/flash_rev1.py                     # build, flash Release-Speed, then the card
    tools/flash_rev1.py --no-build          # flash dist/release-speed as it is
    tools/flash_rev1.py --flavour diagnostic
    tools/flash_rev1.py --card-only         # no flash: just check the card
    tools/flash_rev1.py --factory           # loader and application (once: v8.08 -> v8.09)
    tools/flash_rev1.py --restore FILE      # put a --factory backup back, byte for byte
    tools/flash_rev1.py --import-nvm FILE   # no SWD: v8.08's pages from a flash dump into the store

--factory is the one-time move to flash layout 2 (loader ABI 2, v8.09): the
application grows over the 44 KiB where v8.08 kept settings and calibration,
which live on the SD card now. Before anything is written the whole flash,
the option bytes and the MCU UID are saved under backups/; the loader's
write protection is lifted, the factory image (loader + application, the
whole 512 KiB) written, read back and the protection put back. A radio that
came from layout 1 then boots into its NO SETTINGS STORE hold with the USB
console up, and this script puts v8.08's settings and calibration pages on
the card as SYSTEM/NVM808.BIN and runs `storage import nvm`, which also takes
v8.08's card files; every calibration key of the dump must then be in the
store. --restore undoes all of it from the backup (loader, application and
the old settings pages). After the import every calibration key the radio
stored is compared with this script's own reading of the dump (and of
v8.08's M32CAL.BIN where the card was v8.08's store): same key, same record.

--import-nvm is the same import without an ST-Link, for a radio moved to
layout 2 over ROM DFU (docs/USB_FIRMWARE_UPDATE.md): FILE is the whole-flash
read taken before the factory image went in (or just its 44 KiB from
0x08075000), the radio's console is up in its NO SETTINGS STORE hold, and
the dump is tied to the radio the console reports.

Every flash ends with the card. The image itself says what it needs there
(console `card`: NAME:ID:STATE, the ID being the u32 the image expects in
bytes 4..7 of the file), and a radio whose card lacks one of those files, or
holds another build's, brings its USB console up at boot by itself -- so a
fresh radio with an empty card needs nothing done by hand. This script waits
for the radio, copies every file that is not `ok` from this tree (the
flavour's dist/ directory first, then sd/), checks again, and hands the USB
port back to audio unless the radio boots into the console anyway (DIAG ON
BOOT). A radio that comes back as USB audio found every file it needs.

Without --factory only the application is written (rev1_recovery.bin at
0x08001000) and only onto a loader of this layout: an application of layout
2 on a layout-1 loader would not start. Every flash is verified by
STM32_Programmer_CLI and then read back in full.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import rev1_diag  # noqa: E402  (the console, from the same tools directory)

APP_BASE = 0x08001000
FLASH_BASE = 0x08000000
FLASH_SIZE = 0x80000
LOADER_INFO_ADDR = 0x08000FE0
LOADER_INFO_MAGIC = 0x31444F4C  # "LOD1"
LAYOUT_ABI = 2
UID_ADDR = 0x1FFF7590
# Layout 1 (v8.08): settings and calibration pages, and the image footer.
NVM_BASE = 0x08075000
NVM_LENGTH = 0xB000
OLD_FOOTER = 0x080747E0
NVM_MAGIC = 0x384D564E  # "NVM8"
CAL_MAGIC = 0x43414C34  # "CAL4"
CAL_STRIDE = 288
CAL_PAGES = 20
BAND_COUNT = 23
ANALYTIC_COUNT = 5
BACKUPS = ROOT / "backups"
VID, PID_DIAG = "0483", "5742"
DIAG_PRODUCT = "SPARTACUS REV1 DIAG"
AUDIO_PRODUCT = "SPARTACUS REV1"
BOOT_WAIT_S = 40.0


def say(text: str) -> None:
    print(f"flash_rev1: {text}", flush=True)


def fail(text: str) -> int:
    print(f"flash_rev1: FAIL {text}", file=sys.stderr, flush=True)
    return 1


def programmer() -> str:
    found = os.environ.get("STM32_PROGRAMMER_CLI") or shutil.which("STM32_Programmer_CLI")
    if found:
        return found
    for pattern in ("/opt/stm32cubeide/plugins/*cubeprogrammer.linux64*/tools/bin/STM32_Programmer_CLI",
                    str(Path.home() / "STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI")):
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[-1]
    raise SystemExit("flash_rev1: STM32_Programmer_CLI not found; set STM32_PROGRAMMER_CLI")


# The programmer colours its output (ESC [39;49m ...) even into a pipe.
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def cli(args: list[str], what: str) -> str:
    """One STM32_Programmer_CLI run over SWD; its output, colour codes
    removed, or a clear stop."""
    run = subprocess.run([programmer(), "-c", "port=SWD", *args], capture_output=True, text=True)
    out = ANSI.sub("", run.stdout)
    if run.returncode != 0:
        sys.stderr.write(out[-2000:] + ANSI.sub("", run.stderr)[-2000:])
        raise SystemExit(f"flash_rev1: FAIL {what}")
    return out


def read_words(address: int, count: int) -> list[int]:
    out = cli(["mode=HOTPLUG", "-r32", hex(address), str(count * 4)], f"reading 0x{address:08X}")
    words: list[int] = []
    for line in out.splitlines():
        m = re.match(r"^\s*0x([0-9A-Fa-f]{8})\s*:\s*((?:[0-9A-Fa-f]{8}\s*)+)$", line)
        if m:
            words += [int(w, 16) for w in m.group(2).split()]
    if len(words) < count:
        raise SystemExit(f"flash_rev1: FAIL could not parse the read of 0x{address:08X}")
    return words[:count]


def read_flash(path: Path) -> bytes:
    cli(["mode=HOTPLUG", "-u", hex(FLASH_BASE), str(FLASH_SIZE), str(path)], "reading the flash")
    data = path.read_bytes()
    if len(data) < FLASH_SIZE:
        raise SystemExit("flash_rev1: FAIL the flash read came back short")
    return data[:FLASH_SIZE]


def option_bytes() -> tuple[dict[str, int], str]:
    out = cli(["mode=HOTPLUG", "-ob", "displ"], "reading the option bytes")
    values = {m.group(1): int(m.group(2), 16)
              for m in re.finditer(r"^\s*([A-Z][A-Z0-9_]*)\s*:\s*0x([0-9A-Fa-f]+)", out, re.MULTILINE)}
    for key in ("RDP", "DBANK", "BFB2", "WRP1A_STRT", "WRP1A_END"):
        if key not in values:
            raise SystemExit(f"flash_rev1: FAIL option byte {key} not in the programmer's listing")
    return values, out


def loader_abi() -> int | None:
    """The ABI of the loader at 0x08000000, None when there is none."""
    magic, abi, app_base, loader_bytes, crc = read_words(LOADER_INFO_ADDR, 5)
    if magic != LOADER_INFO_MAGIC or app_base != APP_BASE or loader_bytes != 0x1000:
        return None
    if zlib.crc32(struct.pack("<4I", magic, abi, app_base, loader_bytes)) != crc:
        return None
    return abi


def set_loader_protection(on: bool) -> None:
    """WRP1A over pages 0-1 (the loader), or off (start above end)."""
    if on:
        cli(["mode=UR", "-ob", "WRP1A_STRT=0x0", "WRP1A_END=0x1"], "protecting the loader")
    else:
        cli(["mode=UR", "-ob", "WRP1A_STRT=0x7F", "WRP1A_END=0x0"], "lifting the loader's protection")
    values, _ = option_bytes()
    protected = values["WRP1A_STRT"] <= values["WRP1A_END"]
    if protected != on:
        raise SystemExit(f"flash_rev1: FAIL loader protection is {'on' if protected else 'off'} after the change")


def storage_key(backend: int) -> int:
    """The analytic backend as v8.08 keyed its records: WEAVER, H12P, and
    the Hilbert family under one key."""
    return 0 if backend == 0 else 4 if backend == 4 else 1


def cal_valid(rec: bytes) -> tuple[int, int, int] | None:
    """The key of a calibration record v8.08 would have taken (CAL4, in
    range, CRC right), None for any other."""
    if len(rec) != CAL_STRIDE or struct.unpack_from("<I", rec)[0] != CAL_MAGIC:
        return None
    if zlib.crc32(rec[4:284]) != struct.unpack_from("<I", rec, 284)[0]:
        return None
    band, sideband, backend = struct.unpack_from("<HBB", rec, 8)
    if band >= BAND_COUNT or sideband > 1 or backend >= ANALYTIC_COUNT:
        return None
    return band, sideband, storage_key(backend)


def flash_cal_records(nvm: bytes):
    """v8.08's calibration pages in its scan order; an erased slot ends a page."""
    for page in range(CAL_PAGES):
        base = 0x1000 + page * 2048
        for slot in range(2048 // CAL_STRIDE):
            rec = nvm[base + slot * CAL_STRIDE: base + (slot + 1) * CAL_STRIDE]
            if struct.unpack_from("<I", rec)[0] == 0xFFFFFFFF:
                break
            yield rec


def card_cal_records(data: bytes):
    """v8.08's M32CAL.BIN: records back to back, as many as its flash held."""
    for i in range(min(len(data) // CAL_STRIDE, CAL_PAGES * (2048 // CAL_STRIDE))):
        yield data[i * CAL_STRIDE:(i + 1) * CAL_STRIDE]


def newest_per_key(records) -> dict[tuple[int, int, int], bytes]:
    """Per key the record with the highest sequence number, a later one
    winning a tie -- v8.08's own choice in flash and in M32CAL.BIN."""
    best: dict[tuple[int, int, int], tuple[int, bytes]] = {}
    for rec in records:
        key = cal_valid(rec)
        if key is None:
            continue
        seq = struct.unpack_from("<I", rec, 4)[0]
        if key not in best or seq >= best[key][0]:
            best[key] = (seq, rec)
    return {key: rec for key, (_, rec) in best.items()}


def cal_keys(nvm: bytes) -> set[tuple[int, int, int]]:
    """Every valid calibration key of a v8.08 flash dump, as the radio's
    store names it: (band, sideband, storage backend)."""
    return set(newest_per_key(flash_cal_records(nvm)))


def stored_crc(rec: bytes) -> int:
    """The CRC the radio's store gives a record it took from v8.08: sequence
    number 0 (the store orders versions itself) and the backend as its key."""
    out = bytearray(rec)
    out[4:8] = b"\0\0\0\0"
    out[11] = storage_key(out[11])
    return zlib.crc32(bytes(out[4:284]))


def nvm_file(flash: bytes, uid: list[int]) -> bytes:
    """SYSTEM/NVM808.BIN: v8.08's settings and calibration pages behind the
    header mod_storage_import.c checks."""
    nvm = flash[NVM_BASE - FLASH_BASE: NVM_BASE - FLASH_BASE + NVM_LENGTH]
    source_fw = struct.unpack_from("<I", flash, OLD_FOOTER - FLASH_BASE + 16)[0]
    header = struct.pack("<5I3II7I", NVM_MAGIC, 1, NVM_BASE, NVM_LENGTH, zlib.crc32(nvm),
                         *uid, source_fw, *([0] * 7))
    assert len(header) == 64
    return header + nvm


def build(flavour: str) -> None:
    env = dict(os.environ)
    tools = sorted(glob.glob("/opt/stm32cubeide/plugins/*gnu-tools-for-stm32*/tools/bin"))
    if tools and not shutil.which("arm-none-eabi-gcc"):
        env["PATH"] = tools[-1] + os.pathsep + env["PATH"]
    jobs = str(os.cpu_count() or 4)
    subprocess.run(["make", "-C", str(ROOT / "Debug"), f"-j{jobs}", "all"], check=True, env=env,
                   stdout=subprocess.DEVNULL)
    script = "build_diagnostic.sh" if flavour == "diagnostic" else "build_release_speed.sh"
    subprocess.run(["bash", str(ROOT / "tools" / script)], check=True, env=env, stdout=subprocess.DEVNULL)


def image_for(flavour: str) -> Path:
    image = ROOT / "dist" / flavour / "rev1_recovery.bin"
    if not image.is_file():
        raise SystemExit(f"flash_rev1: {image} missing; build it (drop --no-build)")
    elf = ROOT / {"release-speed": "Release-Speed", "diagnostic": ".Diagnostic"}.get(flavour, "Release-Speed") / "alt2.elf"
    if elf.is_file() and elf.stat().st_mtime > image.stat().st_mtime + 1.0:
        raise SystemExit(f"flash_rev1: {image.name} is older than {elf}; package it again (drop --no-build)")
    return image


def flash(cli: str, image: Path) -> None:
    abi = loader_abi()
    if abi != LAYOUT_ABI:
        raise SystemExit(f"flash_rev1: FAIL the radio's loader is {'missing' if abi is None else f'ABI {abi}'}; "
                         f"this image needs loader ABI {LAYOUT_ABI} -- run tools/flash_rev1.py --factory once")
    say(f"writing {image.relative_to(ROOT)} ({image.stat().st_size} bytes) at 0x{APP_BASE:08X}")
    run = subprocess.run([cli, "-c", "port=SWD", "mode=UR", "-d", str(image), hex(APP_BASE), "-v", "-rst"],
                         capture_output=True, text=True)
    if run.returncode != 0 or "verified successfully" not in ANSI.sub("", run.stdout).lower():
        sys.stderr.write(ANSI.sub("", run.stdout)[-2000:] + run.stderr[-2000:])
        raise SystemExit("flash_rev1: FAIL the programmer did not verify the download")
    # A full read-back of everything written, independent of the -v pass.
    with tempfile.TemporaryDirectory(prefix="rev1-readback-") as tmp:
        back = Path(tmp) / "back.bin"
        size = image.stat().st_size
        run = subprocess.run([cli, "-c", "port=SWD", "mode=HOTPLUG", "-u", hex(APP_BASE), str(size), str(back)],
                             capture_output=True, text=True)
        if run.returncode != 0 or not back.is_file() or back.read_bytes()[:size] != image.read_bytes():
            sys.stderr.write(run.stdout[-2000:] + run.stderr[-2000:])
            raise SystemExit("flash_rev1: FAIL the read-back differs from the image")
    say("flash written, verified and read back; the radio is restarting")


def usb_products() -> list[tuple[str, str, str]]:
    """(vendor, product id, product string) of every USB device."""
    out = []
    for dev in glob.glob("/sys/bus/usb/devices/*"):
        try:
            vid = Path(dev, "idVendor").read_text().strip()
            pid = Path(dev, "idProduct").read_text().strip()
            name = Path(dev, "product").read_text().strip()
        except OSError:
            continue
        out.append((vid, pid, name))
    return out


def console_port() -> str | None:
    """The radio's console tty, if it has enumerated."""
    for tty in sorted(glob.glob("/sys/class/tty/ttyACM*")):
        dev = Path(tty, "device").resolve().parent
        try:
            if (Path(dev, "idVendor").read_text().strip() == VID and
                    Path(dev, "idProduct").read_text().strip() == PID_DIAG):
                return "/dev/" + Path(tty).name
        except OSError:
            continue
    return None


def audio_present() -> bool:
    return any(v == VID and name == AUDIO_PRODUCT for v, _, name in usb_products())


def wait_for_radio(timeout: float) -> str | None:
    """The console port once it is there, None if the radio settled as USB
    audio instead (it does that only with a complete card)."""
    start = time.monotonic()
    audio_since = None
    time.sleep(2.0)  # the old enumeration may still be listed
    while time.monotonic() - start < timeout:
        port = console_port()
        if port and os.access(port, os.R_OK | os.W_OK):
            time.sleep(0.5)  # let the console finish its greeting
            return port
        if audio_present():
            audio_since = audio_since or time.monotonic()
            # The console comes up 2 s after the main loop starts, and only
            # after the boot log; audio that stays longer is the answer.
            if time.monotonic() - audio_since > 15.0:
                return None
        time.sleep(0.25)
    return None


def local_file(name: str, flavour: str) -> list[Path]:
    return [p for p in (ROOT / "dist" / flavour / name, ROOT / "sd" / name) if p.is_file()]


def file_id(path: Path) -> str:
    head = path.read_bytes()[:8]
    return f"{int.from_bytes(head[4:8], 'little'):08X}" if len(head) == 8 else "--------"


def needs(console: rev1_diag.Console) -> tuple[bool, list[tuple[str, str, str]]]:
    line = " ".join(console.command("card"))
    words = line.split()
    mounted = "card=1" in words
    entries = []
    if "need" in words:
        for item in words[words.index("need") + 1:]:
            parts = item.split(":")
            if len(parts) == 3:
                entries.append((parts[0], parts[1].upper(), parts[2]))
    return mounted, entries


def put(console: rev1_diag.Console, local: Path, remote: str) -> None:
    args = argparse.Namespace(local=str(local), remote=remote)
    if rev1_diag.cmd_sdput(console, args) != 0:
        raise SystemExit(f"flash_rev1: FAIL writing {remote} to the card")


def provision(port: str, flavour: str) -> int:
    console = rev1_diag.Console(port)
    try:
        ident = console.command("id")
        firmware = next((l.split("=", 1)[1] for l in ident if l.startswith("firmware=")), "?")
        say(f"radio is up: firmware {firmware}")
        mounted, entries = needs(console)
        if not mounted:
            return fail("no SD card is mounted; insert a FAT-formatted card and run "
                        "tools/flash_rev1.py --card-only")
        if not entries:
            return fail("the radio did not list the files it needs (firmware older than v8.08?)")
        for name, want, state in entries:
            if state == "ok":
                say(f"{name}: ok ({want})")
                continue
            source = next((p for p in local_file(name, flavour) if file_id(p) == want), None)
            if source is None:
                have = ", ".join(f"{p.relative_to(ROOT)} {file_id(p)}" for p in local_file(name, flavour)) or "none"
                return fail(f"{name}: the radio wants {want}, this tree has {have} -- flash the image "
                            "built from this tree, or rebuild")
            say(f"{name}: {state} on the card, copying {source.relative_to(ROOT)} ({want})")
            put(console, source, name)
        mounted, entries = needs(console)
        bad = [f"{n}:{s}" for n, _, s in entries if s != "ok"]
        if bad:
            return fail("still not right on the card: " + " ".join(bad))
        say("card complete: " + " ".join(f"{n} {w}" for n, w, _ in entries))
        try:
            diagboot = " ".join(console.command("diagboot"))
        except SystemExit:  # not in the Diagnostic flavour, which keeps the console
            diagboot = "diagboot=1"
        if "diagboot=1" not in diagboot:
            console.command("exit")  # the console came up for the card only
            say("USB handed back to audio")
        return 0
    finally:
        console.close()


def write_whole(image: bytes, what: str) -> None:
    """The whole flash from 0x08000000, loader protection lifted around it,
    verified and read back in full."""
    values, _ = option_bytes()
    if values["WRP1A_STRT"] <= values["WRP1A_END"]:
        say("lifting the loader's write protection")
        set_loader_protection(False)
    with tempfile.TemporaryDirectory(prefix="rev1-factory-") as tmp:
        path = Path(tmp) / "image.bin"
        path.write_bytes(image)
        say(f"writing {what} ({len(image)} bytes) at 0x{FLASH_BASE:08X}")
        out = cli(["mode=UR", "-d", str(path), hex(FLASH_BASE), "-v"], f"writing {what}")
        if "verified successfully" not in out.lower():
            raise SystemExit("flash_rev1: FAIL the programmer did not verify the download")
        back = read_flash(Path(tmp) / "back.bin")
        if back[:len(image)] != image:
            raise SystemExit("flash_rev1: FAIL the read-back differs from the image")
    say("flash written, verified and read back; protecting the loader again")
    set_loader_protection(True)
    # The option-byte load normally resets the MCU; make sure it runs.
    subprocess.run([programmer(), "-c", "port=SWD", "mode=HOTPLUG", "-rst"], capture_output=True, text=True)


def backup(tag: str) -> tuple[bytes, list[int], int | None, Path]:
    values, listing = option_bytes()
    if values["DBANK"] != 1 or values["BFB2"] != 0 or values["RDP"] != 0xAA:
        raise SystemExit("flash_rev1: FAIL option bytes need DBANK=1, BFB2=0, RDP level 0 "
                         f"(read DBANK={values['DBANK']} BFB2={values['BFB2']} RDP=0x{values['RDP']:02X})")
    uid = read_words(UID_ADDR, 3)
    abi = loader_abi()
    BACKUPS.mkdir(exist_ok=True)
    stem = BACKUPS / f"rev1_{uid[2]:08X}{uid[1]:08X}{uid[0]:08X}_{time.strftime('%Y%m%d-%H%M%S')}_{tag}"
    flash_now = read_flash(stem.with_suffix(".flash.bin"))
    stem.with_suffix(".ob.txt").write_text(listing)
    say(f"backed up the flash, option bytes and UID to {stem.relative_to(ROOT)}.*  (loader ABI {abi})")
    return flash_now, uid, abi, stem


def storage_fields(console: rev1_diag.Console) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in console.command("storage"):
        for word in line.split():
            if "=" in word:
                key, value = word.split("=", 1)
                out.setdefault(key, value)
    return out


def card_file(console: rev1_diag.Console, name: str) -> bytes | None:
    try:
        console.command(f"sd stat {name}")
    except SystemExit:
        return None
    return rev1_diag.sd_get(console, name)


def import_on_card(port: str, nvm: bytes, keys: set[tuple[int, int, int]]) -> None:
    """NVM808.BIN onto the card of a radio in its NO SETTINGS STORE hold, the
    import, and every calibration key checked against this script's own
    reading of v8.08's records."""
    console = rev1_diag.Console(port, timeout=60.0)
    try:
        state = storage_fields(console).get("store", "?")
        if state not in ("BLANK", "READY"):
            raise SystemExit(f"flash_rev1: FAIL the radio's store is {state}; the import needs a blank card "
                             "(or this radio's store)")
        # v8.08's calibration file, read while the radio still holds at boot
        # (it is quick there): where the card was v8.08's store, its records
        # win over the flash's.
        card_cal = card_file(console, "M32CAL.BIN")
        if card_cal is not None:
            say(f"read v8.08's M32CAL.BIN ({len(card_cal)} bytes) for the check")
        say(f"putting SYSTEM/NVM808.BIN on the card ({len(nvm)} bytes; about a minute)")
        try:
            console.command("sd mkdir SYSTEM")
        except SystemExit:
            pass  # already there
        with tempfile.TemporaryDirectory(prefix="rev1-nvm-") as tmp:
            local = Path(tmp) / "NVM808.BIN"
            local.write_bytes(nvm)
            put(console, local, "SYSTEM/NVM808.BIN")
        # The boot hold imports a complete dump by itself within a second or
        # two; only a radio past its hold (store READY already) is asked.
        fields = storage_fields(console)
        deadline = time.monotonic() + 15.0
        while state == "BLANK" and fields.get("store") != "READY" and time.monotonic() < deadline:
            time.sleep(1.0)
            fields = storage_fields(console)
        if fields.get("nvm") != "1" or fields.get("store") != "READY":
            for line in console.command("storage import nvm"):
                say("  " + line)
            fields = storage_fields(console)
        if fields.get("store") != "READY" or fields.get("nvm") != "1":
            raise SystemExit(f"flash_rev1: FAIL the import did not take the dump (store={fields.get('store')} "
                             f"nvm={fields.get('nvm')})")
        say(f"imported: settings from {'flash' if fields.get('settings') == '1' else 'card' if fields.get('settings') == '2' else 'nowhere'}, "
            f"calibration flash/card {fields.get('cal')}, failed {fields.get('failed')}")
        if fields.get("failed") not in (None, "0"):
            raise SystemExit("flash_rev1: FAIL the radio could not store every record it imported")
        log = card_file(console, "SYSTEM/IMPORT.LOG") or b""
        backend_sd = b"backend=sd" in log
        say("IMPORT.LOG: " + log.decode("ascii", "replace").strip())

        # What the store should hold, by v8.08's rules, read here independently.
        expected = newest_per_key(flash_cal_records(nvm[64:]))
        if backend_sd and card_cal is not None:
            expected.update(newest_per_key(card_cal_records(card_cal)))
        stored: dict[tuple[int, int, int], int] = {}
        for line in console.command("storage list"):
            m = re.search(r"band=(\d+) sb=(\d+) backend=(\d+) crc=([0-9A-Fa-f]{8})", line)
            if m:
                stored[(int(m.group(1)), int(m.group(2)), int(m.group(3)))] = int(m.group(4), 16)
        missing = sorted(keys - set(stored))
        if missing:
            raise SystemExit(f"flash_rev1: FAIL {len(missing)} calibration keys of the flash dump are not "
                             f"in the store: {missing[:8]}")
        differ = sorted(k for k, rec in expected.items() if stored.get(k) != stored_crc(rec))
        if differ:
            raise SystemExit(f"flash_rev1: FAIL {len(differ)} calibration records differ from v8.08's: "
                             f"{differ[:8]} (band, sideband, backend)")
        say(f"store holds every one of v8.08's {len(expected)} calibration records, each identical "
            f"({len(keys)} from the dump's keys, {len(stored)} in the store)")
    finally:
        console.close()


def console_uid(port: str) -> list[int]:
    """The radio's MCU UID from its console `id` (v8.09), low word first."""
    console = rev1_diag.Console(port)
    try:
        text = next((l.split("=", 1)[1] for l in console.command("id") if l.startswith("uid=")), "")
    finally:
        console.close()
    if not re.fullmatch(r"[0-9A-Fa-f]{24}", text):
        raise SystemExit("flash_rev1: FAIL the radio's console did not report its uid (firmware before v8.09?)")
    return [int(text[16:24], 16), int(text[8:16], 16), int(text[0:8], 16)]


def import_nvm(path: Path, port_arg: str | None) -> int:
    """--import-nvm: v8.08's settings pages from a flash dump taken over ROM
    DFU into the store of the radio on the console."""
    data = path.read_bytes()
    if len(data) == FLASH_SIZE:
        flash_bytes = data
    elif len(data) == NVM_LENGTH:
        flash_bytes = bytes(NVM_BASE - FLASH_BASE) + data + bytes(FLASH_SIZE - (NVM_BASE - FLASH_BASE) - NVM_LENGTH)
    else:
        raise SystemExit(f"flash_rev1: FAIL {path.name}: a whole-flash read ({FLASH_SIZE} bytes) or the "
                         f"settings pages from 0x{NVM_BASE:08X} ({NVM_LENGTH} bytes)")
    port = port_arg or wait_for_port_any(BOOT_WAIT_S)
    if port is None:
        return fail("no radio console found; the radio waits in its NO SETTINGS STORE hold with the console up")
    uid = console_uid(port)
    nvm = nvm_file(flash_bytes, uid)
    keys = cal_keys(nvm[64:])
    say(f"{path.name}: {len(keys)} calibration keys in v8.08's flash pages; radio uid "
        f"{uid[2]:08X}{uid[1]:08X}{uid[0]:08X}")
    import_on_card(port, nvm, keys)
    return 0


def factory(flavour: str) -> int:
    image = (ROOT / "dist" / flavour / "rev1_factory.bin").read_bytes()
    if len(image) != FLASH_SIZE:
        raise SystemExit("flash_rev1: FAIL rev1_factory.bin is not a whole-flash layout-2 image")
    flash_now, uid, abi, stem = backup("before-factory")
    nvm = keys = None
    if abi == 1:
        nvm = nvm_file(flash_now, uid)
        keys = cal_keys(nvm[64:])
        stem.with_suffix(".NVM808.BIN").write_bytes(nvm)
        say(f"layout 1 radio: {len(keys)} calibration keys in its flash pages, kept in "
            f"{stem.relative_to(ROOT)}.NVM808.BIN")
    elif abi == LAYOUT_ABI:
        say("the radio already has a layout-2 loader: reinstalling loader and application only")
    else:
        say("no loader found: installing loader and application")
    write_whole(image, "the factory image")
    port = wait_for_port_any(BOOT_WAIT_S)
    if nvm is not None:
        if port is None:
            return fail(f"the radio did not bring its console up; put {stem.name}.NVM808.BIN on the card as "
                        "SYSTEM/NVM808.BIN and run `storage import nvm` on the console")
        import_on_card(port, nvm, keys)
    if port is None:
        return fail("the radio did not come back on USB")
    return provision(port, flavour)


def restore(path: Path) -> int:
    image = path.read_bytes()
    if len(image) != FLASH_SIZE:
        raise SystemExit("flash_rev1: FAIL a backup is the whole flash, 524288 bytes")
    backup("before-restore")
    write_whole(image, path.name)
    say("restored; the radio runs whatever that backup held")
    return 0


def wait_for_port_any(timeout: float) -> str | None:
    """The console port, waiting through the boot hold (no audio shortcut)."""
    start = time.monotonic()
    time.sleep(2.0)
    while time.monotonic() - start < timeout:
        port = console_port()
        if port and os.access(port, os.R_OK | os.W_OK):
            time.sleep(0.5)
            return port
        time.sleep(0.25)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--flavour", default="release-speed", choices=("release-speed", "diagnostic"))
    parser.add_argument("--no-build", action="store_true", help="flash the packaged image as it is")
    parser.add_argument("--card-only", action="store_true", help="do not flash; check and fill the card")
    parser.add_argument("--port", help="console tty (default: found by USB id)")
    parser.add_argument("--factory", action="store_true",
                        help="loader and application, the whole flash (once: layout 1 -> 2); backs up first")
    parser.add_argument("--restore", type=Path, metavar="FILE",
                        help="write a whole-flash backup (backups/*.flash.bin) back")
    parser.add_argument("--import-nvm", type=Path, metavar="FILE",
                        help="no SWD: v8.08's settings pages from a ROM DFU flash read into the store")
    args = parser.parse_args()

    if args.import_nvm:
        return import_nvm(args.import_nvm, args.port)
    if args.restore:
        return restore(args.restore)
    if args.factory:
        if not args.no_build:
            say(f"building {args.flavour}")
            build(args.flavour)
        return factory(args.flavour)
    if not args.card_only:
        if not args.no_build:
            say(f"building {args.flavour}")
            build(args.flavour)
        flash(programmer(), image_for(args.flavour))
    port = args.port or wait_for_radio(BOOT_WAIT_S)
    if port is None:
        if audio_present():
            say("the radio came back as USB audio: its card already holds every file it needs")
            return 0
        return fail("the radio did not come back on USB; check the cable and the boot log on its screen")
    return provision(port, args.flavour)


if __name__ == "__main__":
    sys.exit(main())
