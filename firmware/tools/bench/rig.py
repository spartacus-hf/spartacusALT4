"""The radio on the bench: SWD (persistent openocd) plus the DIAG console.

Keying is always bounded:
  * CW / TX CARRIER TEST: SWD writes to cw_virtual_levers;
  * SSB / AM / FM from the USB sound card: the software mute (cfg_muted and
    mute_target, as MOD_SetMuted writes them) gates the audio.
Every key-down runs inside `with rig.guard():` -- a thread on its own SWD
connection that mutes and releases after GUARD_S (or the limit given) and
resets the MCU through AIRCR if the radio is still transmitting 0.4 s later.
Run transmit tests only into a 50 ohm dummy load.

Settings a script changes go through `with rig.keep(ids):`. The values are
recorded in RESULTS/settings_restore.json before anything changes and put back
on exit; if the script dies first (killed, timeout, lost console), the next
Rig() puts them back.
"""
import glob
import json
import os
import struct
import subprocess
import sys
import threading
import time

import common

AIRCR = 0xE000ED0C
RESET = 0x05FA0004
GUARD_S = 0.95
TX_STATES = (2, 3)            # TX_PREPARE, TRANSMIT
ONE_F32 = 0x3F800000
PENDING = common.result_path("settings_restore")


def _pending():
    try:
        with open(PENDING) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def _store_pending(p):
    if p:
        with open(PENDING, "w") as f:
            json.dump(p, f, indent=1)
    elif os.path.exists(PENDING):
        os.remove(PENDING)


class Rig:
    def __init__(self, console=True):
        self.sym = common.symbols()
        self.o = common.Ocd()
        self.wd = common.Ocd()      # the guard's own connection
        self.con = None
        if console:
            self.open_console()
            self.restore_pending()

    # ---------- plumbing ----------
    def A(self, name):
        return self.sym[name][0]

    def open_console(self, timeout=40.0):
        """The DIAG console. In the audio personality the CAT ports are
        ttyACM devices too, and the console leaves them when the audio
        window ends, so only the DIAG port by-id name is taken."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ports = sorted(glob.glob("/dev/serial/by-id/*SPARTACUS*DIAG*"))
            if ports:
                try:
                    time.sleep(0.5)
                    self.con = common.rev1_diag.Console(ports[0], timeout=4.0)
                    self.con.command("id")
                    return
                except Exception:
                    self.con = None
            time.sleep(0.3)
        raise IOError("the DIAG console did not come back")

    def close_console(self):
        if self.con:
            try:
                self.con.close()
            except Exception:
                pass
        self.con = None

    def cmd(self, text):
        return self.con.command(text)

    def kv(self, text):
        out = {}
        for line in self.cmd(text):
            for tok in line.split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    out.setdefault(k, v)
        return out

    def sget(self, sid):
        return self.cmd(f"set get {sid}")[0].split("=", 1)[1].strip()

    def sput(self, sid, value):
        self.cmd(f"set put {sid} {value}")

    def u8(self, n): return self.o.u8(self.A(n))
    def u32(self, n): return self.o.u32(self.A(n))

    def state(self, o=None):
        return (o or self.o).u8(self.A("state"))

    def health(self):
        return {"lock": self.u8("pipeline_hard_lockout"), "faults": self.u32("faults")}

    def telemetry(self):
        """radio_telemetry_t: millivolts (fwd/rev are the RF detectors)."""
        f = struct.unpack("<14H", self.o.rbytes(self.A("telemetry"), 28))
        return {"bias": f[6], "fwd": f[7], "rev": f[8], "rail5": f[9],
                "envA": f[10], "envB": f[11], "vin": f[12]}

    def reset(self, settle=7.0):
        """MCU reset through AIRCR; the console comes back after diagboot."""
        self.close_console()
        self.o.write(AIRCR, 32, RESET)
        time.sleep(settle)
        self.open_console(timeout=60)
        self.safe()

    # ---------- settings ----------
    def keep(self, ids):
        return Kept(self, ids)

    def restore_pending(self):
        """Put back settings a dead script left changed."""
        p = _pending()
        if not p:
            return {}
        done = {}
        for sid, v in p.items():
            self.sput(sid, v)
            done[sid] = v
        _store_pending({})
        print(f"rig: restored settings a previous run left changed: {done}", file=sys.stderr)
        return done

    # ---------- keying ----------
    def set_mute(self, on, o=None):
        o = o or self.o
        if on:
            o.write(self.A("cfg_muted"), 8, 1)
            o.write(self.A("mute_target"), 32, 0)
        else:
            o.write(self.A("mute_target"), 32, ONE_F32)
            o.write(self.A("cfg_muted"), 8, 0)

    def safe(self, o=None):
        """Mute and release every lever; True once both read back."""
        o = o or self.o
        for _ in range(10):
            try:
                self.set_mute(True, o)
                o.write(self.A("cw_virtual_levers"), 8, 0)
                if o.u8(self.A("cfg_muted")) == 1 and o.u8(self.A("cw_virtual_levers")) == 0:
                    return True
            except Exception as e:
                print("safe retry:", e, file=sys.stderr)
        return False

    def guard(self, limit=GUARD_S):
        return Guard(self, limit)

    def wait_rx(self, timeout=1.0):
        """Seconds until the radio leaves TX_PREPARE/TRANSMIT (None: still in TX)."""
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            if self.state() not in TX_STATES:
                return round(time.monotonic() - t0, 3)
            time.sleep(0.002)
        return None


class Kept:
    """Settings snapshot, put back on exit (see the module comment)."""

    def __init__(self, rig, ids):
        self.rig, self.ids = rig, [int(i) for i in ids]

    def __enter__(self):
        self.saved = {i: self.rig.sget(i) for i in self.ids}
        p = _pending()
        for i, v in self.saved.items():
            p.setdefault(str(i), v)           # an outer snapshot keeps its value
        _store_pending(p)
        return self

    def __exit__(self, *exc):
        r = self.rig
        if r.con is None:
            r.open_console(timeout=60)
        r.safe()
        for i, v in self.saved.items():
            r.sput(i, v)
        p = _pending()
        for i, v in self.saved.items():
            if p.get(str(i)) == v:
                p.pop(str(i))
        _store_pending(p)
        return False


class Guard:
    """Independent key-down limit on its own SWD connection."""

    def __init__(self, rig, limit=GUARD_S):
        if limit > 4.5:
            raise ValueError("key-downs stay under 5 s")
        self.rig, self.limit = rig, limit
        self.done = threading.Event()
        self.fired = None
        self.reset = False

    def _run(self):
        if self.done.wait(self.limit):
            return
        self.fired = time.monotonic()
        ok = self.rig.safe(self.rig.wd)
        t0 = time.monotonic()
        while time.monotonic() - t0 < 0.4:
            try:
                if self.rig.state(self.rig.wd) not in TX_STATES:
                    print(f"GUARD: key-down limit hit, muted/released (ok={ok})", file=sys.stderr)
                    return
            except Exception:
                pass
            time.sleep(0.01)
        print("GUARD: still in TX after mute/release -> MCU reset", file=sys.stderr)
        self.reset = True
        try:
            self.rig.wd.write(AIRCR, 32, RESET)
        except Exception as e:
            print("GUARD: reset write failed:", e, file=sys.stderr)
            subprocess.run(["pkill", "-x", "openocd"])

    def __enter__(self):
        self.th = threading.Thread(target=self._run, daemon=True)
        self.th.start()
        return self

    def __exit__(self, *exc):
        self.done.set()
        self.th.join(2)
        return False
