#!/usr/bin/env python3
"""Receive soak: tuning (console `knob`) every few seconds, a band change a
minute, a mode change every 2 minutes -- settings autosave churn on the SD
store -- with receive/fault/pipeline/SD/storage counters every 30 s and the
event log's new lines at the end. Console only (no SWD, nothing keyed).
MODE, BAND, the carrier and setting 132 are put back.

usage: soak.py [MINUTES=30]
"""
import sys, time, random
import common
rev1_diag = common.rev1_diag
MIN = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
c = rev1_diag.Console(rev1_diag.find_port(None), timeout=20.0)

def cmd(t, tries=20):
    for _ in range(tries):
        try: return c.command(t)
        except SystemExit as e:
            if "busy" in str(e): time.sleep(0.3); continue
            raise
        except Exception:
            time.sleep(0.3)
    raise IOError(f"{t!r} kept failing")

def kv(t):
    out = {}
    for line in cmd(t):
        for tok in line.split():
            if "=" in tok:
                k, v = tok.split("=", 1); out.setdefault(k, v)
    return out

def log_size():
    for line in cmd("sd stat LOGS/EVENTS.LOG"):
        if line.startswith("size="): return int(line.split()[0].split("=")[1])
    return 0

def sd_get_tail(path, start):
    data = bytearray()
    for line in cmd(f"sd get {path}"):
        off, hx, crc = line.split()
        data += bytes.fromhex(hx)
    return bytes(data[start:])

saved = {i: cmd(f"set get {i}")[0] for i in (200, 201, 2, 132)}
print("saved", saved, flush=True)
log0 = log_size()
r0, sd0, st0 = kv("rx"), kv("sd stats"), kv("storage")
print("start rx", {k: r0[k] for k in ("overruns", "pending", "working")}, "sd", sd0, "storage writes", st0["writes"], flush=True)
random.seed(809)
bands, modes = [0, 3, 5, 7, 10, 13, 15, 16, 18, 20], [0, 1, 2, 3, 4]
t0 = time.monotonic(); next_band = t0 + 60; next_mode = t0 + 120; next_report = t0 + 30
knobs = band_changes = mode_changes = 0
try:
    while time.monotonic() - t0 < MIN * 60:
        now = time.monotonic()
        if now >= next_mode:
            cmd(f"set put 200 {random.choice(modes)}"); mode_changes += 1; next_mode += 120
        elif now >= next_band:
            cmd(f"set put 201 {random.choice(bands)}"); band_changes += 1; next_band += 60
        else:
            cmd(f"knob {random.choice([-5, -2, -1, 1, 2, 5])}"); knobs += 1
        if now >= next_report:
            r, st, s = kv("rx"), kv("status"), kv("storage")
            print(f"{(now - t0) / 60:5.1f} min overruns+{int(r['overruns']) - int(r0['overruns'])} "
                  f"faults={st.get('faults')} pipeline={st.get('pipeline')} state={st.get('state')} "
                  f"sd={cmd('sd stats')[0]} store writes={s['writes']} vf={s['verify_fail']} bad={s['bad_copies']} "
                  f"rep={s['repairs']} io={s['io']} deg={s['degraded']} status={s.get('status')}", flush=True)
            next_report += 30
        time.sleep(random.uniform(2.0, 6.0))
finally:
    cmd("set put 200 " + saved[200].split("=")[1]); time.sleep(1.5)
    cmd("set put 201 " + saved[201].split("=")[1]); time.sleep(1.5)
    cmd("set put 2 " + saved[2].split("=")[1]); time.sleep(0.5)
    cmd("set put 132 " + saved[132].split("=")[1]); time.sleep(4)
    print("restored", [cmd(f"set get {i}")[0] for i in (200, 201, 2, 132)], flush=True)
r1, sd1, st1 = kv("rx"), kv("sd stats"), kv("storage")
print("end: knobs", knobs, "band changes", band_changes, "mode changes", mode_changes,
      "overruns+", int(r1["overruns"]) - int(r0["overruns"]), "storage", cmd("storage")[:2], "sd", cmd("sd stats")[0], flush=True)
print("status", cmd("status")[:3], flush=True)
print("event log, new lines:", flush=True)
print(sd_get_tail("LOGS/EVENTS.LOG", log0).decode("ascii", "replace"), flush=True)
c.close()
