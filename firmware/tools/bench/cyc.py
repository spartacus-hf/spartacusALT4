"""CPU accounting from an instrumented image (instrument.py): bench_cyc[2i]
= cycles net of nested counted interrupts, bench_cyc[2i+1] = calls, read
with DWT CYCCNT over SWD."""
NAMES = ["DMA1c1(RF)", "DMA1c2", "DMA1c3(LCD)", "USB", "SysTick", "PendSV-audio", "PendSV-render",
         "ADC(DMA2c1)", "slot8", "slot9", "probe10", "probe11"]


def enable(r):
    """Trace enable + CYCCNT on (DEMCR.TRCENA, DWT_CTRL.CYCCNTENA)."""
    r.o.write(0xE000EDFC, 32, r.o.u32(0xE000EDFC) | (1 << 24))
    r.o.write(0xE0001000, 32, r.o.u32(0xE0001000) | 1)


def snap(r):
    c = r.o.read(r.A("bench_cyc"), 32, 24)
    return {"cyc": r.o.u32(0xE0001004), "b": c, "seq": r.u32("render_next_seq"),
            "rc": r.u32("render_count"), "st": r.u8("state")}


def diff(a, b, clk_hz):
    """Per counter between two snaps: (% of the CPU, calls, cycles per call);
    sum% covers the counted contexts (the rest is the main loop and the
    uncounted interrupts)."""
    dt = (b["cyc"] - a["cyc"]) & 0xFFFFFFFF
    out = {"ms": round(dt / clk_hz * 1e3, 1), "frames": b["seq"] - a["seq"], "render_count": (a["rc"], b["rc"])}
    tot = 0
    for i, n in enumerate(NAMES):
        c = (b["b"][2 * i] - a["b"][2 * i]) & 0xFFFFFFFF
        k = (b["b"][2 * i + 1] - a["b"][2 * i + 1]) & 0xFFFFFFFF
        out[n] = (round(100 * c / dt, 1), k, round(c / k) if k else 0)
        tot += c
    out["sum%"] = round(100 * tot / dt, 1)
    return out
