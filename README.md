<p align="center">
  <a href="https://spartacus-hf.dev">
    <img src="docs/images/spartacus-hf-wordmark-banner.svg" width="560" alt="SPARTACUS-HF">
  </a>
</p>

<h1 align="center">SPARTACUS ALT4</h1>

<p align="center">
  Open-source firmware for the SPARTACUS-HF REV1 transceiver:<br>
  an STM32G474 that generates its own transmit and receive clocks, with digital outphasing and envelope control.
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue" alt="License: GPL-3.0-or-later"></a>
  <img src="https://img.shields.io/badge/MCU-STM32G474-green" alt="STM32G474">
  <img src="https://img.shields.io/badge/board-SPARTACUS--HF%20REV1-lightgrey" alt="SPARTACUS-HF REV1">
</p>

ALT4 is the stable public release line of the SPARTACUS-HF firmware.
The current firmware version is `v9.00`.

[SPARTACUS-HF](https://spartacus-hf.dev) is an HF transceiver for amateur and
emergency radio. Its transmitter is two Class-D switching branches combined by
transformers and driven by digital outphasing (Chireix) with envelope control;
the STM32G474's HRTIM generates the RF and the receiver's quadrature clocks.
The receiver is a Tayloe quadrature sampling detector with a PCM1821 stereo
audio ADC.

> **Transmitting needs an amateur radio licence** for the band and power used.
> Test into a 50 Ω dummy load. REV1 is a development design: it has not
> completed manufacturing, safety, regulatory, transmitter or receiver
> qualification, and the firmware comes with no warranty (see [LICENSE](LICENSE)).

## Features

- **Modes:** USB, LSB, CW, AM and FM, plus DIGI for WSJT-X and similar
  programs, and a TX CARRIER TEST page.
- **Bands:** 160 m to 10 m. On 10 m up to 28.33 MHz, the HRTIM's limit at
  170 MHz.
- **Transmitter:** outphased carrier from the HRTIM with fractional period
  control, per-band clock planning, envelope DMA, ALC, compressor, TX EQ and
  CESSB tools for SSB, and per-band calibration.
- **Protection:** SWR, supply and stray-RF trips, an emergency stop of every
  RF output, and pipeline-loss protection that inhibits RF on any lost
  frame, DMA anomaly or invalid compare value.
- **Receiver:** I/Q calibration from a built-in test clock, a CW decoder, and
  a waterfall and spectrum page.
- **One USB-C data cable, QMX-style:** a USB sound card (receive audio or I/Q,
  transmit audio), two serial ports with Kenwood TS-480 CAT (WSJT-X: rig
  `QRPLabs QMX`) or the console, USB mass storage for the SD card, and a
  separate diagnostic-console personality.
- **USB-C PD:** the radio negotiates its supply (9 V and up) from a USB-C PD
  charger.
- **CW:** iambic keyer with memories, an on-radio Morse trainer for
  beginners, zero-beat aids and radio memories
  ([CW guide](firmware/docs/CW_TRAINER.md)).
- **SD card:** settings and calibration in a two-copy store outside the file
  system, a file browser, WAV and MP3 playback through the transmitter,
  measurement exports, and firmware updates from the card.
- **Self test and diagnostics:** a 54-check self test that never transmits, a
  scriptable USB console (`tools/rev1_diag.py`) and a full-screen terminal
  view with a virtual paddle.
- **S-CTRL LINK:** a framed channel for
  [S-CTRL](https://github.com/spartacus-hf/spartacusControl), the Linux
  desktop companion (live screen mirror, panel, PC speaker, safeguarded TX).

## Hardware

The firmware targets the **SPARTACUS-HF REV1** board (STM32G474VET6).
Its KiCad project is
[spartacus-hf/spartacus_PCB_REV1](https://github.com/spartacus-hf/spartacus_PCB_REV1).
The pin and electrical contract between board and firmware is
[`firmware/docs/REV1.md`](firmware/docs/REV1.md), with every GPIO in
[`firmware/docs/rev1_pins.csv`](firmware/docs/rev1_pins.csv).

## Install a release

Each [release](../../releases) carries the packaged images:

| File | Use |
|---|---|
| `REV1FW.UPD` | Update package: copy to the SD card root and install from the radio |
| `HELP.BIN`, `CWPACK.BIN` | SD card root: the settings help and the Morse trainer's pack |
| `rev1_factory.bin` / `.elf` | One-time install of loader and application (ROM DFU or ST-LINK) |
| `rev1_recovery.bin` / `.elf` | Application only, protected loader left in place (ROM DFU or ST-LINK) |
| `SHA256SUMS` | Checksums of the files above |

On a radio that already runs this firmware: **SD CARD → USB DISK MODE**, copy
the files into the SD root, eject, **EXIT USB DISK**, then
**SD CARD → FIRMWARE UPDATE** and hold the knob. A new board, ROM DFU without
an ST-LINK, recovery and the failure behaviour are in
[`firmware/docs/USB_FIRMWARE_UPDATE.md`](firmware/docs/USB_FIRMWARE_UPDATE.md).
First power-up of a newly assembled board:
[`firmware/docs/BRINGUP.md`](firmware/docs/BRINGUP.md).

## Build from source

You need the STM32 GNU toolchain (`arm-none-eabi-gcc` on `PATH`, or the GNU
Tools for STM32 shipped with STM32CubeIDE under `/opt/stm32cubeide`), GNU
make, Python 3 and a host C compiler for the test suites. From `firmware/`:

```sh
make -C Bootloader all                 # the protected first-stage loader
make -C Debug all                      # application; packages dist/debug/
tools/build_release_speed.sh           # the release image; dist/release-speed/
tools/build_diagnostic.sh              # optional deep-diagnostic image
python3 tools/validate_current.py      # host tests and linked-image policies
python3 tools/validate_current.py --release
```

`dist/release-speed/` then holds the same files as a release. The project
also opens in STM32CubeIDE: import `firmware/` as an existing project. Its
project and ELF are named `alt2`, the firmware's development name, and the
**alt2 Factory Debug** and **alt2 Debug** launch configurations flash it over
ST-LINK. The firmware's internals, real-time rules and validation tiers are
in [`firmware/README.md`](firmware/README.md).

## Repository layout

| Path | What |
|---|---|
| `firmware/Core` | Application: RF and modulation, receiver DSP, storage, UI, USB console |
| `firmware/USB_Device` | USB audio, CAT, mass-storage and console device integration |
| `firmware/Bootloader` | The 4 KiB first-stage loader that installs updates from the SD card |
| `firmware/Drivers`, `firmware/Middlewares` | ST HAL/CMSIS, ST USB libraries, FatFs and dr_libs, with their licences |
| `firmware/Debug` | Make files for command-line builds (generated by STM32CubeIDE) |
| `firmware/sd` | Generated SD-card resources: `HELP.BIN`, `CWPACK.BIN` |
| `firmware/tools` | Validation, packaging, generators, the console host tool, host tests |
| `firmware/tools/bench` | Radio bench harness: regression matrix, CPU and render measurements |
| `firmware/docs` | Board contract, bring-up, firmware update, CW guide, S-CTRL LINK protocol, fonts |

## Documentation

- [REV1 board contract](firmware/docs/REV1.md): clocks, pins, receiver, USB,
  filters, power and SWR meter, USB-C PD, controls, storage.
- [Bring-up, self test and console](firmware/docs/BRINGUP.md)
- [Firmware update and recovery](firmware/docs/USB_FIRMWARE_UPDATE.md)
- [CW: keyer, Morse trainer, memories](firmware/docs/CW_TRAINER.md)
- [S-CTRL LINK protocol](firmware/docs/SCTRL_LINK.md)
- [Firmware internals and validation](firmware/README.md)
- [Radio bench harness](firmware/tools/bench/README.md)
- [Changelog](CHANGELOG.md)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Contact: contact@spartacus-hf.dev.

## Licence

The SPARTACUS-HF firmware is licensed under the
[GNU General Public License v3.0 or later](LICENSE), with an additional
permission to link ST's USB Device and USB-PD libraries, which ST licenses
under SLA0044. Third-party components keep their own licences; see
[NOTICE](NOTICE).
