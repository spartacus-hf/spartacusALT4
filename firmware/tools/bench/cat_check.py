#!/usr/bin/env python3
"""CAT ports in the audio personality (composite USB): each answers read-only
Kenwood-style queries (ID; FA; MD; IF; FW;), and a line ending makes one the
console. Opens a 15 s audio window from the DIAG console; nothing is set or
keyed, no SWD needed.

usage: cat_check.py TAG  -> results/cat_check_TAG.json
"""
import glob, os, sys, time, termios, select
import common
rev1_diag = common.rev1_diag
tag = sys.argv[1]


class Port:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        a[0] = a[1] = a[3] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
    def __enter__(self): return self
    def __exit__(self, *e): os.close(self.fd)
    def reset_input_buffer(self): termios.tcflush(self.fd, termios.TCIFLUSH)
    def write(self, b): os.write(self.fd, b)
    def read(self, n):
        out = b""; t0 = time.monotonic()
        while time.monotonic() - t0 < 0.5 and len(out) < n:
            if select.select([self.fd], [], [], 0.05)[0]:
                out += os.read(self.fd, n - len(out))
        return out
c = rev1_diag.Console(rev1_diag.find_port(None), timeout=10.0)
c.command("audio 15"); time.sleep(0.2); c.close()
t0 = time.monotonic(); ports = []
while time.monotonic() - t0 < 10 and len(ports) < 2:
    ports = sorted(p for p in glob.glob("/dev/serial/by-id/*SPARTACUS*") if "DIAG" not in p); time.sleep(0.2)
time.sleep(1.0)
out = {"tag": tag, "ports": ports, "cat": {}, "console": None}
for p in ports:
    with Port(p) as s:
        rows = {}
        for q in ("ID;", "FA;", "MD;", "IF;", "FW;"):
            s.reset_input_buffer(); s.write(q.encode()); time.sleep(0.15)
            rows[q] = s.read(256).decode("ascii", "replace")
        out["cat"][os.path.basename(p)] = rows
with Port(ports[-1]) as s:
    s.write(b"\r\nid\r\n"); time.sleep(0.6)
    out["console"] = s.read(2048).decode("ascii", "replace").splitlines()
for name, rows in out["cat"].items():
    print(name, rows)
print("console via CAT:", out["console"])
print("results:", common.save_json(f"cat_check_{tag}", out))
