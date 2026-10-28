# Contributing to the SPARTACUS-HF firmware

Bug reports, measurements and patches are welcome. Open an issue first for
anything larger than a fix, and say which board, firmware version and
settings you used.

This repository carries the stable releases. A pull request is reviewed
here; once accepted, it is merged into the development tree and ships in the
next release, which replaces the contents of this repository.

## Changes

- Work from `firmware/`. Before every commit run
  `python3 tools/validate_current.py` (it builds the Debug ELF when stale).
  For RF timing, transport, storage, USB audio, linker-layout or release
  changes, run `python3 tools/validate_current.py --release`.
- One understandable purpose per commit. The tree builds and validation passes
  at every commit.
- A behaviour change comes with a host test that fails without it
  (`firmware/tools/tests/`, run by `tools/host_tests.py`). Logic that can be
  free of the HAL is kept free of it (the `*_logic.c` modules) so that every
  rule has a test.
- Generated files are regenerated, never edited: fonts (`gen_ui_fonts.py`),
  receive filters (`gen_rx_coeff.py`, `gen_rx_channel.py`), the settings help
  (`gen_help_text.py` → `sd/HELP.BIN`) and the CW pack (`gen_cw_pack.py` →
  `sd/CWPACK.BIN`). Validation checks each against its generator.
- STM32CubeIDE owns the `firmware/Debug/` make fragments; change them only
  together with the project configuration.
- A version bump goes through `python3 tools/update_version.py vX.YY`, which
  updates every version representation at once; `tools/version_tool.py check`
  verifies them.
- Contributions are licensed under the GNU GPL v3.0 or later with the
  additional permission in [NOTICE](NOTICE), as the rest of the firmware.

## Real-time rules

The USB audio, TIM6, HRTIM, DMA, CW and RF-enable paths are hard real time.

- Blocking storage, FatFs, SD-card access and UI rendering stay out of
  interrupts, DMA and USB callbacks, render functions, and any code reachable
  while `MOD_AudioRealtimeCritical()` is true. The linked-image policy checks
  (`elf_policy_check.py`, `stack_usage_report.py`) enforce this from the call
  graph.
- UI and menu work is cooperative, low priority and bounded, so it cannot
  starve USB audio, the render FIFO, TIM6/HRTIM DMA, CW timing, TX TEST gating
  or carrier-retune safety.
- A change that moves code in the receive or transmit path needs a CPU
  measurement against the previous image on the radio
  (`firmware/tools/bench/`).

[`firmware/README.md`](firmware/README.md) has the details: the main-loop
priority order, the SD framework rules and the storage invariants.

## Radio safety

The firmware keys a transmitter. On the bench:

- The RF output goes into a **50 Ω dummy load**.
- Key-downs from scripts and tests stay short: the bench harness bounds every
  key-down and keeps them under a second by default, never above 5 s.
- Never provoke risky cases on real hardware on purpose: high SWR, a missing
  low-pass filter, supply faults, unplugging or flashing during TX. Those
  belong in the host test suites.
- The settings and calibration store's layout is persistent ABI
  (`storage_layout_report.py` checks it). Change `SETTINGS_VERSION` only with
  an explicit, tested migration.

## Source comments

Comments explain a contract that is not obvious from the code: units and
ownership, invariants, the radio's behaviour, safety constraints, the reason
for a non-obvious choice. Keep them concise and in the present tense.
Measurements and defect histories belong in the commit message. Never commit
temporary tracing or commented-out experiments.
