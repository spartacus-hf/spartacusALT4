# SPARTACUS REV1 hardware and firmware contract

This is the active board contract for the firmware. The authoritative
pin-by-pin list is [`rev1_pins.csv`](rev1_pins.csv), cross-checked against
the REV1 KiCad project,
[spartacus_PCB_REV1](https://github.com/spartacus-hf/spartacus_PCB_REV1).
The CSV deliberately lists MCU GPIO assignments;
power, ground, connector and test points are called out below.

## MCU, clock and power hold

- The target is **STM32G474VET6**: 512 KiB flash, 96 KiB SRAM and 32 KiB CCM
  SRAM. The CEU6 prototype startup file and linker memories are not used.
- PF0 is driven by the external **DSC1003DL2-008.0000T 8 MHz MEMS
  oscillator** (±25 ppm over −40…105 °C, all causes; not a TCXO) through the
  documented 10 ohm connection. It is an HSE bypass clock input, not a
  crystal. The PLL boots at HSE / 2 × 85 / 2 = 170 MHz; the carrier clock
  planner may move it (see *Clock tree and frequency accuracy*). Clock security
  (CSS) is enabled after the PLL is running; a clock failure enters the NMI
  fail-safe and clamps RF outputs before recovery/reset.
  A TCXO may replace it (see *The host's clock moves too*):
  `CLOCK_HSE_NOMINAL_HZ` in `clock_plan.h` and `HSE_VALUE` name its
  frequency, and 16, 24, 32 or 40 MHz keeps every clock plan as it is (not
  48 MHz: M would pass 16). It must be a logic-level (CMOS) output in the
  2.5 × 2.0 mm footprint; the board pulls pin 1 to 3.3 V, so an enable pin
  suits and a VC-TCXO's tuning pin does not. The pick is
  ECS-TXO-2520-33-320-AN-TR, 32 MHz (from the drift study of 2026-10-04,
  *The host's clock moves too*).
- PB10 is `MCU_PWR_HOLD`. It drives Q13 (BSS123) through R10 (1 kΩ). Q13
  holds the gates of Q2 low. Q2 is an APM4953 with back-to-back P-FETs, so
  it blocks both ways; R8 (470 kΩ) pulls its gates high. Every supply (USB-C
  PD and DC) reaches VIN (C15, 1000 µF) through Q2 and JP21. SW8 pulls the
  same gate node low through R9 (1 kΩ). This is the QMX+ soft switch
  (Q103/Q105 with Q104 on `PWR_HOLD`), with two differences:
  - No MCU input sees SW8. QMX powers off when its power button is pressed
    again. REV1 powers off from `POWER OFF` in the menu or from SW4/SW5 set to
    POWER and held for 3 s.
  - Q13's gate has no pull-down (QMX+ has R105, 100 kΩ). A watchdog or fault
    reset therefore normally keeps the board on; on QMX a reset powers it off.
    Reset startup drives PB10 high again before C runtime initialization.
- Firmware drives PB10 low only from the shutdown sequence, never on a
  protection fault. The sequence stops RF, the bias buck, the envelope and
  receiver clocks, and the PCM1821 clocks (≥ 10 ms before power loss). It
  closes the SD player and cancels any SD copy or delete, which closes the
  files and removes a partial copy. It then saves settings and requests 5 V
  from a PD source (≤ 1 s). If the MCU is still running 3 s after PB10 goes
  low, something else is holding Q2 on (SW8 pressed, or bench power fed past
  Q2). Firmware then restarts normally instead of staying dark.
- The ST ROM bootloader (BOOT0/SW12) does not drive PB10. Hold SW8 while
  flashing through it, or use SWD with a powered board.
- PA13/PA14 SWD, PG10 NRST and PB8 BOOT0 retain their recovery functions.
  PF0, SWD, NRST and BOOT0 are never claimed by application GPIO setup.
- PB7 is `STATUS_LED` **and** the MCU's `PVD_IN`. Firmware drives it push-pull
  and low from `BOARD_EarlyInit` onward, so any code that asks the programmable
  voltage detector for level 7 measures that pin against VREFINT rather than
  VDD, and reads a permanent brownout. `Core/Src/fw_update.c` did exactly that
  until v7.10 and could never install a firmware update as a result; it now
  uses level 6, a real VDD threshold, behind a `_Static_assert`. Keep PVD
  levels at 6 or below while PB7 is an output.

## Clock tree and frequency accuracy

Every frequency REV1 makes is a ratio of the one 8 MHz reference: the TX
carrier and the receive LO (HRTIM periods of SYSCLK × 32), the PCM1821 sample
rate (SYSCLK / 64 / divider) and the TX phase-update rate (TIM6). The exact
arithmetic lives in `Core/Src/clock_plan.c` (host suite `clock_plan`) and the
console command `clock` shows all of it: PLL setting, SYSCLK to the
micro-hertz, the carrier grid and its residual, the receive LO and IF, the
retune counters and the options below.

**The reference error, and REF CAL.** The DSC1003 is a MEMS part rated
±25 ppm, and its error moves every frequency together: each ppm is 14 Hz at
14 MHz and 28 Hz at 28 MHz. `REF CAL PPM` (PA/RF page; `clock ppb N` on the
console; stored with the board block, settings v25) corrects it, and the
correction enters only through the clock arithmetic, so TX, RX and the rates
follow it together. `tools/rev1_diag.py refcal --apply` measures it: the CDC
OUT interrupt captures TIM5 -- a free-running count of SYSCLK -- when each
`clock stamp` packet arrives, and the host fits those stamps for three minutes,
keeping the quickest round trips, against `CLOCK_MONOTONIC_RAW` -- the PC's
bare crystal, which the NTP daemon never slews. That part is good to about
0.01 ppm. The NTP daemon's estimate of the PC crystal then makes it absolute,
and is the real accuracy limit: chrony reports its uncertainty, and for
systemd-timesyncd the tool bounds it from the last offset and poll interval.
It refuses to store a result less certain than 0.5 ppm unless `--force` is
given. Fitting against the wall clock instead, as the first version did, gave
+7.2, +12.2 and +56 ppm for the same board on different runs: timesyncd slews
the wall clock for hours after a boot. On 2026-09-27 this board read
+18.08 ± 0.01 ppm against this PC's crystal, whose timesyncd correction was
still unsettled (±21 ppm), so its absolute error is not yet known; an SWD
cycle-count run against the settled wall clock the day before gave +7.19 ppm.
Keep the radio on one frequency while it runs: a PLL retune aborts the
measurement.

**The reference moves while it runs, so the NCOs follow the USB host.** REV1's
DSC1003 does not hold still: against the USB host's 1 kHz SOF it sat at
+15 to +18.5 ppm and moved by up to a ppm within a few hundred milliseconds,
continuously (2026-10-02, every SOF stamped and read over SWD) -- ±12 Hz at
14 MHz, where FT8's tones are 6.25 Hz apart. Through that, 20 m FT8 decoded
nothing although the signals stood well over the noise; tracking one decoded
station's tones showed its frequency following the reference's measured
wander one for one. A static REF CAL cannot follow it; QMX never sees it (its
Si5351 runs from a 25 MHz TCXO), and QRP Labs warns of the same failure in
digitally compensated TCXOs ("frequency stepping"). So whenever a USB host is
there, the receive and transmit NCOs follow the reference's error against the
host's crystal, the way KiwiSDR corrects its ADC clock against GPS:
`USB_LP_IRQHandler` reads TIM5 first thing when the SOF raised it, and
`radio_clocks.c` tracks those stamps by the host's frame number -- per 10 ms
the mean of the stamps within 6 cycles of the earliest (the ones the interrupt
took on time), groups further than 25 cycles off dropped (masked sections, and
the bench laptop's own SOF steps of ~5 µs every ~15 s, which no received signal
shows), groups further than 5 cycles off coasted too, and half of the phase
and a tenth into the frequency. The NCOs
follow that frequency plus the phase taken, spread over the next 10 ms
(`RADIO_RefTrimPpb`, relative to REF CAL): the receiver per DSP block, the
transmitter's carrier NCO per DMA half. Chosen against 20 m recordings with
every SOF stamped: the causal tracker decoded what a non-causal fit of the same
stamps decoded (30 signals in four slots against none). On air, alternating
pairs of slots: 21-28 decodes a slot following, 0-2 not.

The 5-cycle step gate and the half phase came later, against a QMX+ on the same
40 m EFHW (2026-10-02, `bench/qmx_ref_20261002`). Taking every group whole,
about 2 % of them -- a burst of two to four every ~1.02 s on this laptop's
xHCI, 10-18 cycles late as a whole -- moved the NCOs 10-20 ppm for 10 ms and
back, 9 rad at 14 MHz: every strong FT8 station drew a broadband streak in
WSJT-X's waterfall each second (6-14 such events a slot; QMX+ 0.1-0.4), and
with following off they were gone. No oscillator moves 30 ns in 10 ms; the
other groups' errors sit at a median 0.9 cycles, 95 % under 4. With the gate
the events fell to 0-1 a slot. Half of each group's phase (a pole at 0.7,
~30 ms) then decoded more than all of it in each of four interleaved 40 m
rounds (26.9 against 23.6 a slot, the same SNR on the stations both heard):
the stamps' quantisation is filtered and the MEMS's wander still followed; 0.3
was no better. `clock sof` shows the tracker (state, ppb, the trim, dropped
groups, groups the step gate coasted, host steps absorbed, acquisitions,
the phase share and the gate), `clock sof A S` sets the phase share (percent)
and the gate (cycles) until the next restart, and `clock usb off` holds the
NCOs at REF CAL. Without a host nothing is followed; the last figure stands
for 60 s after the last group taken or the last RF DMA interrupt, whichever
is later, so a long over keeps it (since 2026-10-04: before, a WSPR over
whose stamps all came late could drop the carrier to REF CAL half-way). A figure
outside REF CAL's own ±100 ppm is never followed: the
transmit carrier follows it too, and no band-edge or filter check sees the
trim.

**After a coast, steady groups first** (bench, 2026-10-02, SWD traces of the
tracker at ~2 ms). The gate used to widen by a cycle for each group coasted.
Every ~15 s the laptop's SOF jumped some hundreds of cycles late and slewed
back over ~200 ms, the widened gate took the slew's tail (~+20 cycles) as the
reference, and the NCOs ran +6 ppm for 10 ms and +1.2 ppm for the next second
(17 Hz at 14 MHz) until the frequency had walked far enough to coast 80
groups and acquire again -- four times a minute, in receive and transmit.
Now nothing is taken after a coast until three groups in a row agree within
4 cycles: steady inside the gate, the loop goes on; steady anywhere else, the
host's timing has stepped, and the new origin is taken with nothing published
moving. Idle, 120 s: no acquisition (4 in 60 s before), 14 steps absorbed,
the published figure within 2.2 ppm of its 1 s mean (7.3 before).

**RF DMA and the stamps.** The RF DMA interrupts share USB's priority and win
when both are pending (lower IRQ numbers). While the transport runs --
transmitting, or with PTT held -- each spends ~140 µs a 1 ms frame
interleaving compares, and the TX DMA's rate follows the USB audio's, so a
SOF that lands in that window stays there: every stamp late by the
interrupt and timed by this board's own clock. With PTT held after the audio
(SWD PC samples: `dma_interleave_half` 12.5 % of the samples), the tracker
coasted, acquired again on those stamps and published −0.1 ppm for +17 for
two seconds. `RADIO_RefSofAfterIsr` now marks a SOF still pending as an RF
DMA interrupt returns, and its stamp is not taken; the same 5.8 s PTT hold
afterwards: no coast, no acquisition, the figure at +17.7 to +21.6 ppm. In
every key-down the transmit NCO's trim matched the tracker (+17.2 to
+20.9 ppm against the console's +18.2 to +20.4).

**The host's clock moves too** (bench, 2026-10-04). The bench laptop's USB
clock slowed by
4.5 ppm when its CPU warmed from 34 to 51 °C, 9.4 ppm over six cycles to
95 °C, and the radio, following it, moved every received signal by the same
amount (+70 Hz at 15.5 MHz in a minute) while its own oscillator held. With
WSJT-X's decoding bursts alone, a 13 s FT8 over slanted by 2.5 Hz rms (up to
5 Hz) at 14 MHz: that is the "drift after TX and at random". A hair dryer on
the board moved the DSC1003 by +6 ppm and nothing received showed it: the
tracker cancels the oscillator; it cannot tell the host's drift from it.
`clock sof` also shows the MCU's temperature (`t=`, milli-degrees C), which
follows the oscillator's heating but not its random wander (0.2-0.35 ppm
over 1-20 s), so it cannot replace the host. The cure is a TCXO in place of
the DSC1003 with **REF SOURCE** set to LOCAL (PA/RF page, beside REF CAL;
console `set put 224 1`): the NCOs then stay at REF CAL and the host's clock
is not followed. USB HOST (the default, and how a stored v8.09 set reads) is
the behaviour described above; `clock usb on|off` is the same switch.

**Absolute accuracy is the host's crystal.** Following the host, the radio
sits on the laptop's crystal (the bench laptop's: −10.3 to −11.4 ppm on the
7265 and 7340 kHz broadcast carriers, which the QMX+'s TCXO put within
0.05 ppm of each other -- about 80 Hz at 7 MHz, 155 Hz at 14 MHz). For FT8 it
cancels in a QSO, since receive and transmit share it. REF CAL cannot take it
out while following: the trim is the host's figure less REF CAL, added to a
plan that holds REF CAL. Correcting a known host would need an offset of its
own (not implemented); WSPR's 200 Hz window on 20 m is the case that wants
it.

**Few system clocks, and the receiver sets the floor.** The PLL input must be
2.66-16 MHz, so an 8 MHz reference allows M = 1, 2 or 3 and SYSCLK moves in
4/3 MHz steps: 165.333, 166, 166.667, 168, 169.333 and 170 MHz in the old
165-170 MHz window. The lower bound that matters is the receive worker's: a
PCM1821 frame lasts 64 × the I2S divider SYSCLK cycles, and the worker must
finish each 64-frame half inside that. It costs about 49k cycles, and with USB
audio running its peak (interrupts included) reached 55.8-57.7k on the bench.
Divider 15 gives 61.4k per half; 164 MHz keeps it (170.8 kHz), so AUTO uses
164-170 MHz, seven clocks. Below 163.2 MHz the divider drops to 14 (57.3k, the
154 MHz peak was 55.8k: 97 %) and below 152.4 MHz to 13 (53.2k: 150 MHz overran
47 times in 18 s of USB audio). HRTIM itself has no stated minimum clock.
ST's `SystemCoreClockUpdate` divides 8 MHz by M first and so reported
165333292 for 165.333 MHz; the receiver planned its LO from that and was 4.5-6 Hz
off on the three M = 3 settings. The division is now done last and rounded, and
the RF arithmetic uses the exact value anyway.

**What the transmit NCO does, and what it costs.** The branch timers run a
whole HRTIM period P (one step of P is 36 kHz at 14 MHz, 148 kHz at 28 MHz),
but the master that resets them runs N periods plus k ticks (k = 0..8,
`clock_carrier_t`), so the carrier averages P + k/N and lands within
hundreds of hertz of the dial instead of up to half a step from it. The TX
NCO in `mod_transport.c` rotates the phase to move what is left exactly onto
the dial (the step is computed to well under a milli-hertz from the true
phase-update rate), so the transmitted frequency is exact on any plan. But it
steps the phase only at the phase-update rate (192 kHz), and that staircase
leaves images at ±192 kHz of about 20·log10(ε / 192 kHz) dBc, where ε is the
distance from the carrier to the transmitted spectrum: 18 kHz gives -20 dBc
and 1.4 kHz -43 dBc. The fraction has costs of its own, priced the same way:
its k ticks enter as one step per master period, a sawtooth with the
sidebands of a residual of k/P × 192 kHz, and a master reset the timer drops
(below) jitters the phase by up to g ticks. `CLOCK_CarrierPlan` takes the plan
whose largest term is smallest, and the clock choice is a spectral-purity
choice on top. What AUTO achieves (USB, every 1 kHz of each band, 10 m up to
28.333 MHz; that cost's median / 90th percentile and the image level at
each):

| Band | whole period, AUTO (to v8.11) | with the fraction, AUTO 164-170 MHz | with WIDE CLOCK RANGE | with the key-down hop |
|---|---|---|---|---|
| 160 m | 28 / 82 Hz (-77 / -67 dBc) | 78 / 142 Hz (-68 / -63) | 36 / 112 Hz (-75 / -65) | same |
| 80 m | 118 / 384 Hz (-64 / -54) | 131 / 250 Hz (-63 / -58) | 109 / 138 Hz (-65 / -63) | same |
| 40 m | 1.4 / 3.4 kHz (-43 / -35) | 257 / 565 Hz (-57 / -51) | 254 / 336 Hz (-58 / -55) | same |
| 30 m | 794 Hz / 1.8 kHz (-48 / -41) | 362 / 392 Hz (-54 / -54) | 361 / 371 Hz (-54 / -54) | same |
| 20 m | 6.6 / 16.7 kHz (-29 / -20) | 511 Hz / 1.0 kHz (-51 / -45) | 511 / 560 Hz (-51 / -51) | 509 Hz / 1.0 kHz (-52 / -46) |
| 17 m | 6.6 / 11.2 kHz (-29 / -24) | 658 Hz / 1.3 kHz (-49 / -43) | 658 / 698 Hz (-49 / -49) | 655 Hz / 1.3 kHz (-49 / -43) |
| 15 m | 18.2 / 34.8 kHz (-20 / -13) | 1.5 / 3.6 kHz (-42 / -34) | 875 Hz / 2.8 kHz (-47 / -36) | 1.5 / 3.1 kHz (-42 / -36) |
| 12 m | 10.6 / 16.0 kHz (-25 / -21) | 906 Hz / 4.6 kHz (-46 / -32) | 906 Hz / 1.5 kHz (-46 / -42) | 901 Hz / 1.2 kHz (-47 / -44) |
| 10 m | 24.5 / 47.3 kHz (-17 / -10) | 2.0 / 6.0 kHz (-40 / -30) | same | 2.0 / 4.0 kHz (-40 / -33) |

The whole-period column is the residual alone; those builds also dropped
resets on advances (below), so they were worse than it reads. The fraction
column never goes under one tick of the period (192 kHz / P: 66 Hz on 160 m)
where the reference trim could advance the phase, which is why 160 m reads a
little higher; it is still under -60 dBc.

What is left sits in windows on 15 and 10 m (`clock_plan_test`'s survey, with
`CLOCK_SURVEY=1`, every 100 Hz). AUTO keeps the clock whose receive IF is near
12 kHz, and that clock's plans are poor for about 24 kHz above each of its
whole periods. So at key-down AUTO hops (`MOD_TxClockHop`): when the running
cost is not settled, it moves to the clock that transmits best here and still
receives cleanly (`CLOCK_ChooseTx`), if that at least halves the cost, and
returns once the radio has been idle for 3 s. Above -40 dBc, without / with
the hop:

| Band | CW | USB |
|---|---|---|
| 17 m | 1 / 0 kHz (worst -37 / -42 dBc) | 1 / 1 kHz (-38 / -38) |
| 15 m | 170 / 155 kHz of 450 (-30 / -31) | 184 / 183 kHz (-31 / -31) |
| 12 m | 15 / 0 kHz of 100 (-28 / -40) | 25 / 0 kHz (-28 / -37) |
| 10 m | 132 / 107 kHz of 333 (-27 / -27) | 156 / 131 kHz (-14 / -14) |

With WIDE CLOCK RANGE 15 m CW comes to 116 kHz with the hop. The worst on
15 m, -31 dBc at 21.335-21.359 MHz, no clock can help: every SYSCLK this tree
makes is a multiple of 2/3 MHz (8 MHz HSE, M ≤ 3), so 64/3 = 21.3333 MHz is a
whole period on all of them, and the dials just above it need a fraction near
1, whose k ticks are the cost. On 10 m only 169.333 and 170 MHz reach a
192-tick period. USB's -14 dBc there is the band's top 1.3 kHz
(28.3317-28.333 MHz): the passband lies above 28.3333 MHz, the fastest
carrier 170 MHz makes, so the NCO must advance on a 192-tick period, whose
roll-over the master's reset meets in the same fHRTIM cycle every time (the
held reset, below). The hop costs a PLL switch at key-down, about 10 ms
plus up to 8 ms of headphone duck, and USB audio the frames rendered from its
first packets. Only a programmable reference (an Si5351-class part on HSE,
so SYSCLK can make every dial an exact period) removes the windows.

`TX PHASE 384K` (off by default) doubles the update rate and takes 6 dB off
every such image, the modulation's own included; the compare buffer is held as halfwords (zero-extended into the
32-bit burst register) and the outphase table as Q17 so that fits in RAM. On
the bench it first failed within 2 ms with `DMA_CHANNEL_SKEW`: DAC3 ran at
165-170 MHz with `HFSEL` = 00, which RM0440 does not allow above 80 MHz (10 is
required at 160 MHz and above, and only lengthens the allowed DAC update
interval, so it is safe at any SYSCLK here). With `HFSEL` = 10 the 384 kHz
stream runs, and an SWD probe of the live compare registers during a stream
start showed legal compares, a constant branch separation and the expected
NCO phase advance at both rates. The images have not been measured on air or
on an analyzer.

**A phase advance lost a carrier cycle.** The master timer runs at two carrier
periods and resets timer A (C: timer E) at MCMP1 (MCMP2); in between, the
branch timer rolls over on its own. Its output rose only on that roll-over
(PER). When the reset moves earlier -- each NCO step on a grid below the dial,
and any SSB phase advance -- it lands before the counter reaches PER, the
roll-over never happens, and the rising edge went with it: the push-pull pair
sat in one state for 1.5 periods. The first TX bench run showed it on 160 m
as a sine interrupted every few cycles by a malformed one, while 60 m, whose
170 MHz grid sits 2.6 kHz above the dial, was clean. With the clock pinned at
170 MHz on 1.840 MHz (period 2957, into 50 Ω), the forward detector read
2497 mV and VIN sagged to 7837 mV with the grid 298 Hz below the dial, against
1700 mV and 7877 mV with it 302 Hz above or on the dial. The resetting master
compare is now a second source of the same edge (`mod_hrtim_branch_edges`,
SETx1R = PER | MSTCMP1 and RSTx2R likewise), never of the opposite one, so a
coincident roll-over cannot conflict with it; the three cases then read
1708-1726 mV with the same VIN. Across the band presets, every one with the
grid below the dial dropped to its clean level (80 m 1895 → 1517 mV, 41 m BC
1856 → 1532 mV) and the ones above it did not move. The host model in
`modulation_reference_test` reproduces the lost edge for every negative NCO
step at periods 193-2957 and passes with the fix.

**A reset just before the roll-over was lost.** RM0440 28.3.4: when two
counter resets come in the same fHRTIM cycle, the last one is taken. The
branch timer's own roll-over every P ticks is one reset, the master compare
the other, so a master reset landing within a tHRTIM (32 ticks) before the
roll-over is overridden by it: the timer runs on from its roll-over, and its
phase slides until a later reset lands after the roll-over again. The first
fractional build let k run from -8 to +8; on the scope (Q6 gate, 50 Ω) its
TX TEST preset halfway between two grid points (k = -1, N = 2) smeared on 20
and 17 m, while k > 0 was clean. NCO advances under 32 ticks do the same. k
is now never negative (a reset k ticks after the roll-over only delays the
timer). Whole periods take a master of three periods first: an odd N × P
moves the roll-over through all 32 ticks of the fHRTIM cycle, so a held reset
waits a tick or two, where P ≡ 0 mod 32 (192, 256 and 384 at 170 MHz) never
moves it. And `CLOCK_CarrierCostHz` prices a held reset wherever the advances
-- the residual's when the carrier is under the dial, plus a 5 ppm margin for
the reference trim (`CLOCK_TX_TRIM_MARGIN_PPB`) -- outrun the k ticks a master
period that catch the timer up: up to g ticks late, g the largest power of two
dividing N × P. `clock_plan_test` runs every plan from 14 to 28.3 MHz through
a model of the reset rule and checks that cost against it. On the bench
(2026-10-27) the halfway presets then looked like the exact-grid ones on all
of 20, 17, 15, 12 and 10 m.

**An update's reset too close to the opposite edge.** RM0440 28.3.7
(Fig. 242): the master reset sets TA1 and resets TA2, CMP1 does the opposite,
and a set within one tHRTIM of a reset is discarded -- TA1 then stays low a
whole period -- while within two it is a one-tHRTIM runt for the gate driver.
The TX DMA fill keeps every update's reset out of [CMP1 - 1 tHRTIM, CMP1 +
2 tHRTIM) (`mod_hrtim_step_legal_raw`), moving it to the nearest legal tick for
that update only. It runs only where one update can reach the window (the
modulation's lerp plus the NCO step against `step_guard_room`): with the
fraction the NCO step is a few ticks, so mostly on 10 m (P = 192: 32 ticks of
room) and with large PA duty offsets.

**TX TEST's grid presets** (a bench aid for the gate on the scope). Past the
band presets, the TX CARRIER TEST band selector steps through three presets
each on 20, 17, 15, 12 and 10 m, planned on the running clock: GRID, the dial
on an odd whole period inside the band (no fraction; the receiver sits at DC
there, its LO being on the same grid); HALF, just short of halfway to the next
period, a fraction of 1/2; and WORST, the dial in that step whose plan costs
most. The title shows the preset, the ticks line the fraction (`382+1/2
TICKS` on 20 m HALF), and NCO STEP the NCO's step per update in ticks and degrees with
the strongest image the plan leaves. While the page is open the PLL waits
(no clock upgrade under a measurement); a preset the running clock cannot
receive on moves it at once and is planned again on the new clock.

**A phase lost a tick on odd periods.** `mod_hrtim_phase_raw_tick` scaled a
phase in [-π, π) to [0, P) as `(phase + 1) * (P / 2)` with the half period in
integer ticks, so on every odd period the phase-to-tick map was a tick short
of a full turn: once per cycle of the modulation the phase skipped a tick.
Every TX mode carries it on bands whose period is odd (14.074 MHz at
164 MHz: 373 ticks): on the host model a spur on the dial, an image and
third-order products near −51 dBc. With the half period as a float they are at
−70 to −83 dBc (`mod_transport_logic.h`).

**15 m to 10 m faulted at key-down.** `render_compare_frame_valid` checked the
atomic compares against `min/max_compare_ticks` ([3, period - 4]), the limits
of the old local compares. Atomic compares are master reset times, 96 plus the
raw phase, so anything above period - 100 failed: P/2 plus the common phase
does from about period 254 (21.3 MHz) up. TX TEST, CW and SSB tripped a hard
`RF CMP` lockout on every key-down from 15 m to 10 m, and SSB, whose phase
covers the whole circle, would have on every band. The check now accepts
exactly the encoder's range (`mod_hrtim_phase_compare_legal`). On the bench
every preset then transmits within 52-63 ms of key-down, with no fault in
0.8 s key-downs, apart from the three TX blocks by design (90 m, 49 m and 25 m
BC, below their filter's TX floor).

On 20 m and up the forward detector depends on the grid error: 28.200 MHz
(ε -13.5 kHz) reads 353-390 mV, 28.170 MHz (+16.5 kHz) 606-618 mV, with the
same drain voltage and VIN; `TX PHASE 384K` brought the latter to 455 mV.
The detector is a peak detector and each 192 kHz phase step (25-67° there)
rings the filter, so the reading is not a power measurement on those bands.

**15 m to 10 m power, 2026-10-07 (TX TEST, TUNE 50 %, 50 Ω).** The meter showed
1.2 W on 28.074 MHz and 0.1-0.4 W on 21.3, 27.205, 28.02 and 28.2 MHz. With
the grid residual (the NCO step) frozen over SWD the detector holds a constant
envelope and reads the carrier itself: 21.33 MHz 0.04 W, 21.67 MHz 0.63 W,
24.95 MHz 0.53 W, 27.2 MHz 0.18 W, 28.04 MHz 0.13 W, 28.19 MHz 0.12 W, 20 m
0.91 W. 28.074 MHz reads 1.24 W at 192 kHz updates (33 ticks a step), 0.15 W
with `TX PHASE 384K` (16.5) and 0.13 W frozen, on the same carrier, VIN sag and
drain voltages; at 24.94 MHz an injected 44-tick step lifts 0.53 W to 0.83 W
with no more current. Steps above one fHRTIM clock (32 ticks) pump the SS14
detector (0.1 µF ‖ 100 kΩ) with transients, most where the carrier is weak.
A Timer D capture probe (reset by MSTCMP1/2 or TIMACMP1, captures on TA1/TE1
set and reset) found the branch edges identical, A and E aligned, in the 1.24 W
and 0.15 W states. So 10 m really gives about 0.13-0.18 W here, and the
"good" preset was the meter.

- **15 m is on a filter that is already rolling off.** `sweep lpf` puts slot 3
  flat to 18.5 MHz, then −8.5 dB at 19.5, −16 dB at 20.6 and −20 dB at
  21.7 MHz (the QMX+ model: −3 dB at 22.4 MHz), and 21.3 MHz transmits 0.04 W
  on slot 3 against 0.63 W at 21.65 MHz on slot 4. Check L515/L516; slot 4's
  20.5 MHz TX floor would also allow `LPF 3 UPPER MHZ` below 21 MHz.
- **10 m current is mostly shoot-through.** 28.074 MHz frozen at 0.13 W sagged
  VIN 1.5 V with no dead time and 0.37-0.39 V with 5.9 ns (one fHRTIM clock),
  same output; 7.35 ns and up leave pulses too short for the UCC27524 and the
  output dies. Sub-clock values (0.7-4.4 ns) behaved irregularly, as RM0440
  warns for dead times under 3 fHRTIM clocks. On 20 m, 5.9 ns costs 2 dB.
- **`DEADTIME` was 64 times too long until this date.** DTPRSC was written as
  HAL's `MUL8`, which on G4 is 110 (tDTG 47 ns, RM0440 table 237), while the
  ns-to-tick conversion assumes 000 (735 ps): 1 ns held both outputs of a
  28 MHz branch low. `MOD_DEADTIME_DTPRSC` now ties the two together.

**HRTIM review against RM0440, 2026-10-07.** Holds: DTEN only changes with the
counters stopped and the outputs disabled; a phase step landing on CMP1 resets
both outputs (reset wins), never sets both; every streamed compare is
96 + [0, P) under MPER = 2P; all timers that exchange events share CKPSC 000;
halt and system fault park the outputs inactive. Fixed:

- `DUTY OFFSET TKS` was clamped to ±(P/2 − 8) while the geometry needs CMP1
  96 ticks from both edges (on 10 m only 0..+1 at P = 193, 28.2 MHz, and 0 at
  P = 192): any other value
  failed every carrier apply on that band and halted the radio in
  `Error_Handler`, at every boot once saved. It now clamps to the geometry
  (`mod_hrtim_duty_offset_bounds`), and a refused geometry no longer stops the
  running carrier first.
- Every carrier reconfigure enabled the four TX outputs, so they sat in RUN,
  under parked counters, while receiving; they now keep the state radio.c
  gives them (TX entry and exit).
- TX readiness lost while keyed (tuned out of the TX range, PD, rail, the
  WATERFALL guard) dropped TRANSMIT to SETUP before `service_tx_exit`, which
  then returned early: the TX outputs stayed enabled into RECEIVE and the AUX
  TX lines high until the next over ended. That path, and
  `RADIO_BeforeRetune`, now leave TX through `service_tx_exit`. Bench: mask
  cleared at 0.12 s of a 20 m key-down, SETUP with OENR 0 and bias off 4 ms
  later.
- The HRTIM fault interrupt was never enabled (HAL applies
  `HRTIMInterruptRequests` only in `WaveformCountStart_IT`): IER is now set.
- DLL calibration follows RM0440 (one-shot, DLLRDY, then periodic) instead of
  HAL's pair, whose poll could leave the handle locked on a timeout.
  Periodic calibration re-sets DLLRDY every ~12 µs.
- The DMA channel is disabled before Timer F's DMA request, as RM0440 asks.
- An emergency stop landing while a carrier reconfigure ran with interrupts on
  had its ODISR undone when the TX outputs were restored, and TX entry could
  enable them just after `RADIO_Service` checked the latch. Both now skip the
  enable under the latch, as the AUX lines and the RF gate already did.
- A calibration or self test started while keyed (TX_PREPARE or TRANSMIT)
  held `RADIO_Service` off and returned to SETUP, skipping the TX exit: bias,
  AUX TX lines and TX outputs stayed on. `RADIO_CalStart` now refuses while
  keyed (the ELF policy keeps diagnostic code away from the AUX outputs, so it
  cannot leave TX itself).
- A PLL move stops Timer F with the other TX timers, as a reconfigure does.

**Carrier clock policies.** `CLOCK POLICY` (OPERATE and PA/RF pages):

- `AUTO`, the default since settings v25 (a stored SAFE, the old default, is
  migrated; PRECISE and EXACT are kept). Of the seven settings that keep the
  receive budget (164-170 MHz), the ones that can transmit and give the best
  receive plan -- a clean IF within 8 kHz of the
  12 kHz target -- and among those the smallest ε from the transmitted
  spectrum's centre (the dial ± 1500 Hz in SSB, the dial otherwise). The running
  setting is kept unless another cuts the images by 3 dB and 150 Hz and the
  current ε is above 200 Hz. A better setting is an *upgrade*: the frequency is
  applied on the running clock at once (the NCO makes it exact), TX is not
  held, and the PLL moves after 350 ms with the radio receiving and the TX path
  idle, costing the receiver about 10 ms.
- `EXACT` and `PRECISE` choose for the transmitter, but now only among settings
  with a clean receive plan when one exists (before, `EXACT` put 14.074 MHz at
  a 36 kHz IF and 18.1 and 28.074 MHz at DC).
- `SAFE`: 170 MHz unless the receiver needs another setting.

Receive comes first in AUTO because the stored I/Q correction only holds near
the 12 kHz IF: `rxcal` measured the Q phase error at 11.8° at 9.2 kHz, 28.5° at
19 kHz and 82.3° at 37.2 kHz from the LO, far from a fixed skew, so a 36 kHz IF
would leave about 11 dB of image rejection. On 20 m that costs TX images:
14.074 MHz USB takes 164 MHz (-30 dBc, IF +4.3 kHz; 166 MHz gave -22 dBc) but
not 168 MHz (-39 dBc, IF 23 kHz).

The receiver no longer goes silent while a frequency or clock change is
pending: it plans its LO on whatever SYSCLK is running, and only a PLL switch
under way (or one that failed without fallback) stops it. Before, every tune
that needed another clock muted receive for about 360 ms.

`WIDE CLOCK RANGE` (off) lets AUTO also take the eleven divider-14 settings,
153.333-162.667 MHz: the WIDE column above, -10 dB at 14.074 MHz (154 MHz,
-32 dBc) and -13 dB at 18.100 MHz (153.333 MHz, -33 dBc), nothing on 10 m (a
192-tick period needs 170 MHz at 28.3 MHz). The price is 7 % of the receive
budget: with USB audio the worker peaked at 97 % of the half at 154 MHz, with
no overrun in 51 s but no margin to speak of, which is why it stays off. The
divider-13 settings (150-152 MHz) are never used. Turning it off moves a
running divider-14 clock back up at once.

The transmitter needs a fixed number of cycles a millisecond too, so since
v8.06 a divider-14 clock is taken only where the running mode's worst case
fits 92 % of it (`CLOCK_TxFits`, measured on the bench with the speech tools
on and a full-scale tone, plus about 3 %): SSB with the speech tools needs
147.8k cycles a millisecond at 384 kHz and 137.6k at 192 kHz, under DIGI CLEAN
127.6k / 118.4k, AM 120.2k / 111.1k, CW 116.3k / 106.8k, FM 96.9k / 87.4k. So
voice SSB with `TX PHASE 384K` keeps to 164-170 MHz and at 192 kHz starts at
154.667 MHz, and DIGI CLEAN SSB (FT8), AM, CW and FM may use every divider-14
clock. The clock is chosen while receiving and kept through a transmission,
which is why the worst case decides; with VOICE the speech tools are assumed
on because they can be switched mid-over. Before, USB-audio SSB with the
tools and 384 kHz took 99.7 % of the CPU at 153.333 MHz. The choice depends on
the mode (the SSB spectrum centre, the receive channel, the TX load), so a
mode, sideband, USB audio policy or phase-rate change re-plans once the radio
is idle, and a running clock that no longer fits is moved rather than kept as
an upgrade for later (on the bench: 12 m DIGI CLEAN on 157.333 MHz moved to
170 MHz 0.36 s after the policy was set to VOICE). The PCM1821 locked at every rate
tried (170.8-182.7 kHz), and the PLL retunes caused no fault.

**Two receive birdies from the clock tree.**

- *The TX timers.* The TX HRTIM timers (master, A, E, F) ran during receive at
  the carrier grid, within a few kHz of the dial, and leaked into the receiver
  inside the MCU although the branch pins were GPIO-low: at 9.996 MHz a comb
  around the 10.000 MHz grid reached -92 dBFS inside the audio passband.
  Stopping them over SWD removed it. `PARK TX IN RX` (default on) stops them
  whenever the radio receives with an idle transport; a TX stream start and the
  TX entry restart them in one register write, and A and E realign on the next
  master compare. A smaller ε pulls this leak closer to the dial, so the park is
  what makes AUTO safe to default. On the bench (9.996 MHz, 50 Ω load) parking
  lowered the strongest audio-band tone from -84.5 to -116.8 dBFS.
- *The "Q comb": the ADC repeating samples.* Lines at k·fS/312 and
  fS/64 ± k·fS/312 around the LO's DC, locked to the I2S frame at every SYSCLK
  and divider tried, were first taken for a Q-channel disturbance. Raw `iq`
  snapshots (2026-10-02) show what it is: at 162-190 kHz the ADC sends, in
  each channel, two of every thirteen frames as exact copies of the sample two
  frames before (15.4 %), I and Q three frames apart; 24 × 13 = 312. It is not
  only the DC: every received signal carries copies at k·fS/312 (the injected
  test tone's first at −4 dBc), in I and Q alike. Setting the I2S divider live
  over SWD: clean at 48, 86.5, 95-96 and 103.75 kHz, all zeros at
  112-153 kHz, the repeats at every rate from 162 to 185 kHz. The straps are
  right (DRE off, linear phase, I2S), and TI's auto-clocking parts recognise
  rate "buckets" (96 kHz: ~87-97 kHz; 192 kHz: ~175-194 kHz), which fits. So
  the receiver runs the ADC near 96 kHz wherever the LO plan allows (see
  *Receiver clocking*); on 20 m FT8 that decoded three times as many signals,
  about 10 dB stronger, as 170.8 kHz. `RX Q COMB CANCEL` (off) learns the
  2496-frame pattern and subtracts it, which cannot undo repeated samples; it
  took only 8 dB off the lines and stays off.

## RF and receive pins

| Function | Pins | Firmware behavior |
|---|---|---|
| TX branch A | PA8/PA9, HRTIM A1/A2 AF13 | Physical outputs stay GPIO-low until an accepted frame and RF permit. |
| TX branch B | PC8/PC9, HRTIM E1/E2 AF3 | Same interlock as branch A. The logical legacy C/TC names are not physical timer names. |
| Driver enables | PC10 (`RF_EN2`, branch A), PC6 (`RF_EN`, branch B) | Each pin drives a BSS123 (Q30/Q7, 10 kΩ gate pull-down) whose drain has a 470 Ω pull-up to the 5 V rail (R32/R145, reworked from VIN). MCU high pulls ENx/OE low; MCU low lets it rise to 5 V. MCU high is the reset, idle and fault level for both assemblies. The default assembly is UCC27524 on both branches. |
| RX enable | PC7 | Never transmitted; TX and RX requests are mutually excluded. |
| RX quadrature | PB12/PB13, HRTIM C1/C2 AF13 | PB12 (CLK0 → U11 S1) is high for [0, half). PB13 (CLK1 → U11 S0) **leads** by about 90°: set at Timer C CMP2 (half + rise) and reset by Timer B CMP2 (Timer C event 3) at `fall`. Timer B gets its half-period offset from a shortened first period after a simultaneous start, not from a cross-timer counter reset. The period, IF and integer-tick quarters are chosen per frequency (see *Receiver clocking*). A frequency with no usable LO shows `RX CLOCK N/A`; this is not a protection fault. |
| RX BPF | PD0/PD1 | U10 S0/S1 binary channel. The firmware picks the configured slot with the smallest upper edge that covers the frequency, so the slot order need not follow frequency. Fitted as the QMX 60–15 m set with default edges; see *Receive band-pass filters*. |
| RX test injection | PB14, HRTIM D1 AF13 | Low GPIO except during the explicit five-second RX calibration action and the BPF sweep; the PCB provides the R106 100 kΩ path into the LPF input. |
| LPF select | PE2..PE6 | One-hot, active-high BSS123. Off filters get reverse bias through 470 kΩ (R73/R72/R71/R141/R142) from B+: VIN through D8, raised during TX by the C63/D7/D6/C64 doubler from the PA output (QMX+ D505–D507). Same smallest-covering-edge slot selection as the BPF. Fitted as the QMX+ set without 6 m; each slot also has a harmonic-safe TX floor. See *Low-pass filters*. |
| LPF bias | PD12, TIM4 CH1 AF2 | QMX+ PIN bias buck (Q14 BSS123 with R85 100 kΩ pull-down, Q12 BC817/D18 gate drive, Q15 BSS84, D19 SS14, L5 330 µH, C73 470 µF, R86 10 kΩ to ADC_BIAS). PWM high turns Q15 on. Controlled as in QDX/QMX by a per-diode current (`PIN BIAS MA`, default 30 mA); see *PIN diode bias*. |
| Auxiliary jack | PD8..PD10 | MCU high sinks the external open-drain-compatible jack lines; low is inactive. |
| Rotary encoder | Three of the spare JP20/JP7/JP19 pins, default PC11/PC12/PC13 | Optional front-panel encoder; see *Front-panel controls*. |

PA0 and PA1 are headphone PWM outputs (TIM2 CH1/CH2 AF1), not RF signals or
DAC pins. The PCB couples them through 2.2 µF, 560 Ω, 2.2 µF and 3.3 kΩ into the
NE5532 inverting stage with no shunt capacitor. That means there is no
reconstruction filter, so the PWM carrier reaches the amplifier. Firmware
therefore picks TIM2 ARR 448–575 (about 295–380 kHz) per RF frequency. The
nearest carrier harmonic then stays at least 100 kHz from the receive or
transmit frequency (≥ 125 kHz across 1–30 MHz at 165–175 MHz SYSCLK). The
48 kHz demodulated audio is linearly interpolated per PWM period and quantized
by a libm-free second-order error-feedback shaper, with one code for both
channels (`RADIO_PhoneRender` in `radio_dsp.h`, host-tested in `rev1_audio`).
Clamping, gain and sidetone run once per 48 kHz sample; the per-period loop has
no float compare and costs ~8,700 cycles per 256-period DMA half.

DMA2 channel 3 moves the codes into `TIM2->DMAR` (a CCR1/CCR2 burst) with a
**32-bit peripheral size**. TIM2 is a 32-bit timer on APB, which ignores the
byte lanes, so a 16-bit DMA write lands duplicated in both halves of the
register: through v8.02 a code of 232 became CCR = 0x00E800E8, far beyond ARR,
which held both outputs high -- DC into the NE5532 and silence at any volume.
`source_policy_check.py` rejects a DMA into TIM2/TIM5 without `DMA_CCR_PSIZE_1`,
and the self test's `HEADPHONE PWM PATH` row reads the live compare registers.

Volume is squared and doubled: at 100 % a signal held by the AGC (0.35 of full
scale) reaches 0.70 of the PWM range, 2.2 dB under the renderer's ±0.90 clamp.
The applied gain slews to the setting over at most 5 ms, which is also the
fade-in after every receiver start, so volume steps and muting do not click.

Retunes restart the receiver. While it is suspended TIM2 keeps running at 50 %
duty, so the coupling capacitors stay at their operating point and the gap is
silent rather than a pop; the restart reprograms TIM2 in one short run of
register writes immediately before CEN (enabling the outputs early and then
spending the DSP set-up with the counter stopped held PA0/PA1 at a static
level, an audible click on every encoder detent). The AGC gain and S-meter
level carry over from the previous receiver, so audio returns at its level
instead of swelling back from unity gain.
The headphone PWM and microphone acquisition are mutually exclusive: when a
microphone takes the audio workspace, firmware disables the PA0/PA1 channel
outputs and stops TIM2 (`phone_pwm_park`). Idling at 50 % is not silence
electrically; it leaves the full PWM carrier switching next to MIC1/MIC2. A
disabled channel is Hi-Z (RM0440, standard tim_ocx output control), so the
coupling capacitors keep their charge and the next start at 50 % puts no step
into the headphones. Bias/envelope calibration and `RADIO_AudioStop` park TIM2
the same way.

Driver enables follow the UCC27524 and MC74ACT541 datasheets:

| MCU PC10/PC6 | Enable node | UCC27524 (ENx active high, internal pull-up) | MC74ACT541 (OE1/OE2 active low, 3-state) |
|---|---|---|---|
| high | GND via BSS123 | disabled, OUTx held low | enabled, outputs follow the low RF inputs |
| low | 5 V rail via 470 Ω | enabled, outputs follow INx | disabled, outputs **high-Z** |

The PA gates (Q3–Q6) have no pull-down resistors, so the ACT541 high-Z state
would leave them floating. Firmware therefore never uses it. The reset handler
sets both enables high before clock or C runtime startup. `BOARD_PermitRf(0)`,
`BOARD_WriteRf(0)` (including every keyed/SSB pause) and `BOARD_EmergencyStop()`
force the four RF inputs to GPIO-low and both enables high. `BOARD_WriteRf(1)`
drives an enable low only for a branch configured as UCC27524. ACT541 and
unknown branches stay high. The inputs switch to HRTIM AF in the same PRIMASK
critical section.

Faults do not all end the same way, and it matters when reading a dark board:

| Fault | Handler | Outcome |
|---|---|---|
| ADC, DMA, HRTIM | `RADIO_Trip` | RF and RX parked, `RADIO_FAULT` **latched** until foreground acknowledgement |
| CSS (lost TXCO) | `NMI_Handler` | RF clamped, then `NVIC_SystemReset()` — a **reset**, not a latch |
| Unexpected interrupt | `Default_Handler` | `BOARD_EmergencyStop()`, then spins with the IWDG unkicked — a **watchdog reset** |
| Boot-time init failure | `Error_Handler` | RF parked, the failing stage written to the boot list, then a **held** blink with the IWDG kicked |

`Error_Handler` deliberately keeps kicking the watchdog. Without that, a
failure after `iwdg_start()` became a silent ~2 s reset loop, because
`BOARD_EarlyInit` re-asserts PB10 on every reset; and a failure before it hung
with nothing on screen. The stage name (`CLOCK`, `HRTIM`, `TIM6`, `DMA`) is now
the first thing on the display.

## Analog pins and reference

- MIC1 is PA2/ADC1 IN3, MIC2 is PA3/ADC1 IN4 and MIC3 is PE9/ADC3 IN2.
- ADC2 scans PC0 IN6 bias, PC1 IN7 forward, PC2 IN8 reflected, PC3 IN9 5 V
  rail, PC4 IN5 envelope A, PC5 IN11 envelope B and PB2 IN12 VIN.
  VIN and envelope inputs use the documented 47 kΩ/10 kΩ (5.7:1) dividers;
  the 5 V rail uses 22 kΩ/22 kΩ. PC0 is a direct detector input with only
  the documented series resistance; firmware does not invent a current
  conversion.
- Buttons PD2..PD6 have no external pull resistors. Their common is set by
  R93 (GND, fitted, so the buttons are active low); R92 (3.3 V) is not fitted. Idle lines float, so the jumper cannot
  be read at boot. Until a press is seen, firmware alternates the internal
  pull-up and pull-down every 4 ms. It locks the polarity after three matching
  samples and saves it with the next settings save. All buttons read released
  until then. A disagreeing setting only restarts learning, so a wrong value
  cannot lock the UI out.
- VREF+ is capacitor-only. Firmware enables the internal VREFBUF 2.9 V scale
  and converts ADC readings with the measured VREFINT value. Calibration and
  ADC telemetry are quiesced before RF PLL retune and restarted afterward.
- DAC3 CH2 feeds the internal OPAMP3 follower to envelope A at PB1; DAC3 CH1
  feeds OPAMP6 follower to envelope B at PB11. Both channels use the internal
  follower path (`VMSEL`/`VPSEL`), not a fabricated external DAC pin.

## PCM1821 and audio buses

The bench REV1 has a PCM1820 fitted (the pin-compatible part with 7 dB more
dynamic range and a DRE, which MD1 low keeps off); everything here applies to
both. The receiver is on I2S3: PA15 WS, PB3 CK and PB5 SD, all AF6. Strap
levels from TI SBASA61A Tables 8-1, 8-2, 8-7 and 8-20 (VIL ≤ 0.8 V, VIH ≥ 2.1 V;
a pin with both resistors fitted sits at mid-supply and is undefined):

| Pin | Required | Fit | Leave open | Meaning |
|---|---|---|---|---|
| MSZ (13) | low | R51 (GND) | — | slave: MCU drives BCLK/FSYNC |
| FMT0 (19) | low | R52 (GND) | **R53** (3.3 V) | I2S (high would be TDM) |
| MD0 (12) | low | R49 (GND) | **R50** (3.3 V) | linear-phase decimation filter |
| MD1 (11) | low | R47 (GND) | **R48** (3.3 V) | DRE off (the DRE exists only on PCM1820) |

In I2S the left slot is channel 1 (IN1 = U9 path) and the right slot is
channel 2 (IN2 = U12 path); every word is 32 bits.

**Receiver orientation.** U11's two mux halves are cross-wired
(1Bn = 2B(5−n)). U9 amplifies (1B1 − 1B4) into IN1 and U12 amplifies
(1B3 − 1B2) into IN2. A switch-level model of this netlist (host suite
`rev1_receiver`) shows that with PB13 leading PB12, I = IN1 and Q = IN2 put a
signal above the LO at positive frequency. `RX IQ INVERT` (default OFF) exists
only as a bench correction.

**Sample rate.** Slave mode accepts only the listed 176.4/192 kHz families
(BCLK = 64 × FSYNC). SYSCLK is 164–170 MHz, so the I2S divider picks the
closest rate: 170 MHz / (64 × 15) = 177.08 kHz, which is +0.39 % from
176.4 kHz. The linear-phase filter is flat to 0.3 fS (53 kHz) and stops at
0.473 fS. If only all-zero words arrive for 750 ms (the ADC rejected the rate),
the receiver switches to the other family and counts a fallback. The monitor
page shows `RX <rate>HZ ... FB <n>`.

### Receiver clocking compared with QRP Labs QMX

QMX (QRP Labs operating manual 1.03.002 §8.6.3; G0UPL FDIM 2023) drives
this QSD topology from an Si5351A. With an even-integer output divider its
90° outputs are exact at every frequency, and the fractional PLL sets the
frequency to the hertz. The SDR is a superhet with a **12 kHz IF**: the image
is 24 kHz away and nothing of interest sits at DC. The 24-bit I/Q is sampled at
48 ksps, and the unwanted sideband is about 60 dB down with no I/Q
compensation. REV1 generates the clocks from HRTIM ticks (5.44 GHz at 170 MHz),
so two things are handled explicitly:

1. **LO placement (`RADIO_RxPlanLo`).** For the tuned frequency and mode, the
   firmware searches the HRTIM periods. It needs the channel (USB 300–3000 Hz,
   LSB mirrored, CW ±350 Hz, AM ±5 kHz, FM ±10 kHz) clear of DC by 600 Hz and
   inside ±40 kHz with the ADC at ~96 kHz -- where it does not repeat samples
   (*Two receive birdies*, below) -- or, only where no LO fits that, inside
   ±50 kHz with the ADC at 170-190 kHz; then it chooses the IF closest to
   **12 kHz**. One tick moves the LO by rf²/(SYSCLK × 32), so with AUTO's
   choice of clocks (which ranks the high ADC rate last) every dial from 160 m
   to 12 m has a ~96 kHz plan, 95 % of 15 m and some of 10 m. `clock adc96
   on|off|auto` pins either for tests. Above 400 ticks an
   odd period carries a 4 kHz handicap. The DSP NCO removes the exact integer
   residual. If only an in-band LO touching DC exists, it is used and the
   status shows `RECEIVE (IF NEAR DC)`. The carrier clock planner (every
   policy; AUTO by default) then moves to another legal PLL setting
   (164–170 MHz under AUTO) that gives a DC-free IF, once TX is idle.
2. **Quadrature edges (`RADIO_RxClockEdges`).** A double-balanced QSD needs
   both node pairs (1B1/1B4 on S0+S3, 1B2/1B3 on S1+S2) connected for equal
   time with centered windows. Otherwise the pairs' RC integrators get
   different effective time constants and the I/Q phase differs at the IF.
   Integer-tick edges are placed as p = 4m: m/m; 4m+2: fall m, rise m+1;
   4m+1: m/m+1; 4m+3: m+1/m+1. In the netlist-derived time-domain model (IF at
   a quarter of the node RC corner), image rejection is about 65 dB for even
   and 60 dB for odd periods, against 44–54 dB for plain rounding. That is in
   the range QRP Labs quotes for QMX (image rejection varies from unit to unit,
   typically 60–75 dB). A static window-correction matrix and a
   frequency-scaled skew trim were modeled and rejected: their effect reversed
   with the RC time constant. Residual board mismatch is trimmed with
   `RX I GAIN`, `RX Q GAIN` and `RX PHASE TRIM`.

Coverage at SYSCLK 170 MHz with PLL assist, from `RADIO_RxPlanLo`:
160–20 m and 12 m are receivable at 100 % of frequencies, with a DC-free IF
for SSB/CW. On 15 m, SSB/CW are 98–99 % clean and receivable everywhere; AM/FM
are 91 %/76 % clean and 100 %/98 % receivable. On 10 m (28.0–28.33 MHz, the
HRTIM ceiling), one tick moves the LO by about 147 kHz, so only about 81 % of
SSB/CW frequencies (70 % FM) have a usable LO. The rest show `RX CLOCK N/A`;
transmit is unaffected. Closing that gap needs finer LO steps than an
integer-tick HRTIM period can give.

**Retune latency.** `RADIO_RxClockStart` plans the LO before it masks
interrupts; only the RF-interlock check and the HRTIM programming are atomic.
The plan walks every period within ±50 kHz, a count that grows as 1/f² (~40 µs
at 1.435 MHz), and the helper-phase settle is sized to the period (four full
Timer B periods) instead of a fixed ~60 µs loop. Through v8.02 both ran with
interrupts masked, ~100 µs or more on every retune: enough, with the other
priority-1 interrupts, for the 4 kHz telemetry ADC interrupt to miss a scan
(`faults` → `adc_late`, latched `ADC`, receiver parked and silent) and for the
audio DMA halves to miss their deadlines, typically within ten retunes across
the bands. The masked time is now 11 µs at 7 MHz and 40 µs at 1.435 MHz.

**Telemetry interrupt priority.** The ADC scan interrupt (DMA2 channel 1) now
sits at priority 0, above the RF, audio and USB DMA interrupts (1), level with
the HRTIM fault. While transmitting, the RF DMA refill (interpolation to
192 kHz and the envelope DAC fill) holds its priority-1 interrupt for about
265 µs of every 1 ms half-frame, more than one 250 µs scan. On the first TX
bench run every key-down lost a scan within a second: the protection loop
latched `ADC` (`adc_late`), stopped the envelope DMA, and the next RF boundary
reported the stopped channel as a pipeline `DMA SKEW`. The 265 µs is inferred
from where the stopped envelope DMA stood (CNDTR 140/141 of 384, all three
times); the CPU profiler that would time `dma_pop` exists only in the
diagnostic flavour.

**Channel filtering.** After the NCO, I and Q each pass an 8th-order elliptic
filter (0.1 dB ripple, 80 dB stop band) with ±1350 Hz around ±1650 Hz for
USB/LSB, ±350 Hz for CW, ±5 kHz for AM and ±10 kHz for FM. The passband is flat
within 1 dB over 300–3000 Hz, and the opposite sideband is ≥ 70 dB down from
300 Hz audio. The previous 6th-order Butterworth gave only 19 dB at 300 Hz and
35 dB at 1 kHz; QMX is about 60 dB.

The production DSP resamples to 48 kHz, corrects the measured carrier residual
in its NCO, and filters, demodulates and applies AGC. An S-meter (channel
power after filtering and before AGC, dBFS) replaces the TX level bar on the
home screen while receiving.

**Receive bandwidth.** The 48 kHz capture stream is usable to ±19 kHz
(measured: flat to 16 kHz, −0.17 dB at 18 kHz, −2.7 dB at 20 kHz, −35 dB at
24 kHz, −70.7 dB at 25 kHz, −84 dB at 42 kHz) and everything from 25 kHz out is
70 dB down, so a panadapter can be trusted to about ±19 kHz and the ±24 kHz
Nyquist edge is empty floor. That is within a few kHz of QMX, whose PCM1804 is
flat to 0.454 × 48 kHz = 21.8 kHz. The limit is the decimation table
(`Core/Src/radio_rx_coeff.inc`, 96 taps per phase with a 25 kHz stopband, held
as float, generated by `tools/gen_rx_coeff.py`), not the ADC: the PCM1821's own
filter is flat to 0.454 × fS = 80 kHz at the 177 kHz sample rate and would
allow a 96 kHz stream if the USB contract were changed. The QSD's node RC
corner (~4× the IF) costs about 0.3 dB at 20 kHz. The branch length sets both
the span and the receive half's CPU: 96 float taps measures 42,413 cycles of
the 61,370-cycle I2S half (69 %, peak 47,579 = 78 %) against 36,192 (59 %) at
the previous 64 taps, and the deadline counters (`pending` and `working` in
`rx`) stay at zero across the whole receive bench. Float rather than Q15 rows
pays twice: the int-to-float convert on each of the ~170,000 coefficient
fetches a second came to 2,200 cycles of the half on its own, and the Q15
rounding left the far stopband 8 dB shallower (−76 dB at 42 kHz against −84 dB
for the designed coefficients). A 128-tap Q15 design reaches 20.5 kHz but
measured 54,565 cycles (89 %, peak 99 %), and float rows there would need 16 KB
of flash v7.12 did not have (468,056 of 473,056 used, budget 469,000). v8.09's
flash layout 2 has the room, but not the receive half: 96 taps is where this
board stops.

### Receive I/Q calibration (`RX IQ CAL ~2S`, console `rxcal`)

QMX takes its quadrature from an Si5351A, whose even integer divider makes
90 degrees exact, and ships **no** I/Q correction at all; QRP Labs quote 60-75 dB
of image rejection, varying from unit to unit. REV1 builds the same quadrature
out of HRTIM ticks and has carried `RX I GAIN`, `RX Q GAIN` and `RX PHASE TRIM`
through the hot path since the receiver was written, with no way to measure
what to put in them. This closes that: what balanced integer-tick edges leave
behind -- tolerance in T401, U9/U12 and the two RC integrators -- is a fixed
gain and phase error, and three coefficients cancel it.

**The measurement.** TEST_CLK (PB14, HRTIM timer D) reaches the LPF input
through R106 100 k. Running timer D one or a few ticks from the QSD LO period
puts a carrier a known offset *delta* from the LO, through the real receive
path. One tick is about 9.3 kHz at 7 MHz and 18.8 kHz at 10.1 MHz, so the
offset cannot be asked for in hertz: `RADIO_TestClockStartPeriod` takes the
period directly. `RADIO_IqChoosePeriod` picks the smallest offset inside
6-45 kHz, above the LO where it can, which is clear of the PCM1821's ~44 Hz
DC-removal filter and inside the linear-phase filter's flat +-50 kHz.

Delta is exact without knowing SYSCLK. Both clocks divide the HRTIM clock and
fS divides the same SYSCLK, so it cancels:

    delta / fS = 2048 x i2s_divider x (p_lo - p_test) / (p_lo x p_test)

**The solve.** Correlating I and Q *separately* at +delta gives
`X = (A/2) e^(j phi)` and `Y = -j (1 + g) e^(j theta) X`, so
`(1 + g) e^(j theta) = j Y / X` and the carrier's arrival phase divides out.
Solving from the two *sideband* bins instead would not work: their ratio
carries a factor `e^(-2 j phi)`, so it is only right when the tone happens to
start at zero phase. The correction that follows,

    q_gain = 1 / ((1 + g) cos theta)      phase = tan theta      i_gain = 1

is exactly the `q = q * q_gain - i * phase` the hot path already applies
(`radio_rx_front.c`).

**Three passes of 370 ms**, 65536 samples each, a 2.7 Hz detector bandwidth:
injection off for the floor, injection on for the imbalance, then injection on
with the solved coefficients applied inside the correlator -- so the residual
reported is measured, not predicted. Two more passes optionally move the
demodulator (not the carrier) to put the same tone in the wanted and then the
opposite sideband, which is opposite-sideband rejection end to end. That one
only means anything in USB or LSB; CW, AM and FM have symmetric channel
filters. It is also a lower bound rather than a measurement once the rejected
tone falls below the receiver's own channel noise: at the roughly -26 dBFS the
100 k injector delivers, that ceiling is around 60 dB.

**Orientation comes free.** `Re(k) < 0` means the carrier is in the negative
bin, which is a swapped I/Q pair rather than an imbalance. The run reports it
and offers `RX IQ INVERT` with the coefficients, so that setting stops being a
guess made against a voice signal.

**Interference.** A signal *above* the LO reaches the detector through the same
imbalance and obeys the same `Y = -j k X`, so it moves X and Y together and
cannot move their ratio, however strong or close it is. One *below* the LO is
the opposite: I and Q are real, so a carrier at `-(delta + e)` puts its
conjugate component e hertz from the bin -- not 2 delta away -- carrying
`-conj(k)`. Sixty hertz is about twenty bins, where a rectangular window still
leaks -40 dB. That is why the correlator is Hann weighted. A 50 ohm dummy load
is still the right way to run it.

**Cost and safety.** The correlator is one oscillator update and two complex
MACs per sample in the existing PendSV worker, about 1.7 % CPU and only while a
calibration is reserved; its state is CPU-only and lives in CCM. The injection
is tens of microamps through 100 k. `RADIO_IqCalApply` is a separate step, like
`APPLY SWEEP BPF EDGES`, and refuses a result outside the settings' own bounds
(0.1..10 gain, +-0.5 phase) rather than clamping it into looking reasonable.
`rev1_rxcal` exercises every formula against a modelled imbalanced QSD and
checks the residual after applying the firmware's own correction expression.

**Not in the deep-diagnostic flavour.** That image carries its own pipeline
instrumentation and its FLASH_APP region is full; the receive bench costs about
4 KB it does not have. `rxcal` and `sweep` say so there, as the full-screen
`ui` view already does.

The self test's `I/Q AMPLITUDE BAL`, `I/Q PHASE ERROR`, `IMAGE REJECTION` and
`OPPOSITE SIDEBAND` rows report the last run rather than starting one, the way
`BPF SWEEP + FIT` reports the last sweep. `IMAGE REJECTION` is the corrected
residual; the two imbalance rows are the raw hardware.

### USB audio: the sound card and the I/Q stream (QMX `IQ mode`)

The USB audio personality is a USB Audio Class 1 sound card with two streams:
playback (host to radio, the TX audio when `TX AUDIO INPUT = USB`) and capture
(radio to host). It appears to the host as the sound card `SPARTACUS REV1`. The
capture stream is 48 kHz, stereo, 16-bit, and the
`USB RAW IQ` setting (`AUDIO`, or `BOARD` > `RX TRIM`) chooses what it carries:

- `USB RAW IQ = OFF` (default): the demodulated receiver audio, the same in both
  channels. This is what `tools/rev1_diag.py audio N --out x.wav` records and
  what `tools/rx_audio_analyze.py` reads without `--iq`.

  **DIGI receive** (USB/LSB with `TX AUDIO INPUT = USB` and `USB AUDIO POLICY =
  DIGI CLEAN`, i.e. CAT `MD9`/`MD6`) sends what QMX's Digi mode sends: the
  channel filter is QMX's 150-3200 Hz (`RADIO_DIGI_RX_LOW_HZ`/`_HIGH_HZ`, flat
  within 0.05 dB; the SSB filter's 300-3000 Hz was ~19 dB down on FT8 at
  3150 Hz), and the stream is the demodulated signal before the AGC at a fixed
  gain of 10 (`RADIO_DSP_DIGI_USB_GAIN`, 30 dB under the AGC's maximum). QMX
  has no AGC in Digi (its AGC works in CW only) and a fixed Band config RF
  gain. On a 40 m EFHW against a QMX+ (2026-10-02): through the AGC the band
  noise reached WSJT-X ~30 dB louder than from QMX+, its floor dropping 6 dB
  under each slot's stations and back in the gap, and the radio decoded 25 a
  slot to QMX+'s 30-32; with AGC OFF the full gain clipped 1.3 % of the
  samples. At the fixed gain the floor sits where QMX+ puts it (−42 against
  −43 dBFS in the same measure), steady through the slot, peaks −23 dBFS.
  `RX AGC` still sets the headphones.
- `USB RAW IQ = ON`: the complex receive I/Q, left = I, right = Q, taken after
  the front end (DC blockers, I/Q gain/phase/skew correction, NCO, anti-alias
  and decimation) and before the channel filter and demodulator. The tuned
  frequency sits at DC, because the front-end NCO removes the planned IF, so an
  SDR program's centre frequency is the radio's dial frequency.

This is QMX's `IQ Mode` (operating manual 1.03.002: "the raw I and Q channels
from the ADC are fed to the USB soundcard directly, without any demodulation
... permitting use of SDR software as a panadapter"). Differences worth
knowing: REV1 streams *corrected* I/Q rather than raw ADC words, so the image
rejection is what `RX IQ CAL` achieved (60–75 dB, the same range QRP Labs
quote for the uncorrected QMX), and the radio keeps receiving normally —
headphones and S-meter included — exactly as QMX behaves since firmware
1_00_012.

**Usable span.** The stream is 48 kHz wide. The front-end decimation table
passes to ±19 kHz within 1 dB and is 70 dB down from 25 kHz (measured on the
production path: −0.17 dB at 18 kHz, −2.7 at 20 kHz, −35 at 24 kHz, −70.7 at
25 kHz, −84 at 42 kHz), so a panadapter can be trusted out to about ±19 kHz.
Anything it draws past ±24 kHz is floor, not aliases. The DC blockers run
before the NCO, so there is no artificial spike at the centre; the mirror
rejection is the board's own `RX IQ CAL` result.

**Host software.** Any soundcard-I/Q program works. Used with QMX and known
here:

- gqrx (Linux) through the **SoapyAudio** module, which stock gqrx does not
  include. Install it, confirm `SoapySDRUtil --find="driver=audio"` lists the
  card, then use gqrx's "Other..." input with
  `driver=audio,device_id=N,soapy=0,chan=stereo_iq` (without `chan=stereo_iq`
  the two channels are treated as real samples and the waterfall mirrors about
  0 Hz).
- HDSDR, SDR Console or SDR# (Windows): select the capture device as a sound
  card input and type the dial frequency as the centre frequency.
- GNU Radio: `audio.source` (2 channels) → `float_to_complex` → FFT sink.
- Nothing installed: `tools/rev1_diag.py audio 30 --iq 1 --out iq.wav`, then
  `python3 tools/rx_audio_analyze.py iq.wav --iq --dial <dial Hz>`.

**What it does not do.** Since v8.10 the sound card has CAT beside it (the
next section), so an SDR program that speaks Kenwood/QMX CAT can follow the
dial; one that does not still has its centre frequency typed by hand. `Q91;`
and `Q90;` switch IQ mode from the host. Transmit audio from
the host keeps working on the same card, but the capture stream is silence
while transmitting, because the receive front end is switched off (`RX_EN`,
PC7): there is no panadapter during TX, unlike QMX in CW. And with IQ mode on,
the radio is no longer a sound card for WSJT-X — QMX's own warning applies
unchanged. The capture ring is filled only while a host has the interface open
(`capture_streaming` in `Core/Src/radio_audio.c`), so an idle receiver pays
nothing for it; the console's `status` line reports `capture=0/1`.

### USB serial ports: CAT and terminal, as QMX (v8.10)

The audio personality is a composite device, as QMX presents itself
(operating manual 1_02_000, "USB serial ports"): the sound card above plus two
CDC-ACM virtual serial ports, on Linux
`/dev/serial/by-id/usb-Chireix_SPARTACUS_REV1_<serial>-if03` and `-if05`. The
sound card keeps interfaces 0–2, so its name on the host is unchanged.

**Every port is CAT or terminal**, like QMX's: it starts as CAT, and a CR or LF
turns it into the console (the same one the DIAG personality has, with `ui`,
`paddle`, `set` ...) until the host closes it. CAT never sends a CR or LF.
One port at a time can be the console; a line ending on the other is answered
`?;`. The console here is the operator's: entering it does not force TRANSMIT
MUTE the way `USB DIAG CONSOLE` does, and `audio N` / `exit` stay DIAG-only.
`cat` on the console shows the ports, the commands taken, CAT transmit and
USB VOX.

**CAT** is QMX's Kenwood TS-480 set (CAT programming manual 1_02_006), so
WSJT-X, JS8Call and others use Hamlib's **QRPLabs QMX** rig (model 2057):
`ID` (020), `VN`, `OM`, `IF` (TS-480's 38 bytes), `FA`/`FB`, `FR`/`FT`/`SP`
(VFO A, no split), `MD`, `TX`/`RX`/`TQ`, `AI`, `PS`, `AG`, `KS`, `FW`, `SM`,
`PC`, `SW`, `RT`/`RC`/`XT`, `Q1` sideband, `Q3` USB VOX, `Q9` IQ mode, `QB`/`QC`
CAT timeout, `SS` SSB TX source (0 USB, 2 microphone), and the menu manager:

- `MMpath;` reads a setting, `MMpath=value;` writes it (saved like a panel
  change), `MMpath?;` describes a node (`type|list|name`: 0 sub-menu,
  1 page or action, 3 number, 5 list), `MLn;` lists list type `n`'s values.
  Paths are this radio's menu: `AUDIO|USB RAW IQ`, `OPERATE|TRANSMIT MUTE`,
  `BOARD|ASSEMBLY|DRIVER A`; names are case-insensitive, indexes from 0 work,
  and a section title may be named (`AUDIO|USB INPUT|USB VOX`).

Modes: `MD1` LSB, `MD2` USB, `MD3` CW (`MD7` reads back 3), `MD4` FM, `MD5` AM,
and QMX's DIGI as Hamlib sends it for PKTUSB/PKTLSB: `MD9` USB-DIGI and `MD6`
LSB-DIGI, the sideband with `USB AUDIO POLICY = DIGI CLEAN` and
`TX AUDIO INPUT = USB`. `MD1`/`MD2` put the voice processing back. A mode
change waits, like the panel's, until the audio path is idle (at most 5 s).
`FA` tunes through the knob's path (paced, in place); 1.0–28.3 MHz.

**Transmit, QMX's way.** With `USB VOX` OFF (the default, `AUDIO` > USB INPUT;
QMX's `VOX`), USB audio keys the transmitter only while CAT transmit is on
(`TX;`/`TQ1;` .. `RX;`). Audio arriving without it is held exactly as
TRANSMIT MUTE holds it, so a sound server's idle stream or a system sound
neither transmits nor makes the receiver stand down, and does not hold the
panel off. `USB VOX` ON keys on any arriving USB audio, the behaviour before
v8.10. TRANSMIT MUTE overrides both. Closing the port that asked for transmit
ends it, and so does the CAT timeout (`QB`/`QC`, on, 120 s, QMX's default)
when no CAT command follows a transmit for that long.

**DIGI TONE TX** (`AUDIO` > USB INPUT, id 223, ON by default) sends DIGI
CLEAN's USB audio the way QMX does: the audio is measured, not modulated
(`mod_digi_tone_logic.h`). Each rising zero crossing is timed to 1/65536 of a
sample, the tone is the mean period over the last >= 10 ms of crossings, and
the carrier is a constant-envelope, phase-continuous tone at that offset from
the dial at full drive, with a 10 ms smoothstep at key-down and key-up.
WSJT-X's Pwr slider and the sound server's volume do not set the power. Where
QMX keys only above 80 % of full scale -- measured on a QMX+ 1_04_010: RF at an
audio peak of −1.6 dBFS, none at −2.2 (the "two dots" with a PC level a
little low) -- any tone above −50 dBFS keys here. A cycle an eighth off the
mean is not a tone (speech, noise, two tones), an FSK shift settles in four
cycles, a cut stream ends the carrier within ~9 ms and a dropped USB frame or
two does not. On the host model: power constant from 0 to −48 dBFS, FT8
decoding as from an ideal transmitter, the opposite sideband ~−74 dBc;
without RF on the bench the measured tone followed every FT8 tone exactly at
full and −30 dB volume, at 26 % CPU against linear DIGI CLEAN's 38 %. OFF
gives the linear DIGI CLEAN path (any audio, e.g. several tones).

**WSJT-X:** Rig `QRPLabs QMX`, the `-if03` port, PTT method CAT, Mode
`Data/Pkt` (MD9 on every band change), the `SPARTACUS REV1` sound card for
input and output at 100 %. Starting WSJT-X with `--rig-name SPARTACUS`
keeps these settings in an instance of their own.

**Packet memory** is the reason there are two ports and not QMX's three:
the 1 KB of USB SRAM holds the double-buffered playback and capture
(2 × 200 B each) and, since v8.10, a 32-byte control endpoint (was 64) and
per port an 8-byte OUT and 32-byte IN buffer; the two notification
endpoints share one 8-byte buffer because nothing is ever sent on them
(`usbd_conf.c`). Endpoint numbers 4–7 are all the FS peripheral has left.

### DIGI mode: what the radio knows while WSJT-X decodes (v8.10)

**DIGI is a mode** (`UI_UMODE_DIGI`), after FM in the MODE cell: SSB with
`USB AUDIO POLICY = DIGI CLEAN` and `TX AUDIO INPUT = USB`, upper sideband
from the panel, either sideband from CAT (`MD9` USB-DIGI, `MD6` LSB-DIGI, what
Hamlib sends for PKTUSB/PKTLSB). The header reads `DIGI`; `MD1`/`MD2` or
another MODE put the voice processing back. What changes with it:

- **Receive** as QMX's Digi: the 150–3200 Hz channel, and the USB capture
  taken before the AGC at a fixed gain of 10 (the AGC's level is for the
  headphones only). Samples are rounded to 16 bits (VCVTR), not truncated.
- **Headphones off.** TIM2 is parked (PA0/PA1 Hi-Z, the coupling
  capacitors keep their charge, so leaving DIGI starts them without a pop)
  and the 48 kHz ring is drained unread; VOLUME leaves the slide row. The
  receive worker's share of the core: 69 → 63 % idle, 79 → 70 % with
  WSJT-X recording (20 m, 2026-10-05).
- **The FT8 slot clock** (`radio_slot_logic.c`), learnt from the passband:
  channel power in 100 ms bins folded modulo 15 s on the millisecond tick.
  FT8 is silent for ~2.4 s before each boundary; the quietest 2 s window
  places it. Locks after three slots once the busy part stands 2 dB over
  the gap (on 20 m: 10–12 dB), re-learns continuously, needs no UTC. A CAT
  PTT (WSJT-X raises it at the boundary) refines the phase if it agrees
  with the band within 1 s; a Tune press elsewhere does not move it.
- **Disruptions wait for the gap.** AUTO's deferred PLL upgrade and the
  re-plan after a mode change each restart the receiver (~10 ms and a fresh
  DSP). WSJT-X's capture stream is not USB audio to the retune gate, so they
  could land mid-slot; in DIGI they wait until 13.4–14.7 s into the slot
  (`RADIO_DigiQuiet`), at most 15 s. Tuning (`FA`, the knob) is never held.

**On the LCD.** In DIGI the home zone shows the session instead of the
receiver's diagnostics; `DIAGNOSTICS` gains a sixth page, `DIGI`, and opens
on it in DIGI:

| cell | shows | warns |
|---|---|---|
| HOST | what WSJT-X receives: level in dBFS, or `NO STREAM` while the host pulls no frames | amber over −3 dBFS peak, red `CLIP` for 10 s after a clip |
| GAPS | stream gaps (padded or dropped frames, missed halves) in the last 10 min | amber when any |
| SLOT | seconds into the FT8 slot (`s TX` when a CAT PTT set it), `LEARNING` | — |
| REF | `HOST` and the tracker's ppm (10 s mean), or `LOCAL` | red `LOCAL MEMS` on the 8 MHz DSC1003 |
| TRACKER | `LOCKED`, `ACQUIRING`, `NO HOST`, `OFF` | amber `n STEP/10M`: re-acquisitions, each a frequency step (a host's first minute and a bus reset excepted) |
| TRANSMIT | as on the voice home, or the fault | as there |

The page adds the host's peak and clips, gaps in this slot, receiver
restarts, the ADC's own peak, the slot's activity (busy over gap, dB), and
the IF with the worker's load. Console: `digi` (four lines, the same
figures). There is no drift cell: the tracker's trim follows the DSC1003's
own wander (±1.5 ppm from one 2 s sample to the next, cancelled), and the
host's clock, which the dial does follow, cannot be seen from the radio.

**What limits FT8 decodes on this radio** (20 m EFHW, receive only,
2026-10-05; `tools/bench/digi_rx.py` and off-air carriers):

- Not the receiver. Its own floor (LPF off) sits 29.5 dB under the 20 m
  band noise; impulse noise, the headphone PWM, the 1 kHz USB comb (~20 dB
  under the band noise), 16-bit quantisation at the fixed gain and the
  ADC's level (peaks −52 dBFS) cost nothing measurable. No sample gaps in
  any recording; decodes' DT median +0.2 s. Widening the passband past
  3.2 kHz found nothing to decode there.
- **The frequency reference.** Following the host (REF HOST), the dial
  moves with the laptop's crystal: three off-air carriers fell together by
  9–10 Hz in 50 s (0.7 ppm/min, ~11 Hz/min at 14 MHz, 2.4 Hz across an FT8
  over) while the laptop was busy, 1.35 Hz/min with it quiet. Simulated
  slants cost 5 % of decodes at 2 Hz an over, 13 % at 3 and 23 % at 5; a
  host-load burst slanted single slots by 12–20 Hz (9 and 0 decodes where
  de-slanting recovered 21 and 8). REF LOCAL on the MEMS is far worse
  (±20 Hz a minute; 0.8 decodes a slot against 21). The fix is the TCXO
  (*The host's clock moves too*); until then, keep heavy jobs off the
  WSJT-X laptop while decoding.
- A band-locked receive AFC (the common shift of all FT8 signals between
  the halves of each slot, removed before WSJT-X) was tried offline on 78
  slots: +17 % on bursty slots, −7 % on steady ones where the estimate
  invented drift; even a per-region median estimator misread steady slots
  by up to 7 Hz. Not shipped.
- **WSJT-X:** AP decoding adds 3.5 % (jt9 `-d 3`, the a1 tags); turn it
  on (`FT8AP=true` in the instance's settings). The Wide Graph's 4.2 kHz
  span is the FT8 search range; `F Low`/`F High` are FST4's.

Bench: 16.5 decodes a slot at the start of the work (b0, during a build),
21.2 with the host quiet (b1), 23.0–23.75 on the DIGI image (d1/d2,
another hour's band: not an A/B).

### Low-pass filters (QMX+ 160–10 m set)

The five LPFs are the QMX+ filters with the 6 m filter (L519/L520) left out.
Slot 4 (12/11/10 m, L517/L518) is therefore the top designed filter, and no code
path can select a sixth slot.

> **This build: all five slots are populated.** `LPF_UNFITTED_MASK` in
> `radio_config.c` is 0, so a fresh install gets every QMX+ edge and TX floor
> and the **usable range is 1.0–28.3 MHz** (160 m through 10 m). The stock
> 14.02 MHz tune frequency is inside it again.
>
> `RADIO_ConfigLpfUnfitted()` remains, because a part shortage is the normal
> reason to ship a partial filter set. Setting a bit in the mask zeroes that
> slot's upper edge and TX floor, which removes it from `covering_slot()`
> entirely: it is never selected for receive or transmit and its BSS123 never
> turns on. Above the top live slot the home screen reads `NO LPF FOR FREQ` and
> the receiver stays off, because the same filter carries transmit and, from
> the antenna side, receive — with no toroid there is no RF path either way.
> `LPF n UPPER MHZ` writes the same zero from the menu, so one filter can be
> retired without a rebuild.
>
> **A board commissioned while a slot was out of service keeps the zeros in
> its saved settings**, since the settings migration only runs for images older
> than v22. Press `QMX+ LPF EDGES` in BOARD > FILTERS once and save, or send
> `set put <id> 0` for that action on the console. TX on a restored slot still
> needs its `LPF TESTED MASK` bit, which is a separate bench result.
Parts, values and designators match QMX+ (assembly manual 3.07, schematic
Rev 2/3). The KiCad file carries placeholder capacitor values for these parts:
LPF_2–LPF_4 all read 220/560/33/270 pF. The netlist confirms the topology
below.

- PA output (`RF_RAW_OUT`, C62 2.2 µF) → PA-side rail. This rail carries the
  input PIN diodes, the L3 bias feed and the receive tap (C65 → Q8).
- Each filter is C–L–C–(L‖C)–C. A 47 µH choke returns the diode bias from the
  middle node to its BSS123.
- Antenna-side rail → C69‖C107 → T5 SWR bridge → antenna. This rail carries
  the output diodes, the L7 bias feed and TEST_CLK (R106).

The same filter serves transmit and, from the antenna side, receive.

| Slot | Pin | Input C | Input L | Middle C | Output L ‖ notch C | Output C | QMX+ bands | Upper edge | TX floor |
|---|---|---|---|---|---|---|---|---|---|
| 0 | PE2 | C514 1200 pF | L506 4.42 µH | C517 2200 pF | L511 3.54 µH ‖ C520 220 pF | C523 820 pF | 160 m | 2.5 MHz | 1.70 MHz |
| 1 | PE3 | C515 680 pF | L508 1.83 µH | C518 1200 pF | L512 1.55 µH ‖ C521 100 pF | C524 680 pF | 80, 60 m | 5.6 MHz | 3.45 MHz |
| 2 | PE4 | C516 390 pF | L510 1.03 µH | C519 680 pF | L513 866 nH ‖ C522 43 pF | C525 390 pF | 40, 30 m | 10.3 MHz | 6.30 MHz |
| 3 | PE5 | C531 180 pF | L515 518 nH | C532 330 pF | L516 435 nH ‖ C533 27 pF | C534 180 pF | 20, 17, 15 m | 21.45 MHz | 12.2 MHz |
| 4 | PE6 | C535 120 pF | L517 313 nH | C536 180 pF | L518 268 nH ‖ C537 15 pF | C538 82 pF | 12, 11, 10 m | 28.333 MHz | 20.5 MHz |

The host suite `rev1_lpf` models these filters (50 Ω both ends). The model
includes diode resistance, inductor Q and self-capacitance, capacitor ESL, the
bias chokes and the capacitance of the eight reverse-biased diodes of the
unselected filters. The −3 dB points are about 2.9, 6.2, 11.2, 22.4 and 38 MHz.

- **Upper edges.** Every amateur band stays on its QMX+ filter. Above each
  band, receive moves to the next filter before the loss rises; the next
  filter passes each crossover with < 0.6 dB loss. 15 m sits at the top of
  LPF_3, as on QMX+, with 1.6–2.5 dB loss in this model.
- **TX floors.** The notches (L‖C) sit near the 3rd harmonic because the
  push-pull PA has low even harmonics. Below a floor, harmonics fall into that
  filter's passband, so `RADIO_ConfigTxBlock` refuses TX there
  (`BELOW LPF TX MIN`). A floor is the lowest frequency where the filter adds at
  least 10 dB at 2f, 40 dB at 3f and 50 dB at 4f–9f. That is QMX+'s own worst
  case: at the bottom of 80 m it gives 11.9 dB at 2f and 43.1 dB at 3f. The
  floors hold over five parasitic variants: off-diode capacitance 5–15 pF,
  3 pF / 3 nH parasitics, and Q 80 with 2 Ω diodes. 3 % lower, the nominal
  model already fails.
- **No-TX gaps.** No fitted filter transmits cleanly below 1.7 MHz, or in
  2.5–3.45, 5.6–6.3 and 10.3–12.2 MHz. Receive is unaffected. Regulatory band
  limits remain the operator's responsibility.
- **Defaults and storage.** These edges and floors are defaults, alongside the
  QMX 60–15 m BPF. The floors use the former `reserved_words` bytes (uint16 kHz
  per slot), so the settings layout and size are unchanged. A v21 image
  without floors gains the QMX+ floors, plus the QMX+ edges unless edges were
  entered. A floor of 0 blocks TX on that slot. `LPF n TX MIN MHZ` edits a
  floor, and `QMX+ LPF EDGES` restores edges and floors. TX still needs
  `ASSEMBLY CONFIRM`, the drivers, drains, bias and the `LPF TESTED MASK` bit
  for the slot.
- **Monitor page.** It shows the selected LPF/BPF and the first missing TX
  condition for the tuned frequency, for example `LPF 1 BPF 3 TX LPF UNTESTED`.
- **Bias.** See *PIN diode bias* below. In receive the diodes run from the 5 V
  rail through R74/R77 (about 4 mA), as in QMX+.
- **Switching order.** `RADIO_SelectFilters` drops the RF permit, turns the
  bias buck off and clears RX_EN *before* it moves the one-hot select, so the
  PIN diodes are never hot-switched under drive. The write is a single
  `GPIOE->BSRR` that sets at most one of PE2..PE6 and clears the other four; a
  slot index of 255 clears all five, which is what the firmware now does when
  no configured filter covers the tuned frequency, rather than leaving the
  previous filter biased for a band it does not serve.

### PIN diode bias (QRP Labs method)

The circuit is the QMX+ transmit-only PIN bias buck, which QRP Labs derived
from QDX Rev 4 (QDX assembly manual 1.24, "A new bias circuit for PCB Rev 4";
QMX operating manual 1.03.002, Band configuration and Diagnostics). Firmware
follows that design:

- **Current setting.** `PIN BIAS MA` is the forward current per diode during
  TX, like QMX's `PIN fwd bias (mA)`. The default is 30 mA. The range is
  5–45 mA; QMX allows 60 mA, but the REV1 ADC reads only up to its 2.9 V
  reference and 45 mA already needs about 2.6 V. As in QDX/QMX, 5 mA keeps
  the buck off, and the diodes run on the ~4 mA receive bias.
- **Voltage target.** ADC_BIAS measures TX_BIAS, before the 33 Ω feeds
  R75/R76. The target voltage comes from QRP Labs' measured model:
  - the node after each 33 Ω sits at 0.6741 V + 12.1 Ω × I, which lumps the
    diode drop, both 47 µH chokes, the LPF toroids and the LPF switch;
  - the current R74/R77 supply from the measured 5 V rail is subtracted from
    what the buck must provide.

  At a 5 V rail this gives 1.896 V for 30 mA; about 0.86 V then drops across
  each of R75 and R76. The monitor page shows TX_BIAS, the modeled current
  and the duty, like the QMX `BIAS SMPS` diagnostics line.
- **Accuracy.** The model was measured on QDX. REV1 has QMX+'s BSS123 switches
  driven from 3.3 V. In the host plant model, a node that differs by
  −74/+76 mV and −4/+6 Ω moves a 30 mA setting by up to 5 mA. If the drop
  across R75/R76 in `BIAS TEST 5S` shows a different current, adjust
  `PIN BIAS MA`.
- **PWM frequency.** QMX runs this buck near 116 kHz and moves the frequency
  so that no harmonic falls near the operating frequency. On TX entry, REV1
  picks a TIM4 period for 100–130 kHz that keeps every harmonic at least
  50 kHz from the carrier (host suite `rev1_audio`). The buck runs only during
  TX and `BIAS TEST 5S`.
- **Loop.** It runs at 4 kHz on every ADC scan (QMX: 1 kHz).
  - The reference starts at the measured TX_BIAS, or at the target if C73 is
    already higher, and slews 16 mV per scan.
  - VIN feed-forward (duty ≈ reference/VIN) supplies most of the duty, as in
    QMX's supply-voltage control term. A 10 Hz VIN-scheduled integrator trims
    losses, with a back-calculated clamp.
  - While TX_BIAS is more than 150 mV above the reference, pulses are skipped.
  - Duty is capped at 50 %, QMX's maximum, and at `1024 × (limit + 1.5 V) / VIN`.
  - The `rev1_policy` plant includes BSS84 on-resistance, the SS14 drop,
    0.5–7 Ω inductor DCR, 6.5–14 V VIN and 10–45 mA. It settles in 40–80 ms
    without overshoot. TX starts once the 4 ms mean (below) has stayed within
    ±30 mV (about ±0.7 mA) for 10 ms: at most about 110 ms after the key with
    0–60 mV rms of ADC noise per scan.
- **Measurement.** ADC_BIAS is sampled once per 4 kHz scan through R86
  (10 kΩ). On the bench one scan scatters 27–29 mV rms (638–797 mV around a
  721 mV receive node, 2026-09-27), white from scan to scan, while C73 keeps
  TX_BIAS itself quiet. The loop and the trips below act on each scan. The
  TX-entry window, the loop's starting point, `bias_mv` in telemetry, the
  monitor page and the self test use an exponential mean over 16 scans (4 ms,
  about 5 mV rms), restarted whenever the ADC restarts. Raw scans remain in
  `adc_samples` for SWD. Comparing single scans with the window, the keyed
  main loop, which sees nearly every scan, could never hold it for 10 ms, and
  every key-down would have ended in `FAULT_BIAS`.
- **Performance envelope.** QMX shuts a buck down when its duty leaves the
  range expected for the supply voltage. REV1 trips `FAULT_BIAS` when TX_BIAS
  stays below half of duty × VIN (less 200 mV) for 5 ms. That catches a
  shorted C73 or feed, which is a known QMX C504 failure, as well as an open
  sense path or a dead switch. In the plant model a short trips in 10 ms at
  0.13 A, below the BSS84's 0.52 A pulsed rating. The model has no false trips
  from power-up, precharged or low-VIN starts.
- **Other trips.**
  - TX_BIAS above `BIAS LIMIT MV` for 2 ms while switching.
  - During TX, more than 100 mV below the target for 10 ms.
  - `TX SETTLING` without reaching the window within 500 ms.
  - `BIAS OVER LIMIT` blocks TX when the target at a 4.4 V rail would come
    within 50 mV of the limit.

### Receive band-pass filters (QMX 60–15 m set)

U10 (74CBT3253, S1:S0 = PD1:PD0) selects one of four series-tuned sections
between RX_IN and T401. They are fitted like the QMX 60–15 m kit (assembly
manual L401a/L401b and IC402 tables; slot mapping as G0UPL gave it on the
QRP Labs forum, 2024-05-02). Every coil starts at L401 pin C1A (RX_IN):

| Slot | U10 | Capacitor | Coil (T37-6) | QMX bands | Default upper edge |
|---|---|---|---|---|---|
| 0 | 2B1 | C403 39 pF | L401a tap, 15 of 24 t | 15 m | 28.333 MHz |
| 1 | 2B2 | C401 56 pF | L401a, all 24 t | 20 m, 17 m | 18.5 MHz |
| 2 | 2B3 | C404 150 pF | L401b tap, 11 of 34 t | 30 m | 13.5 MHz |
| 3 | 2B4 | C402 220 pF | L401b, all 34 t | 60 m, 40 m | 8.5 MHz |

The default crossovers sit where the QMX RF sweep curves (operating manual
1.03) swap order. Slot 3 rolls off above about 8 MHz. At 13 MHz slot 2 is
still flat while slot 1 is about 6 dB below its peak. Slot 1 has a deep notch
just above 18.3 MHz, so 18.5 MHz keeps 17 m on slot 1 and gives 15 m to slot 0.
Every amateur band therefore uses the same filter as on a QMX (host suite
`rev1_policy`). Below 5 MHz slot 3 is the closest match, but this set is not
designed for 80/160 m; expect reduced sensitivity there, as on a QMX 60–15 m.
When a saved configuration has all four BPF edges at zero, it loads these
defaults; edges that were entered are kept. `QMX 60-15M BPF EDGES` restores
the defaults.

**Measured crossovers (`BPF SWEEP ~20S`).** This is the REV1 counterpart of
the QMX RF sweep. It runs only while receiving and needs at least two BPF
edges plus LPF edges that cover the swept frequencies. It steps 64 log-spaced
frequencies from 4.0 to 28.33 MHz (3.2 % apart). At each frequency the firmware:

1. selects the covering LPF;
2. runs the QSD LO and TEST_CLK (Timer D) at exactly the same period;
3. measures each of the four BPF slots.

TEST_CLK enters the LPF input through R106, so every measurement includes the
LPF, Q8, U10, the BPF section, T401 and the QSD.

An integer HRTIM period cannot place a test tone a few kHz from the LO above
about 16 MHz, so the test clock converts to DC instead. The PCM DMA interrupt
switches the injection on and off every 64 I2S frames (about 1.4 kHz). That
rate is far above the PCM1821 DC-removal high-pass (about 44 Hz). A
Hann-weighted on-minus-off average gives the level. Op-amp offsets and LO
leakage cancel. In the host model, signals away from the chop harmonics are
about 100 dB down. The same model reads a −40 dBFS carrier within ±0.2 dB at
any chop phase and has a −76 dBFS floor.

Each slot waits 40 halves (the decimation filter's group delay) and then
averages 128 halves. One slot takes about 60 ms, one frequency about 275 ms
and the whole run about 18 s. Headphone audio is muted during the run.

Level checks:

- One no-injection reading at the start sets the floor. The strongest point
  must be 20 dB above it, or the run ends with `NO TEST SIGNAL`.
- Frequencies that no LPF covers are skipped. If none is covered, the run ends
  with `NO LPF COVERS THIS RANGE`.
- `DONE (ADC CLIPPED)` means some samples reached 98 % of full scale.

The fit keeps the frequency order of the current edges and cuts the sweep
into one contiguous run per slot. It chooses the cuts that maximize the summed
level of the selected filter (dynamic programming over the 64 points). Each
edge is the geometric mean of the two frequencies around its cut. The top slot
keeps 28.333 MHz, and ties keep the cut nearest the current edge.

The page plots the four curves over the 30 dB below the strongest point. The
filter that the fitted (else current) edges select is drawn bold, with dotted
crossover lines. Each slot shows its current and fitted upper edge and its
peak. `SEL` stops the run; protection trips and a 60 s limit stop it too.
`APPLY SWEEP BPF EDGES` copies the fitted edges. They persist only after
`SAVE`. Levels are relative to ADC full scale, not absolute sensitivity.

**SD export.** When a card is mounted and not shared as a USB disk, each run
that measured anything is written to `EXPORTS/BPFnnn.CSV` (first free 000–999).
This includes completed, failed and stopped runs. The page shows the file name.
The file has five header lines:

1. the slot legend;
2. status, SYSCLK, clip flag and floor;
3. `edges_hz_in_use`;
4. `edges_hz_fitted`;
5. the column names.

Then comes one row per frequency, with the columns
`hz,bpf0_dbfs,bpf1_dbfs,bpf2_dbfs,bpf3_dbfs,bpf_in_use,bpf_fitted`. Empty
fields are points that were not measured. Files from runs before and after
re-tuning the coils can be compared directly.

## QRP Labs hardware-test equivalence

QMX's Hardware Tests menu (operating manual 1.03.002 §8.6) is the reference
bench method for this class of receiver. Every tool that does not transmit has
a REV1 counterpart, so measurements can be put next to QRP Labs' published
figures. All of them want a 50 Ω dummy load, as QMX says of its own.

| QMX tool | REV1 | What good looks like |
|---|---|---|
| 8.6.1 Audio filter sweep | `afsweep run`, then `afsweep table`; `EXPORTS/AFSnnn.CSV` | Wanted curve flat between the 150 Hz and 3.2 kHz corners; opposite sideband ≥ 60 dB down (QMX quotes about 60 dB) |
| 8.6.2 RF filter sweep | `sweep rf` per band, or `sweep run` for the whole range with the crossover fit; `EXPORTS/RFSnnn.CSV`, `BPFnnn.CSV` | Peak near band centre; compare directly with the QMX 60–15 m curves |
| 8.6.3 Image sweep | `imagesweep run`; `EXPORTS/IMGnnn.CSV` | Four responses visible, wanted over image better than QMX's uncorrected 60–75 dB |
| 8.6.4 SWR sweep / Tune SWR | TX TEST at `TX TEST TUNE %` (QMX's Tune %, 50 % by default) with the live FWD / REF / SWR line; `meter` | A 50 Ω load reads SWR ≤ 1.1 once `metercal leak` has run on the band (below) |
| 8.6.5 LPF sweep | `sweep lpf`; `EXPORTS/LPFnnn.CSV` | Five roll-offs near the 3 dB points of the QMX+ model (2.9 / 6.2 / 11.2 / 22.4 / 38 MHz). QMX has to transmit for this; REV1 does not |
| 8.6.6 PA mod. test | `ARM ANALOG TESTS` + the ENVELOPE rows of `selftest run analog` | Transmit-side; opt-in |
| 8.6.7 Test ADC I/Q | `iq` | Equal I and Q amplitude, clean 90°, symmetric peaks |
| 8.6.8 SSB calibration | — | Transmit only |
| 8.6.9 Microphone test | `mic` | Biased mid-scale, `rms_x16` moving with room noise |
| 8.6.10 Diagnostics | `diag`, `telem`, `keys`, `pins` | Supply, rails, bias buck, controls, inputs |
| 8.8.1 CPU monitor | `cpu` | Receive and headphone halves inside 61,370 cycles |
| (no QMX equivalent) | `rxcal` | I/Q imbalance measured and corrected per board; QMX ships no correction at all |

Each of those exports lands in `EXPORTS/` as a CSV, and
`tools/rx_audio_analyze.py <file> --compare-qmx` reads any of them back and
prints the measurement beside the QRP Labs figure for the same test: the
audio passband corners and the worst in-passband opposite-sideband ratio
against QMX's ~60 dB, the wanted-over-image ratio against QMX's uncorrected
60-75 dB, each LPF slot's measured -3 dB corner against the QMX+ model, and
the per-slot BPF peak and span. Where QRP Labs publish a curve rather than a
number it says so instead of inventing a threshold.

### What the HRTIM resolution costs at the top of the range

The receive LO is an integer HRTIM period off SYSCLK x 32 (5.44 GHz at
170 MHz), so one tick moves it by `rf^2 / (SYSCLK x 32)`: 9 kHz at 7 MHz,
36 kHz at 14 MHz, 148 kHz at 28 MHz. Three consequences, all measured on REV1
on 2026-09-20 into a 50 ohm load.

**Tuning holes.** `RADIO_RxPlanLo` needs the IF inside the flat +-50 kHz span,
so once the step passes 100 kHz -- above about 23.3 MHz -- some frequencies
have no LO at all. Scanning the planner on a 100 Hz grid:

| Band | No LO plan | Usable (quality 2) |
|---|---|---|
| 160 / 80 / 60 / 40 / 30 / 20 m | 0 % | 100 % |
| 17 m | 0 % | 92.2 % |
| 15 m | 0 % | 94.8 % |
| 12 m | 13.9 % | 82.2 % |
| 10 m | 29.2 % | 67.3 % |

The widest unusable run is 49.5 kHz from 28.2336 MHz. This is the same number
the self test reports as `LO RX BAND COVER PCT`.

**`rxcal`, `afsweep` and `imagesweep` stop above about 16 MHz.** They need a
test clock one tick from the LO, and one tick is no longer a few kilohertz.
17 m and 15 m return `NO TEST OFFSET AT THIS FREQUENCY`.

**Image rejection falls with the IF, because the I/Q error is a fixed skew.**
The measured phase error tracks the offset it is measured at, not the band:

| Band | offset | phase | implied skew | raw | corrected |
|---|---|---|---|---|---|
| 80 m | 7517 Hz | 9.7 deg | 0.633 samples | 21.4 dB | 59.6 dB |
| 40 m | 9308 Hz | 12.5 deg | 0.657 samples | 19.1 dB | 53.6-66.9 dB |
| 30 m | 18830 Hz | 25.9 deg | 0.676 samples | 12.7 dB | 35.8 dB |
| 20 m | 36043 Hz | 69.9 deg | 0.954 samples | 3.0 dB | 5.7 dB |

The 40 m row is the freshest: four `rxcal` runs behind the 96-tap float table
read 19.1 dB raw and 53.6, 63.0, 66.3 and 66.9 dB corrected. That is the spread
of the fit and of the measurement (the correlator works on a -52 dBFS carrier),
not of the table: `rxcal --sb --apply` stored the coefficients its own run
fitted (q gain 1.009, skew 0.657 samples on this board), and the self test's
`IMAGE REJECTION` row reads the same corrected residual.

**`rxcal apply` is only valid while its offset and the IF agree.** The
calibration correlates at delta, one HRTIM tick from the LO, but received
signals sit at the IF, and `RADIO_RxFrontSample` applies a single
frequency-independent matrix. Where the two offsets differ, a fixed skew makes
the fitted correction wrong in that same ratio. On 40 m delta is 9.3 kHz and
the IF 11.1 kHz, so the correction is within 20 % of right. On 20 m after the
clock fix delta is 37.3 kHz while the IF is 13.6 kHz, so the matrix would
over-correct by 2.7x and make reception worse, even though the verification
pass -- which also measures at delta -- reports 34 dB. Check `rx` for `delta_hz`
against `if_hz` before applying anything, and treat every "corrected" figure as
a number measured at delta.

A fixed skew means the required phase correction is proportional to frequency,
so one phase constant cannot be right on more than one band; the proper fix is
a fractional-sample delay on one channel rather than per-band coefficients.
Since the wanted signal sits at the IF, an IF far from the 12 kHz target costs
rejection in the same proportion, which is what the 20 m row is. v7.10 makes
`carrier_select_solution` shop for another PLL setting when the IF is more than
8 kHz off target, not only when the plan is unusable; that moves 14.020 MHz
from a +35424 Hz IF to -13599 Hz at 165.333 MHz.

**The sweeps are not affected by any of this.** `sweep`, `sweep lpf` and
`sweep rf` call `RADIO_MeasureClocksStart`, which puts the LO and the test
clock on the *same* period and recovers the DC product with the synchronous
chop, so no LO plan is involved. What they do have is a top-end sensitivity
fade above roughly 20 MHz that appears on every slot at once -- including the
38 MHz low-pass, which cannot be rolling off there -- so it is in the injection
and detector chain, not in the filters. Read a fitted crossover that depends on
the 18-28 MHz region with that in mind, and do not apply one blind.

### Audio filter and image sweeps (`afsweep`, `imagesweep`)

QMX sweeps its internal signal generator and leaves the VFO alone. REV1 cannot:
TEST_CLK is an integer HRTIM period and one tick is several kilohertz at HF, so
it cannot be moved 50 Hz. The carrier stays on one period and the demodulator
moves instead — `RADIO_AudioRetuneDsp()` shifts the 48 kHz NCO, which measures
the same filter with the two ends swapped.

`RADIO_DspInit`'s `offset_hz` is the dial frequency in stream terms: a carrier
at `tone` comes out at audio `tone − offset`, and the channel filter passes
+100…+3200 Hz of that in USB and −3200…−100 Hz in LSB. So the wanted curve puts
the carrier at `+f` of the dial in USB and `−f` in LSB, and the opposite curve
on the other side. There is no passband-centre term; adding one shifts both
curves by 1650 Hz and makes them coincide at the centre frequency, reading 0 dB
of rejection exactly where the filter is deepest. Host suite `rev1_af_sweep`
pins this.

What is measured is the power at the channel-filter output **before the AGC**
(`RADIO_DspLastPower`). Measuring the demodulated audio would plot the AGC's
response, not the filter's.

The AF sweep uses QMX's own grid: 100 Hz to 3850 Hz in 50 Hz steps, 76 points,
both curves interleaved so the wanted and opposite readings at one frequency
are taken tens of milliseconds apart and any drift affects them equally. Each
point retunes, waits 88 I2S halves for the elliptic filters to stop ringing,
then integrates 32 halves — 43 ms a point, about 6.5 s for the run.

The image sweep is the same engine over demodulator offsets instead. The
carrier sits at `Δ − IF` in the 48 kHz stream and its image, the conjugate the
QSD imbalance leaves behind, at `−(Δ + IF)`, so the two are `2Δ` apart and the
axis is derived from Δ rather than fixed at QMX's −30.5/+7 kHz. The stream is
only 48 kHz wide, so a band whose smallest available Δ puts the image beyond
±23 kHz reports `IMAGE OUTSIDE THE 48 KHZ STREAM: USE RXCAL` — at a 12 kHz IF
that is any band where one HRTIM tick exceeds about 11 kHz, which includes
30 m and above. Nothing is lost but the picture: `rxcal` correlates at ±Δ in
the raw 177 kHz stream, before decimation, and gives the same rejection as a
number on every band.

`ST_OPP_SB` in the self test reports the AF sweep's worst in-passband ratio
when one has been run this session, and falls back to `rxcal sb`'s single
±1650 Hz pair otherwise; the detail column says which it read.

### Raw I/Q, microphone and diagnostics screens

`iq` is QMX's Test ADC I/Q (8.6.7). It starts the test clock, takes one whole
I2S half of raw PCM1821 words — before the DC blockers, the I/Q correction and
the anti-alias filter — and prints both the samples and what they say. QMX
plots 64 samples and has the operator judge equal amplitude, a clean 90° and
symmetric peaks by eye; `RADIO_IqSnapshotStats` makes those the same judgements
as numbers. For a single tone the normalised cross-correlation
`Σ I·Q / √(Σ I² · Σ Q²)` is `sin θ`, so the departure from quadrature comes out
of the same three sums as the amplitude ratio. The test clock needs the
PCM1821's group delay to appear, so the first `iq` after starting it reports
the injection as settling and the next call reads that completed half.
`iq rearm` explicitly requests a fresh half. `iq raw` stops an active test
injection and takes the same kind of snapshot without a carrier.

QMX's fault list for this screen applies unchanged: a miswired or broken
trifilar transformer (T401 here), a badly soldered BPF or LPF toroid, a missing
dummy load, a railed pre-amp, or a dead converter. QRP Labs' warning applies
too — the traces get noisier above 20 m and that is normal, so do not conclude
the ADC is faulty until the other tools agree.

`mic` is QMX's microphone test (8.6.9). Each row is one `RADIO_MicProbe`, which
polls the ADC for about 12 ms without touching the audio pipeline, so it runs
with the receiver going. `mic [1-3] [rows]` selects MIC1 (PA2), MIC2 (PA3) or
MIC3 (PE9). The columns are raw 12-bit ADC codes -- `min max pp mean_x16
rms_x16`, the last two in sixteenths of a code -- because an electret on this
passive network moves the input by single codes and a millivolt scale rounds
that away. `rms_x16` is the sensitive one: `min`/`max` report the loudest
impulse in the window, which is noise. A live bias network sits mid-scale
(`mean_x16` near 2300 * 16) and `rms_x16` rises when the room does; pinned at
a rail means no microphone or a bias fault.

`mic mon N [seconds]` routes that microphone to USB capture without
transmitting. `mic ref [0-100]` is QMX's reference-channel cancellation
coefficient as a percentage and `mic split [0|1]` puts the uncancelled
microphone and its reference in the left and right capture channels so the
coefficient can be measured rather than inherited -- see the microphone front
end below. TIM2 is stopped for both the monitor and the transmit microphone
path.

### Microphone front end

REV1 acquires the microphone the way QRP Labs documents for QMX: sample far
above the audio rate, decimate to the 12 kHz speech rate keeping the sum's
fractional information, remove DC, and apply a fixed +50 dB. `MIC INPUT GAIN`
is a trim on top of that +50 dB, not the whole gain. TIM3 triggers ADC1 at
699,588 Hz and the decimator in `radio_mic_front.h` divides by 58.3; the
12 kHz stream is then held four times for REV1's 48 kHz transport, where the
existing TX speech filter removes the images.

Two things did not transfer from QMX unchanged, both settled by measurement on
this board:

- **Reference cancellation is off by default.** QMX subtracts 0.68 of a second
  channel on the same ADC peripheral and gains about 24 dB, because its
  converter noise is highly correlated across channels. On REV1 it is not: a
  `mic split` capture of MIC2 against MIC1 measures a coherence of 0.10 to
  0.31 from 100 Hz to 6 kHz, so no coefficient can remove more than 0.5 to
  1.6 dB, and 0.68 measures 0.2 dB *worse* than none. `mic ref` keeps the
  knob for a board that behaves differently.
- **The spare conversion buys oversampling instead.** One 243-cycle trigger
  period fits exactly two 12.5 + 12.5 cycle conversions. With cancellation off
  both go to the microphone through the hardware oversampler (ratio 2,
  shift 1, so the 12-bit scale is unchanged). That measured 2.1 dB less total
  noise, 1.5 dB less floor, and 10 to 14 dB off the alias lines at 981, 3240,
  3770 and 4462 Hz -- REV1's microphone noise is high-frequency-heavy, and
  moving the first alias band from 700 kHz to 1.4 MHz is worth more than the
  3 dB of extra averaging alone. ADC3 (MIC3) has no paired channel and always
  takes this path.

Measured on the bench with a tone played into MIC2 from a loudspeaker: the
line appears 29 to 32 dB over the silent baseline in its bin, the 300-2700 Hz
noise floor is -31.2 dBFS at the default gain, and a speech-shaped stimulus
peaks 27 dB over that floor. The residual floor is the converter's own: MIC1
with no capsule fitted measures the same total noise as MIC2 with one, and the
noise tracks the LO from 6.7 codes RMS at 3.6 MHz to 10.3 at 28.1 MHz.

`diag` is QMX's diagnostics screen (8.6.10), in QMX's own order so the two can
be read side by side: supply (QMX calls 6.0–12.5 V OK), rails, bias SMPS,
controls, inputs. REV1 has an LM1084 linear 5 V rather than QMX's 3V3 and 5V
bucks, so those rows are the measured rails. QMX's Transmitter block becomes a
Receiver block — state, rate, level, selected filters, overruns — with the
transmit-block reason printed where QMX prints its transmitter-disabled reason.

`HEADPHONE PWM PATH` in the self test used to count DMA overruns only, which
proves the path kept its deadline and nothing else. It now also reads back the
codes written to the DMA ring over about 25 ms: centred on half the reload and
moving. A dead interpolator or noise shaper leaves the codes sitting still
while the DMA runs perfectly, and that now reads as a warning rather than a
pass. With `HEADPHONE PCT` at 0 the phone loop takes its silent shortcut, so
the signal half of the check is skipped and the row says so.

The ring is not the comparators, though: through v8.02 it held perfect codes
while TIM2 held 0x00E800E8 and the headphones were silent. The row therefore
first reads CCR1/CCR2 against ARR on the running timer and fails with `TIM2
COMPARE BEYOND ITS PERIOD` if either is ever past it.

### Low-pass filter sweep (`sweep lpf`)

QMX measures its low-pass filters by transmitting into the SWR bridge
(8.6.5). REV1 does not have to: TEST_CLK injects on the **antenna** side of the
low-pass filters, between them and the antenna-side PIN diodes, so the ordinary
receive path already runs through them. The sweep forces each of the five
PE2..PE6 slots in turn at every frequency, holding the band-pass fixed for the
whole point so the five curves differ only by the filter under test.

The axis is 64 log-spaced points from 1.0 MHz — the lowest frequency the test
clock reaches, and low enough to show the 160 m passband — to 28.33 MHz, 5.5 %
apart. `RADIO_SelectFilters` clears RX_EN before it moves the one-hot select,
so the PIN diodes are never hot-switched, and each slot gets 40 ms of settling
after the switch before anything is integrated. Five slots at 64 points take
about 35 s. No crossover fit is attempted: that is a band-pass concept.

### On the LCD

Every sweep has a page, so the radio is a complete bench instrument with no PC
attached. They hang off `BOARD → FILTERS` (`LPF SWEEP ~35S`,
`RF SWEEP THIS BAND ~20S`, beside the existing `BPF SWEEP ~20S`) and
`BOARD → RX TRIM` (`ADC I/Q SCOPE`, `AF SWEEP ~7S`, `IMAGE SWEEP ~4S`).

The band-pass sweep page draws all three of its axes: it takes its slot count
and its frequency ticks from the run, so the five low-pass curves and the
per-band plot are labelled with their own numbers rather than the band-pass
axis's. Crossover lines and the bold selected trace appear only on the
whole-range band-pass axis, because that is the only one a fit means anything
on.

The AF/image page plots at 1 px per 0.5 dB rather than the sweep page's 2, so
65 dB fits and an opposite sideband 60 dB down is on the screen. The wanted
curve is bold and the opposite one single pixels; the worst in-passband ratio
is called out under the plot with QMX's own figure beside it.

The I/Q scope draws both channels on one shared scale, so unequal amplitude
shows as unequal height instead of being hidden by per-channel autoscaling,
with the amplitude and phase numbers above and the peaks below.

A click, `SEL` or a hold leaves any of them, and stops a run that is still going.

### Per-band RF sweep (`sweep rf`)

QMX's RF filter sweep is linear and centred on the band (8.6.2). `sweep rf`
does the same, 64 points over ±40 % of the tuned frequency, which puts the peak
and both skirts of any QMX band-pass section on one plot. `sweep run` keeps the
whole-range log sweep and its crossover fit, which is the part QMX does not
have.

The SD card is **SPI1**: PA4 CS, PA5 SCK, PA6 MISO and PA7 MOSI (AF5). The LCD
is **SPI4**: PE12 SCK and PE14 MOSI (AF5, set in `BOARD_GPIOInit`), with PE10
reset, PE11 CS, PE13 DC and PB9 TIM17 backlight (AF1). The USB data connector is U17 on PA11/PA12;
it is self-powered USB FS data and has no connection to the PD CC port.

## Power and SWR meter

REV1's bridge is QRP Labs' QMX/QMX+ T507: a Stockton tandem match on one
BN43-1502 (10 turns : 1 turn through each hole, so a coupled port carries a
tenth of its wave's line voltage), 49.9 Ω on each port, each read by an SS14
into 0.1 µF ‖ 100 kΩ and straight to ADC2 (IN7 forward, IN8 reflected).
QMX turns the two detector voltages into watts on a fixed scale and into SWR,
checks protection every millisecond, and reads PEP in SSB; QRP Labs call the
result "a useful indication". Two known errors come with it (QRP Labs forum
101694802): a flat 50 Ω load reads 1.03 on 80 m rising to 1.48 on 10 m (the
bridge's own leakage), and SWR reads better at lower power (the diode's
threshold). REV1's meter (`radio_rf_meter.c`) corrects both.

- **Diode.** Each detector's output, its rest level removed, is turned back
  into the port's peak voltage by a 12-point table of the ideal peak
  detector with an SS14-like diode (`tools/gen_rf_meter_table.py`, within
  1 mV above 6 mV). The rest level, ~2 mV of ADC offset and rectified noise
  (3.5-4.3 mV rms a scan), is learnt while no RF is up, unless FWD/REV ZERO
  are set by hand; through the diode's square law it is ~9 mV of port
  voltage, which read SWR 1.09 on a 50 Ω load at 0.04 W before.
- **Watts.** P = (10 · V<sub>port</sub>)² / 100 Ω, the bridge's theory,
  times a per-band factor a reference instrument can set (`metercal ref <W>`,
  P = Vpp² / 400 across the load). Carriers (CW, TX TEST, FM) read the mean
  over the scans above 90 % of the previous interval's peak, which averages
  the ADC scatter and leaves out keying ramps; SSB and AM read the peak of a
  1 ms mean (PEP, as QMX).
- **Reflection.** |Γ| is the least-squares ratio of the linearized reflected
  and forward voltages over the interval. The bridge's own reflection on a
  50 Ω load has two parts on REV1: one proportional to the forward wave and,
  from 40 m up, a reflected-port signal that does not follow the drive
  (~63 mV on 20 m, ~69 mV on 10 m; likely gate-drive feedthrough).
  `metercal leak`, run on a TX TEST carrier into the dummy load at full drain
  and again at about a third of it, measures both per band, and the reading
  takes both out in the power domain (the unknown phase averages out there):
  a 50 Ω load then reads 1.01 / 1.05 / 1.06 / 1.10 / 1.07 on 80 / 40 / 20 /
  15 / 10 m at the check power, against 1.03 / 1.11 / 1.25 / 1.18 / 1.29
  uncorrected. The uncorrected figure is kept (`swr_raw`, DIAG "raw").
- **Protection** is unchanged in kind (`RADIO_FAULT_SWR`, latched): reflected
  over the limit's share of forward for 8 scans (2 ms), now on the
  linearized voltages, above a 0.1 W gate (QMX: 0.5 W; REV1's drains sit at
  full voltage at any output in OUTPHASING mode). The leakage's constant
  floor (at most 0.1 V taken) comes off the reflected voltage first: it is a
  detector offset, and at 0.1 W it alone reads |Γ| 0.2, so quiet SSB into an
  SWR 1.6 antenna would have tripped a 3.0 limit. QMX met the same thing
  ("SWR protection kicked in sometimes at very low powers at the end of
  transmissions") and moved its gate to 0.5 W. The proportional leakage stays
  in, so the trip still errs towards tripping.
- **Against QMX/QMX+** (operating manual 1.03.002, firmware notes): QMX
  checks every 1 ms against a threshold (3 by default, can be switched off),
  shows `S` and silences the sidetone; it clears by button (default), on the
  next TX→RX, by a timer or only on menu exit/power cycle, and turns
  protection off in its SWR tools. REV1 keeps the full PROTECTION latch, keeps
  the limit on in TX TEST, and adds a jump trip under the limit (below).

**On the LCD.** While transmitting, and for `METER HANG MS` (1 s) after, the
home screen's diagnostics zone is the TX panel: forward watts and SWR in the
large digits, a power bar (0.5 W a segment) and an SWR bar (0.2 a segment
from 1.0, QMX's scale), reflected watts, the supply as it sags under the
key-down, and the PA drain; the meter row shows power. While the display
service is held off in TX, the TX cue redraws only those, once per reading
(`METER UPDATE MS`, 100). TX TEST shows FWD / REF / SWR / DRAIN / VIN /
TUNE; DIAG's transmit page shows watts beside the detector mV, SWR and raw
SWR. `TX PANEL` turns the panel off.

**CPU** (DWT on every handler, bench): the 1 ms detector mean costs 0.2 % in
receive; the meter's part of the ADC interrupt while RF is up 1.0 % in TX
TEST and 0.5 % in CW (~2.5 µs a scan at priority 0, from a RAM table found
in four compares); the TX panel's redraw 1-2 % of main-loop time. RF DMA,
the render worker and the receive worker read the same with the panel on and
off, without an underrun, an overrun or a pipeline event.

**Accuracy without a reference instrument:** SWR within about ±0.1 to 2:1 on
80-20 m once `metercal leak` has run; watts on the bridge's theory, about
±10 % on 160-30 m. 15 m and 10 m likely read low (the SS14's junction
capacitance) until `metercal ref`, and on 20 m and up the carrier's NCO grid
error makes the detected PEP move between clock plans (15 m repeated within
±0.012 in |Γ|).

Console: `meter` (the last reading, its band's calibration, the learnt rest
levels), `metercal [show|leak|ref <W>|table a,b,..|table default|clear [all]]`,
`telem` line 13. Settings v28 carries the table, the per-band gain, leakage
and floor, `TX PANEL`, `METER UPDATE MS`, `METER HANG MS` and
`TX TEST TUNE %` (zero = default throughout, so a v27 record reads back
unchanged).

## USB-C PD power connector

U18 is a separate power-only USB-C receptacle. PB4 is CC2 and PB6 is CC1,
each through the PCB's 470 Ω path. R3/R4 (5.1 kΩ Rd to GND) are fitted so a
USB-C source applies 5 V to the unpowered board. The G4's own dead-battery Rd
cannot provide this because UCPD1_DBCC1/2 (PA9/PA10) are used elsewhere.
Firmware does not claim Rp, VCONN, FRS, role swaps or a source role. D+/D− are
not wired to this connector, so QC negotiation and USB data cannot be
substituted for PD.

**CC termination** (`PD INTERNAL RD`, persisted, default OFF):

| Setting | Hardware | UCPD CCENABLE | USB-C cold start |
|---|---|---|---|
| OFF (default) | R3/R4 fitted | always 00: internal pulls off, so Rd is not paralleled down to 2.55 kΩ (below a 3 A source's vRd threshold) | yes |
| ON | R3/R4 removed | ST's sequence: both lines unattached, active line after attach | no; needs DC power first |

> **Status (v7.11, 2026-09-21): a 9 V contract is qualified on REV1.** With
> R3/R4 fitted and `PD INTERNAL RD` OFF, the production image negotiates on
> its own from a cold plug of the USB-C supply, with no console command:
> firmware senses CC2, receives the source capabilities and requests
> 9 V / 1 A. `pd` reports `contract=1 mv=9000 ma=1000 cc=2 supply=usb-pd
> why=ok probe=won` with `failures=0 resets=0`, firmware measures 8.84 V and a
> multimeter measures 9 V at VIN.

**Measured on the board (v7.11).** RM0440 §46.4.6 describes CCENABLE as
enabling the CC "analog PHYs (pull-ups and pull-downs)", while the Type-C
detectors have separate CC1TCDIS/CC2TCDIS controls, so on the face of it the
detector might have worked with the pulls off. It does not. With the setting
OFF and a PD charger plugged in and supplying 5 V, `pd raw` reported
`ccenable=0` with `cc1=0 (open/vRa) cc2=0 (open/vRa)`. Table 440 is right:
sink mode has no coding that enables the PHY without also applying Rd. ST's
`CAD_Check_HW_SNK` attaches only when one `TYPEC_VSTATE_CCx` is not vOPEN, so
the CAD never attached, `RADIO_PdTick` returned before the policy engine, and
the source parked at its default 5 V because R3/R4 are there. **That was a
board that powered up from USB-C and never negotiated.**

> **Status (v8.02, 2026-09-25): negotiation is supervised, not a one-shot.**
> Measured on REV1 against a 5/9/15/20 V + PPS charger with the debugger
> attached: a restart under a live 9 V contract resyncs in about 60 ms with
> VBUS never leaving 8.8 V (`resyncs=1`, `why=ok`); a forced PD hard reset and
> a physical unplug/replug each return to 9 V unaided within about 2.5 s.
> Without the debugger, a cold power-up 30 s after plugging in the charger --
> which by then had stopped advertising -- reaches 9 V with no warning.

### The attach supervisor

The one-shot boot probe of v7.11–v8.01 left three holes. It ran only inside
vSafe5V, so an MCU restart under a live contract (a fault, the watchdog, an SD
firmware install, the debugger) found 9 V on VIN, skipped PD and reported
`why=needs-rd` -- a SUPPLY warning on a board already at 9 V, with nothing
answering the source. Joining a source that holds a contract, or one that has
stopped advertising, ran ST's policy engine into its 500 ms SinkWaitCap hard
reset. And a source plugged in, or returning after a hard reset, after boot
was never seen again.

A hard reset is the thing to avoid on this board. It removes VBUS for a second
or more; VBUS is the MCU's only supply, and once the MCU is down Q13 lets go of
the Q2 power switch (its gate has no pull-down, see *MCU, clock and power
hold*), so the radio **switches itself off** until SW8 is pressed. The charger
used for qualification also hard-resets on its own after about 3.5 s of a
silent sink, and a compliant source stops advertising altogether after
nCapsCount -- so with the radio off and the charger already plugged in, the
time between plugging in and pressing SW8 decided whether v8.01 negotiated.

`radio_pd.c` now runs an attach supervisor from SysTick (`pd_supervise`):

- **Sense.** Read `TYPEC_VSTATE_CCx` with the PHY off, assert CCENABLE for
  2 ms, read again, put it back. A Type-C source debounces a CC change for
  tCCDebounce (100–200 ms) before it re-evaluates the attachment, so a
  two-millisecond pull is invisible to it and cannot cost VBUS. It reads a
  source one current step low (2.55 kΩ of Rd turns a 3 A Rp into the 1.5 A
  band). Boot does it in place inside `RADIO_PdInit`; afterwards it is split
  over two ticks. It runs as soon as VBUS appears (a plug, or VBUS back after
  a hard reset) and every second while detached with VBUS present.
- **Attach.** A source on CC with VBUS at or above 4.4 V attaches in the
  sensed orientation with the internal pull off again: fitted R3/R4 remain the
  only sustained Rd. The DMA and UCPD BMC pin are configured directly and CAD
  stays disabled, because its Type-C detector freezes with CCENABLE=0.
- **Resync.** A source that already holds a contract sends no capabilities of
  its own, and ST's policy engine honours a DPM Soft_Reset request only in
  SNK_READY (in SNK_WAIT_FOR_CAPABILITIES it holds the request and
  hard-resets instead). `pd_send_soft_reset` therefore transmits the
  Soft_Reset control message itself through ST's PHY -- header 0x008D:
  MessageID 0, sink/UFP, revision 3.0 -- with the transmit-complete callback
  borrowed for that one frame so the protocol layer never sees a transmission
  it did not start. The source answers Accept and fresh Source_Capabilities,
  which the waiting policy engine handles as a normal attach. It goes at once
  when VBUS is above vSafe5V and after 150 ms otherwise (a fresh source
  repeats its capabilities every 100–200 ms). `pd` counts them as `resyncs`.
  Measured on a true cold start with the charger plugged in 30 s before SW8
  (the case that used to switch the radio off): the charger had stopped
  advertising, answered the Soft_Reset in about 100 ms and reached 9 V two
  seconds after power-on.
- **Never the hard reset.** No capabilities by 450 ms and the supervisor
  detaches, 50 ms ahead of the policy engine's hard reset, and retries after
  1, 2, 5, 10, 30 and then every 60 s (`why=silent`, "SILENT: REPLUG"). A
  source that ignores Soft_Reset and advertises nothing -- one that gave up
  before SW8 was pressed -- therefore leaves the radio running at 5 V instead
  of switched off; replugging it starts it again.
- **Loss.** VBUS below 3.67 V for 20 ms detaches and re-arms the sense.
- **Bench back-feed.** With a debugger feeding 3.3 V the MCU survives VBUS
  loss, and the debugger back-feeds through the closed Q2 onto VBUS (about
  1.9 V). A source will not re-apply VBUS until it sees vSafe0V, so after a
  hard reset or an unplug it never came back. When the MCU is running with VIN
  below 3 V the supervisor opens Q2 (PB10 low) for 1.5 s at a time and closes
  it for 0.3 s to look -- never while powering off. On a board without a
  debugger the MCU is simply down by then.

`pd fix` (and the PD FIX menu row) skips the back-off and looks now. With
`PD INTERNAL RD` ON the CAD owns attachment as ST designed; only the Soft_Reset
resync is added.

**Collision avoidance.** PD 3.0 lets a sink start a message only while the
source's Rp reads SinkTxOK (3 A). `USBPD_HW_IF_IsResistor_SinkTxOk` read the
Type-C detector, which CCENABLE=0 leaves frozen at the boot sense's 1.5 A
reading, so every sink-initiated message came back USBPD_BUSY: a new PD TARGET
after boot, the 5 V request before power-off, PPS renewal, and the
Get_Source_Cap_Extended identity request. With external Rd the source's Rp
cannot be observed, so the check now lets the message go, as a PD 2.0 sink
does, and GoodCRC and the retry counters settle a collision.

**Interrupt priority.** ST's protocol layer sends GoodCRC from the UCPD
interrupt, and the partner expects it within tReceive (0.9–1.1 ms). UCPD1 is
at priority 1, level with the RF/audio DMA and USB, so it no longer waits
behind a PendSV receive-DSP pass; the policy engine and PD timers stay on
SysTick (3).

**The PD event log.** 64 entries in `.noinit` -- it survives a reset, not a
power loss -- with a millisecond tick and the LPTIM microsecond counter:
ST's policy-engine state changes and the header of every message in and out
(through `USBPD_PE_SetTrace`), the supervisor's sense, attach, detach and
Soft_Reset, hard resets, VBUS band changes, Q2 hold changes, and a boot marker
carrying the reset flags. `pd log` prints it raw, oldest first;
`tools/rev1_diag.py pdlog` decodes it.

`pd raw` prints CCENABLE, ANAMODE, PHYRXEN, the Type-C detector disables, both
`TYPEC_VSTATE_CCx` fields decoded, the raw CR/CFG1/SR/IMR, the dead-battery
bit, the last sense pair with the supervisor's verdict, and what ST's core
allocated out of `pd_arena`.

Three other causes look nothing like that and `pd` covers them in the same
pass — it prints `pending`, the Rp the source advertised at attach, the
requested voltage and current, the failure and hard-reset counts, the last
policy event, and the live `PD ENABLE` / `PD TARGET MV` / `PD CURRENT MA`:

- **A DC supply on the barrel jack masks VBUS.** ADC_VIN is the *combined*
  rail, so with 9 V on the jack VBUS cannot be read: the supervisor still
  attaches when the sense finds a source on CC, but a USB-C unplug is not seen
  until the jack is removed too, and with no source on CC `pd` reports
  `supply=dc-jack`. It prints `vin_mv` and `vsafe5` for this reason. A USB-C
  test belongs on USB-C alone.
- A charger whose ladder has no 9 V step — 5/15/20 V is common on laptop
  supplies — cannot reach the target at all. `PD MAX MV` covers the case where
  it offers something usable above it; `pd` lists every PDO either way and says
  `why=no-offer` when nothing fits.
- `PD ENABLE` off or `PD TARGET MV` not 9000 would do it too; `why=disabled`
  and the printed target say so.

Do not turn `PD INTERNAL RD` ON while R3/R4 are fitted. It puts
5.1 kΩ ∥ 5.1 kΩ = 2.55 kΩ on CC, outside the Type-C Rd range. On the tested
source this made VBUS collapse and remain latched off until the cable was
physically disconnected and reconnected. The one-millisecond orientation
sense is safe because it is far shorter than tCCDebounce; production never
holds the parallel pull.

The permanent options, neither applied blind:

- **Remove R3/R4 and set `PD INTERNAL RD` ON.** Exactly 5.1 kΩ from the MCU
  and correct PD, but U18 can no longer cold-start the board: with the MCU
  unpowered there is no Rd at all, so a USB-C source never applies VBUS.
- **REV2: route UCPD1_DBCC1/DBCC2 to CC1/CC2** and free PA9/PA10, which is
  what RM0440 §46.4.6 requires for dead-battery Rd and what makes the internal
  Rd usable without losing cold start.

The sink advertises a conservative fixed 5 V capability and accepts valid
SPR fixed and PPS source PDOs. It records every received PDO (up to seven),
PDO type, current/power limits, active CC orientation, PD revision, contract,
request/retry state, and optional Source Capabilities Extended/PPS Status
identity fields. Fixed and PPS operating requests are quantized to their
protocol units, are limited to 5.0--14.0 V and 3.0 A, and are only considered
stable after ST reports explicit contract, `POWER_SNK_READY`, or
`STATE_SNK_READY`. Battery/variable, AVS and EPR objects remain
visible as unsupported/informational offers and are never selected.

PD requests are enabled by default with a 9 V target and a 12 V ceiling
(`PD MAX MV`). A USB-C power-up therefore starts at the mandatory 5 V, boots
the MCU and moves to the target as soon as the source advertises it. Selection
prefers the highest offer at or below the target; when nothing above 5 V
reaches it, the **lowest** offer above the target within the ceiling is taken
instead, because every excess volt is burnt in the LM1084-5. 15 V and 20 V stay
unreachable — both are above the 14 V policy cap and the 14.7 V VIN trip — so
against a 5/15/20 V charger the radio stays at 5 V and reports `why=no-offer`.

**What the radio reports about its own supply.** `RADIO_PdSupply` classifies
the input from the last CC sense and the rail: `usb-pd` (source on CC with a
contract), `usb-c` (source on CC, no contract), `usb-5v` (VBUS but no source on
CC — a charge-only cable), `dc-jack` (no source on CC and VIN above 5.5 V,
which no USB-C source can produce without a contract), or `unknown`. This is
what `id` prints as `power=` and what the boot splash shows as `SUPPLY`; it
replaced a plain `attached` flag that read `dc` on USB-C whenever the CC PHY
was down. `RADIO_PdWhyCode` gives the matching reason — `ok`, `below-target`,
`disabled`, `dc`, `no-cc`, `no-supply`, `silent`, `probing`, `brownout`,
`no-attach`, `no-caps`, `no-offer`, `negotiating` or `rejected` — and the POWER SUPPLY INFO page, the
`SUPPLY` self test and `pd` all render the same one sentence for it.

Three refusals used to stop the sink for the rest of the power cycle. They now
re-arm once every 30 s, up to eight times, because a source can start offering
the target later once another load releases. With PD enabled and a source attached, RF TX is inhibited
until the source has advertised capabilities and acknowledged the requested
RDO. With no source attached (DC input), the PD policy does not block TX.
Rejections, waits, malformed objects, stale PS_RDY, cable detach and a source
capability change clear the pending contract; a change during TX trips VIN
protection. PPS contracts are renewed every seven seconds. During shutdown the
SysTick-owned PD state machine requests mandatory 5 V before PB10 is released
when the one-second window permits (before v8.02 the frozen SinkTxOk check
made this request, the PPS renewal and any new target come back BUSY). The configured termination is kept while
stopping.

The LM1084-5 needs about 6.3 V in, so at the 5 V start-up contract the 5 V
rail sits near 3.7 V. A 5 V rail below 4.4 V (debounced 4 ms) inhibits TX
without latching protection, so receive and setup keep working and TX becomes
available once PD raises VIN. A rail above 5.5 V, or one that sags while RF is
active, trips `FAULT_5V`.

VIN is the combined USB/DC rail. Because the PCB has no VBUS switch or source
current monitor, external DC can mask VBUS removal and software cannot force a
physical detach. The 14 V limit is enforced in PDO selection, VIN protection
is set to 14.7 V for tolerance, and the LM1084/UZ1085 power-tree current,
thermal and transient limits require measurement on the assembled board.

## Front-panel controls

The rotary encoder is the primary control, QMX-style; the five buttons on
PD2..PD6 are secondary and everything works from them alone.

| Input | Pin | Role |
|---|---|---|
| Encoder knob and push | spare jumper pins, default PC11/PC12/PC13 | Tune, the home zones, menus (below) |
| FUNC key | spare jumper pin, default PC14 (JP20-7) | The second key on the encoder board: BACK (below) |
| SW_UP / SW_SELECT / SW_DOWN | PD4 / PD3 / PD2 | Home: band up/down, cycle mode, hold SELECT = menu. Lists: move, select, hold = back |
| SW_4 | PD5 | `SW4 ACTION`, default **PTT** |
| SW_5 | PD6 | `SW5 ACTION`, default **POWER** (3 s hold) |
| SW12 | PB8 / BOOT0 | ROM bootloader. Never claimed by firmware; the pin stays analog |
| SW13 | PG10 / NRST | Reset. Never claimed by firmware |

**Knob gestures.** Clockwise is always *next* or *more*.

The home screen is three zones stacked top to bottom: the frequency (with the
meter), a sliding row of settings, and a diagnostics summary.

| Where | Turn | Hold + turn | Click | Hold (0.6 s) |
|---|---|---|---|---|
| Home, tuning | tune by the step under the underline, faster on a quick spin | move the step, 1 Hz..1 MHz | next step: 10 Hz, 100 Hz, 1 kHz, 10 kHz | zone picker |
| Zone picker | move the bracket: frequency, settings, diagnostics | - | enter that zone | menu |
| Settings row | slide along the row | - | edit the highlighted setting (SAVE runs) | back to the picker |
| Editing in the row | change (volume, mic, drive, WPM, backlight apply as you turn; mode and band on click) | x10 | set | cancel, restoring the old value |
| Diagnostics | next / previous page | - | back to tuning | back to tuning |
| WATERFALL | tune, the view following | span (in SCAN: the scan range) | settings list | back |
| WATERFALL settings | move (editing: change) | - | edit / done (DEFAULTS resets) | close the list |
| Menu | move | jump to the next section | open / edit | back |
| Editing a setting | change (volume, mic, drive, WPM, backlight apply as you turn) | x10 | set | cancel, restoring the old value |

The picker and the settings row fall back to tuning after 5 s and 10 s left
alone; a live value stays where it was turned to, anything else is dropped.

**FUNC key** (QMX's "Exit") is BACK everywhere: cancel an edit, leave the
settings row or the picker for tuning, up one menu level, leave a page; held,
it goes straight home to tuning. On the tuning screen there is nothing to go
back from and a press does nothing. FUNC + turn is the same as hold-and-turn
away from home (next section, digit) and steps the band on the tuning screen.
On the SD pages FUNC never confirms: a
delete, format or firmware confirmation is cancelled by it. It uses the
encoder's pull and polarity settings (`ENCODER TO 3V3`, `ENCODER EXT PULL`);
`FUNC KEY PIN` (BOARD › CONTROLS or DISPLAY) moves it or turns it OFF, and a
pin the encoder uses is ignored. The pin choice lives in `boot_flags` bits 2..5,
stored XOR the PC14 index so that an untouched field means PC14.

The settings row holds, most used first, VOLUME, WATERFALL (the only way to
the WATERFALL page; its value is the view the page opens on), MODE, BAND,
POWER (CW SPEED
first in CW), MIC GAIN (USB GAIN when the TX input is USB), TX WIDTH and
COMPRESS (SSB), KEYER and BREAK-IN (CW), TX INPUT, TX MUTE, STEP, BACKLIGHT and
SETTINGS SAVE; four are on screen and arrows show there are more. The
diagnostics zone shows supply, power source, carrier, audio, transmit readiness
and faults in words; entering it opens DIAGNOSTICS, five pages (receive,
transmit, power, audio, system) of live values refreshed four times a second,
which is also where SYSTEM › DIAGNOSTICS (formerly ABOUT) and BOARD › LIVE
BOARD SIGNALS lead. MODE
is one control for the transmitter, the SSB sideband and the receiver
(USB/LSB/CW/AM/FM); the separate `RECEIVE MODE` and `SSB SIDEBAND` settings
remain in the menu for bench work, and the status bar warns `RX xxx` when the
receiver has been set apart. A retune follows the knob at most every 100 ms and
once more 60 ms after it stops, and only when the audio path allows background
work; until then the step underline turns amber.

**WATERFALL.** The second cell of the settings row opens a spectrum and
waterfall page (`ui_waterfall.c` over the engine in `radio_spectrum.c`).
Nothing of it runs while the page is closed: the receive worker then tests
one flag per half. Open, the worker copies a 512-sample I/Q block into the
receiver arena on request and the main loop windows it, runs a 512-point FFT
and folds it into a pixel row, only while receiving; the history, trace and
peak live in the framebuffer rows the colour band covers, so the page adds
no static RAM. The default view is the waterfall alone over the widest span
the ADC passes flat (the PCM1821's +-0.3 fS, about 105 kHz around the LO,
one 346 Hz bin a pixel) with a frequency scale along the bottom; the dial is
a dotted red line and, beside a spectrum, the receive channel is shaded.
The settings list (click) sets MODE (VIEW, or the experimental SCAN), SPAN
(WIDE, 38, 19 or 9.5 kHz around the dial from the 48 kHz channel through
half-band stages), LAYOUT (WATERFALL, SPLIT, SPECTRUM), SPEED (2-25
lines/s), AVERAGE (1-16 FFTs), WINDOW, DETECTOR (average or peak), SCALE
(AUTO tracks the noise floor, FIXED takes REF LEVEL), FLOOR, RANGE (30-90 dB),
PALETTE, FILL, SMOOTH, PEAK HOLD, MARKERS, DC NOTCH, IQ CORRECT, SCAN RANGE
(the band the dial is in, 250 kHz-2 MHz, or the whole filter slot), SCAN
AUDIO (sweep then LISTEN at the dial, CONTINUOUS, or SINGLE on SEL), LISTEN
and SCAN AVG. The settings are kept in RAM and return to their defaults at
every boot. SCAN hops the receive LO (an HRTIM period, no PLL change) across
the range with the receiver muted, pastes each capture into a panorama line,
and restores the dial; a retune, a receiver restart or a key-down ends a
sweep. TX: the engine rests outside RECEIVE, so a transmission always has
the whole core, and `SPEC_TxAllowed` holds TX for 2 s with the page open
whenever a receive deadline is missed or the receive worker and the
waterfall's own share (its main-loop time less the worker's preemption of
it) together reach 95 % of the core (the footer then reads TX HELD).
`tools/ui_preview.py` renders the page against a simulated 20 m band.

`SW4 ACTION` and `SW5 ACTION` offer UNASSIGNED, PTT, MODE, POWER, BAND UP,
BAND DOWN, MENU and TX MUTE. PTT is handled in `RADIO_Service`; mode, band,
menu and mute are latched and run by `UI_AppHandle`, so the UI keeps its own
retune and audio-busy guards. `MODE` (stored as the former RX MODE value)
cycles the unified mode.

**Rotary encoder.** Three spare jumper pins can carry a plain mechanical
encoder: A and B are the quadrature pair and the shaft switch is the knob
press. The defaults are JP20 pins 4/5/6 = **PC11 (A), PC12 (B), PC13 (OK)**,
which is one `GPIOC->IDR` read for all three.

- Selectable pins are exactly those `BOARD_GPIOInit` parks as analog and that
  reach a header: PB0, PB15 (JP20 2–3), PC11..PC15 (JP20 4–8), PD7 (JP19-8),
  PE15, PE8, PE7 (JP7 1–3) and PF10, PF9, PF2, PF1 (JP7 4–7). **PA10 (JP20-1)
  is excluded on purpose**: it is UCPD1_DBCC2 and must stay un-pulled. PD11 and
  PD13 are not connected, and PD14/PD15 are shorted together by R148.
- The common goes to **GND** and the firmware supplies the pull-up, which is
  the KY-040 and bare-EC11 convention. `ENCODER TO 3V3` inverts that,
  `ENCODER EXT PULL` disables the internal pull, and `ENCODER REVERSE` swaps
  the direction. Unlike PD2..PD6 there is nothing to learn: these pins have a
  known pull, so the button polarity probe is not used on them.
- `ENCODER DETENT` is the quadrature edges per reported step; 4 suits a normal
  one-detent-per-cycle encoder. A partial detent is carried, not discarded.
- Sampling runs at 1 kHz from `SysTick_Handler`, next to `RADIO_PdTick`. The
  main loop can sit inside its RF-critical branch for many milliseconds, which
  is long enough to miss edges, so polling it there would drop counts.
- A detent turned while the shaft is pressed is counted apart
  (`BOARD_EncoderReadEvents`, `held_steps`), and that press is then neither a
  click nor a long press. The long press fires only on a still shaft.
- The UI reads signed detent counts once per pass and accelerates tuning and
  numeric edits on a quick spin; the SD pages take one detent per pass so a
  spun knob cannot queue a burst of card reads.
- Setting any two lines to the same pin is rejected by `RADIO_ConfigValid`, so
  the menu simply keeps the previous pin.

**Knob direction.** The sampler counts the Gray sequence 00-01-11-10 as +1;
with the encoder board on its default pins that is anticlockwise, and the UI
takes clockwise as next / more / tune up. `ENCODER REVERSE` swaps it for an
encoder wired the other way.

**Display.** The ST7789 is driven in landscape, 320 x 240. `SCREEN ROTATION`
(first row of `DISPLAY`) chooses 90 or 270 degrees for the way the panel sits
in the case; it is stored in `boot_flags` bit 1 of the REV1 configuration, so
a factory reset of the settings keeps it and firmware before v8.00 ignores it.
The boot log turns to it as soon as the settings are loaded. The panel runs
without display inversion and in RGB order; v7.x sent INVON with the BGR bit,
which on this panel showed every colour inverted with red and blue swapped, so
its black background appeared white.

## Setup gating: an inert first boot

The MCU cannot read the driver, drain or filter assembly, so a board that has
not been described does nothing at all. `RADIO_ConfigRxBlock` gates **receive**
as well as transmit:

- Until `ASSEMBLY CONFIRM` is set, `RADIO_Service` holds RX_EN low, runs no QSD
  LO, no TEST_CLK, no bias buck and no envelope DAC, and `RADIO_CalStart`
  refuses every bench routine. The status line reads `SETUP REQUIRED`.
- The UI opens straight on MENU → BOARD → ASSEMBLY instead of the home screen.
- Transmit then adds its own conditions, in `RADIO_ConfigTxBlock` order:
  `DRIVER/DRAIN UNSET`, `DRAIN A/B DIFFER`, `BIAS OVER LIMIT`,
  `ENVELOPE VS VIN DRAIN`, `ENVELOPE UNCALIBRATED`, `NO FILTER`,
  `LPF UNTESTED`, `BELOW LPF TX MIN`.

**`DRAIN A/B DIFFER`** is new. The two drain jumpers are physically
independent, so a mis-entry could describe branch A fed from the envelope
follower and branch B straight from VIN. The follower sits well below VIN even
at full DAC, so the two Chireix branches would run at different drain voltages
and the outphasing null — and the IMD with it — would collapse. Both jumpers
must be in the same position.

**The jumpers pick `DRAIN MODULATION`.** Setting both drain jumpers to ENV
selects ENVELOPE: both branches are driven in phase and the amplitude comes
from the two envelope shapers alone, with no outphasing (`RADIO_EnvelopeFill`
collapses the branch pair to its common phase and turns the phase difference
into drain level). Setting either jumper to VIN selects OUTPHASING, the only
mode a fixed drain can run. HYBRID is kept while both stay on ENV. Only a
change of jumper does this: `DRAIN MODULATION` can still be set by hand
afterwards, and loaded settings keep what was saved. In ENVELOPE mode the TX
carrier test's level is cos(π × `TEST THETA`) of the drain full scale
(`DRAIN LIMIT MV`, capped at 95 % of VIN), so `DRAIN LIMIT MV` is the way to a
low-power carrier.

## Settings layout

The menu opens on nine categories named for what an operator looks for, with
the highlighted category's contents previewed beside it; inside, a category is
one full-width list in which related groups appear as titled sections, and the
panel under the list explains the selected row (or, while editing, shows the
range or the neighbouring choices):

| Category | Holds |
|---|---|
| `OPERATE` | mode, band, tuning step, headphone volume, TX mute, receive mode, SSB sideband, clock policy, frequency entry, A/B swap |
| `AUDIO` | headphone volume, TX audio input, mic gain, USB raw I/Q, USB input |
| `CW` | keyer, key safety, Morse trainer |
| `VOICE` | SSB, voice processing |
| `AM / FM` | FM, AM |
| `TX / RF` | RF output, SSB calibration, TX carrier test |
| `DISPLAY` | screen rotation, home layout and lines, hold-button actions, backlight, knob reverse and detent |
| `BOARD` | the seven commissioning pages below |
| `SYSTEM` | SD card, save, load, factory reset, power off, about |

The diagnostic image adds `DIAG` (test signals, runtime counters, DSP engine
and the debug group) when its debug UI is on.

The board block is a commissioning tool, not a set of operating controls. It
used to be a single 65-row scroll with no menu entry at all; `BOARD` opens
seven pages, in the order the bench procedure uses them:

| Page | Holds |
|---|---|
| `ASSEMBLY` | driver A/B, drain A/B, drain modulation, LPF tested mask, `ASSEMBLY CONFIRM`, save |
| `CONTROLS` | button polarity, SW4/SW5 actions, encoder pins and options, backlight, volume |
| `FILTERS` | LPF edges and TX floors, `QMX+ LPF EDGES`, BPF edges, `QMX 60-15M BPF EDGES` |
| `BIAS + PROTECT` | `PIN BIAS MA`, bias/VIN/envelope limits, envelope DAC scale, SWR limit and detector trim |
| `RX TRIM` | receive mode, microphone source and gain, USB I/Q, I/Q gain, phase trim, invert |
| `POWER / PD` | PD enable/target/`PD MAX MV`/current, `PD INTERNAL RD`, TX aux mask, `PD INFO`, power off |
| `CALIBRATE` | monitor page, bias/envelope/RX tests, BPF sweep and apply, clear fault, save |

`BACK` (hold) from a page returns to that list. Two host suites guard the
layout: `ui_settings_policy_test` that every `setting_id_t` is in some group,
and `ui_menu_model_test` that every one is reachable through the categories.

## Storage, timing and qualification

Settings and typed calibration records are migrated to the REV1 format; old
CEU6 calibration is retained only as historical data and is never auto-applied.

Since v8.09 the settings and calibration live only on the SD card, in the
store of `mod_sd_store.c`: raw sectors 256-767 between the MBR and the first
partition, two copies of every record (A at 256-511, B at 512-767), each
record with a sequence number and a CRC-32, each copy headed by the MCU's UID
(README, *Storage And Calibration*). A save goes to the copy without the
newest version and is read back; a power cut leaves the previous version.
Nothing of it is a file, USB mass storage refuses writes there, and the
in-radio format starts its volume at sector 2048. The MCU's flash holds no
settings: flash layout 2 (loader ABI 2) gives the application everything up
to the image footer at `0x0807F7E0`, so no flash write runs outside a
firmware install any more.

**Boot and the card.** With no store this radio can use, boot stops before RF
DMA and the watchdog with the USB console up, and the LCD says why and what a
click does:

| Card | Click | Console |
|---|---|---|
| none, unreadable | look again (also every second) | `storage nocard` |
| blank, `SYSTEM/NVM808.BIN` on it | import v8.08's settings (also by itself) | `storage import nvm` |
| blank, v8.08's `M32CFG.BIN`/`M32CAL.BIN` | import them | `storage import` |
| blank | new store from the running settings | `storage new` |
| another radio's (UID) | use its settings and calibration | `storage adopt` (`storage new wipe` replaces it) |
| both headers lost | repair, keeping the records | `storage adopt` |
| partition over sectors 256-767, GPT, no MBR | -- (format it in the radio) | |
| an import or restore cut short | run it again, or keep the store if its source is gone | `storage import`, `storage restore`, `storage keep` |
| this radio's, settings record will not read | read again (also every second); nothing is saved meanwhile | |

Holding the knob or SELECT runs without a card until the next boot (NO SD
CARD - SETTINGS NOT SAVED; nothing is saved). With a store, a band whose
calibration records cannot be read **locks TX** until they read (a
`RADIO_ConfigTxBlock` reason, `tx_block=CAL NOT LOADED`; no unmute gets past
it): a card lost while the radio runs never makes it transmit on values it
did not mean. The read a band change makes during its own retune, or with
a USB audio stream open, waits quietly and goes once TX is locked, the
transmitter down and the retune done; `CAL NOT LOADED - TX LOCKED` is shown
for a read that failed or one still waiting after 3 s. To transmit
uncalibrated anyway, run without the card: the boot hold, `storage nocard`,
or, with the card lost while running, SETTINGS STORE in the SD menu clicked
twice. What the radio does for each kind of card trouble is tabled in the
README ("The card in trouble while the radio runs").

**Console.** `storage` shows the store's state, origin, counters (reads,
writes, copies that did not verify, bad copies, repairs, mirrors, I/O
errors), the header copies, the last import and the backup job; `storage
list` each slot's two copies and its record; `sd stats` the driver's CRC,
timeout, retry, re-initialisation and abort counts, the card's last refusal
(`reject=` stage:bytes@block) and the writes failed in a row. A read the I/O
gate or a timeout cut short is an error, never the older of the two copies,
and the scrub leaves that slot for its next pass. `help <command>` prints
that command's line. `LOGS/EVENTS.LOG` on the card records boots, trips, pipeline events, crash records, store events and SD
re-initialisations. The settings help (and, since v8.09, most console help
and usage lines, the menu's category descriptions and the sweep export
headers; since the cw-trainer work also the menu's page, action and group
help, the setting help index, the Morse trainer's instructions and the
self test's row details) is `HELP.BIN` on the card; the Morse trainer's
courses and words are `CWPACK.BIN` (docs/CW_TRAINER.md). Flash with
`tools/flash_rev1.py`, which also puts on the card what the image needs.
With no card the self test still reports every verdict, value and limit;
its LCD page shows the selected row's detail under the list.
The HRTIM period uses the STM32 high-resolution definition `PER = fHRCK / fPWM`
(no ARR-style minus one), with a legal 96-tick half-cycle floor. RF DMA,
USB, UCPD, ADC and PCM ownership is checked by the build-time ELF policy and
the production host suites. At v8.02 the debug image uses 440,676 bytes of
flash, 89,440 bytes of SRAM and 28,504 bytes of CCM SRAM (Debug linked image,
validation policy view); the Diagnostic image uses 432,872 bytes of flash
against the 470,000-byte policy budget. The nested stack bound is 5,652 bytes
against a 6,144-byte linker reserve.

**Protection summary.** VIN above the limit trips. 5 V rail above 5.5 V trips;
below 4.4 V it only inhibits TX, unless RF is active. Bias over the limit for
2 ms trips while switching. So does TX_BIAS below half of duty × VIN for 5 ms
(performance envelope). During TX, bias more than 100 mV low for 10 ms also
trips.
SWR: a reflected-detector clip or |Γ| above the limit for 2 ms trips. A
clipped forward detector at the 2.9 V ADC full scale only overstates |Γ|.
Envelope above its limit + 0.5 V trips. ADC/DMA/HRTIM/CSS faults latch
immediately.

Scan-level trips (`radio_guard_logic.h`, host-tested; a few compares in the
4 kHz ADC interrupt, 4 bytes of CCM):
- **VIN collapsing while TRANSMIT** under max(6.3 V, 70 % of VIN with TX off)
  for 2 ms trips VIN. The PD contract normally sags 9 → 7-8 V at full drain
  (bench: 8.80 → 8.21 V at 3.3 W on 40 m); REV1 has no current sense, so this
  is also the overcurrent trip, and below 6.3 V the LM1084-5 lets the 5 V
  rail go.
- **|Γ| jumping within an over**: learnt from the first 16 ms of forward RF
  over 0.5 W, then 0.25 above it for 2 ms of such scans trips SWR (a broken
  wire, an arc, a connector), below the limit. Not in TX TEST, where a
  manual tuner is turned with the carrier up.
- **RF with TX not permitted**: 0.1 W forward for 2 ms trips SWR and takes the
  receiver off -- a driver that did not stop, or a transmitter close by.
  Judged only in RECEIVE with an LPF selected, no calibration running, and
  100 ms after RF, a filter switch or any other state: with no LPF path
  conducting, R74/R77 lift the PIN bias network and both detectors sit near
  0.44 V DC (SETUP, PROTECTION -- the first version tripped at boot on it).
  The meter's rest level is no longer learnt in those states either.

When the CPU cannot act:
- **A halted core** (debugger, breakpoint, hot-plug flash during TX): the
  HRTIM is frozen on a halt (`DBG_HRTIM1_STOP`) and the four PA-drive outputs
  are fault-protected at the inactive level, so they drop at once and stay
  down until the next TX entry sets them again (RM0440 28.3.25). The IWDG
  cannot do this: debuggers freeze it. The RX LO outputs only pause.
- **Core LOCKUP or a flash double ECC error**: routed to the HRTIM system
  fault (`SYSCFG_CFGR2` CLL, ECCL), which drops the same outputs in hardware
  before any handler runs; CSS does so always. Every fault handler then runs
  `BOARD_EmergencyStop` and resets.
- **USB gone mid-over**: CAT PTT ends when its port closes (DTR), on USB
  suspend or reset (the port is no longer configured), or after the CAT
  timeout (120 s, QMX's); a USB stream reset or an empty render queue gives
  null frames (drive gated off) within one DMA half.

Software tests cannot qualify TXCO startup margin, HRTIM edge timing, driver
polarity, RF waveform/IMD, analog rail behavior, USB enumeration, PD
compliance or the PCM1821's actual passband. Those checks require the assembled
REV1 board, an oscilloscope/spectrum analyzer, load and PD protocol analyzer.

## PCB findings and their status

From the REV1 KiCad netlist (`v1sch.kicad_pcb`, BOM `v1sch.csv`), with the
rework reported on 2026-09-16:

1. **Resolved: R32/R145 pull-ups moved from VIN to 5 V.** Disabled-state
   dissipation is now 53 mW at a regulated 5 V rail (was 0.31–0.42 W). The
   ACT541 OE inputs no longer exceed VCC + 0.5 V. At the USB-C 5 V start-up
   contract the rail is about 3.7 V, still above the UCC27524 EN and 74ACT OE
   high thresholds.
2. **Resolved: R92 is not fitted.** R93 ties the button common to GND (active low).
3. U5/U7 and U6/U8 share the gate nets through 10 Ω resistors; populate only
   one driver type per branch (UCC27524 is fitted).
4. **Headphone PWM filtering.** No RC reconstruction filter exists between
   PA0/PA1 and U14. Firmware keeps PWM harmonics away from the operating
   frequency. A shunt capacitor at R64/R65 (for example 4.7 nF) or across
   R56/R62 would further reduce ultrasonic output if the carrier is audible or
   radiated.
5. **R148 (0 Ω) connects PD14 and PD15.** Both remain analog. Never configure
   both as outputs.
6. **USB-C CC termination.** R3/R4 stay fitted for cold start. The one-ms boot
   sense finds orientation, then direct BMC startup negotiates while internal
   Rd remains off; `pd raw` prints the `sense_off` / `sense_on` pair and the
   verdict. The connected REV1 board completed a 9 V contract on CC2. PA10
   (JP20 pin 1) is also UCPD1_DBCC2 and floats; while the MCU is
   unpowered, a 10–100 kΩ pull-down there keeps the dead-battery Rd inactive,
   as ST requires when dead-battery Rd is not wired. PA9 (DBCC1) is held low
   by the UCC27524 input pull-down.
7. **Envelope OPAMPs.** OPAMP3/OPAMP6 drive C88/C92 (1 nF) directly, as in the
   QRP Labs design this follows. Note that QMX drives it from a DAC output
   buffer rather than a G4 internal op-amp follower, so glance at PB1/PB11 for
   ringing during envelope calibration.
8. **Bias switch rating and ADC range.** Q15 BSS84 is rated for 130 mA
   continuous and 520 mA pulsed; 45 mA per diode draws about 85 mA from the
   buck. The duty caps, the performance-envelope trip and the over-limit trip
   bound the output. ADC_BIAS reads up to the 2.9 V VREFBUF full scale (limit
   setting ≤ 2.85 V), which caps `PIN BIAS MA` at 45 mA. The LPF BSS123s are
   driven from 3.3 V as on QMX+. Check the drop across R75/R76 during
   `BIAS TEST 5S` (≈ 0.86 V each at 30 mA).
9. **`alt2.ioc` is a pin map, not a project file.** Its `Mcu.IP*` list names
   only DMA, HRTIM1, NVIC, RCC, SPI1, SPI4, I2S3, SYS, TIM2, TIM4, TIM6, TIM17,
   USB and USB_DEVICE. The firmware also configures ADC1/2/3, DAC3, OPAMP3/6,
   UCPD1, TIM3, TIM7, CORDIC, CRS, IWDG, DMA2 and DMAMUX1 by hand. The pin
   table matches the CSV line for line and is worth keeping in step;
   **regenerating code from CubeMX would delete the entire analog, PD and
   telemetry bring-up.**
10. **PCM1821 straps.** The BOM lists both pull-down and pull-up for FMT0
   (R52/R53), MD0 (R49/R50) and MD1 (R47/R48). Fit only R52, R49 and R47
   (with R51 on MSZ). Leave R53, R50 and R48 open.
