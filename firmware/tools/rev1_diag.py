#!/usr/bin/env python3
"""Host side of the REV1 USB diagnostic console.

Standard library only: the console is a CDC-ACM port, so a raw tty plus
termios is enough and there is no pyserial dependency to install. On Arch the
port is /dev/ttyACM0 and the `uucp` group owns it.

    tools/rev1_diag.py id
    tools/rev1_diag.py selftest run filters rx
    tools/rev1_diag.py pins
    tools/rev1_diag.py rxcal --sb --repeat 3
    tools/rev1_diag.py sweep --table
    tools/rev1_diag.py audio 30 --iq --out capture.wav
    tools/rev1_diag.py watch telem
    tools/rev1_diag.py console
    tools/rev1_diag.py term
    tools/rev1_diag.py paddle
    tools/rev1_diag.py pdlog
    tools/rev1_diag.py refcal --seconds 180 --apply
    tools/rev1_diag.py raw 'set put 42 30'

This script is an instrument, not part of the validation chain; nothing in
tools/validate_current.py invokes it.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import glob
import os
import re
import select
import subprocess
import sys
import termios
import time
import tty
import zlib

DEFAULT_TIMEOUT = 20.0
# The USB audio personality's product string, for finding its ALSA card.
AUDIO_PRODUCT = "SPARTACUS REV1"


class Console:
    def __init__(self, port: str, timeout: float = DEFAULT_TIMEOUT) -> None:
        self.port = port
        self.timeout = timeout
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        # Raw: no echo, no line discipline, no CR/LF translation either way.
        iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
        iflag &= ~(termios.IXON | termios.IXOFF | termios.ICRNL | termios.INLCR |
                   termios.IGNCR | termios.ISTRIP | termios.BRKINT)
        oflag &= ~termios.OPOST
        lflag &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG |
                   termios.IEXTEN)
        cflag |= termios.CLOCAL | termios.CREAD
        cc = list(cc)
        cc[termios.VMIN] = 0
        cc[termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW,
                          [iflag, oflag, cflag, lflag, ispeed, ospeed, cc])
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.buffer = b""

    def close(self) -> None:
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def _read_some(self) -> bytes:
        try:
            return os.read(self.fd, 4096)
        except BlockingIOError:
            return b""
        except OSError as exc:            # the board re-enumerated under us
            raise SystemExit(f"rev1_diag: port went away ({exc})") from exc

    def command(self, text: str) -> list[str]:
        """Send one line and collect data lines up to the OK/ERR terminator."""
        os.write(self.fd, (text.strip() + "\n").encode())
        lines: list[str] = []
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            chunk = self._read_some()
            if not chunk:
                time.sleep(0.002)
                continue
            self.buffer += chunk
            # Firmware before v7.09 ends an intentionally blank line with a
            # bare CR and no LF, so a run of them would swallow the OK that
            # follows. Treat CR as a terminator too; real lines end CRLF, and
            # the empty fragment that produces is skipped below.
            self.buffer = self.buffer.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
            while b"\n" in self.buffer:
                raw, self.buffer = self.buffer.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                if line in ("", "\r"):
                    continue
                # Commentary for a person, such as the greeting a terminal
                # gets when it opens the port and stays quiet.
                if line.startswith("# "):
                    continue
                if line == "OK":
                    return lines
                if line.startswith("ERR "):
                    raise SystemExit("rev1_diag: " + line[4:])
                lines.append(line)
        raise SystemExit(f"rev1_diag: timed out waiting for a reply to {text!r}")


def find_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    ports = sorted(glob.glob("/dev/ttyACM*"))
    if not ports:
        raise SystemExit(
            "rev1_diag: no /dev/ttyACM* found. Open BOARD > POWER > USB DIAG CONSOLE\n"
            "           on the radio, then plug in USB-C.")
    return ports[0]


def cmd_selftest(console: Console, args: argparse.Namespace) -> int:
    groups = " ".join(args.groups)
    console.command(f"selftest run {groups}".strip())
    last = ""
    while True:
        status = console.command("selftest status")
        text = status[0] if status else ""
        fields = dict(f.split("=", 1) for f in text.split() if "=" in f)
        if text != last:
            print(text)
            last = text
        if fields.get("active", "0") == "0":
            break
        time.sleep(0.25)
    rows = console.command("selftest results")
    width = 0
    parsed = []
    for row in rows[1:]:
        parts = row.split(" ", 8)
        if len(parts) < 8:
            continue
        parsed.append(parts)
        width = max(width, len(parts[2]))
    for p in parsed:
        value = f"{p[4]} {p[5]}".strip()
        print(f"  {p[3]:<4} {p[2]:<{width}}  {value:>12}  {p[8] if len(p) > 8 else ''}")
    return 1 if any(p[3] == "FAIL" for p in parsed) else 0


def cmd_watch(console: Console, args: argparse.Namespace) -> int:
    try:
        while True:
            for line in console.command(args.what):
                print(line)
            print("-" * 40, flush=True)
            time.sleep(args.interval)
    except KeyboardInterrupt:
        return 0


def cmd_console(console: Console, _args: argparse.Namespace) -> int:
    print("rev1 console. Blank line or ctrl-d to quit.\n")
    for line in console.command("help"):
        print(line)
    while True:
        try:
            text = input("> ").strip()
        except EOFError:
            return 0
        if not text:
            return 0
        try:
            for line in console.command(text):
                print(line)
        except SystemExit as exc:
            print(exc)
    return 0


# Ctrl-] leaves `term` and `paddle`, as in telnet and miniterm. While the
# paddle page has the kitty keyboard protocol on, the key arrives as
# CSI 93 ; mods [: action] u instead of the byte 0x1d.
QUIT_KITTY = re.compile(rb"\x1b\[93(?::\d*)*;(\d+)(?::(\d+))?u")
# What the firmware turns on for the paddle page, so it can be undone here if
# the session ends before the firmware says so itself.
KITTY_PUSH = re.compile(rb"\x1b\[>\d+u")
KITTY_POP = re.compile(rb"\x1b\[<\d*u")
TERMINAL_RESET = b"\x1b[?1004l\x1b[?7h\x1b[?25h\x1b[0m\r\n"


def quit_at(keys: bytes) -> int | None:
    """Offset of Ctrl-] in a burst of keyboard input, if there is one."""
    found = keys.find(b"\x1d")
    at = found if found >= 0 else None
    for match in QUIT_KITTY.finditer(keys):
        mods = int(match.group(1)) - 1
        action = int(match.group(2) or 1)
        if mods & 4 and action != 3:
            at = match.start() if at is None else min(at, match.start())
            break
    return at


def write_all(fd: int, data: bytes) -> None:
    while data:
        try:
            data = data[os.write(fd, data):]
        except BlockingIOError:
            time.sleep(0.001)


def cmd_term(console: Console, args: argparse.Namespace) -> int:
    """Raw terminal: every key goes to the radio, every byte comes back here.

    The console's own line editing, prompt and full-screen menu then work as
    they do in PuTTY or minicom. `paddle` opens the virtual paddle page
    directly. Ctrl-] quits and puts this terminal back the way it was.
    """
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        raise SystemExit("rev1_diag: term needs a terminal on stdin and stdout")
    stdin, stdout = sys.stdin.fileno(), sys.stdout.fileno()
    saved = termios.tcgetattr(stdin)
    pushed = 0
    carry = b""
    write_all(stdout, f"rev1 terminal on {console.port}. Ctrl-] quits.\r\n".encode())
    try:
        tty.setraw(stdin)
        # A CR makes the console interactive (echo and prompt); `paddle`
        # carries its own.
        write_all(console.fd, b"paddle\r" if args.action == "paddle" else b"\r")
        while True:
            ready, _, _ = select.select([stdin, console.fd], [], [])
            if console.fd in ready:
                data = console._read_some()
                if data:
                    write_all(stdout, data)
                    scan = carry + data
                    pushed += sum(1 for m in KITTY_PUSH.finditer(scan) if m.end() > len(carry))
                    pushed -= sum(1 for m in KITTY_POP.finditer(scan) if m.end() > len(carry))
                    pushed = max(pushed, 0)
                    carry = scan[-16:]
            if stdin in ready:
                keys = os.read(stdin, 1024)
                if not keys:
                    return 0
                cut = quit_at(keys)
                if cut is not None:
                    if cut:
                        write_all(console.fd, keys[:cut])
                    return 0
                write_all(console.fd, keys)
    finally:
        write_all(stdout, b"\x1b[<u" * pushed + TERMINAL_RESET)
        termios.tcsetattr(stdin, termios.TCSAFLUSH, saved)


def fields(lines: list[str]) -> dict[str, str]:
    """Every key=value token in a reply, flattened into one mapping."""
    out: dict[str, str] = {}
    for line in lines:
        for token in line.split():
            if "=" in token:
                key, value = token.split("=", 1)
                out[key] = value
    return out


def setting_ids(console: Console) -> dict[str, int]:
    """Panel setting name -> numeric id, which is what `set` takes.

    `set list` prints `<id> "<NAME>" <value> <range> <style>`, so the names in
    the menus can be used directly instead of ids that move between builds.
    """
    out: dict[str, int] = {}
    for line in console.command("set list"):
        match = re.match(r'\s*(\d+)\s+"([^"]*)"', line)
        if match:
            out[match.group(2)] = int(match.group(1))
    return out


def cmd_setting(console: Console, args: argparse.Namespace) -> int:
    ids = setting_ids(console)
    if args.name not in ids:
        matches = [n for n in ids if args.name.lower() in n.lower()]
        hint = ("\n  did you mean: " + ", ".join(sorted(matches)[:8])) if matches else ""
        raise SystemExit(f"rev1_diag: no setting called {args.name!r}{hint}")
    identifier = ids[args.name]
    verb = "put" if args.value is not None else "get"
    tail = f" {args.value}" if args.value is not None else ""
    for line in console.command(f"set {verb} {identifier}{tail}"):
        print(line)
    if args.save:
        console.command("save")
        print("saved")
    return 0


def scaled(values: dict[str, str], key: str, divisor: float) -> float | None:
    """A fixed-point console field as a float, or None when absent."""
    raw = values.get(key)
    if raw is None or raw == "-":
        return None
    try:
        return int(raw) / divisor
    except ValueError:
        return None


def poll_until_idle(console: Console, command: str, period: float = 0.2,
                    limit: float = 90.0) -> dict[str, str]:
    """Repeat `command` until it reports active=0, then return its fields."""
    deadline = time.monotonic() + limit
    while True:
        values = fields(console.command(command))
        if values.get("active", "0") == "0":
            return values
        if time.monotonic() > deadline:
            raise SystemExit(f"rev1_diag: {command} did not finish within {limit:.0f}s")
        time.sleep(period)


def show_rxcal(values: dict[str, str]) -> None:
    if values.get("result") != "1":
        collect = values.get("collect")
        progress = f", collect {collect} samples, {values.get('skip_halves', '?')} settle halves left" if collect else ""
        print(f"  no result: {values.get('message', '?')}{progress}")
        return
    delta = values.get("delta_hz", "?")
    print(f"  injected {delta} Hz from the LO ({values.get('lo_hz', '?')} Hz), "
          f"carrier {scaled(values, 'wanted_ddbfs', 10.0)} dBFS "
          f"over a {scaled(values, 'floor_ddbfs', 10.0)} dBFS floor")
    print(f"  imbalance  {scaled(values, 'amp_ddb', 10.0)} dB amplitude, "
          f"{scaled(values, 'phase_mdeg', 1000.0)} deg phase, "
          f"pair {values.get('orientation', '?')}")
    print(f"  image      {scaled(values, 'image_ddb', 10.0)} dB raw -> "
          f"{scaled(values, 'corrected_image_ddb', 10.0)} dB corrected")
    opposite = scaled(values, "opposite_ddb", 10.0)
    if opposite is not None:
        print(f"  opposite sideband {opposite} dB")
    if values.get("clipped") == "1":
        print("  WARNING: the ADC clipped; the numbers are not trustworthy")
    print(f"  coefficients i={int(values['i_gain_x1000']) / 1000.0} "
          f"q={int(values['q_gain_x1000']) / 1000.0} "
          f"skew={int(values['skew_x1000']) / 1000.0} samples "
          f"invert={values.get('invert', '?')}")


def cmd_rxcal(console: Console, args: argparse.Namespace) -> int:
    """Measure the QSD I/Q imbalance, optionally several times, then apply."""
    if args.status:
        show_rxcal(fields(console.command("rxcal status")))
        return 0
    runs = []
    for attempt in range(args.repeat):
        console.command("rxcal run sb" if args.sb else "rxcal run")
        values = poll_until_idle(console, "rxcal status", limit=30.0)
        if values.get("status") != "done":
            raise SystemExit(f"rev1_diag: rxcal {values.get('status')}: {values.get('message')}")
        print(f"run {attempt + 1}:")
        show_rxcal(values)
        runs.append(values)
    if len(runs) > 1:
        for key, divisor, unit in (("amp_ddb", 10.0, "dB"), ("phase_mdeg", 1000.0, "deg"),
                                   ("corrected_image_ddb", 10.0, "dB")):
            taken = [scaled(r, key, divisor) for r in runs]
            taken = [v for v in taken if v is not None]
            if taken:
                print(f"spread {key}: {min(taken):.3f} .. {max(taken):.3f} {unit}")
    if args.apply:
        console.command("rxcal apply")
        console.command("save")
        print("applied and saved")
    return 0


def cmd_sweep(console: Console, args: argparse.Namespace) -> int:
    """Run the receive band-pass sweep and read its curves back."""
    if not args.status:
        console.command(f"sweep {args.axis}")
        values = poll_until_idle(console, "sweep status", period=1.0, limit=args.timeout)
    else:
        values = fields(console.command("sweep status"))
    for line in console.command("sweep status"):
        print(line)
    if args.table:
        for line in console.command("sweep table"):
            print(line)
    if args.apply:
        if values.get("fit") != "1":
            raise SystemExit("rev1_diag: the last sweep did not fit crossovers; not applying")
        console.command("sweep apply")
        console.command("save")
        print("applied and saved")
    return 0


def cmd_afsweep(console: Console, args: argparse.Namespace) -> int:
    """QMX 8.6.1 audio filter sweep, or 8.6.3 image sweep with --image.

    Both need a 50 ohm dummy load and USB or LSB, as QMX says of its own.
    """
    name = "imagesweep" if args.image else "afsweep"
    if not args.status:
        console.command(f"{name} run")
        values = poll_until_idle(console, f"{name} status", period=0.5, limit=args.timeout)
        if values.get("message", "").upper().startswith(("NO ", "SET ", "IMAGE OUT", "CLOCK")):
            raise SystemExit(f"rev1_diag: {name}: {values.get('message')}")
    for line in console.command(f"{name} status"):
        print(line)
    if args.table:
        for line in console.command(f"{name} table"):
            print(line)
    return 0


def cmd_iq(console: Console, args: argparse.Namespace) -> int:
    """QMX 8.6.7 Test ADC I/Q.

    The first call starts the injection and the converter's group delay means
    it has not arrived yet, so ask twice and keep the second.
    """
    command = "iq raw" if args.raw else "iq"
    text = console.command(command + " rearm")
    if "ready=0" in "\n".join(text):
        text = console.command(command)
    for line in text if args.table else text[:3]:
        print(line)
    return 0


def cmd_mic(console: Console, args: argparse.Namespace) -> int:
    """QMX 8.6.9 microphone test."""
    for line in console.command(f"mic {args.source} {args.rows}"):
        print(line)
    return 0


def cmd_micref(console: Console, args: argparse.Namespace) -> int:
    """Set QMX same-ADC microphone reference cancellation percentage."""
    for line in console.command(f"mic ref {args.percent}"):
        print(line)
    return 0


def cmd_micsplit(console: Console, args: argparse.Namespace) -> int:
    """Stream microphone and reference as left/right instead of cancelling."""
    for line in console.command(f"mic split {args.on}"):
        print(line)
    return 0


def cmd_bench(console: Console, args: argparse.Namespace) -> int:
    """The whole ordered receive bench, on stdout.

    Everything here needs a 50 ohm dummy load on the antenna connector, which
    is what QMX asks for too. Nothing transmits. Redirect to keep a report:
    this script is one of the read-only tools and source_policy_check enforces
    that, so it writes no files of its own.
    """
    del args

    def say(text: str = "") -> None:
        print(text)

    def run(command: str, limit: float = 0.0) -> None:
        say(f"$ {command}")
        if limit:
            console.command(command)
            base = command.split()[0]
            poll_until_idle(console, f"{base} status", period=1.0, limit=limit)
            command = f"{base} status"
        for line in console.command(command):
            say("  " + line)
        say()

    say(f"REV1 receive bench, {time.strftime('%Y-%m-%d %H:%M:%S')}")
    say("50 ohm dummy load required; nothing here transmits.")
    say()
    for command in ("id", "status", "diag", "telem", "pins", "keys", "rx",
                    "pd", "pd raw", "pipeline", "faults", "cpu", "mem", "sd status"):
        run(command)
    # Run the measuring instruments before the roll-up. Several self-test
    # rows intentionally consume the last BPF sweep, RX calibration and AF
    # sweep from this session; running the roll-up first made those rows skip.
    run("sweep lpf", limit=120.0)
    run("sweep run", limit=120.0)
    run("sweep rf", limit=120.0)
    run("rxcal run sb", limit=30.0)
    run("iq")
    run("iq")
    # `iq` leaves its test clock injected, exactly as QMX's screen does, and
    # every sweep below arms its own injection and refuses to start while one
    # is running. Without this the AF and image sweeps and the whole roll-up
    # after them report "needs receive, or the radio is busy".
    run("iq stop")
    run("afsweep run", limit=60.0)
    run("imagesweep run", limit=60.0)
    for source in (1, 2, 3):
        run(f"mic {source} 20")
    run("selftest run", limit=300.0)
    for line in console.command("selftest results"):
        say("  " + line)
    say()
    # Last, so they cover everything above: a sweep that stole time from the
    # audio halves shows up here as overruns that the opening `cpu` could not
    # have seen yet.
    for command in ("cpu", "faults", "status"):
        run(command)
    say("Curves are on the SD card under EXPORTS/ (the console `sd` command")
    say("reads them back as hex, or move the card to a PC), then read them")
    say("back against QRP Labs' own figures with")
    say("  tools/rx_audio_analyze.py <file>.CSV --compare-qmx")
    return 0


def find_capture_card(match: str, timeout: float) -> str | None:
    """ALSA card index whose description names the USB audio personality."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with open("/proc/asound/cards", encoding="utf-8", errors="replace") as handle:
                text = handle.read()
        except OSError:
            text = ""
        index = None
        for line in text.splitlines():
            head = re.match(r"\s*(\d+)\s+\[", line)
            if head:
                index = head.group(1)
            if index is not None and match.lower() in line.lower():
                return index
        time.sleep(0.25)
    return None


def wait_for_port(explicit: str | None, timeout: float) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ports = [explicit] if explicit else sorted(glob.glob("/dev/ttyACM*"))
        for port in ports:
            if port and os.path.exists(port):
                return port
        time.sleep(0.25)
    raise SystemExit("rev1_diag: the console did not come back; re-open it from the panel")


def cmd_audio(console: Console, args: argparse.Namespace) -> int:
    """Hand the port to USB audio, capture, and let the console come back.

    The console and USB audio are separate personalities, so this is the only
    way to record the receiver without an operator at the LCD.
    """
    if args.iq is not None:
        ids = setting_ids(console)
        if "USB RAW IQ" not in ids:
            raise SystemExit("rev1_diag: this firmware has no USB RAW IQ setting")
        console.command(f"set put {ids['USB RAW IQ']} {1 if args.iq else 0}")
        print(f"USB RAW IQ {'on' if args.iq else 'off'}")
    # Enumeration consumes part of the firmware's audio window and ALSA needs
    # the full requested duration after the card appears. Leave a fixed margin
    # rather than racing the automatic return to the diagnostic personality.
    window_seconds = args.seconds + 8
    if window_seconds > 600:
        raise SystemExit("rev1_diag: capture plus USB handoff margin exceeds 600 seconds")
    console.command(f"audio {window_seconds}")
    # The firmware switches 50 ms after the final reply packet drains. Keep
    # DTR open through that deadline; closing immediately can make
    # USBD_CdcMinIsOpen() false before USB_DiagService performs the handoff.
    time.sleep(0.15)
    console.close()
    print(f"switched to USB audio for a {window_seconds}s window")
    card = find_capture_card(args.match, timeout=15.0)
    if card is None:
        print(f"no ALSA capture card matching {args.match!r} appeared", file=sys.stderr)
    elif args.out:
        command = ["arecord", "-D", f"hw:{card},0", "-f", "S16_LE", "-r", "48000",
                   "-c", "2", "-d", str(args.seconds), args.out]
        print(" ".join(command))
        subprocess.run(command, check=False)
    else:
        print(f"capture card is hw:{card},0")
    port = wait_for_port(args.port, timeout=window_seconds + 30.0)
    time.sleep(0.5)  # let the CDC interface settle after enumeration
    print(f"console is back on {port}")
    return 0


def _kernel_frequency_ppm() -> float | None:
    """The kernel's frequency correction for this PC's crystal (adjtimex)."""
    try:
        import ctypes
        import ctypes.util

        class Timex(ctypes.Structure):
            _fields_ = [("modes", ctypes.c_uint), ("offset", ctypes.c_long), ("freq", ctypes.c_long),
                        ("maxerror", ctypes.c_long), ("esterror", ctypes.c_long), ("status", ctypes.c_int),
                        ("constant", ctypes.c_long), ("precision", ctypes.c_long),
                        ("tolerance", ctypes.c_long), ("time_sec", ctypes.c_long),
                        ("time_usec", ctypes.c_long), ("tick", ctypes.c_long), ("ppsfreq", ctypes.c_long),
                        ("jitter", ctypes.c_long), ("shift", ctypes.c_int), ("stabil", ctypes.c_long),
                        ("jitcnt", ctypes.c_long), ("calcnt", ctypes.c_long), ("errcnt", ctypes.c_long),
                        ("stbcnt", ctypes.c_long), ("tai", ctypes.c_int), ("pad", ctypes.c_int * 11)]
        libc = ctypes.CDLL(ctypes.util.find_library("c"))
        t = Timex()
        if libc.adjtimex(ctypes.byref(t)) < 0:
            return None
        return t.freq / 65536.0
    except (OSError, AttributeError):
        return None


def host_crystal_estimate() -> tuple[float | None, float, str]:
    """How fast this PC's crystal runs, in ppm, how sure that is, and whence.

    CLOCK_MONOTONIC_RAW is the bare crystal, never slewed; the NTP daemon's
    frequency correction is its estimate of that crystal's error. chrony reports
    the estimate's uncertainty (skew). systemd-timesyncd does not, so it is
    bounded by the last NTP offset over the poll interval -- large for an hour
    or more after a boot, when refcal must not be trusted.
    """
    try:
        out = subprocess.run(["chronyc", "-c", "tracking"], capture_output=True, text=True,
                             timeout=3).stdout.strip().split(",")
        if len(out) > 10:
            # chrony CSV: field 7 is the frequency error in ppm (positive when
            # the clock runs slow, i.e. the correction applied), field 9 the
            # skew, chrony's own bound on that estimate.
            return float(out[7]), float(out[9]), "chrony"
    except (OSError, subprocess.SubprocessError, ValueError):
        pass
    freq = _kernel_frequency_ppm()
    try:
        out = subprocess.run(["timedatectl", "timesync-status"], capture_output=True, text=True,
                             timeout=3).stdout
        offset_ms = poll_s = None
        for line in out.splitlines():
            key, _, value = line.partition(":")
            key, value = key.strip(), value.strip()
            if key == "Offset":
                number = float(re.sub(r"[^0-9.+-]", "", value.replace(",", ".")) or "nan")
                offset_ms = number / 1000.0 if value.endswith("us") else number
            if key == "Poll interval":
                minutes = re.search(r"(\d+)min", value)
                seconds = re.search(r"(\d+)s", value)
                poll_s = (int(minutes.group(1)) * 60 if minutes else 0) + (int(seconds.group(1)) if seconds else 0)
        if freq is not None and offset_ms is not None and poll_s:
            return freq, abs(offset_ms) * 1e3 / poll_s, "systemd-timesyncd"
    except (OSError, subprocess.SubprocessError, ValueError):
        pass
    return freq, float("inf"), "unknown NTP daemon"


def cmd_refcal(console: Console, args: argparse.Namespace) -> int:
    """Measure the reference oscillator against this computer's clock.

    The radio's frequencies are all ratios of its 8 MHz MEMS reference (+-25
    ppm). `clock stamp` returns TIM5 -- a free-running count of SYSCLK -- as it
    stood when the command's USB packet arrived. Those stamps are timed against
    CLOCK_MONOTONIC_RAW, the PC's bare crystal, which the NTP daemon never slews:
    timing them against the wall clock instead picked up the daemon's offset
    slewing, tens of ppm right after a boot. The fit gives the radio against the
    PC's crystal to a few hundredths of a ppm; the NTP daemon's estimate of that
    crystal turns it into an absolute figure, and is the accuracy limit.
    """
    raw = time.CLOCK_MONOTONIC_RAW
    first = console.command("clock stamp")
    fields = dict(tok.split("=", 1) for tok in first[0].split() if "=" in tok)
    m, n, r = (int(v) for v in fields["pll"].split("/"))
    nominal = 8_000_000 * n / (m * r)
    key = (fields["pll"], fields["retunes"])
    print(f"SYSCLK {nominal / 1e6:.6f} MHz nominal (PLL {fields['pll']}), stored correction "
          f"{int(fields['ref_ppb']) / 1000:+.3f} ppm; sampling for {args.seconds:.0f} s")
    samples: list[tuple[float, float, int]] = []
    start = time.clock_gettime_ns(raw)
    deadline = time.monotonic() + args.seconds
    unwrapped = None
    last_t = None
    last_report = time.monotonic()
    while time.monotonic() < deadline:
        t0 = time.clock_gettime_ns(raw)
        lines = console.command("clock stamp")
        t1 = time.clock_gettime_ns(raw)
        fields = dict(tok.split("=", 1) for tok in lines[0].split() if "=" in tok)
        if (fields["pll"], fields["retunes"]) != key:
            raise SystemExit("rev1_diag: the PLL retuned during the measurement; keep the radio idle "
                             "on one frequency and run refcal again")
        stamp = int(fields["stamp"])
        t = (t0 - start) / 1e9
        if unwrapped is None:
            unwrapped = stamp
        else:
            predicted = unwrapped + nominal * (t - last_t)
            unwrapped = stamp + round((predicted - stamp) / 2**32) * 2**32
        last_t = t
        samples.append((t, (t1 - t0) / 1e6, unwrapped))
        if time.monotonic() - last_report >= 30.0:
            last_report = time.monotonic()
            print(f"  {t:5.0f} s, {len(samples)} samples")
        time.sleep(args.interval)
    if len(samples) < 20:
        raise SystemExit("rev1_diag: too few samples; is the console busy?")
    # A stamp is taken when the packet reaches the radio, a little after t0.
    # The quickest round trips are the ones where that delay was smallest.
    rtts = sorted(s[1] for s in samples)
    cut = rtts[max(10, int(len(rtts) * args.keep)) - 1]
    points = [(s[0], float(s[2] - samples[0][2])) for s in samples if s[1] <= cut]
    for _ in range(3):
        k = len(points)
        mt = sum(p[0] for p in points) / k
        mc = sum(p[1] for p in points) / k
        stt = sum((p[0] - mt) ** 2 for p in points)
        slope = sum((p[0] - mt) * (p[1] - mc) for p in points) / stt
        residual = [p[1] - (mc + slope * (p[0] - mt)) for p in points]
        sigma = (sum(e * e for e in residual) / max(1, k - 2)) ** 0.5
        kept = [p for p, e in zip(points, residual) if abs(e) <= 3.0 * sigma]
        if len(kept) == len(points):
            break
        points = kept
    fit_sigma = sigma / stt ** 0.5 / nominal * 1e6
    vs_crystal = (slope / nominal - 1.0) * 1e6
    print(f"fit: {len(points)} of {len(samples)} samples over {samples[-1][0]:.0f} s, "
          f"residual {sigma / nominal * 1e6:.1f} us rms, round trip <= {cut:.2f} ms")
    print(f"radio against this PC's crystal: {vs_crystal:+.3f} ppm (+-{fit_sigma:.3f} statistical)")
    host, host_sigma, source = host_crystal_estimate()
    if host is None:
        print("no NTP frequency estimate for this PC: the absolute error is unknown")
        return 1
    # The daemon's correction speeds the crystal up by `host` ppm to keep true
    # time, so the crystal runs `host` ppm slow and the radio is that much less
    # fast than it looks against it.
    ppm = vs_crystal - host
    uncertainty = (fit_sigma ** 2 + host_sigma ** 2) ** 0.5
    print(f"{source}: PC crystal correction {host:+.3f} ppm, +-{host_sigma:.3f} ppm")
    print(f"reference {ppm:+.3f} ppm (+-{uncertainty:.3f}): {14.074 * ppm:+.1f} Hz at 14.074 MHz, "
          f"{28.074 * ppm:+.1f} Hz at 28.074 MHz")
    ppb = round(ppm * 1000.0)
    if uncertainty > args.max_uncertainty and not args.force:
        print(f"not applied: the uncertainty is above {args.max_uncertainty} ppm. The PC's NTP daemon has "
              "not settled (usually the first hours after a boot); chrony settles faster and reports its "
              "error. Run again later, or pass --force to store it anyway.")
        return 1 if args.apply else 0
    if args.apply:
        for line in console.command(f"clock ppb {ppb}")[:1]:
            print(line)
        console.command("save")
        print(f"stored REF CAL {ppb / 1000:+.3f} ppm and saved")
    else:
        print(f"to store it: tools/rev1_diag.py raw 'clock ppb {ppb}' && tools/rev1_diag.py raw save"
              "  (or refcal --apply)")
    return 0


def cmd_diagnose(console: Console, _args: argparse.Namespace) -> int:
    """Everything worth reading when something has gone wrong, in order."""
    for heading, command in (("what the radio is doing", "status"),
                             ("recent pipeline events, newest first", "events"),
                             ("pipeline health counters", "pipeline")):
        print(f"== {heading}")
        for line in console.command(command):
            print("  " + line)
        print()
    return 0


# `pd log` rows are "ms.us kind a b c". Kinds below 100 are ST's TRACE_EVENT
# (a = its sop/state argument, b = the first two payload bytes, c = the payload
# size); from 100 up they are radio_pd.c's own (PDL_* there).
PD_KINDS = {1: "msg-in", 2: "msg-out", 3: "cad", 4: "pe-state", 9: "notify", 16: "timeout",
            100: "sense", 101: "attach", 102: "detach", 103: "hard-reset", 104: "cad-event",
            105: "notify", 106: "request", 107: "vbus", 108: "q2-hold", 109: "caps",
            110: "soft-reset", 111: "boot"}
PD_CTRL = {1: "GoodCRC", 2: "GotoMin", 3: "Accept", 4: "Reject", 5: "Ping", 6: "PS_RDY",
           7: "Get_Source_Cap", 8: "Get_Sink_Cap", 9: "DR_Swap", 10: "PR_Swap",
           11: "VCONN_Swap", 12: "Wait", 13: "Soft_Reset", 16: "Not_Supported",
           17: "Get_Source_Cap_Extended", 18: "Get_Status", 19: "FR_Swap",
           20: "Get_PPS_Status"}
PD_DATA = {1: "Source_Capabilities", 2: "Request", 3: "BIST", 4: "Sink_Capabilities",
           5: "Battery_Status", 6: "Alert", 15: "Vendor_Defined"}
PD_EXT = {1: "Source_Capabilities_Extended", 2: "Status", 12: "PPS_Status"}
PD_OUTCOME = ("none", "no-source", "seen", "active", "won", "brownout", "timeout")
PD_BAND = ("<0.8V", "<3.67V", "<4.4V", "vSafe5V", "5.5-7.5V", "7.5-10.5V", ">10.5V")


def pd_header(h: int) -> str:
    kind, count = h & 0x1F, (h >> 12) & 7
    table = PD_EXT if h & 0x8000 else (PD_DATA if count else PD_CTRL)
    return f"{table.get(kind, f'type {kind}')} id={(h >> 9) & 7}"


def pd_row(ms: int, us: int, kind: int, a: int, b: int, c: int) -> str:
    name = PD_KINDS.get(kind, f"kind {kind}")
    if kind in (1, 2):
        what = pd_header(b) if c >= 2 else ("Hard_Reset" if a == 5 else f"sop {a}")
    elif kind == 100:
        what = f"cc sense {'CC1' if b & 3 else 'CC2' if b >> 2 & 3 else 'no source'}, {c} mV"
    elif kind == 101:
        what = f"CC{a}{' (contract kept over a restart)' if b else ''}, {c} mV"
    elif kind == 102:
        what = f"{PD_OUTCOME[a] if a < len(PD_OUTCOME) else a}, {c} mV"
    elif kind == 103:
        what = f"status {a}, {c} mV"
    elif kind == 106:
        what = f"object {a}: {b} mV {c} mA"
    elif kind == 107:
        what = f"{PD_BAND[a] if a < len(PD_BAND) else a} ({c} mV)"
    elif kind == 108:
        what = f"PB10 {'high (Q2 on)' if a else 'low (Q2 open: back-feed)'}, {c} mV"
    elif kind == 109:
        what = f"{a} objects, {c} mV"
    elif kind == 110:
        what = f"{'sent' if a == 1 else 'failed'}, {c} mV"
    elif kind == 111:
        what = f"reset flags {a << 24:08X}, {c} mV"
    elif kind in (4, 9, 105):
        what = f"{a}"
    else:
        what = f"a={a} b={b:04X} c={c}"
    return f"{ms:9d}.{us:05d}  {name:10s} {what}"


def cmd_pdlog(console: Console, _args: argparse.Namespace) -> int:
    """The PD event log, decoded; it survives a reset but not a power loss."""
    for line in console.command("pd log"):
        parts = line.split()
        try:
            ms, us = (int(x) for x in parts[0].split("."))
            kind, a, b, c = int(parts[1]), int(parts[2]), int(parts[3], 16), int(parts[4])
        except (ValueError, IndexError):
            print(line)
            continue
        if kind == 111:
            print("-- boot --")
        print(pd_row(ms, us, kind, a, b, c))
    return 0


# The console refuses lines of TERM_LINE_MAX (120) characters or more, and
# `sd put` takes at most 64 bytes a line.
CONSOLE_LINE_MAX = 119
SD_PUT_BLOCK_MAX = 64


def cmd_sdput(console: Console, args: argparse.Namespace) -> int:
    """Copy a host file onto the radio's SD card in console-sized blocks."""
    data = Path(args.local).read_bytes()
    remote = args.remote or Path(args.local).name.upper()
    prefix = len(f"sd put {remote} {len(data)} ")
    block = min(SD_PUT_BLOCK_MAX, (CONSOLE_LINE_MAX - prefix) // 2)
    if block < 8:
        print(f"{remote}: name too long for the console", file=sys.stderr)
        return 1
    for off in range(0, max(len(data), 1), block):
        reply = console.command(f"sd put {remote} {off} {data[off:off + block].hex()}")
        if any(line.startswith("ERR") for line in reply):
            print(f"{remote}: write failed at {off}: {reply}", file=sys.stderr)
            return 1
    back = console.command(f"sd stat {remote}")
    print(f"{remote}: {len(data)} bytes written; {' '.join(back)}")
    return 0


def sd_get(console: Console, remote: str) -> bytes:
    """A file off the radio's SD card through `sd get`, every 32-byte line
    checked against the CRC the radio sends with it, and the whole against the
    file's size (firmware before v8.09 ended a read cut short with OK)."""
    stat = dict(kv.split("=", 1) for line in console.command(f"sd stat {remote}") for kv in line.split() if "=" in kv)
    size = int(stat.get("size", -1))
    data = bytearray()
    for line in console.command(f"sd get {remote}"):
        parts = line.split()
        if len(parts) != 3:
            raise SystemExit(f"rev1_diag: unexpected `sd get` line {line!r}")
        offset, chunk = int(parts[0]), bytes.fromhex(parts[1])
        if offset != len(data):
            raise SystemExit(f"rev1_diag: {remote}: a gap at {len(data)} (line for {offset})")
        if zlib.crc32(chunk) != int(parts[2], 16):
            raise SystemExit(f"rev1_diag: {remote}: CRC mismatch at {offset}")
        data += chunk
    if size >= 0 and len(data) != size:
        raise SystemExit(f"rev1_diag: {remote}: {len(data)} of {size} bytes read")
    return bytes(data)


def cmd_sdget(console: Console, args: argparse.Namespace) -> int:
    data = sd_get(console, args.remote)
    local = Path(args.local or Path(args.remote).name)
    local.write_bytes(data)
    print(f"{args.remote}: {len(data)} bytes to {local}")
    return 0


def cmd_storage_backup(console: Console, args: argparse.Namespace) -> int:
    """`storage backup` on the radio, then SYSTEM/STOREBAK.BIN to this PC:
    every settings and calibration record of the radio's store."""
    console.command("storage backup")
    data = sd_get(console, "SYSTEM/STOREBAK.BIN")
    uid = next((l.split("=", 1)[1] for l in console.command("id") if l.startswith("uid=")), "radio")
    if args.out:
        local = Path(args.out)
    else:
        folder = Path(__file__).resolve().parent.parent / "backups"
        folder.mkdir(exist_ok=True)
        local = folder / f"rev1_{uid}_{time.strftime('%Y%m%d-%H%M%S')}.STOREBAK.BIN"
    local.write_bytes(data)
    entries = int.from_bytes(data[8:12], "little") if len(data) >= 12 else 0
    print(f"store backup: {len(data)} bytes, {entries} records, to {local}")
    return 0


def cmd_storage_restore(console: Console, args: argparse.Namespace) -> int:
    """A store backup (STOREBAK.BIN from `storage-backup`) back into the
    radio's store: copied to SYSTEM/RESTORE.BIN, checked whole by the radio
    (this radio's UID, every CRC) and only then written."""
    sdput = argparse.Namespace(local=args.file, remote="SYSTEM/RESTORE.BIN")
    try:
        console.command("sd mkdir SYSTEM")
    except SystemExit:
        pass  # already there
    if cmd_sdput(console, sdput) != 0:
        return 1
    for line in console.command("storage restore"):
        print(line)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", help="serial device (default: first /dev/ttyACM*)")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    sub = parser.add_subparsers(dest="action", required=True)

    for simple in ("id", "status", "events", "pipeline", "telem", "pins",
                   "keys", "rx", "pd", "faults", "mem", "cpu", "diag"):
        sub.add_parser(simple, help=f"run `{simple}` and print the reply")

    st = sub.add_parser("selftest", help="run the self test and print the table")
    st.add_argument("groups", nargs="*", default=[],
                    help="power clock controls storage filters rx analog | all")

    watch = sub.add_parser("watch", help="repeat a command until interrupted")
    watch.add_argument("what", nargs="?", default="telem")
    watch.add_argument("--interval", type=float, default=1.0)

    rxcal = sub.add_parser("rxcal", help="measure and correct the QSD I/Q imbalance")
    rxcal.add_argument("--sb", action="store_true",
                       help="also measure opposite-sideband rejection (USB/LSB only)")
    rxcal.add_argument("--repeat", type=int, default=1, help="runs, to show the spread")
    rxcal.add_argument("--apply", action="store_true", help="store the coefficients and save")
    rxcal.add_argument("--status", action="store_true", help="print the last result only")

    sweep = sub.add_parser("sweep", help="run a receive filter sweep")
    sweep.add_argument("axis", nargs="?", default="run", choices=("run", "lpf", "rf"),
                       help="whole-range band-pass, per-slot low-pass, or this band")
    sweep.add_argument("--table", action="store_true", help="print the per-point curves")
    sweep.add_argument("--apply", action="store_true", help="store the fitted edges and save")
    sweep.add_argument("--status", action="store_true", help="print the last run only")
    sweep.add_argument("--timeout", type=float, default=90.0)

    afs = sub.add_parser("afsweep", help="audio passband and opposite sideband, QMX 8.6.1")
    afs.add_argument("--image", action="store_true", help="image sweep instead, QMX 8.6.3")
    afs.add_argument("--table", action="store_true", help="print the per-point curves")
    afs.add_argument("--status", action="store_true", help="print the last run only")
    afs.add_argument("--timeout", type=float, default=60.0)

    iq = sub.add_parser("iq", help="raw PCM1821 I/Q samples and quadrature, QMX 8.6.7")
    iq.add_argument("--table", action="store_true", help="print the samples as well")
    iq.add_argument("--raw", action="store_true", help="do not inject a carrier")

    mic = sub.add_parser("mic", help="microphone level rows for setting gain, QMX 8.6.9")
    mic.add_argument("source", nargs="?", type=int, default=1, choices=(1, 2, 3))
    mic.add_argument("rows", nargs="?", type=int, default=20)

    micref = sub.add_parser("micref", help="set QMX microphone reference cancellation")
    micref.add_argument("percent", type=int, choices=range(0, 101),
                        help="reference subtraction coefficient, percent (QMX example: 68)")
    micref.set_defaults(func=cmd_micref)

    micsplit = sub.add_parser("micsplit",
                              help="monitor the microphone and its reference as left/right")
    micsplit.add_argument("on", type=int, choices=(0, 1))
    micsplit.set_defaults(func=cmd_micsplit)

    sub.add_parser("bench", help="the whole ordered receive bench on stdout; redirect to keep it")


    audio = sub.add_parser("audio", help="capture over USB audio, then return to the console")
    audio.add_argument("seconds", type=int)
    audio.add_argument("--out", help="write a WAV here with arecord")
    audio.add_argument("--iq", type=int, choices=(0, 1), default=None,
                       help="set USB RAW IQ before switching")
    audio.add_argument("--match", default=AUDIO_PRODUCT, help="ALSA card description to look for")

    setting = sub.add_parser("setting", help="read or write a panel setting by name")
    setting.add_argument("name", help='menu name, e.g. "CARRIER MHZ"')
    setting.add_argument("value", nargs="?", default=None)
    setting.add_argument("--save", action="store_true", help="persist afterwards")

    refcal = sub.add_parser("refcal", help="measure the 8 MHz reference against this PC's NTP clock")
    refcal.add_argument("--seconds", type=float, default=180.0,
                        help="measurement length (default 180; 0.1 ppm needs about 2 minutes)")
    refcal.add_argument("--interval", type=float, default=0.2, help="seconds between samples")
    refcal.add_argument("--keep", type=float, default=0.5,
                        help="fraction of samples with the quickest round trip to fit")
    refcal.add_argument("--apply", action="store_true", help="store the result as REF CAL and save")
    refcal.add_argument("--max-uncertainty", type=float, default=0.5,
                        help="refuse to store a result less certain than this, ppm (default 0.5)")
    refcal.add_argument("--force", action="store_true", help="store even when the PC clock is unsettled")

    sub.add_parser("console", help="interactive prompt")
    sub.add_parser("term", help="raw terminal to the console, full-screen menu included; Ctrl-] quits")
    sub.add_parser("paddle", help="virtual paddle: , is DIT, . is DAH, t for TX; Ctrl-] quits")
    sub.add_parser("diagnose", help="status, recent events and pipeline health in one go")
    sub.add_parser("pdlog", help="the USB-PD event log, decoded (survives a reset)")

    sdput = sub.add_parser("sdput", help="copy a host file onto the radio's SD card")
    sdput.add_argument("local")
    sdput.add_argument("remote", nargs="?", help="card path (default: the file's name)")
    sub.add_parser("helpinstall", help="put this tree's sd/HELP.BIN (the settings help) on the card")
    sdget = sub.add_parser("sdget", help="copy a file off the radio's SD card")
    sdget.add_argument("remote", help="card path")
    sdget.add_argument("local", nargs="?", help="host file (default: the file's name)")
    sbak = sub.add_parser("storage-backup",
                          help="back the settings store up (storage backup) and fetch STOREBAK.BIN")
    sbak.add_argument("--out", help="host file (default: backups/rev1_<uid>_<time>.STOREBAK.BIN)")
    sres = sub.add_parser("storage-restore", help="put a STOREBAK.BIN back into the radio's store")
    sres.add_argument("file")

    raw = sub.add_parser("raw", help="send one command verbatim")
    raw.add_argument("text")

    args = parser.parse_args()
    console = Console(find_port(args.port), args.timeout)
    try:
        if args.action == "selftest":
            return cmd_selftest(console, args)
        if args.action == "afsweep":
            return cmd_afsweep(console, args)
        if args.action == "iq":
            return cmd_iq(console, args)
        if args.action == "mic":
            return cmd_mic(console, args)
        if args.action == "micref":
            return cmd_micref(console, args)
        if args.action == "micsplit":
            return cmd_micsplit(console, args)
        if args.action == "bench":
            return cmd_bench(console, args)
        if args.action == "watch":
            return cmd_watch(console, args)
        if args.action == "setting":
            return cmd_setting(console, args)
        if args.action == "rxcal":
            return cmd_rxcal(console, args)
        if args.action == "sweep":
            return cmd_sweep(console, args)
        if args.action == "audio":
            return cmd_audio(console, args)
        if args.action == "console":
            return cmd_console(console, args)
        if args.action in ("term", "paddle"):
            return cmd_term(console, args)
        if args.action == "diagnose":
            return cmd_diagnose(console, args)
        if args.action == "refcal":
            return cmd_refcal(console, args)
        if args.action == "pdlog":
            return cmd_pdlog(console, args)
        if args.action == "sdput":
            return cmd_sdput(console, args)
        if args.action == "sdget":
            return cmd_sdget(console, args)
        if args.action == "storage-backup":
            return cmd_storage_backup(console, args)
        if args.action == "storage-restore":
            return cmd_storage_restore(console, args)
        if args.action == "helpinstall":
            args.local = str(Path(__file__).resolve().parent.parent / "sd" / "HELP.BIN")
            args.remote = "HELP.BIN"
            return cmd_sdput(console, args)
        text = args.text if args.action == "raw" else args.action
        for line in console.command(text):
            print(line)
        return 0
    finally:
        console.close()


if __name__ == "__main__":
    sys.exit(main())
