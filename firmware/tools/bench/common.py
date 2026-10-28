"""Paths, ELF symbols and the SWD link that every bench script shares.

The bench talks to the radio two ways:
  * SWD through one persistent openocd (Tcl port 6666, see openocd.sh):
    reads and writes of RAM and registers without halting the CPU;
  * the USB DIAG console (tools/rev1_diag.py).

CHIREIX_ALT2 names the firmware tree that is flashed (default: this
checkout's alt2/); its ELF gives the symbol addresses, so it must be the
build that is on the radio. Results go to BENCH_RESULTS (default
tools/bench/results/, git-ignored).
"""
import glob
import json
import os
import shutil
import socket
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ALT2 = os.path.abspath(os.environ.get("CHIREIX_ALT2", os.path.join(HERE, "..", "..")))
RESULTS = os.path.abspath(os.environ.get("BENCH_RESULTS", os.path.join(HERE, "results")))
ELF = os.environ.get("BENCH_ELF", os.path.join(ALT2, "Release-Speed", "alt2.elf"))

sys.path.insert(0, os.path.join(HERE, ".."))
import rev1_diag  # noqa: E402  (tools/rev1_diag.py of this checkout)


def tool(name):
    """An arm-none-eabi tool: from PATH, else from the STM32CubeIDE install."""
    found = shutil.which(name)
    if found:
        return found
    hits = sorted(glob.glob(f"/opt/stm32cubeide/plugins/*gnu-tools-for-stm32*/tools/bin/{name}"))
    if not hits:
        raise FileNotFoundError(f"{name}: put the GNU Arm toolchain on PATH")
    return hits[-1]


def symbols(elf=ELF):
    """name -> (address, size) from the ELF (local `.constprop.N`-style
    suffixes stripped, the first definition kept)."""
    if not os.path.exists(elf):
        raise FileNotFoundError(f"{elf}: build the flashed tree (or set CHIREIX_ALT2 / BENCH_ELF)")
    out = subprocess.run([tool("arm-none-eabi-nm"), "-S", elf], capture_output=True, text=True,
                         check=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4:
            name = p[3].split(".")[0] if p[3].count(".") == 1 and p[3].split(".")[1].isdigit() else p[3]
            syms.setdefault(name, (int(p[0], 16), int(p[1], 16)))
        elif len(p) == 3:
            syms.setdefault(p[2], (int(p[0], 16), 0))
    return syms


class Ocd:
    """One connection to openocd's Tcl port: memory reads/writes, no halts."""

    def __init__(self, port=6666):
        try:
            self.s = socket.create_connection(("127.0.0.1", port), timeout=3)
        except OSError as e:
            raise IOError("no openocd on Tcl port 6666: run tools/bench/openocd.sh start") from e
        self.lock = threading.Lock()

    def cmd(self, text):
        with self.lock:
            self.s.sendall(text.encode() + b"\x1a")
            buf = b""
            while not buf.endswith(b"\x1a"):
                chunk = self.s.recv(65536)
                if not chunk:
                    raise IOError("openocd closed")
                buf += chunk
            return buf[:-1].decode()

    def read(self, addr, width, count):
        r = self.cmd(f"read_memory 0x{addr:x} {width} {count}")
        return [int(x, 0) for x in r.split()]

    def write(self, addr, width, values):
        if isinstance(values, int):
            values = [values]
        r = self.cmd(f"write_memory 0x{addr:x} {width} {{{' '.join(hex(v) for v in values)}}}")
        if r.strip():
            raise IOError(r)

    def rbytes(self, addr, n):
        return bytes(self.read(addr, 8, n))

    def u8(self, a): return self.read(a, 8, 1)[0]
    def u16(self, a): return self.read(a, 16, 1)[0]
    def u32(self, a): return self.read(a, 32, 1)[0]


def result_path(name, ext="json"):
    os.makedirs(RESULTS, exist_ok=True)
    return os.path.join(RESULTS, f"{name}.{ext}")


def save_json(name, obj):
    """Write obj to RESULTS/name.json and return the path."""
    path = result_path(name)
    with open(path, "w") as f:
        json.dump(obj, f, indent=1, default=str)
    return path
