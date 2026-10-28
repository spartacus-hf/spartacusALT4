#ifndef RADIO_IQ_FIT_H
#define RADIO_IQ_FIT_H

#include <stdint.h>
#include "radio_iq_delay.h"

/* QSD I/Q imbalance: measurement from the built-in test clock, and the
 * closed-form correction for it.
 *
 * QMX drives the same quadrature sampling detector from an Si5351A, whose
 * even-integer output divider makes the 90 degree relationship exact, and
 * ships with no I/Q correction at all: QRP Labs quote 60-75 dB of image
 * rejection, varying from unit to unit (operating manual 1.03.002, 8.6.3).
 * What is left over on any one board is component tolerance in T401, U9/U12
 * and the two RC integrators, and that is a fixed gain and phase error which
 * three coefficients can cancel.
 *
 * REV1 can measure it without a signal generator. TEST_CLK (PB14, HRTIM
 * timer D) reaches the LPF input through R106 100 k, so putting timer D on a
 * period one or a few ticks away from the QSD LO period puts a carrier at a
 * known offset delta from the LO, through the real receive path. A balanced
 * receiver puts all of that carrier in the bin at +delta; whatever appears at
 * -delta is the image, and the complex ratio of the two bins determines the
 * imbalance uniquely.
 *
 * Nothing here touches hardware: radio_bpf_sweep.c drives the clocks and
 * radio_audio.c feeds samples in, so the rev1_rxcal host suite can exercise
 * every formula against a modelled imbalanced QSD. Same split as
 * radio_bpf_fit.c under radio_bpf_sweep.c. */

/* Why no chop, unlike the BPF sweep: that measurement has to run with the test
 * clock at exactly the LO period (above ~16 MHz no integer period lands a few
 * kHz away), so its product sits at DC among the op-amp offsets and the LO
 * leakage, and only a synchronous chop recovers it. Here the product sits at
 * +-delta, several kHz from DC and far from the PCM1821's ~44 Hz DC-removal
 * high-pass, so DC cancels in the integration instead of needing to be
 * modulated away. The injector's odd harmonics beat against the LO's odd
 * harmonics at 3, 5 ... times delta, never at +-delta. */

/* One bin per channel, not one per sideband. Correlating I and Q separately
 * at +delta gives
 *     X = sum w I e^(-j w n) = (A/2) e^(j phi)
 *     Y = sum w Q e^(-j w n) = -j (1 + g) e^(j theta) X
 * so (1 + g) e^(j theta) = j Y / X and the carrier's arbitrary arrival phase
 * phi divides straight out. The sideband bins do not have that property: the
 * image-to-signal ratio carries a factor e^(-2 j phi), so solving from it
 * would only be right when the tone happened to start at zero phase. The two
 * sideband levels are still recovered from X and Y for reporting.
 *
 * Hann weighted, which is not decoration. Off-air signals reach the detector
 * through the same imbalance, so one ABOVE the LO obeys the same Y = -j k X
 * relation as the injection and moves X and Y together: it cannot move their
 * ratio however strong or however close it is. One BELOW the LO is the
 * opposite. I and Q are real, so a carrier at -(delta + e) puts its conjugate
 * component at +(delta + e) -- e hertz from the bin, not 2 delta away -- and
 * it arrives with -conj(k) instead of k. Sixty hertz is about twenty bins at
 * 65536 samples, where a rectangular window still leaks -40 dB: several tenths
 * of a degree of phase error from one strong station on the image side. The
 * taper puts it out of reach. It scales X and Y alike and so cannot bias their
 * ratio itself.
 *
 * A DC offset or LO leakage sits a full delta away, some 3400 bins, and is
 * reported rather than removed. */
typedef struct {
    float osc_i, osc_q, step_i, step_q;   /* e^(j 2 pi delta t) */
    float win_i, win_q, win_step_i, win_step_q; /* 1/N cycles per sample */
    float x_i, x_q;                       /* I channel at +delta */
    float y_i, y_q;                       /* Q channel at +delta */
    float dc_i, dc_q;                     /* offsets and LO leakage */
    float weight, peak;
    float q_gain, skew_samples;           /* correction applied at this bin */
    float cycles_per_sample;              /* delta / fS, signed */
    uint8_t invert;
    uint16_t normalize;                   /* samples since the oscillators were rescaled */
    uint32_t count, samples;
} radio_iq_corr_t;

/* cycles_per_sample is delta / fS, signed. `samples` is the run length; it
 * sets the window, and RADIO_IqCorrAdd ignores anything beyond it, so the
 * caller may keep feeding until it notices. */
void RADIO_IqCorrInit(radio_iq_corr_t *c, float cycles_per_sample, uint32_t samples);
/* Apply this correction to the stream before correlating, exactly as
 * RADIO_RxFrontSample applies the stored settings. Used for the verification
 * pass, so the residual image we report is measured and not predicted. */
void RADIO_IqCorrCorrection(radio_iq_corr_t *c, uint8_t invert, float q_gain,
                            float skew_samples);
/* One raw sample pair, straight off the PCM1821. */
void RADIO_IqCorrAdd(radio_iq_corr_t *c, float i, float q);
uint8_t RADIO_IqCorrDone(const radio_iq_corr_t *c);

typedef struct {
    float wanted_dbfs, image_dbfs;  /* bin levels, dB relative to full scale */
    float image_db;                 /* wanted over image: the rejection */
    float amp_db, phase_deg;        /* the imbalance itself */
    float i_gain, q_gain, skew_samples; /* coefficients that cancel it */
    float dc_i, dc_q;               /* mean offset per channel, full scale */
    uint8_t inverted;               /* wanted bin at -delta: I/Q are swapped */
    uint8_t clipped;                /* a sample reached 98 % of full scale */
    uint8_t valid;
} radio_iq_result_t;

/* 0 when the run is short, the bins are unusable, or the geometry is
 * degenerate (a ratio at 1 is total cancellation, not an imbalance). */
uint8_t RADIO_IqSolve(const radio_iq_corr_t *c, radio_iq_result_t *out);

/* delta / fS for a QSD LO period of p_lo and a test clock period of p_test,
 * at I2S divider i2s_div. Both clocks divide the same HRTIM clock and fS
 * divides the same SYSCLK, so SYSCLK cancels and this is exact:
 *   delta / fS = 2048 x div x (p_lo - p_test) / (p_lo x p_test)
 * Positive means the test clock is above the LO. */
float RADIO_IqDeltaCycles(uint32_t p_lo, uint32_t p_test, uint32_t i2s_div);

/* Test clock period placing delta inside [min_hz, max_hz], preferring the
 * smallest offset (so the tone stays well inside the PCM1821's flat band) and
 * the side above the LO (so the wanted bin is the positive one, as in normal
 * receive). 0 when no period in range works. */
uint8_t RADIO_IqChoosePeriod(uint32_t p_lo, uint32_t i2s_div, float rate_hz,
                             float min_hz, float max_hz, uint32_t *p_test);

/* QMX 8.6.7 "Test ADC I/Q": statistics of a short run of raw sample pairs
 * straight off the PCM1821, before any correction. QMX plots 64 samples (two
 * processing blocks) and has the operator judge equal amplitude, a clean 90
 * degrees and symmetric peaks by eye; these are the same judgements as
 * numbers. `pairs` is interleaved I,Q as the ADC delivers them, full scale
 * +-2^31.
 *
 * For a single tone, I = A cos(wt) and Q = B sin(wt + theta): the normalised
 * cross-correlation sum(I*Q)/sqrt(sum(I^2) sum(Q^2)) is sin(theta), so the
 * departure from quadrature falls out of the same three sums as the amplitude
 * ratio. It needs a whole number of cycles only to second order, and 64
 * samples at the injection offset covers several.
 *
 * `valid` is 0 when there is not enough signal to say anything, which is what
 * a dead converter, a missing injection or a railed pre-amp look like. */
typedef struct {
    float amp_db;        /* Q over I, positive means Q is larger */
    float phase_deg;     /* departure from 90 degrees */
    float i_rms, q_rms;  /* fractions of full scale */
    int32_t i_max, i_min, q_max, q_min;
    uint8_t valid, clipped;
} radio_iq_snapshot_t;
void RADIO_IqSnapshotStats(const int32_t *pairs, unsigned n, radio_iq_snapshot_t *out);

/* HRTIM period limits, as radio_clocks.c enforces them. */
#define RADIO_IQ_PERIOD_MIN 192U
#define RADIO_IQ_PERIOD_MAX 0xFFDCU

#endif
