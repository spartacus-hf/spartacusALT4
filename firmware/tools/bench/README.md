# Radio bench harness

Scripts that test a flashed alt2 image on the REV1 radio: the regression
matrix run before a push, CPU and render-margin measurements, forward power,
profiling, and the USB interfaces. `ssb_equiv/` is the host-only bit-exactness
check for the TX renderers.

## Safety

- Transmit tests run **only into a 50 ohm dummy load**. With an antenna on,
  run only the receive-only scripts (`cat_retune.py`, `cat_check.py`,
  `soak.py`, `rx_cpu.py`, matrix section A/B).
- Every key-down is bounded, with no exceptions:
  - most overs are 0.35 s of audio, and lever key-downs last 0.3 s;
  - every key-down runs inside `rig.guard()`, a watchdog on its own SWD
    connection. It mutes and releases at 0.95 s by default and resets the MCU
    if the radio is still transmitting 0.4 s later;
  - `tx_long.py` raises the guard to the audio length + 0.6 s, and nothing
    goes past 4.5 s.
- Settings a script changes go through `rig.keep()`, which records them in
  `results/settings_restore.json` before changing anything and puts them back
  on exit. If a run dies (killed, timeout, console lost), the next script's
  `Rig()` restores them and says so. Check that before trusting a "default
  settings" measurement.
- Do not provoke protection trips (high SWR, missing low-pass filter, VIN
  faults) on purpose.

## Setup

- ST-LINK/V2 on SWD, plus the radio's USB-C, which carries the DIAG console
  and, in audio mode, the sound card and CAT ports.
- `openocd`, `aplay` (alsa-utils), Python 3 with numpy, and the STM32CubeIDE
  GNU toolchain (found on `PATH` or under `/opt/stm32cubeide`).
- PipeWire must not hold the radio's sound card while a script plays to it.
- The scripts take the symbol addresses from the ELF of the flashed tree:
  - `CHIREIX_ALT2`: that tree's `firmware/` (default: this checkout);
  - `BENCH_ELF`: overrides the ELF itself;
  - `BENCH_RESULTS`: where results go (default `tools/bench/results/`,
    git-ignored).

Run the scripts from `tools/bench/`:

```bash
cd firmware && tools/build_release_speed.sh          # after make -C Debug all
tools/bench/flash.sh                             # programs it, starts openocd
cd tools/bench
python3 matrix.py new                            # ~40 min, every section
```

## Before a push: A/B against main

A failure that the previous image shows too is not a regression of the change.
Run the same scripts on both images:

```bash
git worktree add /tmp/alt2-main main             # or: build_instrumented.sh
(cd /tmp/alt2-main/firmware && make -C Debug all && tools/build_release_speed.sh)
./flash.sh /tmp/alt2-main/firmware
CHIREIX_ALT2=/tmp/alt2-main/firmware python3 matrix.py main
./flash.sh                                       # back to this checkout
python3 matrix.py new
```

Compare the `bad` rows of `results/matrix_main.json` and
`results/matrix_new.json`, and the per-row numbers.

## Scripts

| script | what | needs |
|---|---|---|
| `matrix.py TAG [A,B,..]` | A: receive on 5 modes × 10 bands. B: receive options. C: USB-audio overs per band, mode and SSB ANALYTIC (H12P), DIGI, two-tone. D: CW. E: TX CARRIER TEST. F: worst case (WIDE CLOCK + TX PHASE 384K + VOICE + every speech tool). G: waterfall. H: `save` while receiving. I: USB-PD | any image |
| `tx_long.py TAG BAND MODE BACKENDS WAV` | 3 s overs: render queue and PCM backlog every 10 ms, queue slope, drops | any image |
| `power.py paths TAG` | forward detector per SSB ANALYTIC × USB AUDIO POLICY × level | any image |
| `power.py repeat TAG [N]` | forward-detector scatter: N overs and N TX TEST key-downs | any image |
| `pcprof.py rx/tx TAG ...` | PC-sampling profile; `FOCUS=fn` gives per-PC counts, `PENDING=1` shows PendSV-pending state | any image |
| `cat_check.py TAG` | CAT ports answer queries; a line ending gives the console | any image |
| `cat_retune.py TAG` | WSJT-X's case: sound card streaming both ways while CAT moves the dial (FT8/WSPR, 40/20 m) and the mode; retunes, PLL moves, I/O gate, capture gaps. Receive only | any image |
| `disk_mode.py TAG` | USB DISK MODE from the panel, mass-storage check, reset | any image |
| `soak.py [MIN]` | receive with tuning, band and mode churn, counters, event log | any image |
| `rx_cpu.py TAG [BAND]` | receive worker cycles per call, CPU split per mode | instrumented |
| `tx_render.py TAG BAND MODE [BACKENDS]` | render CPU, cycles per frame, queue slope, probe slot 10 | instrumented |
| `ssb_equiv/run.sh [REF]` | host: TX renderers bit-exact, commit vs working tree | host only |

Tools:
- `flash.sh [TREE]` and `openocd.sh start|stop|status` program the radio and
  run the persistent openocd;
- `build_instrumented.sh NAME [COMMIT] [-- --probe FILE 'CALL;' SLOT]` builds
  a cycle-counted image in a scratch copy;
- `instrument.py` does the counting. Neither is ever applied to the checkout
  or committed.

Common settings (`set get/put` ids):

| id | setting |
|---|---|
| 200 | MODE: 0 USB, 1 LSB, 2 CW, 3 AM, 4 FM |
| 201 | BAND index |
| 2 | carrier frequency |
| 42 | SSB ANALYTIC: 0 WEAVER, 1 H12P |
| 62 | USB AUDIO POLICY: 1 DIGI CLEAN, 0 VOICE |
| 71, 75, 78, 83 | COMPRESSOR, TX EQ, MIC AGC, ALC |
| 207 | WIDE CLOCK |
| 208 | TX PHASE 384K |
| 214 | SSB RX FILTER |
| 222 | USB VOX |

## Reading the numbers

- **USB-audio overs need USB VOX** (222 = 1).
- **DIGI CLEAN output is constant.** In the default USB AUDIO POLICY
  (DIGI CLEAN), a WEAVER over of a steady tone goes through the DIGI tone path:
  a constant carrier at full drive whatever the level, as on the QMX.
- **H12P and VOICE follow the audio level.** With H12P or VOICE policy the
  output follows the audio, so the half-scale test tone gives about a quarter
  of the power. On 2026-10-03 the two paths agreed within 1 %.
- **Render queue.** It holds about 28 frames (1 ms each). A slope near 0
  frames/s means the renderer keeps up. A sustained negative slope ends in an
  underrun, which latches FAULT_DMA and a hard pipeline lockout. Only a reset
  clears that on the bench.
- **The clock plan depends on the route to a band.** `nav.go(..., via=)` and
  `VIA=` reproduce a route. Compare cycle counts only at the same SYSCLK.
- **Code placement moves cycle counts.**
  - A hot loop landing on a halfword boundary cost the receive worker 16 % on
    2026-10-03.
  - Any layout-changing build flag or large code change needs an `rx_cpu.py`
    and `tx_render.py` A/B against the previous image.
- **H12P starts each over with a short burst.** The first ~5 ms are near full
  power while its 255-tap Hilbert fills. This is a property of the H12P core,
  and `power.py paths` shows it in its `onset` samples.

## Host bit-exactness (`ssb_equiv/`)

`run.sh [REF]` builds the TX pipeline from REF and from the working tree on
the host. It renders about 81k frames through both and compares every compare
tick, flag, counter and renderer state word. It covers:
- both SSB ANALYTIC backends;
- 4 carriers and both sidebands;
- three speech-processing setups and every TX bandwidth;
- the feature variations listed in `equiv_main.c`.

Run it for any change meant to leave the TX output unchanged. Check that it can
fail by mutating the renderer once.
