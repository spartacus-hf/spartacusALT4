#!/usr/bin/env python3
"""I/Q artifact report for the SPARTACUS REV1 raw USB stream.

Consumes S16_LE stereo WAV captures (arecord -f S16_LE -r 48000 -c 2) where the two
channels are the raw I and Q of the receive front end. Reports the whole-stream level,
the persistent line list with harmonic numbers, the best-fit comb fundamental, the
time-domain fold significance, the modulation signature of the strongest line, the
coherent-fold waveform of the disturbance, and the quantisation statistics.

Read-only; writes nothing.
"""
from __future__ import annotations

import sys
import wave

import numpy as np

FOLD_CANDIDATES = (1776, 3552, 888)          # 37.00 ms, 74.00 ms, 18.50 ms
FOLD_CONTROLS = (2125, 1717, 1234)           # off-grid controls
LINE_MIN_DB = 12.0                           # peak must exceed local floor by this much
COMB_HZ = 27.028


def load(path: str) -> tuple[np.ndarray, int]:
    with wave.open(path, "rb") as w:
        fs = w.getframerate()
        n = w.getnframes()
        raw = np.frombuffer(w.readframes(n), dtype="<i2").reshape(-1, 2)
    return raw.astype(np.float64), fs


def spectrum(series: np.ndarray, fs: float) -> tuple[np.ndarray, np.ndarray]:
    """One-sided magnitude spectrum in dBFS per count (complex input: full FFT positive half)."""
    n = len(series)
    win = np.hanning(n)
    spec = np.fft.fft((series - series.mean()) * win)[: n // 2]
    freq = np.fft.fftfreq(n, 1.0 / fs)[: n // 2]
    mag = 20.0 * np.log10(np.abs(spec) / np.sum(win) + 1e-30)
    return freq, mag


def persistent_lines(z: np.ndarray, fs: int, segs: int = 40):
    block = len(z) // segs
    acc = None
    for k in range(segs):
        _, mag = spectrum(z[k * block:(k + 1) * block], fs)
        acc = mag if acc is None else acc + mag
    acc /= segs
    return np.fft.rfftfreq(block, 1.0 / fs), acc


def find_peaks(freq: np.ndarray, mag: np.ndarray, floor_win: int = 60):
    keep = []
    for i in range(1, len(mag) - 1):
        lo = max(0, i - floor_win)
        hi = min(len(mag), i + floor_win)
        local = np.median(mag[lo:hi])
        if mag[i] > local + LINE_MIN_DB and mag[i] >= mag[i - 1] and mag[i] >= mag[i + 1]:
            keep.append(i)
    keep = np.array(keep, dtype=int)
    return freq[keep], mag[keep]


def fit_f0(freqs: np.ndarray, mags: np.ndarray) -> tuple[float, float]:
    w = 10.0 ** (mags / 20.0)
    best = (1e9, 0.0)
    for f0 in np.arange(20.0, 60.0, 0.0005):
        r = freqs / f0
        err = np.abs(r - np.round(r))
        score = float(np.sum(w * err) / np.sum(w))
        if score < best[0]:
            best = (score, f0)
    return best[1], best[0]


def fold_ratio(series: np.ndarray, period: int, bins: int = 32) -> float:
    usable = (len(series) // period) * period
    folded = series[:usable].reshape(-1, period)
    prof = np.abs(np.fft.rfft(folded, axis=0))[:bins].sum(axis=0)
    return float(prof.max() / (np.median(prof) + 1e-30))


def report(path: str) -> None:
    raw, fs = load(path)
    z = raw[:, 0] + 1j * raw[:, 1]
    rms = 20.0 * np.log10(np.sqrt(np.mean(np.abs(z) ** 2)) / 32768.0 + 1e-30)
    zeros = int(np.sum((raw[:, 0] == 0) & (raw[:, 1] == 0)))
    odd_i = float(np.mean((raw[:, 0].astype(int) & 1) != 0))
    print(f"== {path}")
    print(f"   frames {len(z)}  fs {fs}  complex rms {rms:.1f} dBFS  "
          f"exact (0,0) frames {zeros} ({100.0 * zeros / len(z):.2f} %)  odd-I {odd_i:.4f}")

    freq, mag = persistent_lines(z, fs)
    pf, pm = find_peaks(freq, mag)
    print(f"   persistent lines >= {LINE_MIN_DB} dB over local floor: {len(pf)}")
    if len(pf) >= 4:
        f0, resid = fit_f0(pf, pm)
        print(f"   comb fit f0 = {f0:.4f} Hz (period {1000.0 / f0:.3f} ms, "
              f"{fs / f0:.1f} samples) residual {resid:.5f}")
        for i in np.argsort(-pm)[:10]:
            print(f"      {pf[i]:+10.2f} Hz  n={round(pf[i] / f0):4d}  {pm[i]:+7.1f} dB")

    dz = np.abs(np.diff(z))
    for period in FOLD_CANDIDATES + FOLD_CONTROLS:
        tag = "candidate" if period in FOLD_CANDIDATES else "control  "
        print(f"   fold |dz| at {period:5d}: {fold_ratio(dz, period):5.2f}x  [{tag}]")

    usable = (len(z) // 1776) * 1776
    folded = z[:usable].reshape(-1, 1776).mean(axis=0)
    print(f"   coherent fold of z at 1776: peak {np.abs(folded).max():.1f} counts "
          f"at bin {int(np.argmax(np.abs(folded)))}, median {np.median(np.abs(folded)):.1f}")


if __name__ == "__main__":
    for name in sys.argv[1:]:
        report(name)
