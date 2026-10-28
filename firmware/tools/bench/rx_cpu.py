#!/usr/bin/env python3
"""Receive-side CPU per mode (needs an image from build_instrumented.sh): on
one band, per mode, the share of every counted context over WINDOW_S, the
receive worker's cycles per call (PendSV-audio), receive overruns. Compare
the worker's cycles per call between images at the same SYSCLK: hot-loop
placement alone has moved it by 16 % (2026-10-03). Nothing is keyed; band
and mode are put back.

usage: rx_cpu.py TAG [BAND=6] [MODES=USB,LSB,CW,AM,FM]  -> results/rx_cpu_TAG.json
"""
import sys
import time

import common
import cyc
import nav
import rig

WINDOW_S = 3.0


def main():
    tag = sys.argv[1]
    band = int(sys.argv[2]) if len(sys.argv) > 2 else 6
    modes = sys.argv[3].split(",") if len(sys.argv) > 3 else list(nav.MODES)
    r = rig.Rig()
    r.safe()
    cyc.enable(r)
    out = {"tag": tag, "elf": common.ELF, "band": band, "rows": []}
    with r.keep((200, 201)):
        for name in modes:
            clk = nav.go(r, band, nav.MODES[name])
            time.sleep(1.0)
            o0 = r.u32("overruns")
            a = cyc.snap(r)
            time.sleep(WINDOW_S)
            b = cyc.snap(r)
            d = cyc.diff(a, b, clk * 1e6)
            row = {"mode": name, "clk": clk, "overruns+": r.u32("overruns") - o0, "faults": r.u32("faults"),
                   "worker_cyc_per_call": d["PendSV-audio"][2], "cyc": d}
            out["rows"].append(row)
            print(f"{tag} {name:3s} clk={clk:.3f} MHz worker={d['PendSV-audio'][0]}% "
                  f"({d['PendSV-audio'][2]} cyc/call) adc={d['ADC(DMA2c1)'][0]}% usb={d['USB'][0]}% "
                  f"lcd={d['DMA1c3(LCD)'][0]}% systick={d['SysTick'][0]}% counted={d['sum%']}% "
                  f"overruns+={row['overruns+']}", flush=True)
    r.close_console()
    print("results:", common.save_json(f"rx_cpu_{tag}", out))


if __name__ == "__main__":
    main()
