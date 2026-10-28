#!/usr/bin/env python3
"""WSJT-X's case, without WSJT-X: the radio's sound card open both ways (PipeWire
plays silence into it and records from it, a packet every millisecond, as WSJT-X
keeps it all the time it runs) while the dial moves over CAT as WSJT-X moves it
(FT8 and WSPR on 40 m and 20 m) and the mode changes (MD3, MD9). Over SWD, per
step: the carrier retune's state, its pending apply, the PLL clock, and how often
the blocking-I/O gate refused in 2 s (autosave, card, mode changes wait on it).
Then how long the captured audio went silent at each step.

Receive only: USB VOX must be off and nothing sends TX; -- safe on an antenna.
Opens an audio window from the DIAG console for the run.

Pass: every dial move applied (state idle, nothing pending) and AUTO's PLL move
done (upgrade_pending 0) within 4 s, no I/O refusals, MD3/MD9 taken, faults and
pipeline clean. Before 2026-10-03 the stream held all of it: a dial on another
PLL setting sat on RETUNE WAIT, an upgrade stayed pending and the I/O gate
refused every millisecond, for as long as the stream ran.

usage: cat_retune.py TAG  -> results/cat_retune_TAG.json
"""
import array, glob, os, select, subprocess, sys, termios, time, wave
import common
import health

rev1_diag = common.rev1_diag
tag = sys.argv[1]
STEPS = [(7074000, "FT8 40m"), (7038600, "WSPR 40m"), (14074000, "FT8 20m"), (14095600, "WSPR 20m"),
         (7074000, "FT8 40m"), (7038600, "WSPR 40m")]
WINDOW_S = 90


class Port:
    """A CAT port, raw."""
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        a[0] = a[1] = a[3] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        termios.tcflush(self.fd, termios.TCIOFLUSH)

    def close(self): os.close(self.fd)

    def cat(self, cmd, wait=0.8):
        os.write(self.fd, cmd.encode())
        out, t0 = b"", time.monotonic()
        while time.monotonic() - t0 < wait and not out.endswith(b";"):
            if select.select([self.fd], [], [], 0.05)[0]:
                out += os.read(self.fd, 64)
        return out.decode(errors="replace")


def node(kind):
    lines = subprocess.run(["pactl", "list", "short", kind], capture_output=True, text=True).stdout
    return next(l.split()[1] for l in lines.splitlines()
                if "Chireix_SPARTACUS" in l and ".monitor" not in l.split()[1])


c = rev1_diag.Console(rev1_diag.find_port(None), timeout=10.0)
vox = c.command("set get 222")[0]
if not vox.endswith("=0"):
    sys.exit(f"USB VOX is on ({vox}): this test must not transmit; set it off first")
c.command(f"audio {WINDOW_S}"); time.sleep(0.2); c.close()
t0 = time.monotonic(); ports = []
while time.monotonic() - t0 < 10 and len(ports) < 2:
    ports = sorted(p for p in glob.glob("/dev/serial/by-id/*SPARTACUS*") if "DIAG" not in p); time.sleep(0.2)
time.sleep(2.0)
silence = common.result_path("silence_120s", "wav")
if not os.path.exists(silence):
    w = wave.open(silence, "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
    w.writeframes(b"\0\0" * 48000 * 120); w.close()
capture = common.result_path(f"cat_retune_{tag}", "wav")
play = subprocess.Popen(["pw-play", "--target", node("sinks"), silence],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
rec = subprocess.Popen(["pw-record", "--target", node("sources"), "--rate", "48000", "--channels", "1", capture],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

S = common.symbols()
o = common.Ocd()


class _R:            # what health.health() reads through
    def __init__(self): self.o = o
    def A(self, name): return S[name][0]


def u8(n): return o.u8(S[n][0])
def u32(n): return o.u32(S[n][0])


def snap():
    return {"t": round(time.monotonic() - t_start, 2), "retune_state": u8("carrier_retune_state"),
            "apply_pending": u8("carrier_frequency_apply_pending"), "upgrade_pending": u8("carrier_upgrade_pending"),
            "dial_hz": u32("current_carrier_requested_hz"), "applied": u32("carrier_retune_applied_count"),
            "hrtim_mhz": u32("mod_hrtim_input_clock_hz") / 1e6, "usb_stream": u8("usb_stream_active"),
            "config": u8("active_config")}


def io_refusals(sec=2.0):
    i = health.REASONS.index("sched_io_deferred")
    a = health.health(_R())["reasons"][i]; time.sleep(sec)
    return health.health(_R())["reasons"][i] - a


out = {"tag": tag, "steps": [], "modes": []}
p = Port(ports[0])
t_start = time.monotonic()
try:
    time.sleep(2.0)
    p.cat("Q90;", 0.15)
    for hz, label in STEPS:
        p.cat(f"FA{hz:011d};", 0.15)
        trace = []
        for _ in range(16):                       # 4 s: the 0.35 s settle and AUTO's 3 s PLL move
            trace.append(snap()); time.sleep(0.25)
        row = {"label": label, "hz": hz, "first": trace[0], "last": trace[-1],
               "applied_after_s": next((s["t"] - trace[0]["t"] for s in trace
                                        if s["retune_state"] == 0 and not s["apply_pending"]
                                        and s["dial_hz"] == hz), None),
               "io_refusals_2s": io_refusals(), "fa": p.cat("FA;")}
        out["steps"].append(row)
        print(label, {k: row[k] for k in ("applied_after_s", "io_refusals_2s", "fa")}, row["last"], flush=True)
    for md in ("MD3;", "MD9;"):
        p.cat(md, 0.15); time.sleep(2.5)
        row = {"sent": md, "md": p.cat("MD;"), **snap()}
        out["modes"].append(row); print(row, flush=True)
finally:
    p.close()
    play.terminate(); rec.terminate(); time.sleep(0.5)

w = wave.open(capture); a = array.array("h", w.readframes(w.getnframes())); w.close()
gaps, start = [], None
for i, v in enumerate(a):                          # silent runs of 2 ms or more
    if v == 0 and start is None: start = i
    elif v != 0 and start is not None:
        if i - start >= 96: gaps.append((round(start / 48000, 2), round((i - start) / 48.0, 1)))
        start = None
out["capture_gaps_s_ms"] = gaps
time.sleep(max(0.0, WINDOW_S - (time.monotonic() - t0) + 3))
c = rev1_diag.Console(rev1_diag.find_port(None), timeout=20.0)
out["status"] = c.command("status"); c.close()
print("capture gaps (s, ms):", gaps)
print("\n".join(out["status"]))
ok = (all(s["applied_after_s"] is not None and s["last"]["upgrade_pending"] == 0 and s["io_refusals_2s"] == 0
          for s in out["steps"])
      and [m["md"] for m in out["modes"]] == ["MD3;", "MD9;"]
      and "faults=00" in out["status"] and "pipeline=clean" in out["status"])
out["pass"] = ok
print("PASS" if ok else "FAIL", common.save_json(f"cat_retune_{tag}", out))
sys.exit(0 if ok else 1)
