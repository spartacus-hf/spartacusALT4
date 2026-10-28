# REV1 bring-up, self test and diagnostic console

How to power a newly assembled SPARTACUS REV1 for the first time, what the
built-in self test checks, how to reach the USB diagnostic console, and the
rules for calling a board qualified. Pins, nets and the reasoning behind
each limit are in [`REV1.md`](REV1.md). Installing and recovering the firmware
is covered in [`USB_FIRMWARE_UPDATE.md`](USB_FIRMWARE_UPDATE.md).

Transmit tests go **only into a 50 Ω dummy load**, on bands and powers the
operator's licence covers.

## First power-up order

1. Before power, check R93 fitted/R92 open, R32/R145 to 5 V, only U5/U6
   fitted, and PCM1821 straps R51, R52, R49 and R47 fitted with R53, R50 and
   R48 **open**.
2. Current-limited bench supply at about 9 V on VIN, no USB-C. Check that the
   PB10 latch holds after SW8 is released, PF0 has the 8 MHz clock, and VREF+
   reads about 2.9 V.
3. The LCD shows the boot log. Scope with PA8/PA9/PC8/PC9 low and PC6/PC10
   high: gate nets Q3–Q6 low, UCC27524 OUTx low. Press any button once and
   confirm that the menus respond.
4. Complete setup (drivers, drains; the LPF and BPF edges default to the
   fitted QMX+ and QMX 60–15 m sets). Check LPF one-hot outputs,
   the ~0.8 V ADC_BIAS level with an LPF selected, and PB12/PB13 quadrature
   (PB13 leading) at the receive frequency. The monitor page must show
   `RX 177083HZ ... FB 0`. Tune a known USB signal: intelligible in USB and
   the S-meter moves (set `RX IQ INVERT` only if it needs LSB). With a signal
   generator, measure the image 2×IF away on 40 m and 20 m and trim
   `RX I GAIN`, `RX Q GAIN` and `RX PHASE TRIM` for the deepest null; expect
   about 60 dB. Then check headphone audio and USB capture. With an SD card
   inserted, run `BPF SWEEP ~20S` with a 50 Ω dummy load on the antenna
   connector (the working source impedance, without off-air signals). The
   four curves should peak in the order 15, 20/17, 30 and 60/40 m, and 17 m
   should sit below the slot-1 notch. `APPLY SWEEP BPF EDGES` and `SAVE` if
   the fitted edges differ, and keep `EXPORTS/BPFnnn.CSV` as the per-board
   reference.
5. USB-C only (no DC): plug in, confirm the 5 V cold start, then `PD INFO`
   attach/CC/PDO list and the automatic 9 V request with VIN tracking; then
   12/14 V requests. If it never attaches, follow the CC termination fallback
   in [`REV1.md`](REV1.md) (*USB-C PD power connector*).
6. PIN bias with the JP10/JP11 drain jumpers removed (drains unpowered). Leave
   `PIN BIAS MA` at 30 and run `BIAS TEST 5S`:
   - The monitor should show about 1.9 V, 30 MA and a duty near
     1.9 V / VIN plus losses.
   - Each of R75 and R76 should drop about 0.86 V; the current is the drop
     ÷ 33 Ω plus about 4 mA from R74/R77. If it is off by more than a few mA,
     adjust `PIN BIAS MA`.
   - Scope TX_BIAS for settling within about 80 ms and no ringing, and PD12
     for 100–130 kHz.

   Then the envelope DAC transfer, then low-power RF into a dummy load on one
   band with a spectrum analyzer. Check 2nd/3rd harmonics at the band edge and
   at that LPF's TX floor before setting its `LPF TESTED MASK` bit.
7. Power switch: press SW8 to start and release after the boot log appears.
   `POWER OFF` must remove power within a second. A second `POWER OFF` while
   holding SW8 must restart the radio after 3 s.

## Self test

`BOARD > ASSEMBLY > SELF TEST` runs 54 checks in seven groups and reports
PASS / WARN / FAIL / SKIP per row, on the LCD and to `EXPORTS/STnnn.CSV`. The
verdict rules live in `Core/Src/mod_selftest_logic.c` with no HAL
dependency; the `rev1_selftest` host suite exercises every limit against
modelled hardware.

- **It never transmits.** `tools/elf_policy_check.py` proves it from the
  linked call graph: the diagnostic roots cannot reach `service_tx_entry`,
  `RADIO_RequestTx`, `BOARD_SetAux` or `MOD_TxCarrierTestStart`.
- **The low-pass filters are checked receive-only.** With the bias buck off,
  R74/R77 push about 4 mA through whichever filter is selected and TX_BIAS
  settles near `(674.1 + 0.0121 x V5) / 1.0121` mV (710 mV at a 3.7 V rail,
  732 mV at 5.5 V); nothing selected floats to the rail. A filter's toroid,
  PIN diodes and BSS123 select are checked with no antenna, no dummy load and
  no extra energy. The conducting limit (900 mV) never fails a healthy filter,
  at the cost of reporting a single open branch as marginal rather than
  certain; a fully open filter reads the rail.
- **It runs before `ASSEMBLY CONFIRM`**, unlike every other bench routine:
  the filter, receive and analog groups are skipped with the reason, the
  supply, clock, control and storage groups still run.
- **Energised tests are opt-in.** The PIN bias run and the envelope DAC ramp
  need `ARM ANALOG TESTS` since the last run; pull the JP10/JP11 drain jumpers
  first.
- **Measurements are offered, never applied.** A row that produces a value a
  setting could take offers an apply action, as `APPLY SWEEP BPF EDGES` +
  `SAVE` does.
- The I/Q, image and audio sweeps report SKIP in an unattended run: they need
  a dummy load for valid results.

## USB diagnostic console

`BOARD > POWER / PD > USB DIAG CONSOLE` re-enumerates the radio as a CDC-ACM
port (`SPARTACUS REV1 DIAG`), a personality of its own beside USB audio and
mass storage. In the audio personality the CAT ports also become the console
after a line ending (see [`REV1.md`](REV1.md), *USB serial ports*). The host
side is `tools/rev1_diag.py` (Python standard library only).

It carries two interfaces to the same state:

- a **line protocol** for scripts: one request line in, data lines out,
  terminated by `OK` or `ERR <reason>`. `help` lists every command
  (`help <cmd>` explains one); values are integers in stated units.
- a **full-screen view** (`ui`) for a person with a terminal, fitted to
  80 × 24.

Where to start:

- `status`: what the radio is doing and why, in names: radio state, the
  receive and transmit block reasons, decoded fault bits, a latched pipeline
  and its cause, the selected filters, the USB audio counters and the self-test
  verdict.
- `events`: the last pipeline events, newest first, decoded.
- `pipeline`: the health counters, including conditions that are counted but
  do not latch a screen notice; `pipeline ack` clears a latch.
- `pins`: every named GPIO with its live mode, pull, level and alternate
  function next to its net name from [`rev1_pins.csv`](rev1_pins.csv).
- `tools/rev1_diag.py diagnose` runs `status`, `events` and `pipeline`;
  `tools/rev1_diag.py term` is a raw terminal, `paddle` the virtual paddle.

Any terminal program works (PuTTY, minicom, picocom, screen). The port speaks
the plain line protocol until the host sends a CR; from then on it echoes,
handles Backspace/DEL, Ctrl-U and Ctrl-C (which also stops a reply), prompts
with `> ` and consumes escape sequences whole. Each opening of the port (DTR)
starts a fresh session.

The console does not compete with the radio's real-time work: the endpoint
callbacks only copy one packet, the service runs in the main loop with a
bounded budget per pass, and it emits nothing while
`MOD_AudioRealtimeCritical()` is true.

**Virtual paddle** (`ui` screen 6, or `paddle`): `,` is DIT and `.` is DAH.
The levers join the KEY jack after CW SWAP, so keyer mode, speed, weight,
paddle protection, sidetone and break-in all apply. A terminal with the kitty
keyboard protocol (kitty, foot, WezTerm, ghostty, alacritty 0.13+) reports key
releases, so hold, squeeze and iambic work as on a paddle; elsewhere each
press is one element. The page opens with TX muted as sidetone practice; `t`
unmutes, and leaving the page, closing the port or the console going away
mutes again. The levers are released on focus loss.

## Qualification rules

Configuration begins unconfirmed. Driver polarity and envelope-jumper choices
must be explicit; prototype PA calibration is not evidence for this PCB.
Software checks do not qualify RF timing, audio quality, analog stability or
USB-PD electrical behavior. Record measured limits by assembly and band.

HRTIM high-resolution pulse limits follow RM0440. No overclock or unsupported
subminimum compare values are permitted. The approximate symmetric-pulse
ceiling at 170 MHz is 28.33 MHz before duty/dead-time margins.

VREF+ is capacitor-only: initialize internal 2.9 V VREFBUF and account for
ES0430 PB13 clock coupling. OPAMP3/6 use follower mode, not internal PGA.
PCM1821 requires slave/I2S/linear-phase strap population; opposite pull-up and
pull-down alternatives must not both be populated.
