#!/usr/bin/env python3
"""Long USB-audio overs (up to 4 s of audio; the SWD guard sits at the audio
length + 0.6 s, under the 5 s key-down limit). Every ~10 ms: state, render
queue (frames), PCM backlog (samples), drops, underruns, lockout. A short
over can hide a renderer that falls behind slowly; this shows the queue's
slope and its lowest level mid-over. Works on any image.

  SETS / VIA as tx_render.py. WAV: a name from audio.py (tone1k_3s.wav,
  speech_3s.wav) or a path.

usage: [SETS=..] [VIA=..] tx_long.py TAG BAND MODE BACKENDS WAV
       -> results/tx_long_TAG.json
"""
import os
import sys
import time

import numpy as np

import audio
import common
import nav
import rig

NAMES = ("state", "render_count", "pcm_count", "pcm_drop_samples", "render_underruns", "pipeline_hard_lockout")


def main():
    tag, band, mode = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    backends = [int(x) for x in sys.argv[4].split(",")]
    wav = sys.argv[5]
    secs = audio.seconds(wav)
    if secs > 4.0:
        raise SystemExit("keep key-downs under the 5 s limit: 4 s of audio at most")
    sets = [tuple(int(v) for v in x.split("=")) for x in os.environ.get("SETS", "").split(",") if x]
    via = [int(x) for x in os.environ.get("VIA", "").split(",") if x]
    r = rig.Rig()
    r.safe()
    res = {"tag": tag, "elf": common.ELF, "wav": wav, "sets": sets, "via": via, "rows": []}
    with r.keep((42, 222, *(k for k, _ in sets))):
        for k, v in sets:
            r.sput(k, v)
        r.sput(222, 1)
        for be in backends:
            r.sput(42, be)
            clk = nav.go(r, band, mode, via)
            card = audio.window(r, int(secs) + 6)
            time.sleep(0.4)
            addrs = [r.A(n) for n in NAMES]
            u0 = r.u32("render_underruns")
            trace = []
            with r.guard(limit=secs + 0.6) as g:
                r.set_mute(False)
                time.sleep(0.02)
                p = audio.play(card, wav)
                t0 = time.monotonic()
                while time.monotonic() - t0 < secs + 0.5:
                    v = [r.o.u8(a) if n in ("state", "pipeline_hard_lockout") else r.o.u32(a)
                         for n, a in zip(NAMES, addrs)]
                    trace.append([round(time.monotonic() - t0, 3)] + v)
                    if any(x[1] == 3 for x in trace) and v[0] not in rig.TX_STATES:
                        break
                    time.sleep(0.008)
                r.safe()
            p.wait(timeout=5)
            tx = [x for x in trace if x[1] == 3]
            mid = [x for x in tx if tx[0][0] + 0.1 <= x[0] <= tx[-1][0] - 0.15] if tx else []
            sl = None
            if len(mid) >= 5:
                t, rc = np.array([(x[0], x[2]) for x in mid]).T
                sl = round(float(np.polyfit(t, rc, 1)[0]), 2)
            row = {"backend": be, "clk": clk, "guard": g.fired is not None,
                   "tx_s": round(tx[-1][0] - tx[0][0], 2) if tx else 0,
                   "queue_min_mid": min((x[2] for x in mid), default=None), "queue_slope": sl,
                   "pcm_max": max((x[3] for x in tx), default=None), "pcm_drops": r.u32("pcm_drop_samples"),
                   "underruns+": r.u32("render_underruns") - u0, **r.health(), "names": ("t",) + NAMES,
                   "trace": trace}
            res["rows"].append(row)
            print(tag, {k: v for k, v in row.items() if k not in ("trace", "names")}, flush=True)
            time.sleep(4.6)
            if row["lock"] or row["faults"]:
                r.reset()
            else:
                r.open_console(timeout=60)
                r.safe()
    r.close_console()
    print("results:", common.save_json(f"tx_long_{tag}", res))


if __name__ == "__main__":
    main()
