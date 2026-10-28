#!/usr/bin/env python3
"""The REV1 power/SWR detector table: port peak volts for a detector output.

REV1's bridge is QRP Labs' QMX/QMX+ T507: a tandem match on a BN43-1502
(10 turns : 1 turn per hole, 49.9 ohm on each coupled port), each port read
by an SS14 Schottky into 0.1 uF || 100 k, straight to ADC2. The DC on the
capacitor is not the RF peak: the diode keeps some of it, most of all near
its threshold, where the output falls to a square law. Firmware reads the
peak from this table (radio_rf_meter.c), so a reflected port at a tenth of
a volt is not mistaken for nothing -- what makes SWR read better at lower
power on a meter that ignores the diode (QRP Labs forum, G3SPL).

The curve is the ideal peak detector with a resistive load: the diode's
average current equals the load's,

    exp(Vdc / nVt) * (1 + Vdc / (Is * R)) = I0(Vport / nVt)

with nVt the diode's thermal voltage times its ideality and Is*R its
saturation current times the 100 k load. The defaults are SS14-like
(nVt 28 mV, Is*R 0.3 V): about 0.15 V of drop at 2 V, the square law below
0.1 V. A bench fit (tools/tests or `metercal table`) may replace them.

Usage:
  gen_rf_meter_table.py                  print the C initializer (defaults)
  gen_rf_meter_table.py --nvt 0.028 --isr 0.3
  gen_rf_meter_table.py --check FILE     compare FILE's RF_METER_DEFAULT_PORT_MV
  gen_rf_meter_table.py --fit vdc:drain,... fit Is*R (and nVt) to pairs of
                                         detector mV and drain mV, whose port
                                         peak is proportional to the drain
"""
import argparse
import math
import re
import sys

from scipy.optimize import brentq
from scipy.special import i0e

VDC_MV = (0, 6, 15, 30, 60, 120, 220, 380, 650, 1100, 1800, 2900)
NVT_DEFAULT = 0.028
ISR_DEFAULT = 0.3


def port_volts(vdc: float, nvt: float = NVT_DEFAULT, isr: float = ISR_DEFAULT) -> float:
    """Port peak volts that leave `vdc` volts on the detector capacitor."""
    if vdc <= 0.0:
        return 0.0
    rhs = vdc / nvt + math.log1p(vdc / isr)          # log I0(x) wanted
    f = lambda x: math.log(i0e(x)) + x - rhs          # log I0(x) = log i0e(x) + x
    hi = vdc / nvt + 10.0 * (1.0 + rhs)
    return nvt * brentq(f, 0.0, hi, xtol=1e-12)


def table(nvt: float, isr: float) -> list[int]:
    return [int(round(1000.0 * port_volts(v / 1000.0, nvt, isr))) for v in VDC_MV]


def initializer(nvt: float, isr: float) -> str:
    return "{" + ", ".join(str(v) for v in table(nvt, isr)) + "}"


def fit(pairs: list[tuple[float, float]]) -> tuple[float, float, float, float]:
    """Best (nvt, isr, k, rms) for port = k * drain over the given pairs."""
    best = None
    for nvt in (0.024, 0.026, 0.028, 0.030, 0.032, 0.035):
        for i in range(61):
            isr = 0.01 * 10.0 ** (i / 20.0)              # 0.01 .. 10 V
            y = [port_volts(vdc / 1000.0, nvt, isr) for vdc, _ in pairs]
            x = [d / 1000.0 for _, d in pairs]
            k = sum(a * b for a, b in zip(x, y)) / sum(a * a for a in x)
            rms = math.sqrt(sum((b - k * a) ** 2 for a, b in zip(x, y)) / len(x))
            if best is None or rms < best[3]:
                best = (nvt, isr, k, rms)
    return best


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nvt", type=float, default=NVT_DEFAULT)
    ap.add_argument("--isr", type=float, default=ISR_DEFAULT)
    ap.add_argument("--check", metavar="HEADER")
    ap.add_argument("--fit", metavar="PAIRS")
    a = ap.parse_args()
    if a.fit:
        pairs = [tuple(float(v) for v in p.split(":")) for p in a.fit.split(",")]
        nvt, isr, k, rms = fit(pairs)
        print(f"nvt={nvt:.3f} isr={isr:.4f} k={k:.5f} rms_port_mv={1000 * rms:.2f}")
        print("table " + " ".join(str(v) for v in table(nvt, isr)))
        return 0
    if a.check:
        text = open(a.check, encoding="utf-8").read()
        m = re.search(r"#define\s+RF_METER_DEFAULT_PORT_MV\s+(\{[^}]*\})", text)
        if not m:
            print(f"FAIL no RF_METER_DEFAULT_PORT_MV in {a.check}")
            return 1
        want = initializer(a.nvt, a.isr)
        have = re.sub(r"\s+", " ", m.group(1)).replace("{ ", "{").replace(" }", "}")
        if have != want:
            print(f"FAIL {a.check}: {have} != {want}")
            return 1
        print("PASS detector table matches the diode model")
        return 0
    print(f"/* nVt {a.nvt * 1000:.0f} mV, Is*R {a.isr:.3f} V */")
    print(f"#define RF_METER_DEFAULT_PORT_MV {initializer(a.nvt, a.isr)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
