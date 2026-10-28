#!/usr/bin/env python3
"""USB DISK MODE from the panel (events over SWD): HOME -> MENU -> SYSTEM ->
SD CARD -> USB DISK MODE (hub row 6), check the mass-storage device the host
sees (size, partition table signature), leave disk mode again and reset the
MCU so the DIAG console comes back (diagboot). Nothing is written to the card.

usage: disk_mode.py TAG  -> results/disk_mode_TAG.json
"""
import glob, os, subprocess, sys, time
import common
import rig

tag = sys.argv[1]
r = rig.Rig()
r.safe()
UI, EV = r.A("ui"), r.A("ui_ev")
CUR = r.A("cursor")
out = {"tag": tag}


def ev(off, val, settle=0.4):
    r.o.write(EV + off, 8, val & 0xFF)
    time.sleep(settle)


def f():
    u = r.o.rbytes(UI, 64)
    return {"page": u[0], "home_level": u[2], "menu_level": u[48], "menu_cat": u[49], "menu_row": u[50]}


for _ in range(6):                    # FUNC: back to home, tuning level
    if f()["page"] == 0 and f()["home_level"] == 0:
        break
    ev(11, 1)
ev(3, 1); ev(3, 1, 0.6)               # hold: zone picker, hold: menu
s = f(); out["menu"] = s
assert s["page"] == 1 and s["menu_level"] == 0, s
ev(0, 8 - s["menu_cat"])              # category SYSTEM
ev(2, 1)                              # click: its rows
for _ in range(12):
    if f()["menu_row"] == 0:
        break
    ev(0, -1)
out["system_row"] = f()
ev(2, 1, 1.0)                         # click: SD CARD
out["sd_page"] = f()["page"]
assert out["sd_page"] == 8, f()
for _ in range(10):                   # hub cursor to USB DISK MODE (row 6)
    c = r.o.u8(CUR)
    if c == 6:
        break
    ev(0, 1 if c < 6 else -1)
out["cursor"] = r.o.u8(CUR)
assert out["cursor"] == 6, out
r.close_console()
before = set(glob.glob("/dev/disk/by-id/usb-*"))
ev(2, 1, 0.5)                         # click: USB DISK MODE on
t0 = time.monotonic(); dev = None
while time.monotonic() - t0 < 15 and dev is None:
    new = [p for p in set(glob.glob("/dev/disk/by-id/usb-*")) - before if "-part" not in p]
    dev = new[0] if new else None
    time.sleep(0.3)
out["enumerated_s"] = round(time.monotonic() - t0, 2)
out["by_id"] = dev
out["lsusb"] = [l for l in subprocess.run(["lsusb"], capture_output=True, text=True).stdout.splitlines() if "0483" in l]
if dev:
    time.sleep(1.0)
    node = os.path.realpath(dev)
    out["lsblk"] = subprocess.run(["lsblk", "-no", "NAME,SIZE,VENDOR,MODEL,REV,TRAN,FSTYPE,LABEL", node],
                                  capture_output=True, text=True).stdout.strip().splitlines()
    out["mounted"] = subprocess.run(["findmnt", "-rn", "-S", node + "1"], capture_output=True, text=True).stdout.strip() or \
        subprocess.run(["findmnt", "-rn", "-S", node], capture_output=True, text=True).stdout.strip()
    try:
        with open(node, "rb") as fh:
            sec = fh.read(512)
        out["sector0_sig"] = sec[510:512].hex()
    except OSError as e:
        out["sector0_sig"] = f"not readable: {e.strerror}"
time.sleep(2.0)
out["state_in_disk_mode"] = {"page": f()["page"], "cursor": r.o.u8(CUR), "faults": r.u32("faults"),
                             "lock": r.u8("pipeline_hard_lockout"), "rx_on": r.u8("rx_enabled")}
ev(2, 1, 3.0)                         # click: USB DISK MODE off
out["after_exit"] = {"disk_gone": dev is not None and not os.path.exists(dev),
                     "lsusb": [l for l in subprocess.run(["lsusb"], capture_output=True, text=True).stdout.splitlines() if "0483" in l]}
ev(11, 1, 0.6)                        # FUNC: back
for _ in range(4):
    if f()["page"] == 0:
        break
    ev(11, 1, 0.6)
out["page_end"] = f()["page"]
r.reset()                             # diagboot brings the DIAG console back
out["status_after_reset"] = r.cmd("status")[:3]
r.close_console()
for k, v in out.items():
    print(f"{k}: {v}")
print("results:", common.save_json(f"disk_mode_{tag}", out))
