"""USB sound-card transmit audio: test signals and the audio window.

`window(r, seconds)` switches the radio to its audio personality (USB sound
card + CAT ports) for that long and returns the ALSA card; the DIAG console
is closed meanwhile and comes back when the window ends (r.open_console).
USB-audio overs need USB VOX (setting 222) on. In the default USB AUDIO
POLICY (62 = 1, DIGI CLEAN) a WEAVER over of a steady tone goes through the
DIGI tone path: a constant carrier at full drive whatever the level. H12P
and USB AUDIO POLICY 0 (VOICE) follow the audio level.
"""
import os
import subprocess
import time
import wave

import numpy as np

import common
import rig

DIR = os.path.join(common.RESULTS, "audio")
FS = 48000


def _write(name, x):
    x = np.clip(x, -1.0, 1.0)
    d = (x * 32767).astype("<i2")
    with wave.open(os.path.join(DIR, name), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(FS)
        w.writeframes(np.stack([d, d], 1).ravel().tobytes())


def ensure():
    """Generate the test signals once (results/audio/)."""
    os.makedirs(DIR, exist_ok=True)
    t = lambda s: np.arange(int(s * FS)) / FS                     # noqa: E731
    specs = {
        "tone1k_350ms.wav": lambda: 0.5 * np.sin(2 * np.pi * 1000 * t(0.35)),
        "loud1k_350ms.wav": lambda: 0.99 * np.sin(2 * np.pi * 1000 * t(0.35)),
        "two_tone_350ms.wav": lambda: 0.45 * (np.sin(2 * np.pi * 700 * t(0.35)) + np.sin(2 * np.pi * 1900 * t(0.35))),
        "tone1k_3s.wav": lambda: 0.5 * np.sin(2 * np.pi * 1000 * t(3.0)),
        "speech_3s.wav": lambda: _speech(t(3.0)),
    }
    for name, make in specs.items():
        if not os.path.exists(os.path.join(DIR, name)):
            _write(name, make())
    return DIR


def _speech(tt):
    """Noise under a 4 Hz syllable envelope over a quiet hum: speech-like level swings."""
    rng = np.random.default_rng(7)
    env = (0.5 + 0.5 * np.sin(2 * np.pi * 4 * tt)) ** 3
    return 0.8 * env * rng.uniform(-1, 1, tt.size) + 0.002 * np.sin(2 * np.pi * 180 * tt)


def path(name):
    ensure()
    return name if os.path.isabs(name) else os.path.join(DIR, name)


def seconds(name):
    with wave.open(path(name)) as w:
        return w.getnframes() / w.getframerate()


def window(r, seconds_=25):
    """Audio personality for seconds_; returns the ALSA card (muted, receiving)."""
    r.cmd(f"audio {seconds_}")
    time.sleep(0.15)
    r.close_console()
    for _ in range(60):
        r.safe()
        time.sleep(0.03)
    card = common.rev1_diag.find_capture_card("SPARTACUS REV1", 15.0)
    r.safe()
    time.sleep(1.0)
    assert r.u8("cfg_muted") == 1 and r.u8("state") not in rig.TX_STATES
    return card


def play(card, name):
    """Start playing a test signal to the radio's sound card (returns the Popen)."""
    return subprocess.Popen(["aplay", "-q", "-D", f"hw:{card},0", path(name)])
