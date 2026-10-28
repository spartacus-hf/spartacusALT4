#!/usr/bin/env python3
"""Forward power (RF detector, millivolts; power goes as the square) per
transmit path, and its repeatability.

  paths   USB-audio overs for SSB ANALYTIC (42) x USB AUDIO POLICY (62) x a
          half-scale and a full-scale tone. In DIGI CLEAN (62 = 1) a WEAVER
          tone is sent by the DIGI tone path at full drive whatever its level;
          H12P and VOICE (62 = 0) follow the level, so a half-scale tone gives
          about a quarter of the power. Their medians should agree.
  repeat  N overs (USB, tone1k_350ms) and N TX CARRIER TEST key-downs (0.3 s)
          on each band: peak and median per key-down, to tell a level change
          from the detector's scatter.

usage: power.py paths TAG [BAND] [MODE]       -> results/power_paths_TAG.json
       power.py repeat TAG [N] [BANDS=20,7]   -> results/power_repeat_TAG.json
"""
import statistics
import sys
import time

import audio
import common
import nav
import rig


def audio_over(r, band, mode, wav):
    nav.go(r, band, mode)
    card = audio.window(r, 5)
    time.sleep(0.4)
    fwd, start = [], None
    with r.guard() as g:
        r.set_mute(False)
        time.sleep(0.02)
        p = audio.play(card, wav)
        t0 = time.monotonic()
        seen = False
        while time.monotonic() - t0 < 0.9:
            st = r.u8("state")
            if st == 3:
                if start is None:
                    start = time.monotonic()
                seen = True
                fwd.append((round(time.monotonic() - start, 4), r.telemetry()["fwd"]))
            elif seen and st not in rig.TX_STATES:
                break
            time.sleep(0.002)
        r.safe()
    p.wait(timeout=3)
    v = [x[1] for x in fwd]
    row = {"n": len(v), "fwd_peak": max(v or [0]), "fwd_median": statistics.median(v) if v else 0,
           "onset": fwd[:6], "guard": g.fired is not None, **r.health()}
    time.sleep(4.6)
    if row["lock"] or row["faults"]:
        r.reset()
    else:
        r.open_console(timeout=60)
        r.safe()
    return row


def txtest(r, band):
    nav.go(r, band, 0)
    r.o.write(r.A("requested_config"), 32, 18)
    time.sleep(1.0)
    fwd = []
    with r.guard() as g:
        t0 = time.monotonic()
        r.set_mute(False)
        r.o.write(r.A("cw_virtual_levers"), 8, 1)
        while time.monotonic() - t0 < 0.3:
            if r.u8("state") == 3:
                fwd.append(r.telemetry()["fwd"])
            time.sleep(0.005)
        r.safe()
    r.wait_rx(1.0)
    time.sleep(0.5)
    r.o.write(r.A("requested_config"), 32, 0)
    time.sleep(1.0)
    return {"n": len(fwd), "fwd_peak": max(fwd or [0]), "fwd_median": statistics.median(fwd) if fwd else 0,
            "guard": g.fired is not None, **r.health()}


def paths(tag, band=7, mode=0):
    r = rig.Rig()
    r.safe()
    rows = []
    with r.keep((42, 62, 222)):
        r.sput(222, 1)
        for policy in (1, 0):
            for be in (0, 1):
                for wav in ("tone1k_350ms.wav", "loud1k_350ms.wav"):
                    r.sput(62, policy)
                    r.sput(42, be)
                    row = {"policy": "DIGI CLEAN" if policy else "VOICE", "backend": r.sget(42), "wav": wav,
                           **audio_over(r, band, mode, wav)}
                    rows.append(row)
                    print(tag, row, flush=True)
    r.close_console()
    print("results:", common.save_json(f"power_paths_{tag}", rows))


def repeat(tag, n=4, bands=(20, 7)):
    r = rig.Rig()
    r.safe()
    out = {"tag": tag, "over": {}, "txtest": {}}
    with r.keep((222,)):
        r.sput(222, 1)
        for band in bands:
            out["over"][band] = [audio_over(r, band, 0, "tone1k_350ms.wav") for _ in range(n)]
            out["txtest"][band] = [txtest(r, band) for _ in range(n)]
            for k in ("over", "txtest"):
                rows = out[k][band]
                print(tag, k, "band", band, "peak", [x["fwd_peak"] for x in rows], "median",
                      [x["fwd_median"] for x in rows], "faults", [x["faults"] for x in rows], flush=True)
    r.close_console()
    print("results:", common.save_json(f"power_repeat_{tag}", out))


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a or a[0] not in ("paths", "repeat"):
        raise SystemExit(__doc__)
    if a[0] == "paths":
        paths(a[1], int(a[2]) if len(a) > 2 else 7, int(a[3]) if len(a) > 3 else 0)
    else:
        repeat(a[1], int(a[2]) if len(a) > 2 else 4,
               tuple(int(x) for x in a[3].split(",")) if len(a) > 3 else (20, 7))
