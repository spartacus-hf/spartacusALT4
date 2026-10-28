#!/usr/bin/env python3
"""Independently regenerate and analyze the H12P analytic filter design."""

from __future__ import annotations

import argparse
import cmath
import json
import math
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True

from validation_support import ROOT, ValidationError


HEADER = ROOT / "Core" / "Inc" / "mod_analytic_logic.h"
SAMPLE_RATE_HZ = 48_000.0
LOW_RATE_HZ = 12_000.0
VOICE_BAND_HZ = range(100, 3_001)
WARMUP_SAMPLES = 4_096
MEASURE_SAMPLES = 48_000


def i0(value: float) -> float:
    """Modified Bessel I0, evaluated independently with its power series."""
    term = 1.0
    total = 1.0
    order = 1
    while abs(term) > abs(total) * 1.0e-18:
        term *= value * value / (4.0 * order * order)
        total += term
        order += 1
    return total


def kaiser(index: int, length: int, beta: float) -> float:
    position = 2.0 * index / (length - 1) - 1.0
    argument = beta * math.sqrt(max(0.0, 1.0 - position * position))
    return i0(argument) / i0(beta)


def design_lowpass() -> list[float]:
    length = 79
    cutoff = 4_500.0 / SAMPLE_RATE_HZ
    center = (length - 1) / 2.0
    taps: list[float] = []
    for index in range(length):
        offset = index - center
        ideal = (
            2.0 * cutoff
            if offset == 0.0
            else math.sin(2.0 * math.pi * cutoff * offset) / (math.pi * offset)
        )
        taps.append(ideal * kaiser(index, length, 7.0))
    gain = sum(taps)
    return [tap / gain for tap in taps]


def design_hilbert_half() -> list[float]:
    length = 255
    center = (length - 1) / 2.0
    taps: list[float] = []
    for pair in range(64):
        index = pair * 2
        offset = index - center
        ideal = 2.0 / (math.pi * offset)
        taps.append(ideal * kaiser(index, length, 6.0))
    return taps


def parse_array(source: str, name: str) -> list[float]:
    match = re.search(rf"{name}[^=]*=\s*\{{(.*?)\}};", source, flags=re.DOTALL)
    if match is None:
        raise ValidationError(f"cannot find {name} in {HEADER.relative_to(ROOT)}")
    return [
        float(token[:-1])
        for token in re.findall(
            r"[-+]?\d+(?:\.\d*)?(?:e[-+]?\d+)?f",
            match.group(1),
            flags=re.IGNORECASE,
        )
    ]


def response(taps: list[float], frequency_hz: float, sample_rate_hz: float) -> complex:
    omega = 2.0 * math.pi * frequency_hz / sample_rate_hz
    return sum(tap * cmath.exp(-1j * omega * index) for index, tap in enumerate(taps))


def analytic_response(hilbert: list[float], frequency_hz: float) -> complex:
    omega = 2.0 * math.pi * frequency_hz / LOW_RATE_HZ
    delayed = cmath.exp(-1j * omega * 127.0)
    quadrature = response(hilbert, frequency_hz, LOW_RATE_HZ)
    i_path = (delayed - quadrature) / math.sqrt(2.0)
    q_path = (delayed + quadrature) / math.sqrt(2.0)
    return i_path + 1j * q_path


class ReferenceH12P:
    """Direct-form double-precision model, independent of the firmware rings."""

    def __init__(self, lowpass: list[float], hilbert_half: list[float]) -> None:
        self.lowpass = lowpass
        self.hilbert_half = hilbert_half
        self.decim = [0.0] * len(lowpass)
        self.hilbert = [0.0] * 255
        self.interp_i = [0.0] * 20
        self.interp_q = [0.0] * 20
        self.phase = 0

    def process(self, sample: float, sideband_sign: float) -> complex:
        self.decim.insert(0, sample)
        self.decim.pop()
        phase = self.phase
        if phase == 0:
            low = sum(tap * delayed for tap, delayed in zip(self.lowpass, self.decim))
            self.hilbert.insert(0, low)
            self.hilbert.pop()
            quadrature = sum(
                coefficient
                * (self.hilbert[2 * pair] - self.hilbert[254 - 2 * pair])
                for pair, coefficient in enumerate(self.hilbert_half)
            )
            delayed = self.hilbert[127]
            i_low = (delayed - sideband_sign * quadrature) / math.sqrt(2.0)
            q_low = (delayed + sideband_sign * quadrature) / math.sqrt(2.0)
            self.interp_i.insert(0, i_low)
            self.interp_i.pop()
            self.interp_q.insert(0, q_low)
            self.interp_q.pop()

        i_signal = 4.0 * sum(
            self.lowpass[tap] * self.interp_i[lag]
            for lag, tap in enumerate(range(phase, len(self.lowpass), 4))
        )
        q_signal = 4.0 * sum(
            self.lowpass[tap] * self.interp_q[lag]
            for lag, tap in enumerate(range(phase, len(self.lowpass), 4))
        )
        self.phase = (phase + 1) % 4
        return complex(i_signal, q_signal)


def reference_two_tone(
    lowpass: list[float], hilbert_half: list[float], sideband_sign: float
) -> float:
    model = ReferenceH12P(lowpass, hilbert_half)
    frequencies = (700, 1900, -700, -1900, -500, 3100, 0)
    accumulators = {frequency: 0j for frequency in frequencies}
    oscillators = {frequency: 1.0 + 0j for frequency in frequencies}
    steps = {
        frequency: cmath.exp(-2j * math.pi * frequency / SAMPLE_RATE_HZ)
        for frequency in frequencies
    }
    total = WARMUP_SAMPLES + MEASURE_SAMPLES
    for sample_index in range(total):
        sample = 0.35 * (
            math.cos(2.0 * math.pi * 700.0 * sample_index / SAMPLE_RATE_HZ)
            + math.cos(2.0 * math.pi * 1900.0 * sample_index / SAMPLE_RATE_HZ)
        )
        output = model.process(sample, sideband_sign)
        if sample_index >= WARMUP_SAMPLES:
            for frequency in frequencies:
                accumulators[frequency] += output * oscillators[frequency]
        for frequency in frequencies:
            oscillators[frequency] *= steps[frequency]

    magnitudes = {
        frequency: abs(accumulator) / MEASURE_SAMPLES
        for frequency, accumulator in accumulators.items()
    }
    if sideband_sign > 0.0:
        desired_frequencies = frequencies[:2]
        unwanted_frequencies = frequencies[2:]
    else:
        desired_frequencies = frequencies[2:4]
        unwanted_frequencies = frequencies[:2] + frequencies[4:]
    desired = min(magnitudes[frequency] for frequency in desired_frequencies)
    unwanted = max(magnitudes[frequency] for frequency in unwanted_frequencies)
    return 20.0 * math.log10(desired / unwanted)


def reference_continuity(lowpass: list[float], hilbert_half: list[float]) -> float:
    samples = [
        0.35
        * (
            math.cos(2.0 * math.pi * 700.0 * n / SAMPLE_RATE_HZ)
            + math.cos(2.0 * math.pi * 1900.0 * n / SAMPLE_RATE_HZ)
        )
        for n in range(4_096)
    ]
    continuous = ReferenceH12P(lowpass, hilbert_half)
    framed = ReferenceH12P(lowpass, hilbert_half)
    expected = [continuous.process(sample, 1.0) for sample in samples]
    frame_sizes = (1, 7, 48, 53, 192)
    actual: list[complex] = []
    offset = 0
    frame = 0
    while offset < len(samples):
        end = min(offset + frame_sizes[frame % len(frame_sizes)], len(samples))
        actual.extend(framed.process(sample, 1.0) for sample in samples[offset:end])
        offset = end
        frame += 1
    return max(abs(a - b) for a, b in zip(actual, expected))


def reference_switch_transient(
    lowpass: list[float], hilbert_half: list[float]
) -> tuple[float, float, int]:
    continuous = ReferenceH12P(lowpass, hilbert_half)
    for n in range(WARMUP_SAMPLES):
        continuous.process(
            math.cos(2.0 * math.pi * 700.0 * n / SAMPLE_RATE_HZ), 1.0
        )

    restarted = ReferenceH12P(lowpass, hilbert_half)
    errors: list[float] = []
    continuous_levels: list[float] = []
    first_output = 0.0
    for offset in range(2_048):
        n = WARMUP_SAMPLES + offset
        sample = math.cos(2.0 * math.pi * 700.0 * n / SAMPLE_RATE_HZ)
        expected = continuous.process(sample, 1.0)
        actual = restarted.process(sample, 1.0)
        if offset == 0:
            first_output = abs(actual)
        errors.append(abs(actual - expected))
        continuous_levels.append(abs(expected))

    reference_rms = math.sqrt(
        sum(level * level for level in continuous_levels) / len(continuous_levels)
    )
    peak_relative = max(errors) / reference_rms
    last_difference = max(
        (index for index, error in enumerate(errors) if error > 1.0e-12),
        default=-1,
    )
    return first_output, peak_relative, last_difference + 1


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


def evaluate() -> dict[str, float | int]:
    source = HEADER.read_text(encoding="utf-8")
    production_lpf = parse_array(source, "mod_analytic12_lpf")
    production_half = parse_array(source, "mod_analytic12_hilbert_half")
    oracle_lpf = design_lowpass()
    oracle_half = design_hilbert_half()

    require(len(production_lpf) == 79, "H12P low-pass must contain 79 taps")
    require(len(production_half) == 64, "H12P reduced Hilbert must contain 64 taps")
    lpf_error = max(abs(a - b) for a, b in zip(production_lpf, oracle_lpf))
    hilbert_error = max(abs(a - b) for a, b in zip(production_half, oracle_half))
    require(lpf_error <= 1.0e-8, f"H12P low-pass differs from the Kaiser-7 design: {lpf_error}")
    require(
        hilbert_error <= 1.0e-8,
        f"H12P Hilbert differs from the Kaiser-6 Type-III design: {hilbert_error}",
    )

    full_hilbert = [0.0] * 255
    for pair, coefficient in enumerate(oracle_half):
        near = pair * 2
        full_hilbert[near] = coefficient
        full_hilbert[254 - near] = -coefficient
    lpf_symmetry_error = max(
        abs(oracle_lpf[index] - oracle_lpf[-1 - index])
        for index in range(len(oracle_lpf))
    )
    hilbert_symmetry_error = max(
        abs(full_hilbert[index] + full_hilbert[-1 - index])
        for index in range(len(full_hilbert))
    )
    require(lpf_symmetry_error <= 1.0e-15, "H12P low-pass lost even symmetry")
    require(hilbert_symmetry_error <= 1.0e-15, "H12P Hilbert lost odd symmetry")

    minimum_image_db = 1000.0
    minimum_amplitude = float("inf")
    maximum_amplitude = 0.0
    minimum_image_frequency_hz = 0
    for frequency in VOICE_BAND_HZ:
        positive = abs(analytic_response(full_hilbert, frequency))
        negative = abs(analytic_response(full_hilbert, -frequency))
        desired = max(positive, negative)
        image = min(positive, negative)
        rejection_db = 20.0 * math.log10(desired / image)
        if rejection_db < minimum_image_db:
            minimum_image_db = rejection_db
            minimum_image_frequency_hz = frequency

        lpf_gain = abs(response(oracle_lpf, frequency, SAMPLE_RATE_HZ))
        amplitude = 0.5 * desired * lpf_gain * lpf_gain
        minimum_amplitude = min(minimum_amplitude, amplitude)
        maximum_amplitude = max(maximum_amplitude, amplitude)

    ripple_db = 20.0 * math.log10(maximum_amplitude / minimum_amplitude)
    pass_3k = abs(response(oracle_lpf, 3_000.0, SAMPLE_RATE_HZ))
    stop_9k = abs(response(oracle_lpf, 9_000.0, SAMPLE_RATE_HZ))
    alias_db = 20.0 * math.log10(pass_3k / stop_9k)
    usb_two_tone_floor_db = reference_two_tone(oracle_lpf, oracle_half, 1.0)
    lsb_two_tone_floor_db = reference_two_tone(oracle_lpf, oracle_half, -1.0)
    two_tone_floor_db = min(usb_two_tone_floor_db, lsb_two_tone_floor_db)
    frame_boundary_error = reference_continuity(oracle_lpf, oracle_half)
    switch_initial, switch_peak_relative, switch_settle_samples = (
        reference_switch_transient(oracle_lpf, oracle_half)
    )

    require(minimum_image_db >= 60.0, f"H12P theoretical image rejection is {minimum_image_db:.2f} dB")
    require(ripple_db <= 0.10, f"H12P theoretical ripple is {ripple_db:.4f} dB")
    require(alias_db >= 80.0, f"H12P 9 kHz alias rejection is {alias_db:.2f} dB")
    require(
        two_tone_floor_db >= 80.0,
        f"H12P 700+1900 Hz image/IMD/carrier floor is {two_tone_floor_db:.2f} dB",
    )
    require(frame_boundary_error == 0.0, "H12P framing changes sample output")

    return {
        "coefficient_error": max(lpf_error, hilbert_error),
        "lpf_symmetry_error": lpf_symmetry_error,
        "hilbert_symmetry_error": hilbert_symmetry_error,
        "minimum_image_rejection_db": minimum_image_db,
        "minimum_image_frequency_hz": minimum_image_frequency_hz,
        "passband_spread_db": ripple_db,
        "alias_rejection_db": alias_db,
        "two_tone_unwanted_floor_db": two_tone_floor_db,
        "usb_two_tone_unwanted_floor_db": usb_two_tone_floor_db,
        "lsb_two_tone_unwanted_floor_db": lsb_two_tone_floor_db,
        "frame_boundary_error": frame_boundary_error,
        "switch_initial_output": switch_initial,
        "switch_peak_relative": switch_peak_relative,
        "switch_settle_samples": switch_settle_samples,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="emit machine-readable results")
    args = parser.parse_args()
    results = evaluate()
    if args.json:
        print(json.dumps(results, indent=2, sort_keys=True))
    else:
        print(
            "H12P independent double model: "
            f"coefficient error {results['coefficient_error']:.2e}, "
            f"image {results['minimum_image_rejection_db']:.2f} dB, "
            f"ripple {results['passband_spread_db']:.4f} dB, "
            f"alias {results['alias_rejection_db']:.2f} dB, "
            f"two-tone floor {results['two_tone_unwanted_floor_db']:.2f} dB, "
            f"frame error {results['frame_boundary_error']:.2e}, "
            f"reset settle {results['switch_settle_samples']} samples"
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValidationError as exc:
        print(f"FAIL  {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
