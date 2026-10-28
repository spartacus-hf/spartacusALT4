#!/usr/bin/env python3
"""Transmit render cost and render-queue health: one short USB-audio over per
SSB ANALYTIC choice (needs an image from build_instrumented.sh).

Per over: the renderer's share of the CPU (PendSV-render, nested interrupts
excluded) and its cycles per frame, the counted total, cycles per millisecond in probe slot 10 if the image
has one (e.g. --probe ... 'h12p_analytic_chunk(...);' 10), and the render
queue's slope in frames/s over the middle of the over: about 0 keeps up,
negative falls behind (the queue holds ~28 frames; when it empties the
underrun latches FAULT_DMA and a hard lockout).

  SETS=id=v,...   settings for the run (put back afterwards), e.g. the worst
                  case 207=1,208=1,62=0,71=1,75=1,78=1,83=1
  VIA=band,...    bands to tune through first (the clock plan depends on it)

usage: [SETS=..] [VIA=..] tx_render.py TAG BAND MODE [BACKENDS=0,1] [WAV]
       -> results/tx_render_TAG.json
A/B per mode: for m in 0 1 3 4; do tx_render.py TAG_$m 5 $m 0; done
"""
import os
import sys
import time

import numpy as np

import audio
import common
import cyc
import nav
import rig


def slope(snaps):
    tx = [(s["t"], s["rc"]) for s in snaps if s["st"] == 3 and s["rc"] > 0 and not s["lock"]]
    if len(tx) < 5:
        return None
    t, rc = np.array(tx).T
    keep = (t >= t[0] + 0.03) & (t <= t[0] + 0.22)        # the tone's tail drains the queue anyway
    return round(float(np.polyfit(t[keep], rc[keep], 1)[0]), 1) if keep.sum() >= 5 else None


def over(r, backend, band, mode, via, wav):
    r.sput(42, backend)
    clk = nav.go(r, band, mode, via) * 1e6
    cyc.enable(r)
    card = audio.window(r, 5)
    time.sleep(0.4)
    u0 = r.u32("render_underruns")
    snaps = []
    with r.guard() as g:
        r.set_mute(False)
        time.sleep(0.02)
        p = audio.play(card, wav)
        t0 = time.monotonic()
        while time.monotonic() - t0 < 0.8:
            s = cyc.snap(r)
            s["t"] = round(time.monotonic() - t0, 4)
            s["lock"] = r.u8("pipeline_hard_lockout")
            snaps.append(s)
            if any(x["st"] == 3 for x in snaps) and s["st"] not in rig.TX_STATES:
                break
        r.safe()
    p.wait(timeout=3)
    out = {"backend": backend, "band": band, "mode": mode, "clk": clk / 1e6,
           "guard": g.fired is not None, "underruns+": r.u32("render_underruns") - u0, **r.health(),
           "queue_slope": slope(snaps), "trace": [(s["t"], s["st"], s["rc"], s["lock"]) for s in snaps]}
    tx = [s for s in snaps if s["st"] == 3 and not s["lock"]]
    tx = [s for s in tx if s["t"] <= tx[0]["t"] + 0.25] if tx else tx     # before the tone ends
    if len(tx) >= 2:
        d = cyc.diff(tx[0], tx[-1], clk)
        if d["frames"] > 0:
            out["render_cyc_per_frame"] = round(d["PendSV-render"][0] / 100 * clk * d["ms"] / 1000 / d["frames"])
        out.update({"tx_ms": d["ms"], "render_pct": d["PendSV-render"][0], "sum_pct": d["sum%"],
                    "probe10_pct": d["probe10"][0], "cyc": d})
        if d["probe10"][1]:     # the probed call's cycles per millisecond (one frame)
            out["probe10_cyc_per_ms"] = round(d["probe10"][0] / 100 * clk / 1000)
    return out


def main():
    tag, band, mode = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    backends = [int(x) for x in (sys.argv[4] if len(sys.argv) > 4 else "0,1").split(",")]
    wav = sys.argv[5] if len(sys.argv) > 5 else "tone1k_350ms.wav"
    sets = [tuple(int(v) for v in x.split("=")) for x in os.environ.get("SETS", "").split(",") if x]
    via = [int(x) for x in os.environ.get("VIA", "").split(",") if x]
    r = rig.Rig()
    r.safe()
    res = {"tag": tag, "elf": common.ELF, "sets": sets, "via": via, "rows": []}
    with r.keep((42, 222, *(k for k, _ in sets))):
        r.sput(222, 1)
        for k, v in sets:
            r.sput(k, v)
        for be in backends:
            row = over(r, be, band, mode, via, wav)
            res["rows"].append(row)
            print(tag, {k: v for k, v in row.items() if k not in ("trace", "cyc")}, flush=True)
            time.sleep(4.5)
            if row["lock"] or row["faults"]:
                r.reset()
            else:
                r.open_console(timeout=60)
                r.safe()
    r.close_console()
    print("results:", common.save_json(f"tx_render_{tag}", res))


if __name__ == "__main__":
    main()
