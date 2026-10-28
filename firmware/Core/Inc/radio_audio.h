#ifndef RADIO_AUDIO_H
#define RADIO_AUDIO_H
#include <stdint.h>
#include "radio_iq_fit.h"
void RADIO_AudioService(void);
void RADIO_AudioWorker(void);
void RADIO_AudioSuspend(void);
void RADIO_AudioStop(void);
void RADIO_AudioStorageMode(uint8_t on);
uint16_t RADIO_CapturePacket(int16_t *stereo, uint16_t max_frames);
void RADIO_CaptureMark(uint8_t on); /* BENCH (REF_SOF_BENCH): frame markers in the capture */
void RADIO_CaptureRead(int16_t *stereo, uint32_t frames);
/* 1 while a host has the capture interface open (the ring is being filled),
 * so the console can show why the stream costs nothing when nobody listens. */
uint8_t RADIO_CaptureStreaming(void);
/* The S-CTRL PC monitor: flags bit 0 the headphone mix (receive audio,
 * sidetone, trainer prompts, SPOT; before the volume) into the USB capture
 * instead of the receiver's audio, bit 1 the radio's phones silent while it
 * runs; gain in 1/128ths. Ignored in DIGI, with USB RAW IQ and during the
 * microphone monitor. Main loop. */
void RADIO_AudioPcMonitor(uint8_t flags, uint8_t gain);
uint8_t RADIO_AudioPcMonitorFlags(void);
uint32_t RADIO_AudioOverruns(void);
/* DIGI receive health (console `digi`, the DIGI page): the host capture
 * stream as the host gets it. Counters only grow; a reader differences two
 * reads. cap_padded is silence sent while a receiver ran (a gap in the
 * recording), cap_dropped is audio lost to a full ring, cap_silent the
 * stream while no receiver ran (TX, storage, a restart). The levels are
 * ~1 s windows: the stream's rms and peak in dBFS (0 = int16 full scale),
 * and the PCM1820's raw peak, I or Q. */
typedef struct {
    uint32_t cap_sent, cap_padded, cap_dropped, cap_silent, cap_clips, adc_clips, restarts;
    float level_dbfs, peak_dbfs, adc_peak_dbfs;
    uint8_t streaming;
} radio_digi_stats_t;
void RADIO_DigiStats(radio_digi_stats_t *out);
/* Audio failures split by cause, plus the PendSV worker invocation count as a
 * control: while receiving it should advance at about 2 x fS / 64 per second.
 * Counted, never latched. Any pointer may be NULL. */
void RADIO_AudioFailCounts(uint32_t *pending, uint32_t *working, uint32_t *overrun,
                           uint32_t *dma, uint32_t *workers);
/* Receive path cost in core cycles. An I2S half is 64 / fS, about 361 us or
 * 61370 cycles at 170 MHz: rx_avg + phone_avg must sit well inside that.
 * All zero in the deep-diagnostic flavour, which has its own profiler. */
typedef struct {
    uint32_t worker_max, latency_max;
    uint32_t rx_max, rx_avg, phone_max, phone_avg;
    /* Within a receive half: the whole front-end block call, and the
     * demodulator over the outputs it produced. */
    uint32_t fe_avg, demod_avg;
} radio_audio_cycles_t;
void RADIO_AudioCycleCosts(radio_audio_cycles_t *out);
/* The cycle instrument is off unless asked for, so a shipping receiver pays
 * one flag read per DMA half rather than a DWT read. */
void    RADIO_AudioProfileEnable(uint8_t on);
uint8_t RADIO_AudioProfileEnabled(void);
uint32_t RADIO_AudioI2sFallbacks(void); /* PCM1821 rate-family retries */
/* Receive CW decoder (radio_cw_decoder.h). The receive worker only sums the
 * CW channel output into 1 ms blocks; RADIO_AudioService decodes them in the
 * main loop. Text is the last RADIO_CWDEC_TEXT characters, "" if none. */
const char *RADIO_CwDecodeText(void);
/* Characters appended to that text since the decoder started, spaces
 * included (0 again after RADIO_CwDecodeClear): the newest (count - seen)
 * of the text are new to a reader. */
uint32_t RADIO_CwDecodeCount(void);
/* 1 in CW with the decoder on and the receiver running. */
uint8_t RADIO_CwDecodeActive(void);
/* 1 ms blocks lost because the main loop fell a whole ring behind. */
uint32_t RADIO_CwDecodeDropped(void);
/* The sender's speed (0 until a signal is locked), the tracked offset from
 * the pitch in Hz and the keyed/unkeyed level ratio in dB. */
void RADIO_CwDecodeStatus(uint8_t *wpm, float *offset_hz, float *snr_db, uint8_t *locked);
void RADIO_CwDecodeClear(void);
float RADIO_AudioRxLevelDbfs(void);     /* S-meter; -150 when not receiving */
/* The same channel power, linear (~10 ms mean, before the AGC); -1 when the
 * receiver is not running. */
float RADIO_AudioRxPower(void);
/* Cycles the receive worker (PendSV) has used, preempting interrupts
 * included; a reader differences two reads against DWT->CYCCNT. 0 in the
 * deep-diagnostic flavour. */
uint32_t RADIO_AudioBusyCycles(void);
float RADIO_AudioRate(void);
/* Polled microphone level for the self test, independent of the audio
 * pipeline (no PTT, no local-source acquisition, no DMA). RADIO_SOURCE_MIC1,
 * MIC2 or MIC3. Returns 0 when the DMA mic path owns the ADC. Main loop only:
 * it polls for about 12 ms.
 *
 * Results are raw 12-bit ADC codes, not millivolts: an electret on this
 * passive network produces single-code excursions, so a scale that rounds to
 * a millivolt hides exactly the signal the probe exists to find. `mean_x16`
 * and `rms_x16` are sixteenths of a code. RMS deviation is the sensitive
 * figure -- min/max over 12 ms report the worst impulse, which is noise. */
typedef struct {
    uint16_t min;
    uint16_t max;
    uint16_t mean_x16;
    uint16_t rms_x16;
} radio_mic_probe_t;
uint8_t RADIO_MicProbe(uint8_t source, radio_mic_probe_t *out);
/* Microphone monitor: route RADIO_SOURCE_MIC1..MIC3 into the USB audio
 * capture endpoint for `seconds`, so the microphone can be listened to or
 * recorded on a host. Unlike the PTT microphone path this neither requests
 * TX nor claims the modulator's audio source. It suspends receiver DMA and
 * borrows the receiver workspace for the capture ring, so headphone TIM2 and
 * RF receive audio are off for the bounded monitor window. Needs
 * RADIO_RECEIVE and a free microphone ADC; returns 0 otherwise. PTT, a fault,
 * storage mode or the deadline ends it. */
uint8_t  RADIO_MicMonitorStart(uint8_t source, uint32_t seconds);
void     RADIO_MicMonitorStop(void);
/* QMX same-ADC reference cancellation coefficient, as an integer percentage.
 * QRP Labs' published example is 68; 0 provides the decisive no-cancellation
 * A/B. A running microphone is restarted at a DMA boundary. */
uint8_t  RADIO_MicrophoneReference(uint8_t percent);
uint8_t  RADIO_MicrophoneReferencePercent(void);
/* Console diagnostic for that coefficient: while it is on, the microphone
 * monitor puts the uncancelled microphone in the left capture channel and
 * its same-ADC reference in the right one, both at the same scale, instead
 * of one cancelled mono signal. A host recording that pair can solve for the
 * coefficient this board needs. Ignored for MIC3, which has no paired
 * channel on ADC3. */
uint8_t  RADIO_MicMonitorSplit(uint8_t on);
uint8_t  RADIO_MicMonitorSplitEnabled(void);
/* BPF sweep chop detector (see radio_bpf_sweep.h). Start gates TEST_CLK on
 * alternate I2S halves (inject = 0: gate stays off, measures the floor);
 * Measure discards settle halves, then integrates an even number of halves;
 * Result returns 1 once that is complete. */
void RADIO_AudioChopStart(uint8_t inject);
void RADIO_AudioChopMeasure(uint16_t settle_halves, uint16_t halves);
uint8_t RADIO_AudioChopResult(float *dbfs, uint8_t *clipped);
void RADIO_AudioChopStop(void);
/* Headphone PWM statistics over a bounded window, for the self test. Begin
 * claims `halves` phone halves (about 776 us each); Stats returns 1 once they
 * have been integrated and reports the mean code, the peak-to-peak swing and
 * the PWM reload they are relative to. A silent path reads mean = top/2 with
 * no swing, which is how a dead interpolator or noise shaper shows up even
 * when the DMA never misses a deadline. */
void    RADIO_AudioPhoneStatsBegin(uint16_t halves);
uint8_t RADIO_AudioPhoneStats(uint32_t *mean, uint32_t *swing, uint32_t *top);
/* What the comparators themselves hold, which the statistics above -- read
 * from the DMA ring in RAM -- cannot see. Returns 0 if TIM2 is running and
 * CCR1 or CCR2 was ever found beyond ARR over a burst of reads: PWM mode 1
 * then holds PA0/PA1 high, DC into the NE5532 and silence at any volume.
 * (A 16-bit DMA into this 32-bit timer did exactly that, duplicating each
 * code into both halves of the register.) */
uint8_t RADIO_AudioPhoneCompareOk(void);
/* QMX 8.6.7 "Test ADC I/Q": one I2S half of raw PCM1821 sample pairs, before
 * the DC blockers, the I/Q correction and the anti-alias filter. Arm claims
 * the next whole half; Read copies min(n, Frames) pairs out once it has
 * landed and returns how many, or 0 while it has not. Suspending the receiver
 * abandons a pending snapshot. RADIO_IqSnapshotStats turns it into numbers. */
void     RADIO_AudioIqSnapshotArm(void);
unsigned RADIO_AudioIqSnapshotRead(int32_t *pairs, unsigned first, unsigned n);
unsigned RADIO_AudioIqSnapshotFrames(void);
/* Demodulated channel level over a bounded window, for the AF and image
 * sweeps (see radio_af_sweep.h). Measure discards settle_halves while the
 * channel filters ring after a retune, then integrates `halves` I2S halves;
 * Result returns 1 once that window is complete and reports dBFS referred to
 * a full-scale sine. Suspending the receiver abandons a pending window. */
void RADIO_AudioLevelMeasure(uint16_t settle_halves, uint16_t halves);
uint8_t RADIO_AudioLevelResult(float *dbfs, uint8_t *clipped);
void RADIO_AudioLevelStop(void);
/* QSD I/Q calibration correlator (see radio_rx_cal.h). Not built into the
 * deep-diagnostic flavour, which has no room for the receive bench. Start arms
 * it on the raw PCM1821 stream after settle_halves; Result returns 1 once the run is
 * complete and copies the accumulator out for RADIO_IqSolve, which is a
 * foreground call. Suspending the receiver abandons a pending run. */
void RADIO_AudioIqCalStart(float cycles_per_sample, uint32_t samples, uint16_t settle_halves,
                           uint8_t invert, float q_gain, float skew_samples);
uint8_t RADIO_AudioIqCalResult(radio_iq_corr_t *out);
void RADIO_AudioIqCalStop(void);
/* Live progress of that correlator: raw samples accumulated of the run length
 * and the settle halves still to skip. A collect that never reaches its run
 * length is otherwise indistinguishable from one that is merely slow, which is
 * what `rxcal status` kept reporting as NO RECEIVE DATA. */
void RADIO_AudioIqCalProgress(uint32_t *count, uint32_t *samples, uint16_t *skip);
/* Retune the demodulator of the running receiver without restarting it, so a
 * fixed carrier can be moved between the wanted and the opposite sideband. */
void RADIO_AudioRetuneDsp(float offset_hz);
/* Seamless tuning (radio.c service_rx_clock). Running: a receiver that can be
 * retuned in place. RADIO_AudioRetune hands the front end the IF terms for
 * dial_hz on the LO now running (phase-continuous) and releases the duck;
 * DuckWait silences the headphones first around an in-place LO move. PwmClear:
 * the headphone PWM carrier still keeps its harmonics away from rf_hz. */
uint8_t RADIO_AudioRunning(void);
uint8_t RADIO_AudioPwmClear(uint32_t rf_hz);
void RADIO_AudioDuckWait(void);
void RADIO_AudioDuckRelease(void);
/* The receiver out of the headphones, the sidetone kept (the trainer). */
void RADIO_AudioRxQuiet(uint8_t on);
void RADIO_AudioRetune(uint32_t dial_hz, uint8_t lo_moved);
/* Waterfall capture (radio_spectrum.c). One block of RADIO_SPEC_N complex
 * samples as interleaved I,Q floats, full scale +-1: either the raw PCM1821
 * stream (after the Q comb cancel, before any correction; the LO at DC) or
 * the front end's 48 kHz output (the dial at DC), the latter optionally
 * through one or two half-band stages to 24 or 12 kHz. Arm claims the next
 * block after `skip_halves` whole I2S halves; Take returns it once full and
 * NULL before, and the block then stays the caller's, unwritten, until the
 * next Arm. Block, decimator and scratch live in the receiver arena, so
 * nothing is reserved while the page is closed and the receiver pays one
 * flag test per half. Every receiver start (each retune) abandons a pending
 * block, zeroes the scratch and advances RADIO_AudioGeneration; Scratch is
 * NULL while no receiver runs. */
#define RADIO_SPEC_N 512U
#define RADIO_SPEC_SCRATCH_BYTES 1124U
enum { RADIO_SPEC_SRC_RAW = 0, RADIO_SPEC_SRC_CHANNEL = 1 };
void RADIO_AudioSpecArm(uint8_t source, uint8_t halvings, uint16_t skip_halves);
float *RADIO_AudioSpecTake(void);
void RADIO_AudioSpecStop(void);
void *RADIO_AudioSpecScratch(void);
uint32_t RADIO_AudioGeneration(void);
/* Scanner: while on, the receive worker only unpacks and captures -- no
 * front end, demodulator or AGC, which would chew on foreign LOs -- and the
 * headphones fade to silence as they do for the BPF sweep. The 48 kHz ring
 * is fed silence at its own rate so nothing counts as an overrun. */
void RADIO_AudioScanMute(uint8_t on);
/* Receive worker load in percent of the core (receive plus headphone halves,
 * exponential means), 0 while the cycle profile is off or nothing runs. */
uint8_t RADIO_AudioLoadPct(void);
/* I2S divider behind the live PCM1821 rate, for the exact injection offset. */
uint32_t RADIO_AudioI2sDivider(void);
#endif
