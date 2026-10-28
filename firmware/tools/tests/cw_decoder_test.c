/* The receive CW decoder on keyed signals in noise, through the production
 * CW receive DSP.
 *
 * A Morse message is keyed at a given speed with 5 ms raised-cosine edges,
 * as complex baseband at 48 kHz (the dial at DC, the carrier `offset` Hz from
 * it), white Gaussian noise is added, and the sum goes through
 * RADIO_DspDemodulate with the radio's CW settings. The decoder is fed exactly
 * what the receive worker feeds it on the radio: the channel-filter output
 * (dsp.last_i/last_q) summed over RADIO_CWDEC_BLOCK samples. The character
 * error rate is the edit distance between what was sent and what came out.
 *
 * SNR is quoted in 2500 Hz, the convention of WSJT-X and CW Skimmer, and in
 * 500 Hz: signal power while keyed over the noise power in that bandwidth.
 *
 *   cw_decoder_test            regression: asserted thresholds (host suite)
 *   cw_decoder_test --sweep    the full table: CER by SNR, speed and offset
 *   cw_decoder_test --one SNR [WPM OFFSET QSB_HZ EFF_WPM QSB_DB]
 *                              each message decoded; env CW_SEED, CW_RATIO
 *                              (dah in dits) and CW_WEIGHT (dits) */
#include "radio_cw_decoder.h"
#include "radio_dsp.h"
#include "radio_config.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FS 48000.0
/* Regression bounds on 18 WPM steady-state CER, set from --sweep with margin. */
#define WEAK6_LIMIT 0.05
#define WEAK8_LIMIT 0.15
/* Whole-message CER, first word included, and the worst of three fists. */
#define FIRST_LIMIT 0.03
#define FIST_LIMIT  0.10
#define TAU 6.283185307179586

static const struct { char c; const char *code; } morse[] = {
    {'A', ".-"}, {'B', "-..."}, {'C', "-.-."}, {'D', "-.."}, {'E', "."}, {'F', "..-."},
    {'G', "--."}, {'H', "...."}, {'I', ".."}, {'J', ".---"}, {'K', "-.-"}, {'L', ".-.."},
    {'M', "--"}, {'N', "-."}, {'O', "---"}, {'P', ".--."}, {'Q', "--.-"}, {'R', ".-."},
    {'S', "..."}, {'T', "-"}, {'U', "..-"}, {'V', "...-"}, {'W', ".--"}, {'X', "-..-"},
    {'Y', "-.--"}, {'Z', "--.."}, {'0', "-----"}, {'1', ".----"}, {'2', "..---"},
    {'3', "...--"}, {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
    {'8', "---.."}, {'9', "----."}, {'/', "-..-."}, {'?', "..--.."}, {'=', "-...-"},
};

static const char *code_for(char c)
{
    for (unsigned i = 0U; i < sizeof(morse) / sizeof(morse[0]); ++i)
        if (morse[i].c == c) return morse[i].code;
    return NULL;
}

/* xorshift64* and Box-Muller: reproducible noise. */
static uint64_t rng_state;
static double uniform(void)
{
    rng_state ^= rng_state >> 12; rng_state ^= rng_state << 25; rng_state ^= rng_state >> 27;
    return ((rng_state * 2685821657736338717ULL) >> 11) * (1.0 / 9007199254740992.0);
}
static double gauss(void)
{
    double u = uniform(), v = uniform();
    if (u < 1e-300) u = 1e-300;
    return sqrt(-2.0 * log(u)) * cos(TAU * v);
}

/* Keying envelope: 1/3/7 spacing, as a list of (start, end) seconds for each
 * element. With `eff_wpm` below `wpm` the gaps between characters and words
 * are stretched for Farnsworth timing (ARRL: characters at `wpm`, PARIS at
 * `eff_wpm` overall), as W1AW code practice and most trainers send. A fist
 * other than a keyer's: dahs of `key_ratio` dits, and `key_weight` dits added
 * to every mark and taken from every space. */
static double key_ratio = 3.0, key_weight = 0.0;
typedef struct { double on, off; } element_t;
static unsigned key_text(const char *text, double wpm, double eff_wpm, element_t *out, unsigned cap, double *total)
{
    const double dit = 1.2 / wpm;
    double char_gap = 3.0 * dit, word_gap = 7.0 * dit;
    if (eff_wpm > 0.0 && eff_wpm < wpm) {
        const double ta = (60.0 * wpm - 37.2 * eff_wpm) / (wpm * eff_wpm);
        char_gap = 3.0 * ta / 19.0;
        word_gap = 7.0 * ta / 19.0;
    }
    double t = 0.5; /* half a second of noise first */
    unsigned n = 0U;
    for (const char *p = text; *p; ++p) {
        if (*p == ' ') { t += word_gap - char_gap; continue; }
        const char *code = code_for(*p);
        if (!code) continue;
        for (const char *e = code; *e; ++e) {
            const double len = ((*e == '-' ? key_ratio : 1.0) + key_weight) * dit;
            if (n < cap) { out[n].on = t; out[n].off = t + len; ++n; }
            t += len + (1.0 - key_weight) * dit;
        }
        t += char_gap - dit;
    }
    *total = t + 1.0;
    return n;
}

typedef struct {
    double wpm, offset_hz, snr2500_db;
    uint8_t bw_index;
    double qsb_hz;          /* 0: steady; else +-qsb_db fading at this rate */
    double adjacent_hz;     /* 0: none; else a 20 dB stronger keyed signal there */
    uint64_t seed;
    double eff_wpm;         /* 0: standard spacing; else Farnsworth, this overall */
} scenario_t;

static void normalise(const char *in, char *out, size_t cap)
{
    size_t n = 0U;
    int space = 1;
    for (const char *p = in; *p && n + 1U < cap; ++p) {
        if (*p == ' ') { if (!space) { out[n++] = ' '; space = 1; } }
        else { out[n++] = *p; space = 0; }
    }
    while (n && out[n - 1U] == ' ') --n;
    out[n] = '\0';
}

static unsigned edit_distance(const char *a, const char *b)
{
    const size_t la = strlen(a), lb = strlen(b);
    unsigned *row = malloc((lb + 1U) * sizeof(unsigned));
    for (size_t j = 0U; j <= lb; ++j) row[j] = (unsigned)j;
    for (size_t i = 1U; i <= la; ++i) {
        unsigned diag = row[0];
        row[0] = (unsigned)i;
        for (size_t j = 1U; j <= lb; ++j) {
            const unsigned up = row[j];
            unsigned best = diag + (a[i - 1U] != b[j - 1U]);
            if (row[j] + 1U < best) best = row[j] + 1U;
            if (row[j - 1U] + 1U < best) best = row[j - 1U] + 1U;
            row[j] = best;
            diag = up;
        }
    }
    const unsigned d = row[lb];
    free(row);
    return d;
}

static double envelope_at(const element_t *el, unsigned n, double t, double rise)
{
    /* Elements are sorted; a linear scan from a cursor keeps this cheap. */
    static unsigned cursor;
    if (t < 1e-9) cursor = 0U;
    while (cursor < n && el[cursor].off + rise < t) ++cursor;
    if (cursor >= n) return 0.0;
    const element_t *e = &el[cursor];
    if (t < e->on - rise * 0.5 || t > e->off + rise * 0.5) return 0.0;
    if (t < e->on + rise * 0.5) return 0.5 - 0.5 * cos(TAU * 0.5 * (t - (e->on - rise * 0.5)) / rise);
    if (t > e->off - rise * 0.5) return 0.5 + 0.5 * cos(TAU * 0.5 * (t - (e->off - rise * 0.5)) / rise);
    return 1.0;
}

/* The decoder searches the CW filter's width, as radio_audio.c sets it. */
static void decoder_init(radio_cwdec_t *d, uint8_t bw_index)
{
    static const uint16_t table[RADIO_CW_BW_COUNT] = RADIO_CW_BW_TABLE;
    RADIO_CwDecoderInit(d);
    RADIO_CwDecoderSetSpan(d, table[bw_index < RADIO_CW_BW_COUNT ? bw_index : 0U]);
}

/* Runs one scenario, returns the CER (0..1) and the decoded text. */
static int steady_state = 1;
static double qsb_db = 6.0; /* fading depth, +- dB */

static double run(const scenario_t *s, const char *text, char *decoded, size_t cap, radio_cwdec_t *out_d)
{
    static element_t el[4096], adj[4096];
    double total, adj_total;
    const unsigned n = key_text(text, s->wpm, s->eff_wpm, el, 4096U, &total);
    const unsigned na = s->adjacent_hz != 0.0 ? key_text("TEST DE K1XYZ K1XYZ QRL? TEST", 28.0, 0.0, adj, 4096U, &adj_total) : 0U;
    rng_state = s->seed * 0x9E3779B97F4A7C15ULL + 12345U;
    const double amp = 0.01;
    /* SNR in 2500 Hz: A^2 / (N0 * 2500), complex noise of 2 sigma^2 over FS. */
    const double n0 = amp * amp / (2500.0 * pow(10.0, s->snr2500_db / 10.0));
    const double sigma = sqrt(n0 * FS / 2.0);
    radio_dsp_t dsp;
    RADIO_DspInit(&dsp, (float)FS, 0.0f, RADIO_RX_CW);
    RADIO_DspSetCw(&dsp, RADIO_CW_PITCH_DEFAULT_HZ, s->bw_index, RADIO_CW_AGC_SLOW);
    radio_cwdec_t d;
    decoder_init(&d, s->bw_index);
    float acc_i = 0.0f, acc_q = 0.0f;
    unsigned acc_n = 0U;
    const unsigned samples = (unsigned)(total * FS);
    const double phase0 = uniform() * TAU;
    for (unsigned k = 0U; k < samples; ++k) {
        const double t = k / FS;
        double a = amp * envelope_at(el, n, t, 0.005);
        if (s->qsb_hz > 0.0) a *= pow(10.0, qsb_db / 20.0 * sin(TAU * s->qsb_hz * t));
        const double ph = TAU * s->offset_hz * t + phase0;
        double si = a * cos(ph) + sigma * gauss();
        double sq = a * sin(ph) + sigma * gauss();
        if (na) {
            /* A second station 20 dB stronger, keyed independently. The
             * envelope helper keeps one cursor, so recompute it plainly. */
            double e2 = 0.0;
            for (unsigned m = 0U; m < na; ++m) {
                if (t >= adj[m].on && t < adj[m].off) { e2 = 1.0; break; }
                if (adj[m].on > t) break;
            }
            const double p2 = TAU * s->adjacent_hz * t;
            si += 10.0 * amp * e2 * cos(p2);
            sq += 10.0 * amp * e2 * sin(p2);
        }
        (void)RADIO_DspDemodulate(&dsp, (float)si, (float)sq);
        acc_i += dsp.last_i;
        acc_q += dsp.last_q;
        if (++acc_n == RADIO_CWDEC_BLOCK) {
            (void)RADIO_CwDecoderStep(&d, acc_i, acc_q);
            acc_i = acc_q = 0.0f;
            acc_n = 0U;
        }
    }
    char want[512], got[512];
    normalise(text, want, sizeof(want));
    normalise(d.text, got, sizeof(got));
    /* The decoder keeps its last RADIO_CWDEC_TEXT characters; score against
     * the end of the message that could still be in the buffer. */
    const size_t lw = strlen(want);
    const char *tail = want;
    if (lw > RADIO_CWDEC_TEXT) tail = want + (lw - RADIO_CWDEC_TEXT);
    if (decoded && cap) { strncpy(decoded, got, cap - 1U); decoded[cap - 1U] = '\0'; }
    if (out_d) *out_d = d;
    if (!steady_state) return (double)edit_distance(got, tail) / (double)strlen(tail);
    /* Steady state: the first word is where the decoder finds the frequency,
     * the levels and the speed; score the rest. The decoder may have dropped
     * that word or garbled it, so take whichever alignment fits. */
    const char *want_rest = strchr(tail, ' ');
    if (!want_rest) return (double)edit_distance(got, tail) / (double)strlen(tail);
    ++want_rest;
    const char *got_rest = strchr(got, ' ');
    unsigned best = edit_distance(got, want_rest);
    if (got_rest) {
        const unsigned alt = edit_distance(got_rest + 1, want_rest);
        if (alt < best) best = alt;
    }
    return (double)best / (double)strlen(want_rest);
}

static const char *const messages[] = {
    "CQ CQ DE DL1ABC DL1ABC K",
    "GE TNX RST 579 NAME JOE",
    "OK QTH PARIS = WX FB 73 TU",
    "W1AW DE G4XYZ 5NN 14 BK",
    "VVV QRL? PSE QSY 7030 KHZ",
};

/* Mean CER over the messages and a few noise draws. */
static double cer(scenario_t s, unsigned seeds)
{
    double sum = 0.0;
    unsigned count = 0U;
    for (unsigned m = 0U; m < sizeof(messages) / sizeof(messages[0]); ++m)
        for (unsigned k = 0U; k < seeds; ++k) {
            s.seed = 1000U * m + k + 1U;
            sum += run(&s, messages[m], NULL, 0U, NULL);
            ++count;
        }
    return sum / count;
}

/* Lowest SNR (2500 Hz, 1 dB steps down from +10) whose CER stays <= limit;
 * 99 if even +10 dB does not. */
static double threshold_db(scenario_t s, double limit, unsigned seeds)
{
    double last_ok = 99.0;
    for (double snr = 10.0; snr >= -24.0; snr -= 1.0) {
        s.snr2500_db = snr;
        if (cer(s, seeds) <= limit) last_ok = snr;
        else break;
    }
    return last_ok;
}

static char noise_text[RADIO_CWDEC_TEXT + 1U];

static unsigned noise_only_chars(double seconds, uint8_t bw_index, uint64_t seed)
{
    radio_dsp_t dsp;
    RADIO_DspInit(&dsp, (float)FS, 0.0f, RADIO_RX_CW);
    RADIO_DspSetCw(&dsp, RADIO_CW_PITCH_DEFAULT_HZ, bw_index, RADIO_CW_AGC_SLOW);
    radio_cwdec_t d;
    decoder_init(&d, bw_index);
    rng_state = seed;
    float ai = 0.0f, aq = 0.0f;
    unsigned an = 0U;
    for (unsigned k = 0U; k < (unsigned)(seconds * FS); ++k) {
        (void)RADIO_DspDemodulate(&dsp, (float)(0.001 * gauss()), (float)(0.001 * gauss()));
        ai += dsp.last_i; aq += dsp.last_q;
        if (++an == RADIO_CWDEC_BLOCK) { (void)RADIO_CwDecoderStep(&d, ai, aq); ai = aq = 0.0f; an = 0U; }
    }
    memcpy(noise_text, d.text, sizeof(noise_text));
    return d.chars;
}

static uint8_t bw_index_of(unsigned hz)
{
    static const uint16_t table[RADIO_CW_BW_COUNT] = RADIO_CW_BW_TABLE;
    for (uint8_t i = 0U; i < RADIO_CW_BW_COUNT; ++i) if (table[i] == hz) return i;
    return 0U;
}

static void sweep(void)
{
    const double speeds[] = {8.0, 12.0, 18.0, 25.0, 35.0};
    const double offsets[] = {0.0, 50.0};
    printf("CW decoder, 500 Hz filter: lowest SNR (dB in 2500 Hz / in 500 Hz) with CER <= 5%% and <= 20%%\n");
    printf("%6s %8s %16s %16s\n", "WPM", "offset", "CER<=5%", "CER<=20%");
    for (unsigned a = 0U; a < 5U; ++a)
        for (unsigned b = 0U; b < 2U; ++b) {
            scenario_t s = {speeds[a], offsets[b], 0.0, bw_index_of(500U), 0.0, 0.0, 1U, 0.0};
            const double t5 = threshold_db(s, 0.05, 3U), t20 = threshold_db(s, 0.20, 3U);
            printf("%6.0f %6.0f Hz %7.0f / %5.0f %9.0f / %5.0f\n", speeds[a], offsets[b],
                   t5, t5 + 7.0, t20, t20 + 7.0);
        }
    printf("\nBy CW filter bandwidth, 18 WPM on the pitch, CER <= 5%%:\n");
    const unsigned bws[] = {200U, 300U, 400U, 500U, 700U, 1000U};
    for (unsigned b = 0U; b < 6U; ++b) {
        scenario_t s = {18.0, 0.0, 0.0, bw_index_of(bws[b]), 0.0, 0.0, 1U, 0.0};
        printf("  %4u Hz: %4.0f dB (2500 Hz)\n", bws[b], threshold_db(s, 0.05, 3U));
    }
    printf("\nHarder cases, 18 WPM, 500 Hz, CER <= 5%%:\n");
    scenario_t fade = {18.0, 0.0, 0.0, bw_index_of(500U), 0.3, 0.0, 1U, 0.0};
    printf("  QSB +-6 dB at 0.3 Hz:      %4.0f dB\n", threshold_db(fade, 0.05, 3U));
    scenario_t adj = {18.0, 0.0, 0.0, bw_index_of(500U), 0.0, 400.0, 1U, 0.0};
    printf("  +20 dB station 400 Hz off: %4.0f dB\n", threshold_db(adj, 0.05, 3U));
    scenario_t off = {18.0, 90.0, 0.0, bw_index_of(500U), 0.0, 0.0, 1U, 0.0};
    printf("  mistuned 90 Hz:            %4.0f dB\n", threshold_db(off, 0.05, 3U));
    scenario_t deep = {18.0, 0.0, 0.0, bw_index_of(500U), 0.0, 0.0, 1U, 0.0};
    deep.qsb_hz = 0.5;
    qsb_db = 10.0;
    printf("  QSB +-10 dB at 0.5 Hz:     %4.0f dB\n", threshold_db(deep, 0.05, 3U));
    qsb_db = 6.0;
    scenario_t fw10 = {18.0, 0.0, 0.0, bw_index_of(500U), 0.0, 0.0, 1U, 10.0};
    printf("  Farnsworth 18/10 WPM:      %4.0f dB\n", threshold_db(fw10, 0.05, 3U));
    scenario_t fw5 = {18.0, 0.0, 0.0, bw_index_of(500U), 0.0, 0.0, 1U, 5.0};
    printf("  Farnsworth 18/5 WPM:       %4.0f dB\n", threshold_db(fw5, 0.05, 3U));
    unsigned false_chars = 0U;
    for (unsigned b = 0U; b < 6U; ++b) false_chars += noise_only_chars(60.0, bw_index_of(bws[b]), 77U + b);
    printf("\nNoise only, 60 s per bandwidth, 6 bandwidths: %u characters printed\n", false_chars);
}

/* One line for parameter tuning: mean steady-state CER at four SNRs over
 * three speeds, CER through QSB (+-6 dB at 0.3 Hz, 18 WPM) at +10 and 0 dB,
 * and what 60 s of noise prints. CW_SEEDS sets the noise draws per message
 * (default 2). */
static void score(void)
{
    const double speeds[] = {12.0, 20.0, 30.0};
    const double snrs[] = {-6.0, -8.0, -10.0, -12.0};
    const char *env = getenv("CW_SEEDS");
    const unsigned seeds = (env && atoi(env) > 0) ? (unsigned)atoi(env) : 2U;
    printf("steady CER");
    for (unsigned k = 0U; k < 4U; ++k) {
        double sum = 0.0;
        for (unsigned a = 0U; a < 3U; ++a) {
            scenario_t sc = {speeds[a], 20.0, snrs[k], bw_index_of(500U), 0.0, 0.0, 1U, 0.0};
            sum += cer(sc, seeds);
        }
        printf("  %+.0f dB %5.1f%%", snrs[k], 100.0 * sum / 3.0);
    }
    for (double snr = 10.0; snr >= 0.0; snr -= 10.0) {
        scenario_t fade = {18.0, 20.0, snr, bw_index_of(500U), 0.3, 0.0, 1U, 0.0};
        printf("  QSB %+.0f dB %5.1f%%", snr, 100.0 * cer(fade, seeds));
    }
    printf("  noise-only chars/min %u\n", noise_only_chars(60.0, bw_index_of(500U), 99U) +
                                         noise_only_chars(60.0, bw_index_of(200U), 98U));
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--score") == 0) { score(); return 0; }
    if (argc > 1 && strcmp(argv[1], "--sweep") == 0) { sweep(); return 0; }
    if (argc > 1 && strcmp(argv[1], "--noise") == 0) {
        /* What noise alone prints, per filter and seed: "--noise [seeds]". */
        const unsigned bws[] = {200U, 300U, 400U, 500U, 700U, 1000U};
        const unsigned seeds = argc > 2 ? (unsigned)atoi(argv[2]) : 1U;
        for (unsigned b = 0U; b < 6U; ++b)
            for (unsigned k = 0U; k < seeds; ++k) {
                const unsigned n = noise_only_chars(60.0, bw_index_of(bws[b]), 77U + b + 100U * k);
                printf("%4u Hz seed %u: %2u chars  \"%s\"\n", bws[b], k, n, noise_text);
            }
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--one") == 0) {
        scenario_t s = {18.0, 0.0, atof(argv[2]), bw_index_of(500U), 0.0, 0.0, 1U, 0.0};
        if (argc > 3) s.wpm = atof(argv[3]);
        if (argc > 4) s.offset_hz = atof(argv[4]);
        if (argc > 5) s.qsb_hz = atof(argv[5]);
        if (argc > 6) s.eff_wpm = atof(argv[6]);
        if (argc > 7) qsb_db = atof(argv[7]);
        if (getenv("CW_RATIO")) key_ratio = atof(getenv("CW_RATIO"));
        if (getenv("CW_WEIGHT")) key_weight = atof(getenv("CW_WEIGHT"));
        for (unsigned m = 0U; m < sizeof(messages) / sizeof(messages[0]); ++m) {
            char got[256];
            radio_cwdec_t d;
            s.seed = 1000U * m + (getenv("CW_SEED") ? (uint64_t)atoi(getenv("CW_SEED")) : 1U);
            steady_state = 0;
            double e = run(&s, messages[m], got, sizeof(got), &d);
            printf("%5.1f%%  sent \"%s\"  got \"%s\"  wpm %u off %.1f Hz snr %.1f dB\n", 100.0 * e,
                   messages[m], got, RADIO_CwDecoderWpm(&d), (double)RADIO_CwDecoderOffsetHz(&d),
                   (double)RADIO_CwDecoderSnrDb(&d));
        }
        return 0;
    }
    int fail = 0;
    const clock_t start = clock();
    /* Clean copy (+10 dB), 40 Hz off the pitch, from a slow learner's 8 WPM
     * to 30; every run opens with half a second of noise, so the decoder
     * also has to take a strong station arriving out of noise. */
    const double speeds[] = {8.0, 12.0, 20.0, 30.0};
    for (unsigned a = 0U; a < 4U; ++a) {
        scenario_t s = {speeds[a], 40.0, 10.0, bw_index_of(500U), 0.0, 0.0, 1U, 0.0};
        const double e = cer(s, 1U);
        if (e > 0.02) { fprintf(stderr, "clean %.0f WPM: CER %.1f%%\n", speeds[a], 100.0 * e); fail = 1; }
    }
    /* Farnsworth, as W1AW practice and most trainers send: characters at 18
     * WPM, 8 WPM overall. The letters of a word must not come out spaced. */
    scenario_t farns = {18.0, 40.0, 10.0, bw_index_of(500U), 0.0, 0.0, 1U, 8.0};
    const double ef = cer(farns, 1U);
    if (ef > 0.02) { fprintf(stderr, "Farnsworth 18/8 WPM: CER %.1f%%\n", 100.0 * ef); fail = 1; }
    /* The first word as well: decoding starts from the mark that raised the
     * lock, not at the next character gap. */
    steady_state = 0;
    scenario_t first = {18.0, 40.0, 10.0, bw_index_of(500U), 0.0, 0.0, 1U, 0.0};
    const double e1 = cer(first, 2U);
    steady_state = 1;
    if (e1 > FIRST_LIMIT) { fprintf(stderr, "first word, 18 WPM: CER %.1f%%\n", 100.0 * e1); fail = 1; }
    /* Fists other than a keyer's, 20 WPM clean: light weighting (+0.3 dit),
     * short dahs (2.3 dits) and a bug's long ones (4.5). */
    static const double fists[3][2] = {{3.0, 0.3}, {2.3, 0.0}, {4.5, 0.0}};
    double efist = 0.0;
    for (unsigned k = 0U; k < 3U; ++k) {
        key_ratio = fists[k][0];
        key_weight = fists[k][1];
        scenario_t fist = {20.0, 40.0, 10.0, bw_index_of(500U), 0.0, 0.0, 1U, 0.0};
        const double e = cer(fist, 1U);
        if (e > FIST_LIMIT) {
            fprintf(stderr, "fist %.1f:1 weight %+.1f: CER %.1f%%\n", key_ratio, key_weight, 100.0 * e);
            fail = 1;
        }
        if (e > efist) efist = e;
    }
    key_ratio = 3.0;
    key_weight = 0.0;
    /* QSB, +-6 dB at 0.3 Hz. */
    scenario_t fade = {18.0, 20.0, 10.0, bw_index_of(500U), 0.3, 0.0, 1U, 0.0};
    const double eq = cer(fade, 2U);
    if (eq > 0.03) { fprintf(stderr, "QSB at +10 dB: CER %.1f%%\n", 100.0 * eq); fail = 1; }
    /* Weak signals: bounds from --sweep with margin. */
    scenario_t weak = {18.0, 0.0, -6.0, bw_index_of(500U), 0.0, 0.0, 1U, 0.0};
    const double e6 = cer(weak, 2U);
    if (e6 > WEAK6_LIMIT) { fprintf(stderr, "18 WPM at -6 dB/2500 Hz: CER %.1f%%\n", 100.0 * e6); fail = 1; }
    weak.snr2500_db = -8.0;
    const double e8 = cer(weak, 2U);
    if (e8 > WEAK8_LIMIT) { fprintf(stderr, "18 WPM at -8 dB/2500 Hz: CER %.1f%%\n", 100.0 * e8); fail = 1; }
    /* A 700 Hz filter (the default) holds stations the operator hears well
     * off the pitch: one 250 Hz off is decoded like one on it. */
    scenario_t wide = {18.0, 250.0, 10.0, bw_index_of(700U), 0.0, 0.0, 1U, 0.0};
    const double ew = cer(wide, 1U);
    if (ew > 0.02) { fprintf(stderr, "700 Hz filter, 250 Hz off the pitch: CER %.1f%%\n", 100.0 * ew); fail = 1; }
    /* Zero-beat: a carrier on the dial reads 0 Hz off the pitch, inside the
     * home screen's ZERO window (+-10 Hz, home_cw_tune in ui_render.c), and
     * one 40 Hz above or below reads +-40, the side the tuning bar marks. */
    double zero_err = 0.0;
    for (int k = -1; k <= 1; ++k) {
        scenario_t z = {18.0, 40.0 * k, 10.0, bw_index_of(500U), 0.0, 0.0, 7U, 0.0};
        char got[256];
        radio_cwdec_t d;
        (void)run(&z, messages[0], got, sizeof(got), &d);
        const double err = fabs((double)RADIO_CwDecoderOffsetHz(&d) - z.offset_hz);
        if (!RADIO_CwDecoderLocked(&d) || err > (k ? 5.0 : 3.0)) {
            fprintf(stderr, "carrier %+.0f Hz off the dial: read %+.1f Hz, locked %u\n", z.offset_hz,
                    (double)RADIO_CwDecoderOffsetHz(&d), (unsigned)RADIO_CwDecoderLocked(&d));
            fail = 1;
        }
        if (err > zero_err) zero_err = err;
    }
    /* Blocks the main loop could not take: a short hole passes unnoticed, a
     * long one advances the time base and drops the character under way. */
    {
        radio_cwdec_t h;
        decoder_init(&h, bw_index_of(500U));
        h.sym_bits = 1U; h.sym_len = 2U; h.synced = 1U;
        const uint32_t t0 = h.now_ms;
        RADIO_CwDecoderLost(&h, 20U);
        if (h.now_ms != t0 || h.sym_len != 2U) { fprintf(stderr, "a 20 ms hole was not passed over\n"); fail = 1; }
        RADIO_CwDecoderLost(&h, 400U);
        if (h.now_ms != t0 + 400U || h.sym_len || h.synced) { fprintf(stderr, "a 400 ms hole kept the character\n"); fail = 1; }
    }
    /* Noise alone prints nothing much, through every CW filter. */
    unsigned junk = 0U;
    for (uint8_t b = 0U; b < RADIO_CW_BW_COUNT; ++b) junk += noise_only_chars(60.0, b, 99U + b);
    if (junk > 3U) { fprintf(stderr, "noise only: %u characters in %u x 60 s\n", junk, (unsigned)RADIO_CW_BW_COUNT); fail = 1; }
    /* The decoder is main-loop state of a couple of kilobytes. */
    if (sizeof(radio_cwdec_t) > 2048U) { fprintf(stderr, "decoder state %zu bytes\n", sizeof(radio_cwdec_t)); fail = 1; }
    if (fail) return 1;
    printf("CW decoder: clean 8-30 WPM and Farnsworth 18/8; first word CER %.1f%%; fists (weight +0.3, "
           "dahs 2.3/4.5) worst CER %.1f%%; QSB CER %.1f%%; 18 WPM at -6/-8 dB SNR (2500 Hz) "
           "CER %.1f%%/%.1f%%; 250 Hz off in 700 Hz CER %.1f%%; offset 0/+-40 Hz within %.1f Hz; "
           "%u chars from %u min of noise; %zu bytes (%.1f s host): PASS\n",
           100.0 * e1, 100.0 * efist, 100.0 * eq, 100.0 * e6, 100.0 * e8, 100.0 * ew, zero_err, junk,
           (unsigned)RADIO_CW_BW_COUNT, sizeof(radio_cwdec_t),
           (double)(clock() - start) / CLOCKS_PER_SEC);
    return 0;
}
