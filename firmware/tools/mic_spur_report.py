#!/usr/bin/env python3
"""Report voice/HF levels and persistent lines in REV1 microphone captures.

The USB WAV is 48 kHz, while the microphone ADC normally runs at
170 MHz / 3541. Line positions are converted back into the ADC-rate domain so
they can be compared directly with folded clock and PWM harmonics.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
import wave

import numpy as np

BANDS = (("voice", 300.0, 2700.0), ("mid", 2700.0, 4500.0),
         ("hf", 4500.0, 24000.0))
LINE_MIN_DB = 12.0
FLOOR_BINS = 60
DEFAULT_RATE = 170e6 / 3541.0
CLOCKS = {
    "HSE8": 8e6,
    "LO7.139108": 7139108.0,
    "PWM@463": 170e6 / 464.0,
    "PWM@448": 170e6 / 449.0,
}


def load(path: str) -> tuple[np.ndarray, int]:
    with wave.open(path, "rb") as handle:
        channels = handle.getnchannels()
        rate = handle.getframerate()
        raw = np.frombuffer(handle.readframes(handle.getnframes()), dtype="<i2")
    if channels not in (1, 2):
        raise SystemExit(f"mic_spur_report: {path}: expected mono or stereo")
    return raw.reshape(-1, channels)[:, 0].astype(np.float64) / 32768.0, rate


def band_dbfs(signal: np.ndarray, rate: int, lo: float, hi: float) -> float:
    window = np.hanning(len(signal))
    spec = np.fft.rfft((signal - signal.mean()) * window)
    freq = np.fft.rfftfreq(len(signal), 1.0 / rate)
    keep = (freq >= lo) & (freq < hi)
    power = 2.0 * float(np.sum(np.abs(spec[keep]) ** 2)) / float(np.sum(window)) ** 2
    return 10.0 * math.log10(power + 1e-30)


def persistent_lines(signal: np.ndarray, rate: int, segments: int) -> list[tuple[float, float]]:
    block = len(signal) // segments
    if block < 1024:
        raise SystemExit("mic_spur_report: capture too short for the segment count")
    acc = None
    window = np.hanning(block)
    for index in range(segments):
        part = signal[index * block:(index + 1) * block]
        spec = np.abs(np.fft.rfft((part - part.mean()) * window))
        mag = 20.0 * np.log10(spec / np.sum(window) + 1e-30)
        acc = mag if acc is None else acc + mag
    mag = acc / segments
    freq = np.fft.rfftfreq(block, 1.0 / rate)
    found = []
    for index in range(1, len(mag) - 1):
        lo = max(0, index - FLOOR_BINS)
        hi = min(len(mag), index + FLOOR_BINS + 1)
        if (mag[index] > np.median(mag[lo:hi]) + LINE_MIN_DB
                and mag[index] >= mag[index - 1]
                and mag[index] >= mag[index + 1]):
            # Quadratic interpolation in log magnitude removes the 3 Hz bin
            # quantisation of an 8 s / 24-segment report. This matters because
            # the clock-identification test uses a 0.5 Hz tolerance.
            denom = mag[index - 1] - 2.0 * mag[index] + mag[index + 1]
            offset = (0.5 * (mag[index - 1] - mag[index + 1]) / denom
                      if abs(denom) > 1e-12 else 0.0)
            offset = min(0.5, max(-0.5, float(offset)))
            peak_hz = float(freq[index] + offset * rate / block)
            peak_db = float(mag[index] - 0.25 *
                            (mag[index - 1] - mag[index + 1]) * offset)
            found.append((peak_hz, peak_db))
    return found


def fold(value: float, rate: float) -> float:
    rest = value % rate
    return min(rest, rate - rest)


def harmonic_of(line_hz: float, source_hz: float, rate_hz: float,
                harmonics: int = 24, tolerance: float = 0.5) -> int:
    for harmonic in range(1, harmonics + 1):
        if abs(fold(harmonic * source_hz, rate_hz) - line_hz) <= tolerance:
            return harmonic
    return 0


def report(path: str, rate_adc: float, segments: int, top: int) -> dict:
    signal, rate = load(path)
    bands = {name: band_dbfs(signal, rate, lo, hi) for name, lo, hi in BANDS}
    lines = sorted(persistent_lines(signal, rate, segments),
                   key=lambda item: item[1], reverse=True)[:top]
    return {
        "path": path,
        "rate_hz": rate,
        "rate_adc_hz": rate_adc,
        "rms_dbfs": 20.0 * math.log10(float(np.sqrt(np.mean(signal ** 2))) + 1e-30),
        "bands": bands,
        "buzz_db": bands["hf"] - bands["voice"],
        # The real REV1 fixtures already place the WAV line positions on the
        # microphone clock's alias grid; applying rate_adc/rate a second time
        # moves the identified PWM lines 3 Hz away from their predictions.
        "lines": lines,
    }


def print_report(data: dict) -> None:
    print(f"=== {data['path']}  {len(data['lines'])} lines")
    print(f"    rms {data['rms_dbfs']:7.2f} dBFS   rate {data['rate_adc_hz']:.3f} Hz")
    for name, lo, hi in BANDS:
        print(f"    {name:5s} {lo:6.0f}-{hi:6.0f} Hz : {data['bands'][name]:7.2f} dBFS")
    print(f"    buzz (hf - voice)  : {data['buzz_db']:+7.2f} dB")
    for line, db in data["lines"]:
        guess = ""
        for name, hz in CLOCKS.items():
            harmonic = harmonic_of(line, hz, data["rate_adc_hz"])
            if harmonic:
                guess = f"   <- {name} k={harmonic}"
                break
        print(f"    {line:9.2f} Hz {db:8.2f} dB{guess}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+")
    parser.add_argument("--source-rate", type=float, default=DEFAULT_RATE)
    parser.add_argument("--segments", type=int, default=24)
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    reports = [report(path, args.source_rate, args.segments, args.top)
               for path in args.captures]
    if args.json:
        print(json.dumps(reports, indent=2))
    else:
        for data in reports:
            print_report(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
