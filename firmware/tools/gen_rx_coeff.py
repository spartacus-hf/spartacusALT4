#!/usr/bin/env python3
"""Generate Core/Src/radio_rx_coeff.inc, the REV1 receive decimation table.

32 polyphase rows x M taps of windowed-sinc as float, unity DC gain per row.
The branch length M sets the transition width (df ~ (A - 7.95) * fs / (14.36 * (M - 1))
for a Kaiser window), so M is what buys receive bandwidth: the 48 kHz output
folds content above 24 kHz back into the band, and the QSD's own image sits at
-(2 * IF + delta), so the table has to be quiet by ~25 kHz and can then only pass
as much as the transition allows.

Float, not Q15, on purpose. The rows are read 2*M times per output -- about
170,000 coefficient fetches a second -- and the int-to-float convert that a
Q15 table needs on every one of them was a measurable share of the receive
half (see the loop comment in Core/Src/radio_rx_front.c). Float rows also
remove the Q15 rounding of the coefficients themselves, so the filter is the
designed filter: nothing here is quantised.

Design in use: M=96, Kaiser A~72 dB, stopband edge 25 kHz, cutoff 20718 Hz
(125 Hz under the stop - df/2 rule) -> -0.31 dB at 18 kHz, -1 dB near
18.7 kHz, -39 dB at 24 kHz, -83 dB at 25 kHz, at least 71.9 dB from there on
(the A~70 integer-tap table before it: -0.17 dB at 18 kHz, 69.4 dB). The
lower cutoff is what lets tools/tests/rev1_audio_test.c's corrected image
at an 18 kHz IF stay within 3 dB of the ideal; the 64-tap Blackman table
before both passed 12 kHz.

Row order: the FIR walks the tap window oldest sample first, so row[0]
multiplies the oldest sample of the window and row[M-1] the newest. That is
the prototype's natural order; there is no reversal step.

One prototype, half stored: the window is evaluated at each tap's own
fractional position (x = n + p/32 - (M-1)/2, the same +-(M-1)/2 span the
integer-tap window had), so every row is a slice of one symmetric
windowed sinc. Row 32 - p is then row p reversed one tap along, and the
last tap of rows 1..31 lies past the window and is zero -- exactly, float
rounding included, since mirrored rows have equal sums. The file holds rows
0..16 (rx_coeff_half, 6.5 KB instead of 12); RADIO_RxFrontCoefficientsTo
writes out the rest. Against the integer-tap window: passband and stopband
the same or better (70.0 dB from 25 kHz), and the gain of the 32 phases
agrees to 0.0007 dB instead of 0.0032 across the passband.

Validated by: gen_rx_coeff.py --check (the checked-in file is exactly this
output) and tools/tests/rev1_front_filter_test.c, which runs the production
front end on the host and asserts the response at 16/18/20/24/25/42 kHz.

Usage:
  gen_rx_coeff.py                       design + write Core/Src/radio_rx_coeff.inc
  gen_rx_coeff.py --check               fail if the file on disk is not this output
  gen_rx_coeff.py --list                the candidate table (plan II.3), one line each
  gen_rx_coeff.py --design M ATT STOP [--blackman] [--fc HZ] --out FILE
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / "Core" / "Src" / "radio_rx_coeff.inc"

FS_IN = 177083.333333          # PCM1821 at SYSCLK/(64 x 15), 170 MHz
PHASES = 32                    # 48000/32 Hz of phase resolution

# (tag, M, Kaiser attenuation, stopband edge Hz) - plan II.3's menu, re-measured
# on the production path after the wrap mask was decoupled from the tap count.
CANDIDATES = [
    ("k64a70",     64, 70, 24000),
    ("k96a70s25",  96, 70, 25000),
    ("k96a72s25",  96, 72, 25000),    # the design in use
    ("k128a70s25", 128, 70, 25000),
    ("k128a60s26", 128, 60, 26000),
    ("k256a70s24", 256, 70, 24000),
]
CHOICE = ("k96a72s25", 96, 72, 25000)
CHOICE_FC = 20718.0  # see "Design in use" above


def kaiser_beta(att_db: float) -> float:
    if att_db > 50.0:
        return 0.1102 * (att_db - 8.7)
    if att_db >= 21.0:
        return 0.5842 * (att_db - 21.0) ** 0.4 + 0.07886 * (att_db - 21.0)
    return 0.0


def transition_hz(M: int, att_db: float, fs_in: float = FS_IN) -> float:
    return (att_db - 7.95) * fs_in / (14.36 * (M - 1))


def window_at(x: np.ndarray, half: float, att_db: float, window: str) -> np.ndarray:
    """The window at positions x (samples from the centre), zero past +-half."""
    inside = np.abs(x) <= half
    if window == "kaiser":
        u = np.clip(1.0 - (x / half) ** 2, 0.0, None)
        w = np.i0(kaiser_beta(att_db) * np.sqrt(u)) / np.i0(kaiser_beta(att_db))
    else:
        w = 0.42 + 0.5 * np.cos(np.pi * x / half) + 0.08 * np.cos(2 * np.pi * x / half)
    return np.where(inside, w, 0.0)


def design(M: int, fc_hz: float, att_db: float, window: str = "kaiser",
           fs_in: float = FS_IN) -> np.ndarray:
    n = np.arange(M, dtype=np.float64)
    half = (M - 1) / 2.0
    rows = np.zeros((PHASES, M))
    for p in range(PHASES):
        x = n + (p / PHASES) - half
        rows[p] = np.sinc(fc_hz / fs_in * x * 2.0) * window_at(x, half, att_db, window)
    rows /= rows.sum(axis=1, keepdims=True)          # unity DC gain per row
    # Round to float32 here, so the literals written below are the exact values
    # the target will hold and --check compares like with like.
    rows = rows.astype(np.float32).astype(np.float64)
    for p in range(1, PHASES // 2 + 1):
        assert rows[p][M - 1] == 0.0 and rows[PHASES - p][M - 1] == 0.0, "a last tap inside the window"
        assert np.array_equal(rows[PHASES - p][:M - 1], rows[p][M - 2::-1]), "rows do not mirror"
    assert np.array_equal(rows[0], rows[0][::-1]), "row 0 is not symmetric"
    return rows


def render(table: np.ndarray, comment: str) -> str:
    """Rows 0..PHASES/2; the firmware mirrors the rest (see design)."""
    keep = table[:PHASES // 2 + 1]
    lines = [f"/* {comment} */",
             "/* Rows 0..16 of 32: row 32 - p is row p reversed one tap along, its",
             " * last tap zero (tools/gen_rx_coeff.py, RADIO_RxFrontCoefficientsTo). */",
             f"static const float rx_coeff_half[{keep.shape[0]}][{keep.shape[1]}] = {{"]
    def literal(v: float) -> str:
        s = f"{v:.9g}"
        return (s if any(ch in s for ch in ".en") else s + ".0") + "f"  # 0 -> 0.0f
    for row in keep:
        lines.append("    {" + ", ".join(literal(v) for v in row) + "},")
    lines.append("};")
    return "\n".join(lines) + "\n"


def report(table: np.ndarray, tag: str, M: int, fc: float, att: float) -> None:
    sums = table.sum(axis=1)
    print(f"{tag:12s} M={M:4d} fc={fc:7.0f} Hz A~{att:3.0f} dB "
          f"df~{transition_hz(M, att):5.0f} Hz dc_gain {sums.min():.7f}/{sums.max():.7f} "
          f"peak {np.abs(table).max():.5f}")


def make(tag: str, M: int, att: float, stop_hz: float, window: str = "kaiser",
         fc: float | None = None) -> tuple[np.ndarray, str]:
    if fc is None:
        fc = stop_hz - transition_hz(M, att) / 2.0
    table = design(M, fc, att, window)
    comment = (f"rx_coeff: {tag} M={M} phases={PHASES} fc={fc:.0f} Hz {window} "
               f"A~{att:.0f} dB stop={stop_hz:.0f} fs_in={FS_IN:.0f} float "
               f"(generated by tools/gen_rx_coeff.py)")
    return table, comment


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--design", nargs=3, metavar=("M", "ATT", "STOP"))
    ap.add_argument("--blackman", action="store_true")
    ap.add_argument("--fc", type=float, default=None)
    ap.add_argument("--out", type=Path, default=TARGET)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args(argv)

    if args.list:
        for tag, M, att, stop in CANDIDATES:
            table, _ = make(tag, M, att, stop)
            report(table, tag, M, stop - transition_hz(M, att) / 2.0, att)
        return 0

    if args.design:
        M, att, stop = (float(v) for v in args.design)
        tag = f"M{int(M)}a{int(att)}"
        window = "blackman" if args.blackman else "kaiser"
    else:
        tag, M, att, stop = CHOICE
        window = "kaiser"
    if args.fc is not None:
        fc = args.fc
    elif not args.design:
        fc = CHOICE_FC
    else:
        fc = stop - transition_hz(int(M), att) / 2.0
    table, comment = make(tag, int(M), att, stop, window, fc)
    report(table, tag, int(M), fc, att)
    text = render(table, comment)

    if args.check:
        on_disk = args.out.read_text() if args.out.exists() else ""
        if on_disk == text:
            print(f"{args.out}: matches the generator (0 differing characters)")
            return 0
        same = sum(1 for a, b in zip(on_disk.split(), text.split()) if a == b)
        print(f"{args.out}: DIFFERS from the generator "
              f"({len(text.split()) - same} of {len(text.split())} fields differ)",
              file=sys.stderr)
        return 1

    args.out.write_text(text)
    print(f"wrote {args.out} (rows 0..{PHASES // 2} of {table.shape[0]}, {table.shape[1]} float taps)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
