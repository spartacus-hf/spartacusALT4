#!/usr/bin/env python3
"""Combination matrix: the regression run before a push. Run it on the new
image and on the previous main image and compare -- a failure that both
show is not a regression of the change (A/B, not absolute).

Sections (50 ohm load; every over <= 0.35 s of audio, every lever key-down
0.3 s, under the SWD guard):
  A  receive survey: 5 modes x 10 bands (worker rate, overruns, faults)
  B  receive option combinations: SSB/AM filters, RX AGC, FM squelch, CW
     filter/AGC/decoder, WIDE CLOCK, Q COMB
  C  USB-audio overs: WEAVER on 6 bands x USB/LSB/AM/FM, every other SSB
     ANALYTIC choice (H12P), the DIGI filter path, a two-tone
  D  CW: the boot arming key-down, then natural key-downs, 20 and 30 WPM
  E  TX CARRIER TEST on 5 bands
  F  the worst case: WIDE CLOCK + TX PHASE 384K + VOICE policy + every
     speech tool, each SSB ANALYTIC choice
  G  WATERFALL page open, an over while it is open
  H  `save` while receiving
  I  USB-PD state
Every setting touched is put back (rig.keep), band and mode included.

usage: matrix.py TAG [SECTIONS]      (SECTIONS e.g. A,C,F; default all)
       -> results/matrix_TAG.json; rows that failed are listed under "bad"
"""
import sys
import time

import audio
import common
import nav
import rig

TAG = sys.argv[1]
SECTIONS = sys.argv[2].split(",") if len(sys.argv) > 2 else list("ABCDEFGHI")
BANDS_RX = (0, 3, 5, 7, 10, 13, 15, 16, 18, 20)
MODES = nav.MODES
KEEP = (200, 201, 2, 42, 222, 8, 207, 208, 62, 71, 75, 78, 83, 209, 211, 212, 213, 214, 215, 216, 217)
WORST = ((207, 1), (208, 1), (62, 0), (71, 1), (75, 1), (78, 1), (83, 1), (222, 1))
R = {"tag": TAG, "elf": common.ELF, "rx": [], "rx_opts": [], "overs": [], "cw": [], "txtest": [], "worst": [],
     "waterfall": {}, "save": [], "pd": None, "bad": []}
r = rig.Rig()
r.safe()


def bad(section, row):
    R["bad"].append([section, row])
    print("  BAD", section, row, flush=True)


def recover(why):
    print("   RECOVER (MCU reset) after", why, flush=True)
    r.reset()


def rx_row(label, secs=0.5):
    w0, o0 = r.u32("worker_calls"), r.u32("overruns")
    time.sleep(secs)
    row = {**label, "clk": nav.sysclk(r), "rx_on": r.u8("rx_enabled"),
           "workers_per_s": round((r.u32("worker_calls") - w0) / secs), "overruns+": r.u32("overruns") - o0,
           **r.health()}
    if not (row["rx_on"] and row["workers_per_s"] > 1500 and row["overruns+"] == 0 and not row["faults"]
            and not row["lock"]):
        bad("rx", row)
    return row


def over(band, mode, wav="tone1k_350ms.wav", label=None):
    nav.go(r, band, mode)
    clk = nav.sysclk(r)
    permitted = r.u8("tx_permitted")
    card = audio.window(r, 5)
    time.sleep(0.4)
    o0, u0 = r.u32("overruns"), r.u32("render_underruns")
    fwd = 0
    with r.guard() as g:
        r.set_mute(False)
        time.sleep(0.02)
        p = audio.play(card, wav)
        t0 = time.monotonic(); tx = back = None; opre = None
        while time.monotonic() - t0 < 0.9:
            st = r.u8("state")
            if st == 3 and tx is None:
                tx = time.monotonic() - t0; opre = r.u32("overruns") - o0
            if st == 3:
                fwd = max(fwd, r.telemetry()["fwd"])
            if tx is not None and st not in rig.TX_STATES:
                back = time.monotonic() - t0
                break
            time.sleep(0.003)
        r.safe()
    p.wait(timeout=3)
    time.sleep(0.8)
    row = {**(label or {}), "band": band, "mode": mode, "clk": clk, "tx_permitted": permitted,
           "tx_at": tx and round(tx, 3), "tx_s": (back and tx) and round(back - tx, 3), "fwd_mv_peak": fwd,
           "rx_glitch_pre": opre, "rx_glitch_total": r.u32("overruns") - o0,
           "underruns+": r.u32("render_underruns") - u0, "rx_on": r.u8("rx_enabled"),
           "guard": g.fired is not None, **r.health()}
    time.sleep(4.6)
    r.open_console(timeout=60)
    r.safe()
    if not (row["tx_at"] and row["underruns+"] == 0 and not row["guard"] and not row["faults"]
            and not row["lock"] and row["rx_on"]):
        bad("over", row)
    if row["lock"] or row["faults"]:
        recover(row)
    return row


def analytic_choices():
    """SSB ANALYTIC values this image accepts, by label."""
    out = {}
    for v in range(5):
        r.sput(42, v)
        out[v] = r.sget(42)
    return out


with r.keep(KEEP) as kept:
    saved = kept.saved
    print("saved", saved, flush=True)
    try:
        if "A" in SECTIONS:
            for mname, m in MODES.items():
                nav.set_band_mode(r, mode=m); nav.settle_real(r)
                for b in BANDS_RX:
                    nav.set_band_mode(r, band=b); nav.settle_real(r); time.sleep(0.3)
                    R["rx"].append(rx_row({"mode": mname, "band": b}))
            print("A rx rows", len(R["rx"]), "bad", sum(1 for s, _ in R["bad"] if s == "rx"), flush=True)

        if "B" in SECTIONS:
            combos = []
            combos += [("USB", 7, ((214, v),)) for v in range(7)]           # SSB RX FILTER incl. DIGI
            combos += [("LSB", 3, ((216, v),)) for v in range(4)]           # RX AGC
            combos += [("AM", 3, ((215, v),)) for v in range(3)]            # AM RX FILTER
            combos += [("FM", 20, ((217, v),)) for v in (0, 3, 9)]          # FM SQUELCH
            combos += [("CW", 7, ((211, v),)) for v in range(4)]            # CW FILTER
            combos += [("CW", 7, ((212, 1),)), ("CW", 7, ((213, 0),))]      # CW AGC, CW DECODER off
            combos += [("USB", 7, ((207, 1),)), ("CW", 13, ((207, 1),)), ("AM", 3, ((207, 1),))]   # WIDE CLOCK
            combos += [("USB", 7, ((209, 1),)), ("USB", 7, ((207, 1), (209, 1)))]                   # Q COMB, + WIDE
            for mname, band, sets in combos:
                nav.go(r, band, MODES[mname])
                for sid, v in sets:
                    r.sput(sid, v)
                nav.settle_real(r); time.sleep(0.5)
                got = {sid: r.sget(sid) for sid, _ in sets}
                R["rx_opts"].append(rx_row({"mode": mname, "band": band, "sets": got}, secs=1.0))
                for sid, _ in sets:
                    r.sput(sid, saved[sid])
            print("B combos", len(R["rx_opts"]), "bad", sum(1 for s, _ in R["bad"] if s == "rx"), flush=True)

        if "C" in SECTIONS:
            r.sput(222, 1)
            plan = [(0, 0), (3, 0), (7, 0), (13, 0), (16, 0), (20, 0), (0, 1), (3, 1), (7, 1), (3, 3), (13, 3),
                    (13, 4), (20, 4)]
            for band, mode in plan:
                R["overs"].append(over(band, mode, label={"backend": "WEAVER"})); print("C", R["overs"][-1], flush=True)
            choices = analytic_choices()
            r.sput(42, saved[42])
            print("C analytic choices", choices, flush=True)
            R["analytic_choices"] = choices
            for name in sorted({b for b in choices.values() if b != choices[0]}):
                r.sput(42, next(k for k, b in choices.items() if b == name))
                for band, mode in [(3, 0), (7, 1), (16, 0), (20, 1)]:
                    R["overs"].append(over(band, mode, label={"backend": name})); print("C", R["overs"][-1], flush=True)
                R["overs"].append(over(7, 0, wav="two_tone_350ms.wav", label={"backend": name + " two-tone"}))
                print("C", R["overs"][-1], flush=True)
                r.sput(42, saved[42])
            r.sput(214, 6)                  # DIGI filter: the QMX-style DIGI path
            R["overs"].append(over(7, 0, label={"backend": "DIGI"})); print("C", R["overs"][-1], flush=True)
            R["overs"].append(over(13, 0, wav="two_tone_350ms.wav", label={"backend": "DIGI two-tone"}))
            print("C", R["overs"][-1], flush=True)
            r.sput(214, saved[214])
            r.sput(222, saved[222])

        if "D" in SECTIONS:   # CW: boot arming (muted), then natural key-downs
            nav.go(r, 13, 2); r.safe()
            lev = r.A("cw_virtual_levers")
            stage0 = r.u8("cw_boot_stage"); tx_arm = False
            r.o.write(lev, 8, 0); time.sleep(0.2); r.o.write(lev, 8, 1); t0 = time.monotonic()
            while time.monotonic() - t0 < 1.3:
                if r.u8("state") == 3:
                    tx_arm = True
                    break
                time.sleep(0.01)
            r.o.write(lev, 8, 0); time.sleep(0.3)
            R["cw_arm"] = {"stage_before": stage0, "stage_after": r.u8("cw_boot_stage"), "tx_during_arm": tx_arm,
                           **r.health()}
            print("D arm", R["cw_arm"], flush=True)
            for band, wpm in [(0, None), (7, None), (13, None), (20, None), (7, 30)]:
                if wpm:
                    r.sput(8, wpm)
                nav.go(r, band, 2)
                o0, u0 = r.u32("overruns"), r.u32("render_underruns"); states = set(); fwd = 0
                with r.guard() as g:
                    t0 = time.monotonic(); r.set_mute(False); r.o.write(lev, 8, 1)
                    while time.monotonic() - t0 < 0.3:
                        states.add(r.u8("state")); fwd = max(fwd, r.telemetry()["fwd"]); time.sleep(0.004)
                    r.o.write(lev, 8, 0)
                    while time.monotonic() - t0 < 0.9 and r.u8("state") in rig.TX_STATES:
                        time.sleep(0.004)
                    t_rx = time.monotonic() - t0
                    r.safe()
                time.sleep(1.0)
                row = {"band": band, "wpm": wpm or int(float(saved[8])), "tx_seen": 3 in states, "fwd_mv_peak": fwd,
                       "back_to_rx": round(t_rx, 3), "rx_glitch": r.u32("overruns") - o0,
                       "underruns+": r.u32("render_underruns") - u0, "rx_on": r.u8("rx_enabled"),
                       "guard": g.fired is not None, **r.health()}
                if wpm:
                    r.sput(8, saved[8])
                R["cw"].append(row); print("D", row, flush=True)
                if not (row["tx_seen"] and row["underruns+"] == 0 and not row["guard"] and not row["faults"]
                        and row["rx_on"]):
                    bad("cw", row)
                if row["lock"] or row["faults"]:
                    recover(row)

        if "E" in SECTIONS:   # TX CARRIER TEST (requested_config 18)
            nav.go(r, 5, 0)
            for band in (3, 7, 13, 16, 20):
                nav.set_band_mode(r, band=band); nav.settle_real(r)
                r.o.write(r.A("requested_config"), 32, 18); time.sleep(1.0)
                ok_cfg = r.u32("active_config") == 18
                states = set(); fwd = 0
                with r.guard() as g:
                    t0 = time.monotonic(); r.set_mute(False); r.o.write(r.A("cw_virtual_levers"), 8, 1)
                    while time.monotonic() - t0 < 0.3:
                        states.add(r.u8("state")); fwd = max(fwd, r.telemetry()["fwd"]); time.sleep(0.01)
                    r.safe()
                r.wait_rx(1.0); time.sleep(0.5)
                row = {"band": band, "entered": ok_cfg, "tx_seen": 3 in states, "fwd_mv_peak": fwd,
                       "guard": g.fired is not None, **r.health()}
                r.o.write(r.A("requested_config"), 32, 0); time.sleep(1.0)
                R["txtest"].append(row); print("E", row, flush=True)
                if not (row["entered"] and row["tx_seen"] and not row["guard"] and not row["faults"]):
                    bad("txtest", row)
                if row["lock"] or row["faults"]:
                    recover(row)

        if "F" in SECTIONS:   # worst case, each SSB ANALYTIC choice
            for sid, v in WORST:
                r.sput(sid, v)
            choices = R.get("analytic_choices") or analytic_choices()
            for v in sorted({min(k for k, b in choices.items() if b == name) for name in set(choices.values())}):
                r.sput(42, v)
                R["worst"].append(over(3, 1, label={"worst": r.sget(42)})); print("F", R["worst"][-1], flush=True)
            r.sput(42, saved[42])
            for sid, _ in WORST:
                r.sput(sid, saved[sid])

        if "G" in SECTIONS:   # WATERFALL page from the home row, an over while it is open
            r.sput(222, 1)
            nav.go(r, 13, 0)
            SP, UI = r.A("sp"), r.A("ui")
            page = lambda: r.o.u8(UI)                                         # noqa: E731
            start = nav.ui_home(r)
            nav.ui_event(r, "hold"); nav.ui_event(r, "click"); nav.ui_event(r, "turn", -8)
            nav.ui_event(r, "turn", 1); nav.ui_event(r, "click", settle_s=1.0)
            wf = {"start": start, "page": page(), "open": r.o.u8(SP)}
            sp0 = r.o.rbytes(SP, 32); o0 = r.u32("overruns"); time.sleep(5.0); sp1 = r.o.rbytes(SP, 32)
            wf.update({"receiving": sp1[2], "lines_0": sp0[22], "lines_5s": sp1[22], "load_rx": sp1[26],
                       "load_wf": sp1[27], "rx_overruns_5s": r.u32("overruns") - o0, **r.health()})
            print("G rx", wf, flush=True)
            card = audio.window(r, 5); time.sleep(0.4)
            o0, u0 = r.u32("overruns"), r.u32("render_underruns")
            with r.guard() as g:
                r.set_mute(False); time.sleep(0.02)
                p = audio.play(card, "tone1k_350ms.wav")
                t0 = time.monotonic(); tx = None
                while time.monotonic() - t0 < 0.9:
                    st = r.u8("state")
                    if st == 3 and tx is None:
                        tx = time.monotonic() - t0
                    if tx is not None and st not in rig.TX_STATES:
                        break
                    time.sleep(0.003)
                r.safe()
            p.wait(timeout=3); time.sleep(1.5)
            sp2 = r.o.rbytes(SP, 32); time.sleep(2.0); sp3 = r.o.rbytes(SP, 32)
            wf.update({"tx_with_page": tx is not None, "tx_at": tx and round(tx, 3),
                       "rx_glitch_tx": r.u32("overruns") - o0, "underruns_tx": r.u32("render_underruns") - u0,
                       "lines_after_tx": (sp2[22], sp3[22]), "receiving_after": sp3[2], "page_after": page(),
                       "guard_tx": g.fired is not None, **r.health()})
            time.sleep(3.5); r.open_console(timeout=60); r.safe()
            nav.ui_event(r, "hold", settle_s=0.8)
            wf["page_closed"] = page(); wf["open_after_close"] = r.o.u8(SP)
            R["waterfall"] = wf; print("G", wf, flush=True)
            if not (wf["page"] == 15 and wf["lines_5s"] > wf["lines_0"] and wf["rx_overruns_5s"] == 0
                    and wf["tx_with_page"] and wf["underruns_tx"] == 0 and not wf["faults"] and wf["page_closed"] != 15):
                bad("waterfall", wf)
            r.sput(222, saved[222])

        if "H" in SECTIONS:   # `save` while receiving
            for mname, band in (("USB", 7), ("CW", 13), ("FM", 20)):
                nav.go(r, band, MODES[mname]); time.sleep(0.5)
                o0 = r.u32("overruns"); t0 = time.monotonic()
                out = r.cmd("save")
                dt = round(time.monotonic() - t0, 3); time.sleep(1.0)
                st = r.kv("status")
                row = {"mode": mname, "band": band, "reply": out[:2], "save_s": dt,
                       "overruns+": r.u32("overruns") - o0, "status_faults": st.get("faults"),
                       "pipeline": st.get("pipeline"), **r.health()}
                R["save"].append(row); print("H", row, flush=True)
                if row["faults"] or row["status_faults"] != "00":
                    bad("save", row)

        if "I" in SECTIONS:   # USB-PD
            R["pd"] = r.cmd("pd") + r.cmd("pd raw")
            print("I", R["pd"], flush=True)
    finally:
        r.safe()
        # band and mode first, the carrier frequency last
        r.sput(200, saved[200]); time.sleep(1.5)
        r.sput(201, saved[201]); time.sleep(1.5)
        print("results:", common.save_json(f"matrix_{TAG}", R), "BAD rows:", len(R["bad"]), flush=True)
now = {i: r.sget(i) for i in KEEP}
print("restored", "OK" if now == saved else {i: (saved[i], now[i]) for i in KEEP if now[i] != saved[i]},
      r.health(), flush=True)
r.close_console()
