/* CW decoder stress bench: the production decoder behind the production CW
 * receive DSP, over real-world impairments the regression suite
 * (cw_decoder_test.c) does not assert: speed, tuning, drift and chirp,
 * fists (dah ratio, weighting, jitter, bugs, hand keys), Farnsworth,
 * fading (sine, Rayleigh, flutter), QRN, QRM and birdies, the CW filters,
 * QSOs of several stations and noise alone. One line per scenario: mean CER
 * over seeds (each seed also picks a different message), and for noise-only
 * cases the characters printed; "first" is the delay from the wanted signal's
 * start to the first character printed, "burst" the most characters printed
 * by one decoder step (a long search that ends in a dump of guesses shows
 * there). Not part of the host suite (it forks one
 * process per scenario, ~30 s on 16 cores); run it for decoder changes and
 * compare its output before and after.
 *
 *   cc -std=gnu11 -O2 -I Core/Inc -o cw_decoder_stress \
 *      tools/tests/cw_decoder_stress.c Core/Src/radio_cw_decoder.c \
 *      Core/Src/mod_morse_tables.c Core/Src/radio_dsp.c Core/Src/radio_config.c -lm
 *   cw_decoder_stress [filter]    all scenarios whose name contains filter
 *   cw_decoder_stress -v name     one scenario, decoded text per seed
 * env SEEDS (default 4), JOBS (default 16). */
#include "radio_cw_decoder.h"
#include "radio_dsp.h"
#include "radio_config.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define FS 48000.0
#define TAU 6.283185307179586

static const struct { char c; const char *code; } morse[] = {
    {'A', ".-"}, {'B', "-..."}, {'C', "-.-."}, {'D', "-.."}, {'E', "."}, {'F', "..-."},
    {'G', "--."}, {'H', "...."}, {'I', ".."}, {'J', ".---"}, {'K', "-.-"}, {'L', ".-.."},
    {'M', "--"}, {'N', "-."}, {'O', "---"}, {'P', ".--."}, {'Q', "--.-"}, {'R', ".-."},
    {'S', "..."}, {'T', "-"}, {'U', "..-"}, {'V', "...-"}, {'W', ".--"}, {'X', "-..-"},
    {'Y', "-.--"}, {'Z', "--.."}, {'0', "-----"}, {'1', ".----"}, {'2', "..---"},
    {'3', "...--"}, {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
    {'8', "---.."}, {'9', "----."}, {'/', "-..-."}, {'?', "..--.."}, {'=', "-...-"},
    {'.', ".-.-.-"}, {',', "--..--"},
};
static const char *code_for(char c)
{
    for (unsigned i = 0U; i < sizeof(morse) / sizeof(morse[0]); ++i)
        if (morse[i].c == c) return morse[i].code;
    return NULL;
}

static const char *const messages[] = {
    "CQ CQ DE DL1ABC DL1ABC K",
    "GE OM TNX FER CALL UR RST 579 579 NAME JOE",
    "QTH NR PARIS = RIG QMX PWR 5W ANT DIPOLE",
    "W1AW DE G4XYZ 5NN 14 BK",
    "VVV QRL? PSE QSY 7030 KHZ",
    "OK TNX FER QSO 73 ES GL SK",
    "HR WX IS SUNNY ES WARM TEMP 25C",
    "R R FB DR OM BTU",
};
#define NMSG (sizeof(messages) / sizeof(messages[0]))

/* ---- random ---- */
typedef struct { uint64_t s; } rng_t;
static double uni(rng_t *r)
{
    r->s ^= r->s >> 12; r->s ^= r->s << 25; r->s ^= r->s >> 27;
    return ((r->s * 2685821657736338717ULL) >> 11) * (1.0 / 9007199254740992.0);
}
static double gauss(rng_t *r)
{
    double u = uni(r), v = uni(r);
    if (u < 1e-300) u = 1e-300;
    return sqrt(-2.0 * log(u)) * cos(TAU * v);
}

/* ---- transmissions ---- */
typedef struct {
    const char *text;       /* NULL: messages[seed] */
    double wpm, eff_wpm;    /* eff: Farnsworth overall speed, 0 none */
    double wpm_end;         /* speed drifts linearly to this over the text, 0 none */
    double ratio;           /* dah/dit, 0: 3 */
    double weight;          /* dits added to each mark, taken from each space */
    double jitter;          /* sigma of each element and space, fraction */
    double cgap, wgap;      /* character and word gaps in dits, 0: 3 and 7 */
    double at;              /* start, s; <0: this long after the previous tx ends */
    double offset, drift, chirp, chirp_ms, step_at, step_hz;
    double level_db;        /* vs the reference level the SNR is quoted for */
    double rise_ms;         /* 0: 5 */
    int wanted;             /* its text is expected */
    double carrier_s;       /* >0: steady carrier this long instead of Morse */
    double sweep_hz_s;      /* carrier sweeps (tune-up) */
} tx_t;

typedef struct {
    const char *name;
    double snr;             /* 2500 Hz, for the reference level */
    unsigned bw;            /* Hz; 0: 500 */
    int fade;               /* 0 none, 1 sine, 2 rayleigh */
    double fade_db, fade_hz;
    double qrn_rate, qrn_db, qrn_ms; /* crashes per s, peak vs signal, decay */
    int no_noise;
    double tail_s;          /* noise after the last tx, 0: 1 */
    double noise_s;         /* noise-only run of this length (no tx) */
    int ntx;
    tx_t tx[4];
    unsigned drop_every_ms, drop_ms; /* the main loop falls behind: blocks lost */
    int score_after_wanted; /* text printed before the wanted signal starts is not scored */
} scen_t;

typedef struct { double on, off, f; } el_t;
typedef struct {
    el_t *el; unsigned n, cur;
    double amp, rise, end, start;
    const tx_t *t;
    double phase;
} keyed_t;

static unsigned key_tx(const tx_t *t, const char *text, double start, rng_t *r, el_t *out, unsigned cap, double *end)
{
    const double ratio = t->ratio > 0.0 ? t->ratio : 3.0;
    const double cg = t->cgap > 0.0 ? t->cgap : 3.0, wg = t->wgap > 0.0 ? t->wgap : 7.0;
    const size_t len = strlen(text);
    double tt = start;
    unsigned n = 0U;
    for (size_t i = 0; i < len; ++i) {
        const double wpm = t->wpm_end > 0.0 ? t->wpm + (t->wpm_end - t->wpm) * (double)i / (double)(len ? len : 1) : t->wpm;
        const double dit = 1.2 / wpm;
        double c_gap = cg * dit, w_gap = wg * dit;
        if (t->eff_wpm > 0.0 && t->eff_wpm < wpm) {
            const double ta = (60.0 * wpm - 37.2 * t->eff_wpm) / (wpm * t->eff_wpm);
            c_gap = 3.0 * ta / 19.0; w_gap = 7.0 * ta / 19.0;
        }
        const char c = text[i];
        if (c == ' ') { tt += w_gap - c_gap; continue; }
        const char *code = code_for(c);
        if (!code) continue;
        for (const char *e = code; *e; ++e) {
            double m = ((*e == '-') ? ratio : 1.0) * dit + t->weight * dit;
            double s = dit - t->weight * dit;
            if (t->jitter > 0.0) {
                m *= 1.0 + t->jitter * gauss(r);
                s *= 1.0 + t->jitter * gauss(r);
            }
            if (m < 0.3 * dit) m = 0.3 * dit;
            if (s < 0.3 * dit) s = 0.3 * dit;
            if (n < cap) { out[n].on = tt; out[n].off = tt + m; ++n; }
            tt += m;
            if (e[1]) tt += s;
        }
        double g = c_gap - t->weight * dit;
        if (t->jitter > 0.0) g *= 1.0 + t->jitter * gauss(r);
        if (g < 0.5 * dit) g = 0.5 * dit;
        tt += g;
    }
    *end = tt;
    return n;
}

static double env_at(keyed_t *k, double t)
{
    if (k->t->carrier_s > 0.0) {
        if (t < k->start || t > k->end) return 0.0;
        const double r = k->rise;
        if (t < k->start + r) return 0.5 - 0.5 * cos(M_PI * (t - k->start) / r);
        if (t > k->end - r) return 0.5 - 0.5 * cos(M_PI * (k->end - t) / r);
        return 1.0;
    }
    const double r = k->rise;
    while (k->cur < k->n && k->el[k->cur].off + r < t) ++k->cur;
    double best = 0.0;
    for (unsigned c = k->cur; c < k->n && c < k->cur + 2U; ++c) {
        const el_t *e = &k->el[c];
        double v;
        if (t < e->on - r * 0.5 || t > e->off + r * 0.5) v = 0.0;
        else if (t < e->on + r * 0.5) v = 0.5 - 0.5 * cos(M_PI * (t - (e->on - r * 0.5)) / r);
        else if (t > e->off - r * 0.5) v = 0.5 + 0.5 * cos(M_PI * (t - (e->off - r * 0.5)) / r);
        else v = 1.0;
        if (v > best) best = v;
    }
    return best;
}

/* frequency of tx k at time t (chirp from its current element's start) */
static double freq_at(const keyed_t *k, double t)
{
    const tx_t *x = k->t;
    double f = x->offset + x->drift * t;
    if (x->step_at > 0.0 && t >= x->step_at) f += x->step_hz;
    if (x->sweep_hz_s != 0.0 && t >= k->start) f += x->sweep_hz_s * (t - k->start);
    if (x->chirp != 0.0 && k->n) {
        unsigned c = k->cur < k->n ? k->cur : k->n - 1U;
        if (k->el[c].on > t && c) --c;
        const double since = t - k->el[c].on;
        if (since >= -0.003) f += x->chirp * exp(-(since > 0 ? since : 0) / (x->chirp_ms * 1e-3));
    }
    return f;
}

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

static uint8_t bw_index_of(unsigned hz)
{
    static const uint16_t table[RADIO_CW_BW_COUNT] = RADIO_CW_BW_TABLE;
    for (uint8_t i = 0U; i < RADIO_CW_BW_COUNT; ++i) if (table[i] == hz) return i;
    return 3U;
}

/* Runs one scenario with one seed: returns CER (or printed chars for a
 * noise-only case) and the texts. */
static double run(const scen_t *sc, unsigned seed, char *want_out, char *got_out, size_t cap, unsigned *chars_out,
                  double *first_out, unsigned *burst_out)
{
    static el_t els[4][4096];
    keyed_t k[4];
    rng_t key_rng = {seed * 0x9E3779B97F4A7C15ULL + 777U};
    rng_t noise_rng = {seed * 0xD1B54A32D192ED03ULL + 12345U};
    rng_t fade_rng = {seed * 0x94D049BB133111EBULL + 99U};
    char want[2048] = "";
    double prev_end = 0.5, total = sc->noise_s > 0.0 ? sc->noise_s : 0.0, wanted_at = -1.0;
    for (int i = 0; i < sc->ntx; ++i) {
        const tx_t *t = &sc->tx[i];
        const char *text = t->text ? t->text : messages[(seed + (unsigned)i) % NMSG];
        const double start = t->at >= 0.0 ? t->at + (i == 0 && t->at == 0.0 ? 0.5 : 0.0) : prev_end - t->at;
        k[i].t = t;
        k[i].el = els[i];
        k[i].cur = 0U;
        k[i].amp = 0.01 * pow(10.0, t->level_db / 20.0);
        k[i].rise = (t->rise_ms > 0.0 ? t->rise_ms : 5.0) * 1e-3;
        k[i].start = start;
        k[i].phase = uni(&key_rng) * TAU;
        if (t->carrier_s > 0.0) { k[i].n = 0U; k[i].end = start + t->carrier_s; }
        else k[i].n = key_tx(t, text, start, &key_rng, els[i], 4096U, &k[i].end);
        if (t->wanted) {
            if (wanted_at < 0.0) wanted_at = start;
            if (want[0]) strcat(want, " ");
            strcat(want, text);
        }
        prev_end = k[i].end;
        if (k[i].end > total) total = k[i].end;
    }
    total += sc->tail_s > 0.0 ? sc->tail_s : 1.0;
    const double n0 = 0.01 * 0.01 / (2500.0 * pow(10.0, sc->snr / 10.0));
    const double sigma = sc->no_noise ? 0.0 : sqrt(n0 * FS / 2.0);
    /* Rayleigh: sum of sinusoids, 16 paths, unit mean power */
    double fd_f[16], fd_p[16];
    for (int m = 0; m < 16; ++m) {
        fd_f[m] = sc->fade_hz * cos(TAU * uni(&fade_rng));
        fd_p[m] = TAU * uni(&fade_rng);
    }
    radio_dsp_t dsp;
    RADIO_DspInit(&dsp, (float)FS, 0.0f, RADIO_RX_CW);
    RADIO_DspSetCw(&dsp, RADIO_CW_PITCH_DEFAULT_HZ, bw_index_of(sc->bw ? sc->bw : 500U), RADIO_CW_AGC_SLOW);
    static radio_cwdec_t d;
    RADIO_CwDecoderInit(&d);
#ifdef RADIO_CWDEC_NEAR
    RADIO_CwDecoderSetSpan(&d, (uint16_t)(sc->bw ? sc->bw : 500U));
#endif
    unsigned block = 0U, lost = 0U, burst = 0U;
    double first = -1.0;
    static char got[8192];
    size_t got_n = 0U;
    uint32_t seen = 0U;
    float ai = 0.0f, aq = 0.0f;
    unsigned an = 0U;
    const unsigned samples = (unsigned)(total * FS);
    double gi = 1.0, gq = 0.0; /* fade gain, updated each ms */
    double qrn_amp = 0.0, qrn_decay = sc->qrn_ms > 0.0 ? exp(-1.0 / (FS * sc->qrn_ms * 1e-3)) : 0.0;
    const double qrn_peak = 0.01 * pow(10.0, sc->qrn_db / 20.0);
    for (unsigned s = 0U; s < samples; ++s) {
        const double t = s / FS;
        if (s % 48U == 0U) {
            if (sc->fade == 1) { gi = pow(10.0, sc->fade_db / 20.0 * sin(TAU * sc->fade_hz * t)); gq = 0.0; }
            else if (sc->fade == 2) {
                double a = 0.0, b = 0.0;
                for (int m = 0; m < 16; ++m) { a += cos(TAU * fd_f[m] * t + fd_p[m]); b += sin(TAU * fd_f[m] * t + fd_p[m]); }
                gi = a / 4.0; gq = b / 4.0;
            }
        }
        double si = 0.0, sq = 0.0;
        for (int i = 0; i < sc->ntx; ++i) {
            keyed_t *x = &k[i];
            const double f = freq_at(x, t);
            x->phase += TAU * f / FS;
            if (x->phase > TAU) x->phase -= TAU;
            if (x->phase < 0.0) x->phase += TAU;
            const double e = env_at(x, t);
            if (e <= 0.0) continue;
            double a = x->amp * e;
            double ci = a * cos(x->phase), cq = a * sin(x->phase);
            if (x->t->wanted && sc->fade) { const double ri = ci * gi - cq * gq; cq = ci * gq + cq * gi; ci = ri; }
            si += ci; sq += cq;
        }
        if (sigma > 0.0) { si += sigma * gauss(&noise_rng); sq += sigma * gauss(&noise_rng); }
        if (sc->qrn_rate > 0.0) {
            if (uni(&noise_rng) < sc->qrn_rate / FS) qrn_amp = qrn_peak;
            if (qrn_amp > 1e-9) {
                si += qrn_amp * gauss(&noise_rng) * 0.7071;
                sq += qrn_amp * gauss(&noise_rng) * 0.7071;
                qrn_amp *= qrn_decay;
            }
        }
        (void)RADIO_DspDemodulate(&dsp, (float)si, (float)sq);
        ai += dsp.last_i; aq += dsp.last_q;
        if (++an == RADIO_CWDEC_BLOCK) {
            const uint32_t chars = d.chars;
            if (sc->drop_ms && (block++ % sc->drop_every_ms) >= sc->drop_every_ms - sc->drop_ms) {
                ++lost; /* the ring overflowed: this block never reaches the decoder */
            } else {
#ifdef RADIO_CWDEC_NEAR
                if (lost) RADIO_CwDecoderLost(&d, lost);
#endif
                lost = 0U;
                (void)RADIO_CwDecoderStep(&d, ai, aq);
            }
            ai = aq = 0.0f; an = 0U;
            if (d.chars - chars > burst) burst = d.chars - chars;
            if (first < 0.0 && d.chars && wanted_at >= 0.0) first = t - wanted_at;
            if (d.emitted != seen) {
                uint32_t n = d.emitted - seen;
                if (n > d.text_len) n = d.text_len;
                if (!sc->score_after_wanted || t >= wanted_at)
                    for (uint32_t j = d.text_len - n; j < d.text_len && got_n + 1U < sizeof(got); ++j) got[got_n++] = d.text[j];
                seen = d.emitted;
            }
        }
    }
    got[got_n] = '\0';
    char w[2048], g[8192];
    normalise(want, w, sizeof(w));
    normalise(got, g, sizeof(g));
    if (want_out) snprintf(want_out, cap, "%s", w);
    if (got_out) snprintf(got_out, cap, "%s", g);
    if (chars_out) *chars_out = d.chars;
    if (first_out) *first_out = first;
    if (burst_out) *burst_out = burst;
    if (!w[0]) return (double)d.chars;
    return (double)edit_distance(g, w) / (double)strlen(w);
}

/* ---- scenarios ---- */
#define TX(...) {__VA_ARGS__}
#define W(wpm_) .wpm = (wpm_), .wanted = 1
static const scen_t scen[] = {
    /* reference */
    {"ref18_clean", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .offset = 20)}},
    {"ref18_-6dB", -6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .offset = 20)}},
    {"ref18_-9dB", -9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .offset = 20)}},
    {"nonoise_18", 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, {TX(W(18), .offset = 20)}},
    {"strong_+40dB", 40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .offset = 20)}},
    {"start_at_0", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .offset = 20, .at = 0.001)}},
    /* speed */
    {"wpm5", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(5))}},
    {"wpm8_-3dB", -3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(8), .offset = 20)}},
    {"wpm12_-6dB", -6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(12), .offset = 20)}},
    {"wpm25_-6dB", -6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(25), .offset = 20)}},
    {"wpm35_clean", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(35), .offset = 20)}},
    {"wpm35_-3dB", -3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(35), .offset = 20)}},
    {"wpm45_clean", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(45), .offset = 20)}},
    {"wpm55_clean", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(55), .offset = 20)}},
    {"wpm45_1k_clean", 10, 1000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(45), .offset = 20)}},
    {"speedup_15to30", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(15), .wpm_end = 30, .text = "CQ CQ CQ DE DL1ABC DL1ABC DL1ABC PSE K CQ CQ DE DL1ABC K")}},
    {"slowdown_30to15", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(30), .wpm_end = 15, .text = "CQ CQ CQ DE DL1ABC DL1ABC DL1ABC PSE K CQ CQ DE DL1ABC K")}},
    /* tuning */
    {"off0_12wpm_0dB", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(12))}},
    {"off37_12wpm_0dB", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(12), .offset = 37)}},
    {"off-73_12wpm_0dB", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(12), .offset = -73)}},
    {"off95_18wpm_0dB", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .offset = 95)}},
    {"drift1Hz/s_10wpm", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(10), .offset = -20, .drift = 1.0, .text = "CQ CQ DE DL1ABC DL1ABC DL1ABC PSE K")}},
    {"drift4Hz/s_18wpm", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .offset = -40, .drift = 4.0)}},
    {"chirp20_15wpm", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(15), .offset = 10, .chirp = 20, .chirp_ms = 30)}},
    {"chirp50_20wpm", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(20), .offset = 10, .chirp = 50, .chirp_ms = 20)}},
    {"step10Hz_12wpm", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(12), .offset = 10, .step_at = 5.0, .step_hz = -10)}},
    {"step40Hz_18wpm", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .offset = 20, .step_at = 4.0, .step_hz = -40)}},
    /* fist */
    {"ratio2.3_20wpm", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(20), .ratio = 2.3)}},
    {"ratio2.6_20wpm", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(20), .ratio = 2.6)}},
    {"ratio3.8_20wpm", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(20), .ratio = 3.8)}},
    {"ratio4.5_20wpm", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(20), .ratio = 4.5)}},
    {"weight+0.3_20wpm", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(20), .weight = 0.3)}},
    {"weight+0.5_20wpm", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(20), .weight = 0.5)}},
    {"weight-0.25_20wpm", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(20), .weight = -0.25)}},
    {"jitter10_18wpm", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .jitter = 0.10)}},
    {"jitter20_18wpm", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .jitter = 0.20)}},
    {"jitter30_15wpm", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(15), .jitter = 0.30)}},
    {"jitter15_-4dB", -4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .jitter = 0.15)}},
    {"bug_25wpm", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(25), .ratio = 4.2, .jitter = 0.10, .weight = -0.1)}},
    {"handkey_12wpm", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(12), .ratio = 3.4, .jitter = 0.2, .weight = 0.15, .cgap = 4.0, .wgap = 8.0)}},
    {"shortwords_5dit", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(20), .wgap = 5.0)}},
    {"longchar_4.5dit", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(20), .cgap = 4.5, .wgap = 9.0)}},
    {"farns_20/10", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(20), .eff_wpm = 10)}},
    {"farns_15/5", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(15), .eff_wpm = 5)}},
    {"rise1ms_30wpm", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(30), .rise_ms = 1)}},
    {"rise12ms_30wpm", 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(30), .rise_ms = 12)}},
    /* fading */
    {"qsb6_0.3Hz_+10", 10, 0, 1, 6, 0.3, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .offset = 20)}},
    {"qsb10_0.5Hz_+10", 10, 0, 1, 10, 0.5, 0, 0, 0, 0, 0, 0, 1, {TX(W(18))}},
    {"qsb10_0.5Hz_0", 0, 0, 1, 10, 0.5, 0, 0, 0, 0, 0, 0, 1, {TX(W(18))}},
    {"qsb20_0.2Hz_+15", 15, 0, 1, 20, 0.2, 0, 0, 0, 0, 0, 0, 1, {TX(W(18))}},
    {"rayl0.1_+10", 10, 0, 2, 0, 0.1, 0, 0, 0, 0, 0, 0, 1, {TX(W(18))}},
    {"rayl0.5_+10", 10, 0, 2, 0, 0.5, 0, 0, 0, 0, 0, 0, 1, {TX(W(18))}},
    {"rayl0.5_+3", 3, 0, 2, 0, 0.5, 0, 0, 0, 0, 0, 0, 1, {TX(W(18))}},
    {"rayl2_+10", 10, 0, 2, 0, 2.0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18))}},
    {"flutter10_+10", 10, 0, 2, 0, 10.0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18))}},
    {"flutter20_+15", 15, 0, 2, 0, 20.0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18))}},
    {"rayl0.5_12wpm_+6", 6, 0, 2, 0, 0.5, 0, 0, 0, 0, 0, 0, 1, {TX(W(12))}},
    /* QRN */
    {"qrn2/s_+20dB_5ms", 5, 0, 0, 0, 0, 2, 20, 5, 0, 0, 0, 1, {TX(W(18))}},
    {"qrn1/s_+30dB_20ms", 5, 0, 0, 0, 0, 1, 30, 20, 0, 0, 0, 1, {TX(W(18))}},
    {"qrn8/s_+15dB_2ms", 5, 0, 0, 0, 0, 8, 15, 2, 0, 0, 0, 1, {TX(W(18))}},
    /* QRM */
    {"qrm+20dB_at+400", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, {TX(W(18)), TX(.wpm = 28, .offset = 400, .level_db = 20, .at = 0.2, .text = "TEST DE K1XYZ K1XYZ TEST DE K1XYZ QRL? TEST")}},
    {"qrm+20dB_at+180", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, {TX(W(18)), TX(.wpm = 28, .offset = 180, .level_db = 20, .at = 0.2, .text = "TEST DE K1XYZ K1XYZ TEST DE K1XYZ QRL? TEST")}},
    {"qrm+10dB_at+120", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, {TX(W(18)), TX(.wpm = 22, .offset = 120, .level_db = 10, .at = 0.2, .text = "TEST DE K1XYZ K1XYZ TEST DE K1XYZ QRL? TEST")}},
    {"qrm+6dB_at+70", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, {TX(W(18)), TX(.wpm = 22, .offset = 70, .level_db = 6, .at = 2.5, .text = "TEST DE K1XYZ K1XYZ TEST DE K1XYZ QRL? TEST")}},
    {"qrm0dB_at-60", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, {TX(W(18), .offset = 10), TX(.wpm = 25, .offset = -60, .level_db = 0, .at = 2.5, .text = "TEST DE K1XYZ K1XYZ TEST DE K1XYZ QRL? TEST")}},
    {"qrm-10dB_at+50", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, {TX(W(18)), TX(.wpm = 25, .offset = 50, .level_db = -10, .at = 0.2, .text = "TEST DE K1XYZ K1XYZ TEST DE K1XYZ QRL? TEST")}},
    {"birdie0dB_at-50", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, {TX(W(18), .offset = 20), TX(.carrier_s = 60, .offset = -50, .level_db = -3, .at = 0.1)}},
    {"birdie+6dB_at+80", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, {TX(W(18)), TX(.carrier_s = 60, .offset = 80, .level_db = 6, .at = 0.1)}},
    {"birdie_in_pass_-10", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, {TX(W(18)), TX(.carrier_s = 60, .offset = 5, .level_db = -10, .at = 0.1)}},
    {"tuneup_sweep", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, {TX(W(18), .offset = 20), TX(.carrier_s = 3, .offset = -150, .sweep_hz_s = 100, .level_db = 10, .at = 4)}},
    /* bandwidth */
    {"bw200_18wpm_-3", -3, 200, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18))}},
    {"bw1000_18wpm_-3", -3, 1000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18))}},
    {"bw200_40wpm", 10, 200, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(40))}},
    {"bw300_off90", 5, 300, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .offset = 90)}},
    /* QSOs */
    {"qso_25_then_15", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, {TX(W(25), .offset = 0), TX(W(15), .offset = 30, .level_db = -6, .at = -1.5)}},
    {"qso_15_then_30", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, {TX(W(15), .offset = 10), TX(W(30), .offset = -20, .level_db = 3, .at = -1.5)}},
    {"qso_strong_weak", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, {TX(W(20), .level_db = 15), TX(W(20), .offset = 15, .level_db = -3, .at = -1.2)}},
    {"qso_weak_strong", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, {TX(W(20), .level_db = -3), TX(W(20), .offset = 15, .level_db = 15, .at = -1.2)}},
    {"qso_4overs", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, {TX(W(22), .offset = 5), TX(W(16), .offset = -25, .level_db = -4, .at = -1.0), TX(W(22), .offset = 5, .at = -1.0), TX(W(16), .offset = -25, .level_db = -4, .at = -1.0)}},
    {"after_12s_pause", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, {TX(W(28), .offset = 0), TX(W(14), .offset = 5, .at = -12.0)}},
    /* wide filters (700 Hz is the default since v8.08): off the pitch, a
     * station at the pitch against a stronger one elsewhere in the filter */
    {"bw700_off+200_18wpm", 5, 700, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .offset = 200)}},
    {"bw700_off-300_18wpm", 5, 700, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .offset = -300)}},
    {"bw1000_off+420_18wpm", 5, 1000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .offset = 420)}},
    {"bw700_18wpm_-3", -3, 700, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .offset = 20)}},
    {"bw700_qrm+6dB_at+250", 5, 700, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, {TX(W(18)), TX(.wpm = 25, .offset = 250, .level_db = 6, .at = 0.2, .text = "TEST DE K1XYZ K1XYZ TEST DE K1XYZ QRL? TEST")}},
    /* alone in the filter the off-pitch station is decoded (not scored);
     * from the moment one starts at the pitch, that one is */
    {"bw700_pitch_takes_over", 5, 700, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, {TX(.wpm = 25, .offset = -220, .level_db = 6, .at = 0.2, .text = "TEST DE K1XYZ K1XYZ TEST DE K1XYZ QRL? TEST TEST DE K1XYZ"), TX(W(18), .offset = 15, .at = 4.0)}, .score_after_wanted = 1},
    /* the main loop falling behind the receive worker (ring overflow) */
    {"drops_150ms/4s_18wpm", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .offset = 20)}, 4000, 150},
    {"drops_60ms/1s_18wpm", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(18), .offset = 20)}, 1000, 60},
    {"drops_300ms/6s_12wpm", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, {TX(W(12), .offset = 20)}, 6000, 300},
    /* noise only / junk */
    {"junk_noise_500", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 60, 0, {TX(.wpm = 1)}},
    {"junk_noise_200", 0, 200, 0, 0, 0, 0, 0, 0, 0, 0, 60, 0, {TX(.wpm = 1)}},
    {"junk_qrn", 0, 0, 0, 0, 0, 4, 25, 10, 0, 0, 60, 0, {TX(.wpm = 1)}},
    {"junk_carrier", 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 60, 1, {TX(.carrier_s = 58, .offset = 20, .at = 1)}},
    {"junk_fading_carrier", 5, 0, 2, 0, 0.8, 0, 0, 0, 0, 0, 60, 1, {TX(.carrier_s = 58, .offset = 20, .at = 1, .wanted = 0)}},
    {"junk_flutter_carrier", 10, 0, 2, 0, 12, 0, 0, 0, 0, 0, 60, 1, {TX(.carrier_s = 58, .offset = -30, .at = 1, .wanted = 0)}},
    /* a strong station just outside the 500 Hz filter */
    {"junk_qrm_out_of_range", 0, 500, 0, 0, 0, 0, 0, 0, 0, 0, 60, 1, {TX(.wpm = 20, .offset = 400, .level_db = 20, .at = 1, .text = "TEST DE K1XYZ K1XYZ TEST DE K1XYZ QRL? TEST TEST TEST DE K1XYZ")}},
    /* a weak steady spur near the pitch (a dummy load shows the receiver's
     * own): noise on it must not read as fast Morse */
    {"junk_spur_bw300_-3dB", -3, 300, 0, 0, 0, 0, 0, 0, 0, 0, 60, 1, {TX(.carrier_s = 58, .offset = -20, .at = 1, .wanted = 0)}},
    {"junk_spur_bw300_0dB", 0, 300, 0, 0, 0, 0, 0, 0, 0, 0, 60, 1, {TX(.carrier_s = 58, .offset = -20, .at = 1, .wanted = 0)}},
    {"junk_spur_bw300_+3dB", 3, 300, 0, 0, 0, 0, 0, 0, 0, 0, 60, 1, {TX(.carrier_s = 58, .offset = -20, .at = 1, .wanted = 0)}},
    {"junk_spur_bw700_0dB", 0, 700, 0, 0, 0, 0, 0, 0, 0, 0, 60, 1, {TX(.carrier_s = 58, .offset = -20, .at = 1, .wanted = 0)}},
    {"junk_spur_bw300_-6dB", -6, 300, 0, 0, 0, 0, 0, 0, 0, 0, 60, 1, {TX(.carrier_s = 58, .offset = -20, .at = 1, .wanted = 0)}},
    {"junk_noise_700", 0, 700, 0, 0, 0, 0, 0, 0, 0, 0, 60, 0, {TX(.wpm = 1)}},
    {"junk_noise_1500", 0, 1500, 0, 0, 0, 0, 0, 0, 0, 0, 60, 0, {TX(.wpm = 1)}},
};
#define NSCEN (sizeof(scen) / sizeof(scen[0]))

static void one(unsigned idx, unsigned seeds, int verbose, char *line, size_t cap)
{
    const scen_t *sc = &scen[idx];
    double sum = 0.0, worst = 0.0, first_sum = 0.0;
    unsigned chars = 0U, burst = 0U, firsts = 0U;
    static char want[8192], got[8192];
    const int junk = sc->noise_s > 0.0;
    for (unsigned s = 1U; s <= seeds; ++s) {
        unsigned c = 0U, b = 0U;
        double f = -1.0;
        const double e = run(sc, s, want, got, sizeof(want), &c, &f, &b);
        chars += c;
        if (b > burst) burst = b;
        if (f >= 0.0) { first_sum += f; ++firsts; }
        if (!junk) { sum += e; if (e > worst) worst = e; }
        if (verbose) printf("  seed %u: %5.1f%%\n    want \"%s\"\n    got  \"%s\"\n", s, junk ? 0.0 : 100.0 * e, want, got);
    }
    if (junk) snprintf(line, cap, "%-24s junk chars %u", sc->name, chars);
    else snprintf(line, cap, "%-24s CER %5.1f%%  worst %5.1f%%  first %4.1fs  burst %2u", sc->name, 100.0 * sum / seeds,
                  100.0 * worst, firsts ? first_sum / firsts : -1.0, burst);
}

int main(int argc, char **argv)
{
    const unsigned seeds = getenv("SEEDS") ? (unsigned)atoi(getenv("SEEDS")) : 4U;
    const unsigned jobs = getenv("JOBS") ? (unsigned)atoi(getenv("JOBS")) : 16U;
    if (argc > 2 && strcmp(argv[1], "-v") == 0) {
        for (unsigned i = 0U; i < NSCEN; ++i)
            if (strcmp(scen[i].name, argv[2]) == 0) {
                char line[256];
                one(i, seeds, 1, line, sizeof(line));
                puts(line);
            }
        return 0;
    }
    const char *filter = argc > 1 ? argv[1] : "";
    unsigned list[NSCEN], n = 0U;
    for (unsigned i = 0U; i < NSCEN; ++i) if (strstr(scen[i].name, filter)) list[n++] = i;
    /* One child per scenario, JOBS at a time; each writes its line to a pipe. */
    static char lines[NSCEN][160];
    int fds[NSCEN];
    pid_t pids[NSCEN];
    unsigned next = 0U, running = 0U, done = 0U;
    while (done < n) {
        while (next < n && running < jobs) {
            int p[2];
            if (pipe(p) != 0) return 1;
            const pid_t pid = fork();
            if (pid == 0) {
                close(p[0]);
                char line[160];
                one(list[next], seeds, 0, line, sizeof(line));
                if (write(p[1], line, strlen(line) + 1U) < 0) _exit(1);
                _exit(0);
            }
            close(p[1]);
            fds[next] = p[0];
            pids[next] = pid;
            ++next; ++running;
        }
        int status;
        const pid_t pid = wait(&status);
        for (unsigned j = 0U; j < next; ++j)
            if (pids[j] == pid) {
                ssize_t r = read(fds[j], lines[j], sizeof(lines[j]) - 1U);
                lines[j][r > 0 ? r : 0] = '\0';
                close(fds[j]);
                --running; ++done;
            }
    }
    for (unsigned j = 0U; j < n; ++j) puts(lines[j]);
    return 0;
}
