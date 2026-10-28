#ifndef RADIO_CW_DECODER_H
#define RADIO_CW_DECODER_H
/* Receive CW decoder. Pure C, no HAL: host suite cw_decoder runs it on the
 * production receive DSP with keyed signals in noise.
 *
 * Input is the CW channel-filter output before the BFO and the AGC -- complex
 * baseband with the dial at DC -- summed over RADIO_CWDEC_BLOCK samples, so the
 * decoder sees 1 kHz complex samples. The receive worker does only that sum
 * (two adds a sample); everything below runs in the main loop.
 *
 *   - Frequency: every 32 ms a DFT of the last 64 ms in 10 Hz bins spans the
 *     CW filter (RADIO_CwDecoderSetSpan: +-BW/2, at least +-100 Hz, at most
 *     +-450 Hz); a smoothed peak that stands clear of the median sets the
 *     detector's frequency. The operator tunes the signal to the sidetone
 *     pitch, so a station within 100 Hz of it is taken first and wins over
 *     one locked further out; only with nothing there is one elsewhere in the
 *     filter followed. Once locked, the search follows the station +-100 Hz.
 *   - Detection: mix to DC and integrate coherently over 0.9 dit (4-48 ms),
 *     near a matched filter for a dit: a detection bandwidth that narrows with
 *     the speed, about 20 Hz at 18 WPM, instead of the channel filter's
 *     hundreds.
 *   - Decision: two levels -- the keyed one from each mark's plateau, the
 *     unkeyed one from the envelope clear of every element's ramps -- with the
 *     threshold half way and hysteresis; nothing is printed while their ratio
 *     says there is no signal (squelch). Both follow QSB.
 *   - Timing: edges debounced by 0.3 dit, marks split into dits and dahs by
 *     two-medians of the last 16. Nothing is printed until a clean split
 *     (dahs 2.2 to 4.5 dits, as keyers, bugs and hands send them) confirms
 *     the speed; the word under way -- from the mark that raised the lock at
 *     the earliest -- is then re-read with it, so it is not lost, but only
 *     when every mark logged for it fits the speed found (no burst of
 *     guesses after a long search). Gaps are
 *     counted in units of half a dit and its space (weighting lengthens one
 *     and shortens the other), characters end after 2 units of silence and
 *     words after 5. */
#include <stdint.h>

#define RADIO_CWDEC_BLOCK   48U   /* 48 kHz samples per decoder sample (1 ms) */
#define RADIO_CWDEC_TEXT    40U   /* characters of decoded text kept */
#define RADIO_CWDEC_BINS    91U   /* up to -450..+450 Hz in 10 Hz steps */
#define RADIO_CWDEC_NEAR    10U   /* bins either side of the pitch taken first: +-100 Hz */
#define RADIO_CWDEC_WINDOW  64U   /* ms of history per spectrum */
#define RADIO_CWDEC_MARKS   16U
#define RADIO_CWDEC_MA_MAX  48U
/* ms of envelope kept: the noise level reads it one integration length
 * (up to RADIO_CWDEC_MA_MAX) back, and a mark's end trims its falling ramp. */
#define RADIO_CWDEC_ENV_HIST 64U
#define RADIO_CWDEC_LOG      64U  /* marks and gaps kept until the speed is known */
#define RADIO_CWDEC_GAPS     8U   /* recent gaps between characters and words */

typedef struct {
    /* spectrum and frequency tracking; the history holds I and Q as
     * bfloat16 in one word, as the receive worker hands them over (lossless) */
    uint32_t hist[RADIO_CWDEC_WINDOW];
    float spec[RADIO_CWDEC_BINS];
    uint8_t span;        /* bins either side of the pitch searched */
    float freq_hz, peak_ratio;
    float nco_i, nco_q, nco_di, nco_dq;
    uint16_t lock_hold, nco_norm;
    uint8_t hist_at, hop, locked;
    uint8_t spec_n;      /* spectra averaged since the retune, to SPEC_WARMUP */
    float char_peak;     /* highest peak_ratio since the last character ended */
    float now_ratio;     /* the tracked bin in the latest window alone, over the median */
    /* coherent detector */
    float ma[RADIO_CWDEC_MA_MAX][2];
    float ma_i, ma_q;
    uint8_t ma_len, ma_at, ma_fill;
    /* levels and decision */
    float env, sig, noise;
    uint16_t env_hist[RADIO_CWDEC_ENV_HIST]; /* bfloat16 */
    float plateau_sum;
    uint16_t plateau_n;
    uint8_t env_at;
    uint8_t raw, keyed, primed;
    uint16_t run, prime_n;
    float prime_sum;
    /* timing */
    uint32_t now_ms, edge_ms, lock_ms, unlock_ms;
    float dit_ms, dah_split_ms;
    float space_ms;      /* the space inside a character, tracked (unit_ms) */
    float marks[RADIO_CWDEC_MARKS];
    uint8_t mark_count, mark_at;
    /* Gaps longer than 2 units, in units, for the word threshold
     * (word_gap_ms), and that threshold as the gaps have it (0 with none). */
    float gaps[RADIO_CWDEC_GAPS];
    float gap_stretch;
    uint8_t gap_count, gap_at;
    uint8_t last_mark_ok; /* the mark before the current gap was decoded */
    uint8_t lock_mark;    /* the mark under way when the lock came: logged, not timed */
    uint16_t gap_ms;      /* the gap before the current or last mark */
    /* Intervals heard before the speed was confirmed, re-read once it is:
     * bit 15 set for a mark, the rest its length in ms. */
    uint16_t log[RADIO_CWDEC_LOG];
    uint8_t log_n;
    uint8_t sym_bits, sym_len, word_done, open, synced, speed_ok;
    /* output */
    char text[RADIO_CWDEC_TEXT + 1U];
    uint8_t text_len;
    uint32_t chars;      /* characters decoded since reset */
    uint32_t emitted;    /* everything appended to text, spaces too: a reader
                          * takes the last (emitted - seen) of it */
} radio_cwdec_t;

void RADIO_CwDecoderInit(radio_cwdec_t *d);
/* After a retune: forget the frequency, the levels, the speed and any
 * half-received character (a new station); keep the text and the span. */
void RADIO_CwDecoderRetune(radio_cwdec_t *d);
/* The CW filter's width: the frequency search spans +-hz/2 of the pitch
 * (clamped to +-100..+-450 Hz). Retunes the decoder when the span changes. */
void RADIO_CwDecoderSetSpan(radio_cwdec_t *d, uint16_t filter_hz);
/* `ms` of 1 ms samples that never arrived (the main loop fell behind the
 * receive worker): their time passes, the character they fell in is dropped. */
void RADIO_CwDecoderLost(radio_cwdec_t *d, uint32_t ms);
/* One 1 kHz complex sample. Returns the character it completed ('\0' if
 * none; ' ' for a word space); the text buffer has it appended already. */
char RADIO_CwDecoderStep(radio_cwdec_t *d, float i, float q);
/* The sender's speed in WPM from the tracked dit, 0 with no signal. */
uint8_t RADIO_CwDecoderWpm(const radio_cwdec_t *d);
/* Tracked frequency offset from the pitch, Hz. */
float RADIO_CwDecoderOffsetHz(const radio_cwdec_t *d);
/* Keyed-to-unkeyed envelope ratio, dB (0 with nothing to measure). */
float RADIO_CwDecoderSnrDb(const radio_cwdec_t *d);
uint8_t RADIO_CwDecoderLocked(const radio_cwdec_t *d);
#endif
