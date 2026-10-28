#!/usr/bin/env python3
"""Generate Core/Src/radio_rx_channel.inc, the REV1 receive channel filters.

The receiver mixes the wanted channel's centre to DC and runs the same real
8th-order low-pass (four biquads, transposed direct form II in float) on I and
Q at 48 kHz, so a channel of width W is a low-pass with cutoff W/2.

rx_channel_sos, one per demodulator (unchanged since v7):
    8th-order elliptic, 0.1 dB ripple, 80 dB stop band; USB/LSB 1350 Hz
    (300-3000 Hz around the 1650 Hz centre), CW 350 Hz, AM 5 kHz, FM 10 kHz.

rx_cw_sos, the selectable CW bandwidths (radio_config_t::cw_bw_10hz):
    every width is an 8th-order Butterworth. Through v8.07 500-1000 Hz were
    the elliptic above (200-400 Hz already Butterworth). The elliptic is flat
    to its edge and 35 dB down 10 % of the width past it, so a CW signal
    drifting across the edge -- the MEMS reference moving, or the other
    station -- vanished and came back rather than fading; operators heard
    that as the filter being too narrow (bench report, 2026-09-30). The
    Butterworth is -3 dB at the edge and -13 dB 10 % past it, a fade, and
    still -48 dB one full width from the centre (-73 dB 1 kHz off at 700 Hz,
    where the single-signal test in rev1_audio wants 60). 700 Hz is the
    default since v8.08.

rx_ssb_sos, the SSB receive widths (radio_config_t::ssb_bw_100hz), and
rx_am_sos, the AM ones (am_bw_khz): the elliptic of rx_channel_sos at each
width's half-width, the passband SSB 300 Hz .. 300 Hz + width around a centre
the DSP moves to DC. The 2700 Hz SSB and 10 kHz AM rows are the frozen
USB and AM rows, so the default widths receive exactly as before v8.08.

rx_digi_sos, DIGI's passband (RADIO_DigiReceive): QMX's digital-mode filter,
150-3200 Hz, the same elliptic at half its width.

Usage:
  gen_rx_channel.py            write Core/Src/radio_rx_channel.inc
  gen_rx_channel.py --check    fail if the file on disk is not this output
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from scipy import signal

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / "Core" / "Src" / "radio_rx_channel.inc"
FS = 48000

# Must match RADIO_SSB_BW_TABLE and RADIO_AM_BW_TABLE in radio_config.h.
SSB_BANDWIDTHS = [1800, 2100, 2400, 2700, 3000, 3600]
# Must match RADIO_DIGI_RX_LOW_HZ / RADIO_DIGI_RX_HIGH_HZ in radio_config.h.
DIGI_PASSBAND = (150, 3200)
AM_BANDWIDTHS = [6000, 8000, 10000]

# (bandwidth Hz, design): must match RADIO_CW_BW_TABLE in radio_config.h.
CW_BANDWIDTHS = [200, 300, 400, 500, 700, 1000, 1500]


# The demodulator filters exactly as shipped since v7. Newer scipy normalises
# an even-order elliptic to a 0 dB passband peak; these were designed with the
# ripple centred on 0 dB (+-0.05 dB), so they are kept verbatim rather than
# regenerated 0.05 dB lower.
FROZEN_CHANNEL_SOS = """static const float rx_channel_sos[5][4][5] = {
    { /* RADIO_RX_USB */
        {1.189579420e-04f, -1.299676432e-04f, 1.189579420e-04f, -1.868187316e+00f, 8.743241091e-01f},
        {1.000000000e+00f, -1.846598909e+00f, 1.000000000e+00f, -1.893376849e+00f, 9.091422772e-01f},
        {1.000000000e+00f, -1.920554818e+00f, 1.000000000e+00f, -1.924693979e+00f, 9.510640487e-01f},
        {1.000000000e+00f, -1.937490253e+00f, 1.000000000e+00f, -1.952690794e+00f, 9.851091792e-01f},
    },
    { /* RADIO_RX_LSB */
        {1.189579420e-04f, -1.299676432e-04f, 1.189579420e-04f, -1.868187316e+00f, 8.743241091e-01f},
        {1.000000000e+00f, -1.846598909e+00f, 1.000000000e+00f, -1.893376849e+00f, 9.091422772e-01f},
        {1.000000000e+00f, -1.920554818e+00f, 1.000000000e+00f, -1.924693979e+00f, 9.510640487e-01f},
        {1.000000000e+00f, -1.937490253e+00f, 1.000000000e+00f, -1.952690794e+00f, 9.851091792e-01f},
    },
    { /* RADIO_RX_CW */
        {9.909858826e-05f, -1.905667778e-04f, 9.909858826e-05f, -1.965425852e+00f, 9.658570422e-01f},
        {1.000000000e+00f, -1.989358310e+00f, 1.000000000e+00f, -1.974494148e+00f, 9.755896081e-01f},
        {1.000000000e+00f, -1.994585632e+00f, 1.000000000e+00f, -1.985220842e+00f, 9.870286284e-01f},
        {1.000000000e+00f, -1.995756899e+00f, 1.000000000e+00f, -1.993900560e+00f, 9.960977445e-01f},
    },
    { /* RADIO_RX_AM */
        {6.174973037e-04f, 7.696984803e-04f, 6.174973037e-04f, -1.523182326e+00f, 5.983372849e-01f},
        {1.000000000e+00f, -5.227822017e-01f, 1.000000000e+00f, -1.509854398e+00f, 7.055990040e-01f},
        {1.000000000e+00f, -1.082776295e+00f, 1.000000000e+00f, -1.504348793e+00f, 8.382791437e-01f},
        {1.000000000e+00f, -1.243858071e+00f, 1.000000000e+00f, -1.529093201e+00f, 9.495934571e-01f},
    },
    { /* RADIO_RX_FM */
        {5.961679336e-03f, 1.088721648e-02f, 5.961679336e-03f, -1.028856634e+00f, 3.175170059e-01f},
        {1.000000000e+00f, 9.979987707e-01f, 1.000000000e+00f, -7.987737912e-01f, 5.200792826e-01f},
        {1.000000000e+00f, 4.128926161e-01f, 1.000000000e+00f, -5.661995320e-01f, 7.470158689e-01f},
        {1.000000000e+00f, 1.744139099e-01f, 1.000000000e+00f, -4.543099145e-01f, 9.221599470e-01f},
    },
};"""


def ellip(cutoff: float):
    """8th-order elliptic, ripple centred on 0 dB like the frozen rows."""
    z, p, k = signal.ellip(8, 0.1, 80, cutoff, fs=FS, output="zpk")
    sos = signal.zpk2sos(z, p, k, pairing="nearest")
    _w, h = signal.sosfreqz(sos, worN=np.linspace(0.0, cutoff, 4000), fs=FS)
    mag = np.abs(h)
    sos[0, :3] /= np.sqrt(mag.max() * mag.min())
    return sos


def frozen_row(mode: int) -> str:
    """One demodulator's rows of FROZEN_CHANNEL_SOS, re-indented."""
    body = FROZEN_CHANNEL_SOS.split("{ /*")[mode + 1]
    lines = [ln.strip() for ln in body.splitlines()[1:] if ln.strip().startswith("{")]
    return "\n".join("        " + ln for ln in lines[:4])


def butter(cutoff: float):
    """8th-order Butterworth with every section at unity DC gain. zpk2sos puts
    the whole 1e-18 gain in the first section, which leaves the float biquads
    passing signals 360 dB down between sections; per-section unity gain keeps
    every state variable at the signal's own scale."""
    z, p, k = signal.butter(8, cutoff, fs=FS, output="zpk")
    sos = signal.zpk2sos(z, p, k, pairing="nearest")
    for section in sos:
        section[:3] *= (1.0 + section[4] + section[5]) / section[:3].sum()
    return sos


def rows(sos, indent: str) -> str:
    out = []
    for b0, b1, b2, _a0, a1, a2 in sos:
        values = ", ".join(f"{v:.9e}f" for v in (b0, b1, b2, a1, a2))
        out.append(f"{indent}{{{values}}},")
    return "\n".join(out)


def render() -> str:
    text = [
        "/* Generated by tools/gen_rx_channel.py -- do not edit. Complex channel",
        " * filters: the same real low-pass on I and Q after the NCO has moved the",
        " * channel centre to DC, at 48 kHz. Rows: b0 b1 b2 a1 a2 (a0 = 1).",
        " *",
        " * rx_channel_sos: scipy.signal.ellip(8, 0.1 dB, 80 dB), zpk2sos(pairing=",
        " * 'nearest'); USB/LSB +-1350 Hz around +-1650 Hz, CW +-350 Hz, AM +-5 kHz,",
        " * FM +-10 kHz.",
        " * rx_cw_sos: the CW bandwidths of RADIO_CW_BW_TABLE, each an 8th-order",
        " * Butterworth (a soft edge, low ringing). */",
    ]
    text.append(FROZEN_CHANNEL_SOS)
    text.append("")
    text.append(f"static const float rx_cw_sos[{len(CW_BANDWIDTHS)}][4][5] = {{")
    for bw in CW_BANDWIDTHS:
        text.append(f"    {{ /* {bw} Hz, 8th-order Butterworth */")
        text.append(rows(butter(bw / 2.0), "        "))
        text.append("    },")
    text.append("};")
    text.append("")
    text.append(f"static const float rx_ssb_sos[{len(SSB_BANDWIDTHS)}][4][5] = {{")
    for bw in SSB_BANDWIDTHS:
        text.append(f"    {{ /* {bw} Hz, 300-{300 + bw} Hz, elliptic */")
        text.append(frozen_row(0) if bw == 2700 else rows(ellip(bw / 2.0), "        "))
        text.append("    },")
    text.append("};")
    text.append("")
    lo, hi = DIGI_PASSBAND
    text.append(f"/* DIGI: {lo}-{hi} Hz, QMX's digital-mode filter, elliptic */")
    text.append("static const float rx_digi_sos[4][5] = {")
    text.append(rows(ellip((hi - lo) / 2.0), "    "))
    text.append("};")
    text.append("")
    text.append(f"static const float rx_am_sos[{len(AM_BANDWIDTHS)}][4][5] = {{")
    for bw in AM_BANDWIDTHS:
        text.append(f"    {{ /* {bw} Hz (+-{bw // 2000} kHz), elliptic */")
        text.append(frozen_row(3) if bw == 10000 else rows(ellip(bw / 2.0), "        "))
        text.append("    },")
    text.append("};")
    return "\n".join(text) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    content = render()
    if args.check:
        if TARGET.read_text(encoding="utf-8") != content:
            print(f"{TARGET} differs from gen_rx_channel.py output", file=sys.stderr)
            return 1
        print("radio_rx_channel.inc matches its generator")
        return 0
    TARGET.write_text(content, encoding="utf-8")
    print(f"wrote {TARGET}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
