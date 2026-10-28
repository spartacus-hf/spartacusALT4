#!/usr/bin/env python3
"""Read a REV1 USB capture and say what the receiver actually delivered.

The radio's USB audio personality sends one of two things, chosen by the
`USB RAW IQ` setting:

  off  both channels carry the demodulated 48 kHz audio;
  on   the left channel is I and the right is Q, taken after the front end has
       already shifted the tuned frequency to DC. The capture is then a
       complex baseband spanning +-24 kHz around the dial.

    tools/rev1_diag.py audio 30 --iq 1 --out iq.wav
    tools/rx_audio_analyze.py iq.wav --iq --dial 7.100e6
    tools/rx_audio_analyze.py audio.wav --plot audio.png

It also reads the sweep CSVs the radio writes to EXPORTS/, and puts them next
to the QRP Labs figures for the same measurement:

    tools/rx_audio_analyze.py AFS000.CSV --compare-qmx

This is an instrument, not part of the validation chain. numpy is required;
matplotlib only for --plot.

The tool says whether the capture really is I/Q; with `USB RAW IQ` off both
channels carry the same demodulated audio.
"""

from __future__ import annotations

import argparse
import sys
import wave

import numpy as np

FULL_SCALE = 32768.0


def read_capture(path: str) -> tuple[np.ndarray, int]:
    with wave.open(path, "rb") as handle:
        if handle.getsampwidth() != 2:
            raise SystemExit("rx_audio_analyze: expected 16-bit samples")
        channels = handle.getnchannels()
        rate = handle.getframerate()
        raw = np.frombuffer(handle.readframes(handle.getnframes()), dtype="<i2")
    if channels not in (1, 2):
        raise SystemExit(f"rx_audio_analyze: expected mono or stereo, got {channels}")
    return raw.reshape(-1, channels).astype(np.float64) / FULL_SCALE, rate


def dbfs(x: np.ndarray | float) -> np.ndarray | float:
    return 20.0 * np.log10(np.maximum(np.abs(x), 1e-12))


def spectrum(signal: np.ndarray, rate: int, complex_input: bool):
    """Averaged periodogram, normalised so a full-scale tone reads 0 dBFS."""
    size = 8192
    if signal.shape[0] < size:
        raise SystemExit("rx_audio_analyze: capture is too short (need ~0.2 s)")
    window = np.hanning(size)
    gain = window.sum()
    blocks = signal.shape[0] // (size // 2) - 1
    total = np.zeros(size if complex_input else size // 2 + 1)
    for index in range(blocks):
        start = index * (size // 2)
        chunk = signal[start:start + size] * window
        frame = np.fft.fft(chunk) if complex_input else np.fft.rfft(chunk)
        total += np.abs(frame) ** 2
    power = total / max(blocks, 1)
    magnitude = np.sqrt(power) / gain * (2.0 if not complex_input else 1.0)
    if complex_input:
        freqs = np.fft.fftfreq(size, 1.0 / rate)
        order = np.argsort(freqs)
        return freqs[order], dbfs(magnitude[order]), size
    return np.fft.rfftfreq(size, 1.0 / rate), dbfs(magnitude), size


def report_peaks(freqs: np.ndarray, levels: np.ndarray, floor: float,
                 count: int, dial: float | None) -> list[tuple[float, float]]:
    """The strongest separated peaks, which is the list of signals heard."""
    found: list[tuple[float, float]] = []
    working = levels.copy()
    spacing = max(1, int(round(200.0 / (freqs[1] - freqs[0]))))  # 200 Hz apart
    for _ in range(count):
        index = int(np.argmax(working))
        if working[index] < floor + 6.0:
            break
        found.append((float(freqs[index]), float(levels[index])))
        low = max(0, index - spacing)
        working[low:index + spacing] = -300.0
    for offset, level in found:
        absolute = f"  ({(dial + offset) / 1e6:.6f} MHz)" if dial else ""
        print(f"    {offset:+10.1f} Hz   {level:7.1f} dBFS{absolute}")
    return found


# --------------------------------------------------------------------------
# Sweep CSVs against the QRP Labs figures (--compare-qmx).
#
# The radio exports one row per measured point and a "#" header saying which
# sweep it was. QRP Labs publish numbers for some of these and only curves for
# others, so this prints the REV1 measurement beside the QMX figure where one
# exists and says so plainly where none does: an empty verdict column would
# read as a failure when it only means there is nothing to compare against.
#
# docs/REV1.md "QRP Labs hardware-test equivalence" is where every figure
# quoted here comes from; keep the two in step.

QMXP_LPF_CORNER_HZ = (2.9e6, 6.2e6, 11.2e6, 22.4e6, 38.0e6)
QMX_OPPOSITE_DB = 60.0            # operating manual 1.03.002, 8.6.1
QMX_IMAGE_DB = (60.0, 75.0)       # 8.6.3, uncorrected, varies unit to unit
AF_PASSBAND_HZ = (150.0, 3200.0)  # the REV1 channel filter's own corners
LPF_LABELS = ("160M", "80/60M", "40/30M", "20/17/15M", "12/10M")


def read_sweep_csv(path: str) -> tuple[list[str], list[str], list[list[str]]]:
    """A sweep CSV as it leaves the radio: comments, column names, rows."""
    comments: list[str] = []
    columns: list[str] = []
    rows: list[list[str]] = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                comments.append(line)
            elif not columns:
                columns = line.split(",")
            else:
                rows.append(line.split(","))
    if not columns or not rows:
        raise SystemExit(f"rx_audio_analyze: {path} has no sweep rows")
    return comments, columns, rows


def curve(columns: list[str], rows: list[list[str]], name: str) -> list[tuple[float, float]]:
    """One named column against the first one, skipping unmeasured points."""
    index = columns.index(name)
    points: list[tuple[float, float]] = []
    for row in rows:
        if len(row) <= index or not row[index] or not row[0]:
            continue
        points.append((float(row[0]), float(row[index])))
    return points


def smooth3(points: list[tuple[float, float]]) -> list[tuple[float, float]]:
    """Median of three. A swept filter measured through its own detector has a
    couple of dB of point-to-point scatter, and one low sample must not be
    allowed to look like the start of a skirt."""
    if len(points) < 3:
        return list(points)
    out = [points[0]]
    for k in range(1, len(points) - 1):
        out.append((points[k][0], sorted(p[1] for p in points[k - 1:k + 2])[1]))
    out.append(points[-1])
    return out


def median(values: list[float]) -> float:
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return 0.5 * (ordered[middle - 1] + ordered[middle])


def interpolate(a: tuple[float, float], b: tuple[float, float], level: float) -> float:
    span = b[1] - a[1]
    return a[0] + ((level - a[1]) / span if span else 0.0) * (b[0] - a[0])


def last_cross_below(points: list[tuple[float, float]], level: float) -> float | None:
    """The highest frequency at which the curve falls through `level` for good.

    Not the first: a passband dip three dB deep is a ripple, not a corner, and
    taking the first crossing reports the skirt of every filter as soon as its
    own ripple happens to touch the threshold."""
    found = None
    for k in range(len(points) - 1):
        if points[k][1] >= level > points[k + 1][1]:
            found = interpolate(points[k], points[k + 1], level)
    return found


def first_cross_above(points: list[tuple[float, float]], level: float) -> float | None:
    """The lowest frequency at which the curve rises through `level`."""
    for k in range(len(points) - 1):
        if points[k][1] < level <= points[k + 1][1]:
            return interpolate(points[k], points[k + 1], level)
    return None


def lowpass_corner(points: list[tuple[float, float]], down_db: float = 3.0):
    """In-band reference and the -3 dB corner of a low-pass response.

    The reference is the median of the bottom quarter of the sweep, which is
    in band for every one of these filters, rather than the peak: a single
    high sample would otherwise drag the threshold up with it."""
    curve = smooth3(points)
    count = max(3, len(curve) // 4)
    reference = median([p[1] for p in curve[:count]])
    return reference, last_cross_below(curve, reference - down_db)


def corners(points: list[tuple[float, float]], down_db: float = 3.0):
    """Peak level and the outermost frequencies that far below it."""
    curve = smooth3(points)
    peak = max(range(len(curve)), key=lambda i: curve[i][1])
    level = curve[peak][1] - down_db
    low = first_cross_above(curve[:peak + 1], level)
    high = last_cross_below(curve[peak:], level)
    return curve[peak][1], low, high


def mhz(value: float | None) -> str:
    return "  --  " if value is None else f"{value / 1e6:6.2f}"


def compare_af(comments: list[str], columns: list[str], rows: list[list[str]]) -> None:
    wanted = curve(columns, rows, "wanted_dbfs")
    if not wanted:
        raise SystemExit("rx_audio_analyze: no wanted-sideband column")
    opposite = dict(curve(columns, rows, "opposite_dbfs")) if "opposite_dbfs" in columns else {}
    reference, low, high = corners(wanted)
    band = [db for hz, db in wanted if AF_PASSBAND_HZ[0] <= hz <= AF_PASSBAND_HZ[1]]
    print("audio filter sweep (QMX 8.6.1)")
    print(f"  peak               {reference:8.1f} dBFS")
    print(f"  -3 dB corners      {low or float('nan'):8.0f} to {high or float('nan'):.0f} Hz"
          f"     design {AF_PASSBAND_HZ[0]:.0f} to {AF_PASSBAND_HZ[1]:.0f} Hz")
    if band:
        print(f"  passband ripple    {max(band) - min(band):8.1f} dB over the same span")
    worst, at = None, None
    for hz, db in wanted:
        if not AF_PASSBAND_HZ[0] <= hz <= AF_PASSBAND_HZ[1] or hz not in opposite:
            continue
        ratio = db - opposite[hz]
        if worst is None or ratio < worst:
            worst, at = ratio, hz
    if worst is not None:
        verdict = "at or above QMX" if worst >= QMX_OPPOSITE_DB else "below QMX"
        print(f"  opposite sideband  {worst:8.1f} dB at {at:.0f} Hz"
              f"     QMX about {QMX_OPPOSITE_DB:.0f} dB -> {verdict}")


def compare_image(comments: list[str], columns: list[str], rows: list[list[str]]) -> None:
    points = curve(columns, rows, "dbfs")
    if not points:
        raise SystemExit("rx_audio_analyze: no level column")
    tone = image = None
    for line in comments:
        if line.startswith("# tone_hz"):
            parts = line.replace(",", " ").split()
            tone, image = float(parts[2]), float(parts[4])
    print("image sweep (QMX 8.6.3)")
    if tone is None or image is None:
        raise SystemExit("rx_audio_analyze: the CSV header carries no tone/image position")

    def near(target: float) -> tuple[float, float]:
        window = [p for p in points if abs(p[0] - target) < 2000.0] or points
        return max(window, key=lambda p: p[1])

    at_tone, at_image = near(tone), near(image)
    ratio = at_tone[1] - at_image[1]
    if ratio >= QMX_IMAGE_DB[1]:
        verdict = "above the QMX range"
    elif ratio >= QMX_IMAGE_DB[0]:
        verdict = "inside the QMX range"
    else:
        verdict = "below the QMX range"
    print(f"  wanted             {at_tone[1]:8.1f} dBFS at {at_tone[0]:+.0f} Hz")
    print(f"  image              {at_image[1]:8.1f} dBFS at {at_image[0]:+.0f} Hz")
    print(f"  rejection          {ratio:8.1f} dB"
          f"     QMX {QMX_IMAGE_DB[0]:.0f}-{QMX_IMAGE_DB[1]:.0f} dB uncorrected -> {verdict}")
    print("  (QMX ships no I/Q correction at all, so run this before and after")
    print("   `rxcal apply` to see what the correction bought this board)")


def compare_lpf(columns: list[str], rows: list[list[str]]) -> None:
    print("low-pass sweep (QMX 8.6.5, which has to transmit for it)")
    print("  slot  band         -3 dB       QMX+ model   difference")
    for slot, model in enumerate(QMXP_LPF_CORNER_HZ):
        name = f"lpf{slot}_dbfs"
        if name not in columns:
            continue
        points = curve(columns, rows, name)
        if len(points) < 4:
            print(f"  {slot}     {LPF_LABELS[slot]:<12} not measured")
            continue
        reference, high = lowpass_corner(points)
        if high is None:
            top = points[-1][0]
            note = (f"flat to the {top / 1e6:.1f} MHz sweep top, as a "
                    f"{model / 1e6:.0f} MHz filter should be" if model > top
                    else "no roll-off found")
            print(f"  {slot}     {LPF_LABELS[slot]:<12} {'  --  ':>6} MHz  {mhz(model)} MHz  {note}")
            continue
        print(f"  {slot}     {LPF_LABELS[slot]:<12} {mhz(high)} MHz  {mhz(model)} MHz"
              f"  {(high - model) / 1e6:+6.2f} MHz   in band {reference:.1f} dBFS")
    print("  (a slot whose toroid is missing or miswound shows no roll-off at all,")
    print("   or the same curve as the slot next to it)")


def compare_bpf(columns: list[str], rows: list[list[str]]) -> None:
    print("RF filter sweep (QMX 8.6.2)")
    print("  slot    peak         -3 dB span")
    for slot in range(4):
        name = f"bpf{slot}_dbfs"
        if name not in columns:
            continue
        points = curve(columns, rows, name)
        if len(points) < 4:
            continue
        reference, low, high = corners(points)
        peak_hz = max(points, key=lambda p: p[1])[0]
        print(f"  {slot}     {mhz(peak_hz)} MHz  {mhz(low)} to {mhz(high)} MHz"
              f"   peak {reference:.1f} dBFS")
    print("  (QRP Labs publish 60-15 m curves rather than numbers, so compare the")
    print("   shapes; what has to be true here is a peak near band centre on each slot)")


def compare_qmx(path: str) -> int:
    comments, columns, rows = read_sweep_csv(path)
    print(f"{path}: {len(rows)} points")
    for line in comments:
        if line.startswith(("# status", "# axis", "# floor", "# tone_hz")):
            print(f"  {line[2:]}")
    if "wanted_dbfs" in columns:
        compare_af(comments, columns, rows)
    elif columns[0] == "offset_hz":
        compare_image(comments, columns, rows)
    elif "lpf0_dbfs" in columns:
        compare_lpf(columns, rows)
    elif "bpf0_dbfs" in columns:
        compare_bpf(columns, rows)
    else:
        raise SystemExit(f"rx_audio_analyze: {path} is not a sweep CSV this knows")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("capture")
    parser.add_argument("--iq", action="store_true",
                        help="the capture is raw I/Q (USB RAW IQ was on)")
    parser.add_argument("--dial", type=float, default=None,
                        help="tuned frequency in Hz, to label peaks absolutely")
    parser.add_argument("--peaks", type=int, default=8)
    parser.add_argument("--plot", help="write a spectrum PNG here")
    parser.add_argument("--compare-qmx", action="store_true",
                        help="the file is a sweep CSV from EXPORTS/, not a capture; "
                             "report it against the QRP Labs figures")
    args = parser.parse_args()

    if args.compare_qmx:
        return compare_qmx(args.capture)

    frames, rate = read_capture(args.capture)
    seconds = frames.shape[0] / rate
    print(f"{args.capture}: {seconds:.1f} s at {rate} Hz, {frames.shape[1]} channels")

    peak = float(np.max(np.abs(frames)))
    print(f"peak sample {dbfs(peak):.1f} dBFS" +
          ("   CLIPPING" if peak >= 0.999 else ""))
    if frames.shape[1] == 2:
        left, right = frames[:, 0], frames[:, 1]
        print(f"channel levels  L {dbfs(np.sqrt(np.mean(left ** 2))):.1f} dBFS   "
              f"R {dbfs(np.sqrt(np.mean(right ** 2))):.1f} dBFS")

    if args.iq:
        if frames.shape[1] != 2:
            raise SystemExit("rx_audio_analyze: --iq needs a stereo capture")
        left, right = frames[:, 0], frames[:, 1]
        # USB RAW IQ off sends the demodulated audio, the same in both
        # channels; a real I/Q capture is two different signals, whether or
        # not anything is in the band. So this one number says whether the
        # radio was in IQ mode for this capture.
        corr = float(np.corrcoef(left, right)[0, 1]) if min(left.std(), right.std()) > 0 else 1.0
        if corr > 0.999:
            print("channels are identical: this is demodulated audio, not I/Q "
                  "(turn USB RAW IQ on and re-record)")
        else:
            print(f"channels differ (correlation {corr:+.3f}): this is an I/Q capture")
        signal = left + 1j * right
    else:
        signal = frames[:, 0]

    freqs, levels, size = spectrum(signal, rate, args.iq)
    floor = float(np.median(levels))
    print(f"median noise floor {floor:.1f} dBFS in a {rate / size:.1f} Hz bin")
    print("strongest peaks, as offsets from the dial:")
    found = report_peaks(freqs, levels, floor, args.peaks, args.dial)

    if args.iq and found:
        # A single dominant carrier lets us read the receiver's image back off
        # the air. With several signals present the mirror bin may hold a real
        # station, so this is a floor on the rejection, not a measurement.
        offset, level = found[0]
        mirror = float(levels[int(np.argmin(np.abs(freqs + offset)))])
        print(f"\nstrongest peak at {offset:+.1f} Hz is {level - mirror:.1f} dB above its mirror")
        print("  (a lower bound on image rejection: the mirror bin may hold a real signal)")
    if not args.iq:
        # The channel filter should be flat over 300-3000 Hz and steep outside.
        band = (freqs >= 300.0) & (freqs <= 3000.0)
        outside = freqs >= 4000.0
        if band.any() and outside.any():
            print(f"\naudio channel  300-3000 Hz {levels[band].mean():.1f} dBFS mean, "
                  f"above 4 kHz {levels[outside].mean():.1f} dBFS mean")

    if args.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        figure, axes = plt.subplots(figsize=(11, 4.5))
        axes.plot(freqs / 1000.0, levels, linewidth=0.7)
        axes.set_xlabel("kHz from the dial" if args.iq else "kHz")
        axes.set_ylabel("dBFS")
        axes.set_title(args.capture)
        axes.grid(alpha=0.3)
        figure.tight_layout()
        figure.savefig(args.plot, dpi=130)
        print(f"wrote {args.plot}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
