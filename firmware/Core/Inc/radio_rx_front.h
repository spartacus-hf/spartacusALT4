#ifndef RADIO_RX_FRONT_H
#define RADIO_RX_FRONT_H
#include "radio_config.h"
#include "radio_iq_delay.h"
/* Taps per polyphase branch of the receive decimation table. The branch length
 * sets the transition width, so this number is the receive bandwidth: 96 taps
 * of Kaiser (A~70 dB, stopband edge 25 kHz) pass the 48 kHz stream to +-19 kHz,
 * where the 64-tap Blackman it replaces passed +-12 kHz. A branch is the whole
 * cost driver of the receive half -- 2 x M multiply-accumulates per output --
 * and that half shares a hard 61,370-cycle deadline with the phone and USB
 * audio work, so how this table is read matters as much as how long it is:
 * see the loop comment in radio_rx_front.c. */
#define RADIO_RX_FILTER_TAPS 96U
/* History samples per channel: a power of two >= RADIO_RX_FILTER_TAPS, so the
 * wrap stays a mask. Keeping it separate from the tap count means the branch
 * length does not have to be a power of two. 2 x 2 x 128 x 4 = 2048 bytes of
 * the workspace. */
#define RADIO_RX_HISTORY 128U
/* Polyphase branches in the decimation table; RADIO_RX_COEFF_FLOATS is the
 * whole table, for a caller that gives it a RAM copy. */
#define RADIO_RX_PHASES 32U
#define RADIO_RX_COEFF_FLOATS (RADIO_RX_PHASES * RADIO_RX_FILTER_TAPS)
/* The part of the front end that depends on the IF: the NCO step and the
 * I/Q delay's gain/phase trim at that offset. A seamless retune hands a new
 * set to a running front end (radio_rx_front_t.next), which takes it at the
 * start of its next block: the NCO keeps its phase, so the tuned signal
 * moves instead of the receiver restarting. */
typedef struct { float step_i, step_q, delay_i_scale, delay_q_scale, delay_trim; } radio_rx_if_terms_t;

typedef struct {
    /* Interleaved (I,Q) float pairs, every sample written twice at `2*index`
     * and `2*(index + RADIO_RX_HISTORY)`, so the tap window is always one
     * contiguous ascending run -- oldest sample of the window first -- that a
     * single pointer can walk with no modulo and no per-channel pointers. */
    float history[2U * 2U * RADIO_RX_HISTORY];
    float dc_i, dc_q, osc_i, osc_q, step_i, step_q, phase, rate;
    float delay_i_scale, delay_q_scale, delay_trim;
    radio_iq_delay_t iq_delay;
    unsigned index, normalize;
    /* Timing correction (radio_rx_front.c): the last RADIO_RX_TC_RING raw
     * outputs and the fraction of a phase step each one's instant missed. */
    float tc_i[16], tc_q[16];
    uint8_t tc_frac[16];
    unsigned tc_at;
    /* Seamless retune: `next` is taken when retune_seq moves past applied. */
    radio_rx_if_terms_t next;
    /* The reference's trim (radio_audio.c, rx_trim_apply): a further rotation
     * per sample on top of step_i/step_q, set between blocks. */
    float trim_i, trim_q;
    volatile uint32_t retune_seq;
    uint32_t retune_applied;
    /* The decimation rows the FIR reads: the flash table after Init, or a
     * RAM copy after RADIO_RxFrontCoefficientsTo. */
    const float *coeff;
} radio_rx_front_t;

/* Receive clock geometry in HRTIM ticks. PB12 (U11 S1) is high for [0, half);
 * PB13 (U11 S0) leads by about a quarter period and is low for
 * [fall, half + rise). */
typedef struct { uint32_t period, half, fall, rise; } radio_rx_geometry_t;

/* Integer-tick edges for period p. A double-balanced QSD needs both node pairs
 * (1B1/1B4 on S0+S3, 1B2/1B3 on S1+S2) connected for equal time and centred
 * windows; unequal connection time gives the pairs different effective RC
 * constants and an I/Q phase error at the IF. p = 4m and 4m+2 balance
 * exactly; odd p leaves one tick, placed on the S0/S3 pair. In a switch-level
 * model with the IF at a quarter of the node RC corner this gives about 65 dB
 * image rejection for even p and 60 dB for odd p, against 44-54 dB for plain
 * rounding. */
void RADIO_RxClockEdges(uint32_t period, radio_rx_geometry_t *g);

/* The per-mode receive channel (Hz around the dial) the planner and the
 * waterfall use; radio.c keeps it in step with the configured filter widths. */
void RADIO_RxSetChannelSpans(const int16_t spans[5][2]);
void RADIO_RxChannelSpan(radio_rx_mode_t mode, int32_t *lo, int32_t *hi);

/* QMX-style LO placement: choose the HRTIM period whose LO puts the selected
 * mode's channel near a 12 kHz IF, clear of DC (+-600 Hz) and inside the
 * +-40 kHz window with the ADC at ~96 kHz, or the +-50 kHz one at 170-190 kHz
 * where no LO fits the first (radio_rx_front.c). Returns 2 for a clean
 * IF, 1 for an in-band LO whose channel touches DC, 0 if nothing fits;
 * RADIO_RxPlanLoAdc also says which ADC rate the plan is for. */
enum { RADIO_RX_ADC_AUTO = 0, RADIO_RX_ADC_96K, RADIO_RX_ADC_HIGH };
void RADIO_RxAdcPolicySet(uint8_t policy); /* `clock adc96 auto|on|off` */
uint8_t RADIO_RxAdcPolicy(void);
void RADIO_RxAdcRunning96(uint8_t on);     /* radio_clocks.c: the running plan's */
uint8_t RADIO_RxPlanLoAdc(uint64_t hr_clock_hz, uint32_t rf_hz, radio_rx_mode_t mode,
                          uint32_t *period, int32_t *offset_hz, uint8_t *adc96);
uint8_t RADIO_RxPlanLo(uint64_t hr_clock_hz, uint32_t rf_hz, radio_rx_mode_t mode,
                       uint32_t *period, int32_t *offset_hz);

/* Seamless tuning keeps the running LO while the IF stays in this window
 * around the 12 kHz target -- where RX IQ CAL's correction holds -- and the
 * channel stays clear of DC; outside it the planner picks a new period. */
#define RADIO_RX_IF_KEEP_MIN_HZ 6000
#define RADIO_RX_IF_KEEP_MAX_HZ 22000
/* 1 if the running LO can serve a dial `offset_hz` (dial - LO) away for a
 * channel spanning [span_lo, span_hi] around the dial. */
uint8_t RADIO_RxKeepLo(int32_t offset_hz, int32_t span_lo, int32_t span_hi);

/* The IF-dependent terms for `offset_hz` at `rate` with the configured skew. */
void RADIO_RxFrontIfTerms(radio_rx_if_terms_t *t, float rate, float offset_hz, float skew_samples);

/* Then RADIO_RxFrontCoefficientsTo: until it has run, blocks produce nothing. */
void RADIO_RxFrontInit(radio_rx_front_t *s, float rate, float offset_hz, float skew_samples);
/* Write the decimation table into `ram` (RADIO_RX_COEFF_FLOATS floats, word
 * aligned) and read it from there. Flash holds half of it (the rows past the
 * middle mirror the others, radio_rx_front.c), and it would sit behind four
 * wait states and a D-cache far smaller than one row set anyway, while the
 * FIR reads 96 of its floats per 48 kHz output; zero-wait SRAM takes those
 * reads off the receive deadline. The copy must outlive `s`. */
void RADIO_RxFrontCoefficientsTo(radio_rx_front_t *s, float *ram);

/* Calibrate/translate full-rate IQ, anti-alias, then produce 48 kHz IQ, over a
 * run of `n` input samples. This is the form the receiver uses. The one-sample
 * entry point below is a real call that reloads the four correction
 * coefficients through `c` and the whole oscillator/decimator state through
 * `s` every time, because the compiler cannot prove the two do not alias;
 * at 177 kHz that reloading is most of what the front end costs outside the
 * FIR. Here the state lives in registers for the whole run.
 *
 * Writes one output per input at most -- size the output arrays with
 * RADIO_RX_FRONT_MAX_OUT(n) -- and returns how many it wrote. */
unsigned RADIO_RxFrontBlock(radio_rx_front_t *s, const radio_config_t *c,
                            const float *in_i, const float *in_q, unsigned n,
                            float *out_i, float *out_q);
/* The decimator emits one output per 48000/rate inputs and the input rate is
 * never below 165 kHz, plus one for a partial phase carried in. */
#define RADIO_RX_FRONT_MAX_OUT(n) ((((n) * 48000U + 79999U) / 80000U) + 1U)

/* One sample, for callers that have no run to give: the host suites and any
 * future single-shot probe. Same arithmetic, same state. */
uint8_t RADIO_RxFrontSample(radio_rx_front_t *s, const radio_config_t *c,
                           float i, float q, float *out_i, float *out_q);
#endif
