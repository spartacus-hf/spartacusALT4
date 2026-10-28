# HRTIM Atomic MCMP Burst-DMA Experiment

Status: implemented only in the RF-inhibited diagnostic build. The production
firmware deliberately retains the two `TIM6_UP` DMA channels until this
experiment passes on the target PCB and the HRTIM output edges have been
measured.

## Why this exists

`mod_transport.c` currently sends each 48 kHz sample pair through independent
DMA1 channels to `MCMP1R` and `MCMP2R`. Matching requests and the CNDTR skew
monitor detect large divergence, but they cannot prove that both preload writes
precede the same HRTIM master update. An update between the two AHB writes can
therefore expose a mixed old/new branch pair. The STM32 DMAMUX documentation
also warns against assigning the same nonzero peripheral request to multiple
DMA channels.

The diagnostic implementation uses one ordered two-word DMA burst. HRTIM
publishes the pair coherently at the first master rollover after the burst:

```text
TIM6 TRGO=UPDATE (48 kHz)
        |
HRTIM EEV10 -> Timer F counter reset -> HRTIM1_F DMA request
        |
DMA1 channel 1, circular, 32-bit, source increment
        |
HRTIM1 BDMADR -> MCMP1 preload, MCMP2 preload
        |
master BRSTDMA=10 -> coherent update at the next master rollover
```

The implementation uses the primitives exposed by STM's STM32G4 HAL and device
headers: `HRTIM_EEV10SRC_TIM6_TRGO`,
`HRTIM_TIMRESETTRIGGER_EEV_10`, `HRTIM_TIMDIER_RSTDE`,
`DMA_REQUEST_HRTIM1_F`, `HRTIM_BURSTDMA_CMP1 | HRTIM_BURSTDMA_CMP2`,
and `BDMADR`. It deliberately avoids the DMAMUX request generator and the
associated ES0430 errata.

## Candidate contract

- Interleave the existing arrays as
  `{mcmp1[0], mcmp2[0], mcmp1[1], mcmp2[1], ...}`.
- Use a 192-word circular DMA buffer: 96 sample pairs. Half-transfer is exactly
  96 words/48 complete pairs; transfer-complete is the second 48 complete pairs.
- Configure HRTIM master burst update for CMP1 and CMP2 only. Both compare
  registers retain preload; `BRSTDMA=10` publishes the complete pair at the
  next master rollover and master repetition updates are disabled.
- Route the existing rational 48 kHz TIM6 update TRGO through EEV10. Timer F
  runs continuously at DIV4 with period `0xFFDF`; EEV10 resets it. `RSTDE` is
  explicitly enabled only while DMA and TIM6 are running and produces the
  `HRTIM1_F` request that starts the two-word BDMADR burst.
- Use one DMA channel with request ID `HRTIM1_F`; channel 2 is never initialized
  in this build. TIM6 update DMA is disabled.
- A 48 kHz edge must not occur while a two-word request burst or its HRTIM
  update is outstanding. Any odd CNDTR position, DMA transfer error, late
  refill, or boundary/sequence anomaly is a hard transport fault.
- `MOD_HRTIM_ATOMIC_EXPERIMENT` defaults to `MOD_DEEP_DIAG_ENABLE`. Compilation
  rejects enabling it without the deep-diagnostic build, and every
  `rf_en_gpio_set()` call is forced low while it is enabled.

The production-shared host model in `tools/tests/transport_logic_test.c`
exercises 10 million paired transfers, both circular boundaries, sequence
tracking, and rejection of odd CNDTR values. It proves buffer arithmetic, not
the internal HRTIM parser or silicon timing. The exact compare mapping and both
complementary three-`tHRTIM` minimum pulse widths are exhaustively checked over
every raw phase at all active period sizes in
`tools/tests/modulation_reference_test.c`.

## Safe start, stop, and recovery sequence

Start with RF_EN forced low. HRTIM initialization selects CMP1+CMP2 in
`BDMUPR`, clears all BDTxUPR registers, configures EEV10 and Timer F, disables
master repetition update, loads the legal idle compares using an independent
stopped-timer software update, then arms `BRSTDMA=10` before starting the HRTIM
counters. Transport initialization fills/interleaves both halves, retargets
DMA1 channel 1 from the Cube-generated TIM6 request to `HRTIM1_F`, verifies the
live HRTIM/TIM6 register contract, and leaves `RSTDE` masked. The bounded start
critical section issues a TIM6 software update to reset Timer F, rewrites
`BDMUPR=0xC0` to initialize the HRTIM burst parser at MCMP1, starts circular DMA
to BDMADR, enables `RSTDE`, and starts TIM6 last.

For stop or error, force RF_EN low first, stop TIM6, mask `RSTDE`, and then
disable DMA. After the channel stops, rewrite `BDMUPR=0xC0`; RM0440 requires a
write to at least one burst update-selection register to reinitialize a burst
interrupted during a transaction. An odd CNDTR is latched as an unsafe partial
pair. With RF still inhibited, the emergency park restores both MCMP registers
to the computed idle pair and requests a master software update. Restart always
refills both complete halves, repeats the register checks, resets Timer F, and
reinitializes the parser before DMA and TIM6 are enabled.

## Bench acceptance

1. Keep the PA/power stage disconnected. Flash a dedicated experimental build;
   do not reuse production settings or permit RF_EN to rise.
2. Through ST-Link, confirm the device ID, clocks, TIM6 3541/3542-tick period
   pattern, EEV10 source, Timer-F reset configuration, `BDMUPR=0xC0`,
   `BRSTDMA=10`, and `MREPU=0`. Confirm `RSTDE=0` while stopped, `RSTDE=1` only
   while the DMA/TIM6 stream runs, even CNDTR at every HT/TC callback, and
   forward progress over at least 10 million sample pairs.
3. Feed alternating, maximally separated but valid MCMP pairs. Probe both HRTIM
   branch outputs with a two-channel scope or logic analyzer. Verify that no RF
   carrier period contains one branch from the old pair and one from the new.
4. Repeat at every supported carrier geometry, around both circular boundaries,
   and while repeatedly starting, stopping, USB-resetting, and injecting DMA
   abort/error recovery.
5. Run USB audio and maximum allowed LCD/background load. The 48 kHz average,
   buffer sequence, phase relationship, and fault counters must remain stable.

ST-Link is sufficient for register, counter, wrap, and fault-path checks. It is
not sufficient for the phase-purity acceptance in step 3. Do not remove the
compile-time RF inhibit or replace the production backend until every step
passes and the measured traces are saved with the firmware version and test
carrier geometry.
