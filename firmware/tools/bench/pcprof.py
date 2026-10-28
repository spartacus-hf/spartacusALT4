#!/usr/bin/env python3
"""Statistical CPU profile: DWT PCSR sampled over the persistent openocd link
(no halts, ~6k samples/s), mapped to the ELF's functions. Works on any image.

  rx  receiving on BAND/MODE for SECS seconds (nothing keyed)
  tx  during USB-audio overs (tone1k_350ms; only bursts that start and end
      in TRANSMIT count), SSB ANALYTIC = BACKEND, OVERS times
Options (env):
  FOCUS=function   also count PCs inside that function (results: focus_pcs;
                   match them against arm-none-eabi-objdump -d)
  PENDING=1        tx: sample SCB ICSR with each PC, to see whether the
                   render worker (PendSV) waits behind an interrupt or the
                   main loop holds it off
  SETS / VIA       as tx_render.py

usage: pcprof.py rx TAG [BAND=6] [MODE=0] [SECS=10]
       pcprof.py tx TAG [BAND=7] [MODE=0] [BACKEND=1] [OVERS=4]
       -> results/pcprof_rx_TAG.json, results/pcprof_tx_TAG.json
"""
import bisect
import collections
import os
import sys
import time

import audio
import common
import cyc
import nav
import rig

PCSR, ICSR = 0xE000101C, 0xE000ED04


class Mapper:
    def __init__(self, sym):
        self.rows = sorted((a & ~1, s, n) for n, (a, s) in sym.items() if s)
        self.starts = [x[0] for x in self.rows]

    def __call__(self, pc):
        i = bisect.bisect_right(self.starts, pc) - 1
        return self.rows[i][2] if i >= 0 and pc < self.rows[i][0] + self.rows[i][1] else f"?{pc:08x}"


def burst(r, n, with_icsr):
    reads = f"[read_memory 0x{PCSR:x} 32 1]" + (f" [read_memory 0x{ICSR:x} 32 1]" if with_icsr else "")
    v = [int(x, 0) for x in r.o.cmd(f"set o {{}}; for {{set i 0}} {{$i < {n}}} {{incr i}} "
                                     f"{{lappend o {reads}}}; return $o").split()]
    return list(zip(v[0::2], v[1::2])) if with_icsr else [(pc, None) for pc in v]


def report(tag, kind, hist, focus, ctx, total):
    print(tag, kind, "samples", total)
    for name, c in hist.most_common(30):
        print(f"{100 * c / max(total, 1):6.2f}% {name}")
    if ctx:
        print("context, PendSV pending:")
        for k, c in ctx.most_common():
            print(f"  {k}: {100 * c / max(total, 1):5.1f}%")
    out = {"n": total, "hist": hist}
    if focus:
        out["focus"] = os.environ["FOCUS"]
        out["focus_pcs"] = {f"{pc:08x}": c for pc, c in focus.most_common()}
    if ctx:
        out["context"] = {str(k): c for k, c in ctx.items()}
    print("results:", common.save_json(f"pcprof_{kind}_{tag}", out))


def rx(tag, band=6, mode=0, secs=10.0):
    r = rig.Rig()
    r.safe()
    cyc.enable(r)
    name_of = Mapper(r.sym)
    hist, focus, total = collections.Counter(), collections.Counter(), 0
    with r.keep((200, 201)):
        nav.go(r, band, mode)
        time.sleep(1.0)
        t0 = time.monotonic()
        while time.monotonic() - t0 < secs:
            for pc, _ in burst(r, 64, False):
                if pc == 0xFFFFFFFF:
                    continue
                total += 1
                name = name_of(pc)
                hist[name] += 1
                if name == os.environ.get("FOCUS"):
                    focus[pc] += 1
    r.close_console()
    report(tag, "rx", hist, focus, None, total)


def tx(tag, band=7, mode=0, backend=1, overs=4):
    sets = [tuple(int(v) for v in x.split("=")) for x in os.environ.get("SETS", "").split(",") if x]
    via = [int(x) for x in os.environ.get("VIA", "").split(",") if x]
    with_icsr = os.environ.get("PENDING") == "1"
    r = rig.Rig()
    r.safe()
    name_of = Mapper(r.sym)
    hist, focus, ctx, total = collections.Counter(), collections.Counter(), collections.Counter(), 0
    with r.keep((42, 222, *(k for k, _ in sets))):
        for k, v in sets:
            r.sput(k, v)
        for ov in range(overs):
            r.sput(222, 1)
            r.sput(42, backend)
            nav.go(r, band, mode, via)
            cyc.enable(r)
            card = audio.window(r, 5)
            time.sleep(0.4)
            with r.guard() as g:
                r.set_mute(False)
                time.sleep(0.02)
                p = audio.play(card, "tone1k_350ms.wav")
                t0 = time.monotonic()
                seen = False
                while time.monotonic() - t0 < 0.8:
                    st0 = r.u8("state")
                    samples = burst(r, 24 if with_icsr else 32, with_icsr)
                    st1 = r.u8("state")
                    if st0 == 3 and st1 == 3 and not r.u8("pipeline_hard_lockout"):
                        seen = True
                        for pc, icsr in samples:
                            if pc == 0xFFFFFFFF:
                                continue
                            total += 1
                            name = name_of(pc)
                            hist[name] += 1
                            if name == os.environ.get("FOCUS"):
                                focus[pc] += 1
                            if icsr is not None:
                                active = icsr & 0x1FF
                                where = "thread" if active == 0 else ("PendSV" if active == 14 else f"exception {active}")
                                ctx[(where, bool((icsr >> 28) & 1))] += 1
                    elif seen and st1 not in rig.TX_STATES:
                        break
                r.safe()
            p.wait(timeout=3)
            h = r.health()
            print(tag, "over", ov, "samples", total, "guard", g.fired is not None, h, flush=True)
            time.sleep(4.6)
            if h["lock"] or h["faults"]:
                r.reset()
            else:
                r.open_console(timeout=60)
                r.safe()
    r.close_console()
    report(tag, "tx", hist, focus, ctx, total)


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) < 2 or a[0] not in ("rx", "tx"):
        raise SystemExit(__doc__)
    if a[0] == "rx":
        rx(a[1], *(int(x) for x in a[2:4]), *(float(x) for x in a[4:5]))
    else:
        tx(a[1], *(int(x) for x in a[2:6]))
