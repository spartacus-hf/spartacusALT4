# Changelog

## v9.00 (2026-10-28): first public release

The first open-source release of the SPARTACUS-HF REV1 firmware, under
GPL-3.0-or-later (see [NOTICE](NOTICE)).

### What the firmware does

- Receive and transmit in USB, LSB, CW, AM and FM on 160–10 m, plus DIGI for
  WSJT-X and similar programs, and a TX CARRIER TEST page for the bench.
- Digital Chireix outphasing of two Class-D branches with envelope control.
  The HRTIM generates the carrier and the receiver's quadrature clocks,
  following a per-band clock plan.
- A composite USB-C device, as QMX: sound card, two serial ports (Kenwood
  TS-480 CAT or the console), USB mass storage for the SD card, and a separate
  diagnostic console. A USB-C PD sink negotiates the supply.
- An iambic keyer with memories, a receive CW decoder, zero-beat aids, radio
  memories and an on-radio Morse trainer that runs a whole course
  (Koch/LCWO, CW Academy or LICW order) from the SD card.
- Settings and calibration in a two-copy store on the SD card, a file
  browser, WAV and MP3 playback through the transmitter, and firmware updates
  from the card through a protected first-stage loader.
- Protection: SWR, supply and stray-RF trips stop every RF output, and the
  pipeline-loss protection inhibits RF on any lost frame, DMA anomaly or
  invalid compare value.
- A 54-check self test that never transmits, a scriptable USB console and a
  full-screen terminal view with a virtual paddle.
- The S-CTRL LINK for the Linux desktop companion S-CTRL.

### Changes since the previous development release (v8.11)

- **Carrier:** the HRTIM master period can be a fractional number of carrier
  periods, so from 14 MHz up the gate no longer steps by up to half a grid
  period at every phase update. In AUTO the clock moves at key-down to the
  one that transmits best on the dial, once per key-down and before the
  first frame; it moves back 3 s after the radio last transmitted. TX TEST
  has GRID/HALF/WORST presets per band for scope checks.
- **HRTIM review against RM0440:** dead time is programmed at the resolution
  it is converted with (every DEADTIME setting was 64 times too long). The
  DUTY OFFSET clamp follows the real geometry (a saved out-of-range value
  could halt the radio at boot). The TX outputs are enabled only while
  transmitting, the HRTIM fault interrupt is enabled, and no TX output is
  re-enabled over an emergency stop.
- **Protection reporting:** an SWR trip at key-down now reads
  PROTECTION SWR instead of RF PIPELINE FAULT, and a knob hold on the TX TEST
  page clears it. A TX TEST preset below its low-pass filter's TX minimum
  shows why, instead of ending in a pipeline fault.
- **Morse trainer:** START LEARNING runs the whole course by itself, with
  SAVE PROGRESS and DELETE PROGRESS. It learns the learner's letter spacing
  (an M keyed as two separate dahs is M), and keying and sidetone work when it
  is opened from DIGI.
- **UI:** an unread help text no longer redraws the screen every pass, and
  the trainer's action panel hides the page under it.

### Known limitations

- Transmit power from 15 m to 10 m is low on REV1: about 0.1–0.6 W at
  TUNE 50 % into 50 Ω on the bench, against 0.9 W on 20 m. LPF slot 3 rolls
  off from about 19 MHz. On 10 m the transmitter reaches 28.33 MHz, the HRTIM's
  limit at 170 MHz. See [the board contract](firmware/docs/REV1.md).
- With the WATERFALL page open, transmit is held for 2 s (TX HELD) whenever
  the receiver and the page together reach 95 % of the core.
- `.flac` files are listed by the file browser but not played.
- Some recent SDXC cards do not support SPI mode and read as NO CARD.
- REV1 and this firmware have not completed manufacturing, safety, regulatory,
  transmitter or receiver qualification.
