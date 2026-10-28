/* DIGI TONE's measurer (mod_digi_tone_logic.h): what keys the carrier, at
 * what offset, and what lets it go. */
#include "mod_digi_tone_logic.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    failures++; \
} } while (0)

#define FS 48000.0
#define PI_D 3.14159265358979323846

typedef struct {
    mod_digi_tone_state_t s;
    double ph;           /* the audio's phase, cycles */
    uint32_t first_key;  /* sample the carrier first started rising, or ~0 */
    uint32_t last_off;   /* sample it last reached zero */
    uint32_t t;
} run_t;

static void run_init(run_t *r)
{
    memset(r, 0, sizeof(*r));
    r->first_key = 0xFFFFFFFFU;
}

static void feed(run_t *r, float x)
{
    const float before = r->s.ramp;
    mod_digi_tone_sample(&r->s, x);
    if ((before == 0.0f) && (r->s.ramp > 0.0f) && (r->first_key == 0xFFFFFFFFU)) r->first_key = r->t;
    if ((before > 0.0f) && (r->s.ramp == 0.0f)) r->last_off = r->t;
    r->t++;
}

static void tone(run_t *r, double hz, double amp, double sec)
{
    const uint32_t n = (uint32_t)(sec * FS);
    for (uint32_t i = 0U; i < n; i++) {
        feed(r, (float)(amp * sin(2.0 * PI_D * r->ph)));
        r->ph += hz / FS;
        r->ph -= floor(r->ph);
    }
}

static void silence(run_t *r, double sec)
{
    const uint32_t n = (uint32_t)(sec * FS);
    for (uint32_t i = 0U; i < n; i++) feed(r, 0.0f);
}

/* The carrier's offset over `sec` of the tone, from its phase. */
static double measured_hz(run_t *r, double hz, double amp, double sec)
{
    const uint32_t n = (uint32_t)(sec * FS);
    double turns = 0.0;
    for (uint32_t i = 0U; i < n; i++) {
        feed(r, (float)(amp * sin(2.0 * PI_D * r->ph)));
        r->ph += hz / FS;
        r->ph -= floor(r->ph);
        turns += r->s.step * 0.5;
    }
    return turns / (n / FS);
}

static void test_level_independent_and_exact(void)
{
    const double hz[] = {150.0, 1500.0, 1501.4648, 2999.0, 4800.0};
    const double amp[] = {0.99, 0.1, 0.0045};
    for (unsigned i = 0U; i < sizeof(hz) / sizeof(hz[0]); i++) {
        for (unsigned k = 0U; k < sizeof(amp) / sizeof(amp[0]); k++) {
            run_t r;
            run_init(&r);
            silence(&r, 0.05);
            tone(&r, hz[i], amp[k], 0.2);
            CHECK(r.s.keyed == 1U);
            CHECK(r.s.ramp == 1.0f);
            CHECK(mod_digi_tone_envelope(&r.s) == 1.0f);
            const double got = measured_hz(&r, hz[i], amp[k], 1.0);
            if (fabs(got - hz[i]) > 0.005) {
                fprintf(stderr, "tone %.4f Hz amp %.4f: carrier %.5f Hz\n", hz[i], amp[k], got);
                failures++;
            }
        }
    }
}

static void test_start_is_prompt_and_ramped(void)
{
    run_t r;
    run_init(&r);
    silence(&r, 0.01);
    const uint32_t start = r.t;
    tone(&r, 1500.0, 0.5, 0.05);
    /* Four cycles of 1500 Hz and the fifth crossing: under 4 ms. */
    CHECK(r.first_key != 0xFFFFFFFFU);
    CHECK(r.first_key - start < 192U);
    /* Then 480 samples of smoothstep. */
    run_init(&r);
    tone(&r, 1500.0, 0.5, 0.004);
    float prev = mod_digi_tone_envelope(&r.s);
    unsigned rising = 0U;
    for (unsigned i = 0U; i < 600U; i++) {
        feed(&r, (float)(0.5 * sin(2.0 * PI_D * r.ph)));
        r.ph += 1500.0 / FS;
        const float e = mod_digi_tone_envelope(&r.s);
        CHECK(e >= prev);
        CHECK(e - prev < 0.004f);
        if (e > prev) rising++;
        prev = e;
    }
    CHECK(rising > 400U);
    CHECK(prev == 1.0f);
}

static void test_below_floor_and_out_of_range_never_key(void)
{
    const struct { double hz, amp; } c[] = {
        {1500.0, 0.0028}, /* -51 dBFS */
        {60.0, 0.9},      /* under 100 Hz */
        {6000.0, 0.9},    /* over 5 kHz */
    };
    for (unsigned i = 0U; i < sizeof(c) / sizeof(c[0]); i++) {
        run_t r;
        run_init(&r);
        tone(&r, c[i].hz, c[i].amp, 0.5);
        CHECK(r.first_key == 0xFFFFFFFFU);
    }
}

static uint32_t lcg = 12345U;
static float noise(void)
{
    lcg = lcg * 1664525U + 1013904223U;
    return (float)((int32_t)lcg) / 2147483648.0f;
}

static void test_noise_and_speech_never_key(void)
{
    run_t r;
    run_init(&r);
    for (unsigned i = 0U; i < 48000U; i++) feed(&r, 0.3f * noise());
    CHECK(r.first_key == 0xFFFFFFFFU);
    /* Two tones far apart: crossings of neither. */
    run_init(&r);
    for (unsigned i = 0U; i < 48000U; i++) {
        const double t = i / FS;
        feed(&r, (float)(0.45 * sin(2.0 * PI_D * 700.0 * t) + 0.45 * sin(2.0 * PI_D * 1900.0 * t)));
    }
    CHECK(r.first_key == 0xFFFFFFFFU);
}

static void test_cut_stream_ends_and_gaps_hold(void)
{
    run_t r;
    run_init(&r);
    tone(&r, 1000.0, 0.8, 0.2);
    CHECK(r.s.keyed == 1U);
    /* A frame or three of zeros: held at full. */
    silence(&r, 0.003);
    CHECK(r.s.keyed == 1U);
    tone(&r, 1000.0, 0.8, 0.05);
    CHECK(r.s.ramp == 1.0f);
    CHECK(r.last_off == 0U);
    /* Cut: off within the hold, then the ramp. */
    const uint32_t cut = r.t;
    silence(&r, 0.05);
    CHECK(r.last_off > cut);
    CHECK(r.last_off - cut <= MOD_DIGI_TONE_HOLD + 2U + 480U);
    CHECK(r.s.level == 0.0f);
    /* The next over keys again, from silence. */
    r.first_key = 0xFFFFFFFFU;
    tone(&r, 1000.0, 0.8, 0.05);
    CHECK(r.first_key != 0xFFFFFFFFU);
    CHECK(r.s.keyed == 1U);
}

/* WSJT-X's end: a 20 ms raised cosine. The carrier is off by the time the
 * audio is (half way down it starts its 10 ms fall). */
static void test_closing_ramp_followed(void)
{
    run_t r;
    run_init(&r);
    tone(&r, 1500.0, 0.6, 0.3);
    const uint32_t nramp = 960U;
    const uint32_t start = r.t;
    for (uint32_t k = 0U; k < nramp; k++) {
        const double g = 0.5 * (1.0 + cos(PI_D * k / nramp));
        feed(&r, (float)(0.6 * g * sin(2.0 * PI_D * r.ph)));
        r.ph += 1500.0 / FS;
    }
    const uint32_t end = r.t;
    silence(&r, 0.05);
    CHECK(r.last_off > start + nramp / 2U);
    CHECK(r.last_off <= end + 48U);
}

static void test_level_steps(void)
{
    run_t r;
    run_init(&r);
    tone(&r, 1200.0, 0.2, 0.2);
    /* Up 12 dB: nothing happens. */
    tone(&r, 1200.0, 0.8, 0.1);
    CHECK(r.last_off == 0U);
    CHECK(r.s.ramp == 1.0f);
    /* Down 12 dB: a dip, then the new level is the level. */
    tone(&r, 1200.0, 0.2, 0.1);
    CHECK(r.s.keyed == 1U);
    CHECK(r.s.ramp == 1.0f);
}

/* AFSK at 45.45 Bd, 170 Hz shift: each element settles in a few cycles. */
static void test_fsk_settles(void)
{
    run_t r;
    run_init(&r);
    tone(&r, 2125.0, 0.7, 0.1);
    const double hz[2] = {2295.0, 2125.0};
    for (unsigned e = 0U; e < 20U; e++) {
        const double f = hz[e & 1U];
        const uint32_t n = (uint32_t)(FS / 45.45);
        double err = 0.0;
        for (uint32_t i = 0U; i < n; i++) {
            feed(&r, (float)(0.7 * sin(2.0 * PI_D * r.ph)));
            r.ph += f / FS;
            r.ph -= floor(r.ph);
            if (i == n - 1U) err = fabs(r.s.step * 24000.0 - f);
        }
        CHECK(err < 0.5);
        CHECK(r.s.keyed == 1U);
    }
    CHECK(r.last_off == 0U);
}

int main(void)
{
    test_level_independent_and_exact();
    test_start_is_prompt_and_ramped();
    test_below_floor_and_out_of_range_never_key();
    test_noise_and_speech_never_key();
    test_cut_stream_ends_and_gaps_hold();
    test_closing_ramp_followed();
    test_level_steps();
    test_fsk_settles();
    if (failures != 0U) {
        fprintf(stderr, "digi_tone_logic: %u failures\n", failures);
        return 1;
    }
    printf("digi_tone_logic: level-independent carrier to 5 mHz down to -47 dBFS, keyed in <4 ms, "
           "noise/two-tone/off-band never keyed, gaps held, cuts and WSJT-X's close followed, FSK settles\n");
    return 0;
}
