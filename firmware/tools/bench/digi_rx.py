#!/usr/bin/env python3
"""DIGI receive bench, receive only: slot-aligned recordings of the radio's
sound card as WSJT-X receives it (or the raw I/Q stream), with optional SWD
toggles, and their analysis with jt9.

  digi_rx.py rec LABEL [--dial HZ] [--iq] [--slots N | --secs S]
                       [--lpf-off] [--pwm-off] [--poll] [--note TEXT]
  digi_rx.py ana LABEL [--lo HZ] [--hi HZ] [--ch 0|1] [--slots N]
  digi_rx.py iq LABEL  [--if-hz HZ]

rec   FA/MD9/Q9 over CAT, a console snapshot before and after, pw-record of
      the SPARTACUS source (48 kHz, 2 ch, s16). --lpf-off clears every LPF
      select line over SWD (GPIOE BSRR; the firmware's own filter record is
      untouched, so it does not put the filter back until the next retune):
      the antenna is cut off and the receiver's own floor remains. --pwm-off
      disables the headphone PWM outputs (TIM2 CCER = 0). Both are restored
      afterwards. --poll reads `rx` on the console every second (console
      traffic is itself a variable, so it is off by default).
ana   per slot: jt9 -8 -d 3 decodes (AP-tagged ones counted apart), the floor
      (10th percentile per bin, 300-2900 Hz) during the slot and in the gap.
      On an I/Q recording the audio is demodulated here (USB, lo..hi Hz above
      the dial; the dial is at DC in the radio's I/Q stream).
iq    floor against the offset from the LO, the strongest narrow lines,
      impulse statistics and the decodes per kHz of audio (occupancy).

Safety: refuses to run unless USB VOX (setting 222) is 0, and sends no TX.
Results: BENCH_RESULTS/digi/LABEL.*
"""
import argparse, csv, glob, json, os, subprocess, sys, tempfile, threading, time, wave
import numpy as np
import common

rev1_diag = common.rev1_diag
FS = 48000
OUT = os.path.join(common.RESULTS, "digi")
JT9 = os.environ.get("JT9", os.path.abspath(os.path.join(
    common.ALT2, "..", "bench", "wsjtx-3.0.2", "squashfs-root", "usr", "bin", "jt9")))
GPIOE_ODR, GPIOE_BSRR, TIM2_CCER = 0x48001014, 0x48001018, 0x40000020
LPF_BITS = 31 << 2
SNAP = ("status", "rx", "digi", "clock", "clock sof", "set get 214", "set get 216", "set get 222")


class Port:
    """A CAT port, raw termios (no pyserial on this host)."""
    def __init__(self, path):
        import termios
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        t = termios.tcgetattr(self.fd)
        t[0] = t[1] = t[3] = 0; t[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        termios.tcsetattr(self.fd, termios.TCSANOW, t)
        termios.tcflush(self.fd, termios.TCIOFLUSH)

    def close(self): os.close(self.fd)

    def cat(self, cmd, wait=0.8):
        import select
        os.write(self.fd, cmd.encode())
        out, t0 = b"", time.monotonic()
        while time.monotonic() - t0 < wait and not out.endswith(b";"):
            if select.select([self.fd], [], [], 0.05)[0]:
                out += os.read(self.fd, 64)
        return out.decode(errors="replace")


def ports():
    cat = sorted(glob.glob("/dev/serial/by-id/usb-Chireix_SPARTACUS_REV1_*-if03"))
    if not cat:
        raise SystemExit("radio not in its audio personality (no -if03 CAT port)")
    return cat[0], cat[0].replace("-if03", "-if05")


def source():
    out = subprocess.run(["pactl", "list", "short", "sources"], capture_output=True, text=True).stdout
    for ln in out.splitlines():
        f = ln.split("\t")
        if len(f) > 1 and "SPARTACUS" in f[1] and f[1].startswith("alsa_input"):
            return f[1]
    raise SystemExit("no SPARTACUS PipeWire source")


def snapshot(con):
    return {k: " | ".join(con.command(k)) for k in SNAP}


def rec(a):
    os.makedirs(OUT, exist_ok=True)
    base = os.path.join(OUT, a.label)
    catp, conp = ports()
    con = rev1_diag.Console(conp, 8.0)
    if not con.command("set get 222")[0].strip().endswith("=0"):
        raise SystemExit("USB VOX is on: refusing (receive-only bench)")
    p = Port(catp)
    p.cat(f"FA{a.dial:011d};", wait=0.3)
    p.cat("MD9;", wait=0.3)
    p.cat(f"Q9{1 if a.iq else 0};", wait=0.3)
    time.sleep(2.0)
    cat_state = {k: p.cat(k) for k in ("FA;", "MD;", "Q9;")}
    before = snapshot(con)
    o, saved = None, {}
    if a.lpf_off or a.pwm_off:
        o = common.Ocd()
        if a.lpf_off:
            saved["lpf"] = o.u32(GPIOE_ODR) & LPF_BITS
            o.write(GPIOE_BSRR, 32, LPF_BITS << 16)
        if a.pwm_off:
            saved["ccer"] = o.u32(TIM2_CCER)
            o.write(TIM2_CCER, 32, 0)
        time.sleep(0.5)
    meter, stop = [], threading.Event()

    def poll():
        while not stop.is_set():
            try:
                meter.append((time.time(), " | ".join(con.command("rx"))))
            except Exception as e:  # keep recording whatever happens
                meter.append((time.time(), f"ERR {e}"))
            time.sleep(1.0)

    if a.slots:
        now = time.time()
        nxt = (int(now) // 15 + 1) * 15
        if nxt - now < 1.5:
            nxt += 15
        dur = 15 * a.slots + 2.5
        time.sleep(max(0.0, nxt - time.time() - 1.0))
    else:
        nxt, dur = None, a.secs
    th = threading.Thread(target=poll, daemon=True)
    if a.poll:
        th.start()
    t_launch = time.time()
    try:
        subprocess.run(["timeout", f"{dur:.2f}", "pw-record", "--target", source(), "--rate", "48000",
                        "--channels", "2", "--format", "s16", base + ".wav"])
    finally:
        t_end = time.time()
        stop.set()
        if a.poll:
            th.join()
        if o is not None:
            if "lpf" in saved:
                # only if the firmware has not reselected meanwhile
                if not (o.u32(GPIOE_ODR) & LPF_BITS):
                    o.write(GPIOE_BSRR, 32, saved["lpf"])
            if "ccer" in saved:
                o.write(TIM2_CCER, 32, saved["ccer"])
        after = snapshot(con)
        if a.iq:
            p.cat("Q90;", wait=0.3)
        p.close()
        con.close()
    meta = dict(label=a.label, dial=a.dial, iq=a.iq, slots=a.slots, secs=a.secs, nxt=nxt, t_launch=t_launch,
                t_end=t_end, lpf_off=a.lpf_off, pwm_off=a.pwm_off, saved=saved, cat=cat_state,
                before=before, after=after, meter=meter, note=a.note,
                utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t_launch)))
    json.dump(meta, open(base + ".json", "w"), indent=1)
    print(json.dumps({k: meta[k] for k in ("label", "utc", "dial", "iq", "lpf_off", "pwm_off")}))
    print("  rx before:", before["rx"])
    print("  rx after: ", after["rx"])


def load(label):
    base = os.path.join(OUT, label)
    w = wave.open(base + ".wav")
    x = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, w.getnchannels()) / 32768.0
    meta = json.load(open(base + ".json"))
    skip = int((meta["nxt"] - meta["t_launch"] - 0.15) * FS) if meta.get("nxt") else 0
    return x, max(skip, 0), meta


def usb_demod(x, lo, hi, if_hz=0.0):
    """Radio I/Q (dial at +if_hz; 0 for the radio's stream) -> USB audio lo..hi Hz."""
    from scipy.signal import firwin, oaconvolve
    z = x[:, 0] + 1j * x[:, 1]
    if if_hz:
        z = z * np.exp(-2j * np.pi * if_hz * np.arange(len(z)) / FS)
    h = firwin(4001, (hi - lo) / 2, fs=FS) * np.exp(2j * np.pi * (lo + hi) / 2 * np.arange(-2000, 2001) / FS)
    return np.real(oaconvolve(z, h, mode="same")) * np.sqrt(2)


def decode(seg, hi=4000):
    """jt9 on one 15 s slot (48 kHz real): [(snr, dt, freq, msg, ap)]."""
    from scipy.signal import resample_poly
    y = resample_poly(seg, 1, 4)[:180000]
    y = y * (800.0 / max(np.sqrt(np.mean(y ** 2)), 1e-12))
    with tempfile.TemporaryDirectory() as d:
        fn = os.path.join(d, "000000_000000.wav")
        with wave.open(fn, "wb") as o:
            o.setnchannels(1); o.setsampwidth(2); o.setframerate(12000)
            o.writeframes(np.clip(y, -32767, 32767).astype(np.int16).tobytes())
        r = subprocess.run([JT9, "-8", "-d", "3", "-H", str(int(hi)), "-a", d, "-t", d, fn],
                           capture_output=True, text=True, cwd=d)
    out = []
    for ln in r.stdout.splitlines():
        p = ln.split()
        if len(p) >= 6 and "DecodeFinished" not in ln:
            ap = p[-1] if p[-1] in ("a1", "a2", "a3", "a4", "a5", "a6", "a7", "?") else ""
            msg = " ".join(p[5:-1] if ap else p[5:])
            out.append((int(p[1]), float(p[2]), int(p[3]), msg, ap))
    return out


def psd(s, n=7680):
    segs = [s[i:i + n] * np.hanning(n) for i in range(0, len(s) - n + 1, n // 2)]
    P = np.mean([np.abs(np.fft.rfft(v)) ** 2 for v in segs], axis=0) / np.sum(np.hanning(n) ** 2)
    return np.fft.rfftfreq(n, 1 / FS), P


def ana(a):
    x, skip, meta = load(a.label)
    aud = usb_demod(x, a.lo, a.hi) if meta["iq"] else x[:, a.ch]
    n = a.slots or int((len(aud) - skip) // (15 * FS))
    rows, per = [], []
    for k in range(n):
        seg = aud[skip + k * 15 * FS: skip + (k + 1) * 15 * FS]
        if len(seg) < 14.9 * FS:
            break
        dec = decode(seg, a.hi if meta["iq"] else 4000)
        fr, Pon = psd(seg[2 * FS:10 * FS])
        _, Poff = psd(seg[int(13.4 * FS):int(14.8 * FS)])
        band = (fr > 300) & (fr < 2900)
        lvl = lambda P: 10 * np.log10(np.percentile(P[band], 10))
        nap = sum(1 for d in dec if d[4])
        per.append((k, len(dec), nap, lvl(Pon), lvl(Poff), 20 * np.log10(np.max(np.abs(seg)) + 1e-12)))
        rows += [(k,) + d for d in dec]
        print(f"slot {k:2d}: {len(dec):3d} decodes ({nap} AP), floor slot {lvl(Pon):6.1f} gap {lvl(Poff):6.1f} dB, "
              f"peak {per[-1][5]:6.1f} dBFS")
    tag = f"_{a.lo}_{a.hi}" if meta["iq"] else f"_ch{a.ch}"
    with open(os.path.join(OUT, a.label + tag + "_dec.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["slot", "snr", "dt", "freq", "msg", "ap"])
        w.writerows(rows)
    P = np.array(per)
    above = sum(1 for r in rows if r[3] > 3200)
    print(f"{a.label}{tag}: {len(P)} slots, decodes/slot {P[:,1].mean():.2f} (AP {P[:,2].mean():.2f}, "
          f"without AP {P[:,1].mean() - P[:,2].mean():.2f}); above 3200 Hz {above / max(len(P), 1):.2f}/slot; "
          f"floor slot-gap {np.median(P[:,3] - P[:,4]):+.1f} dB; peak max {P[:,5].max():.1f} dBFS")


def iq(a):
    x, skip, meta = load(a.label)
    x = x[skip:]
    z = x[:, 0] + 1j * x[:, 1]
    if a.if_hz is None:
        import re
        m = re.search(r"if_hz=(-?\d+)", meta["before"]["rx"])
        a.if_hz = float(m.group(1)) if m else 0.0
    n = FS
    win = np.hanning(n); wn = np.sum(win ** 2)
    S = np.array([np.abs(np.fft.fftshift(np.fft.fft(z[i:i + n] * win))) ** 2 / wn
                  for i in range(0, len(z) - n + 1, n)])
    f = np.fft.fftshift(np.fft.fftfreq(n, 1 / FS))
    p10 = 10 * np.log10(np.percentile(S, 10, axis=0) + 1e-30)
    lo = -a.if_hz  # the LO in the stream (dial at DC, if = dial - LO)
    print(f"{a.label}: {len(z)/FS:.0f} s, I/Q rms {20*np.log10(np.std(z)/np.sqrt(2)):.1f} dBFS, "
          f"peak {20*np.log10(np.max(np.abs(x))):.1f} dBFS, IF {a.if_hz:.0f} Hz (LO at {lo:+.0f} Hz)")
    print("floor p10 dBFS/Hz vs offset from the LO (median of 250 Hz):")
    line = []
    for off in (250, 500, 1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 14000, 16000):
        for sgn in (1, -1):
            c = lo + sgn * off
            m = (f > c - 125) & (f < c + 125)
            if m.any() and abs(c) < 19000:
                line.append(f"{sgn*off:+6d}:{np.median(p10[m]):6.1f}")
    print("  " + " ".join(line))
    ch = (f > 150) & (f < 3200)
    print(f"  DIGI channel (dial +150..3200): {np.median(p10[ch]):.1f} dBFS/Hz")
    # narrow lines: p10 against its own 200 Hz running median
    from scipy.signal import medfilt
    base = medfilt(p10, 201)
    exc = p10 - base
    idx = [i for i in np.argsort(exc)[::-1] if abs(f[i]) < 19000][:400]
    picked = []
    for i in idx:
        if exc[i] < 6:
            break
        if all(abs(f[i] - f[j]) > 20 for j in picked):
            picked.append(i)
        if len(picked) >= 15:
            break
    print("  lines (p10 above local median): " + ", ".join(f"{f[i]:+.0f} Hz {exc[i]:.0f} dB" for i in picked))
    # impulse statistics on the complex envelope
    env = np.abs(z)
    sig = np.median(env) / np.sqrt(np.log(4))  # Rayleigh: median = sigma sqrt(2 ln 2)
    for k in (5, 7, 10):
        print(f"  |z| > {k} sigma: {np.mean(env > k * sig) * 1e6:9.1f} ppm "
              f"(Rayleigh {np.exp(-k * k / 2) * 1e6:.3g} ppm)")
    r = np.real(z) / np.std(np.real(z))
    print(f"  kurtosis(I) {np.mean(r ** 4):.2f} (Gaussian 3.00)")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("rec")
    r.add_argument("label")
    r.add_argument("--dial", type=int, default=14074000)
    r.add_argument("--iq", action="store_true")
    r.add_argument("--slots", type=int, default=0)
    r.add_argument("--secs", type=float, default=30.0)
    r.add_argument("--lpf-off", action="store_true")
    r.add_argument("--pwm-off", action="store_true")
    r.add_argument("--poll", action="store_true")
    r.add_argument("--note", default="")
    s = sub.add_parser("ana")
    s.add_argument("label")
    s.add_argument("--lo", type=int, default=150)
    s.add_argument("--hi", type=int, default=3200)
    s.add_argument("--ch", type=int, default=0)
    s.add_argument("--slots", type=int, default=0)
    q = sub.add_parser("iq")
    q.add_argument("label")
    q.add_argument("--if-hz", type=float, default=None)
    a = ap.parse_args()
    {"rec": rec, "ana": ana, "iq": iq}[a.cmd](a)


if __name__ == "__main__":
    main()
