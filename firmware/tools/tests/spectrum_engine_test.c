/* Host test for radio_spectrum.c, the waterfall engine, against a simulated
 * receiver: capture blocks are synthesised from a list of carriers at their
 * absolute frequencies, relative to whatever LO (or dial) the capture is
 * taken around, so every frequency the engine computes -- the axis, the
 * pixel of a signal, the history shift on a retune, the scan's hops and its
 * panorama -- is checked end to end. */

#include "radio_spectrum.h"
#include "radio_spectrum_dsp.h"
#include "radio_audio.h"
#include "radio_clocks.h"
#include "radio.h"
#include "modulator.h"

#include <assert.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Simulated receiver --------------------------------------------------- */

uint32_t SystemCoreClock = 170000000UL;
static uint32_t tick = 1000U;
uint32_t HAL_GetTick(void) { return tick; }

static radio_config_t config;
const radio_config_t *RADIO_Config(void) { return &config; }
static radio_state_t state = RADIO_RECEIVE;
radio_state_t RADIO_State(void) { return state; }

static uint32_t dial = 14074000UL;
uint32_t MOD_GetCarrierRequestedHz(void) { return dial; }

static const uint64_t hr = 170000000ULL * 32ULL;
static uint32_t period, dial_period, hops, suspends;
static uint8_t muted, profile, restart_pending;
uint64_t RADIO_HrClockHz(void) { return hr; }
static uint32_t lo_hz(void) { return period ? (uint32_t)((hr + period / 2U) / period) : 0U; }
uint32_t RADIO_RxClockHz(void) { return lo_hz(); }
void RADIO_RxClockGeometry(radio_rx_geometry_t *g) { memset(g, 0, sizeof(*g)); g->period = period; }
static const int16_t rx_span[5][2] = RADIO_RX_CHANNEL_HZ;
void RADIO_RxChannelSpan(radio_rx_mode_t mode, int32_t *lo, int32_t *hi)
{
    *lo = rx_span[(unsigned)mode % 5U][0];
    *hi = rx_span[(unsigned)mode % 5U][1];
}
uint8_t RADIO_RxLoHop(uint32_t p)
{
    if (state != RADIO_RECEIVE || !period) return 0U;
    period = p;
    ++hops;
    return 1U;
}
/* The dial's LO: the nearest period below the dial (an IF of a few kHz). */
static void plan_dial(void)
{
    dial_period = (uint32_t)(hr / (dial - 12000U));
    period = dial_period;
}

static const float fs = 177083.33f;
float RADIO_AudioRate(void) { return fs; }
static uint32_t generation = 1U, overruns;
static uint8_t load_pct;
uint32_t RADIO_AudioGeneration(void) { return generation; }
uint32_t RADIO_AudioOverruns(void) { return overruns; }
uint8_t RADIO_AudioLoadPct(void) { return load_pct; }
void RADIO_AudioProfileEnable(uint8_t on) { profile = on; }
uint8_t RADIO_AudioProfileEnabled(void) { return profile; }
void RADIO_AudioScanMute(uint8_t on) { muted = on; }
void RADIO_AudioSuspend(void) { ++suspends; restart_pending = 1U; }

static uint32_t scratch[RADIO_SPEC_SCRATCH_BYTES / 4U];
static float block[2U * RADIO_SPEC_N];
static uint8_t armed, arm_source, arm_halvings;
static uint32_t arms, arm_tick, arm_skip;
void *RADIO_AudioSpecScratch(void) { return scratch; }
void RADIO_AudioSpecArm(uint8_t source, uint8_t halvings, uint16_t skip)
{
    armed = 1U;
    arm_tick = tick;
    arm_skip = skip;
    arm_source = source;
    arm_halvings = halvings;
    ++arms;
}
void RADIO_AudioSpecStop(void) { armed = 0U; muted = 0U; }

/* Carriers on the air, absolute Hz, amplitude relative to full scale. */
static struct { uint32_t hz; float amp; } carriers[4];
static unsigned n_carriers;
static float noise = 1e-4f;

/* The block the receiver would capture now: raw around the LO at fS, or the
 * channel around the dial at 48 kHz / 2^halvings. */
float *RADIO_AudioSpecTake(void)
{
    if (!armed) return NULL;
    const double rate = arm_source ? 48000.0 / (double)(1U << arm_halvings) : fs;
    /* A block takes as long as its samples do, after the skipped halves. */
    const double ms = 1000.0 * (RADIO_SPEC_N / rate + arm_skip * 64.0 / fs);
    if ((double)(tick - arm_tick) < ms) return NULL;
    armed = 0U;
    const double centre = arm_source ? (double)dial : (double)lo_hz();
    static uint32_t seed = 1U;
    for (unsigned n = 0U; n < RADIO_SPEC_N; ++n) {
        double i = 0.0, q = 0.0;
        for (unsigned c = 0U; c < n_carriers; ++c) {
            const double f = ((double)carriers[c].hz - centre) / rate;
            if (fabs(f) >= 0.5) continue; /* outside the stream */
            i += carriers[c].amp * cos(2.0 * M_PI * f * n);
            q += carriers[c].amp * sin(2.0 * M_PI * f * n);
        }
        seed = seed * 1664525U + 1013904223U;
        i += noise * ((double)(seed >> 8) / 16777216.0 - 0.5);
        seed = seed * 1664525U + 1013904223U;
        q += noise * ((double)(seed >> 8) / 16777216.0 - 0.5);
        block[2U * n] = (float)i;
        block[2U * n + 1U] = (float)q;
    }
    return block;
}

/* ---- Harness -------------------------------------------------------------- */

static uint8_t hist[40U * SPEC_LINE_BYTES];
static int16_t trace[SPEC_W];
static uint8_t peak[SPEC_W];

static void run_ms(uint32_t ms)
{
    for (uint32_t t = 0U; t < ms; ++t) {
        ++tick;
        if (restart_pending) { restart_pending = 0U; ++generation; memset(scratch, 0, sizeof(scratch)); }
        (void)SPEC_Service(tick);
        (void)SPEC_Service(tick);
    }
}

static unsigned level_at(const uint8_t *line, unsigned x) { return (line[x >> 1] >> ((x & 1U) * 4U)) & 15U; }

/* Pixel with the highest level of the newest line, searching [a, b). */
static unsigned brightest(unsigned a, unsigned b)
{
    SPEC_Latch();
    const uint8_t *line = SPEC_HistoryLine(0U);
    assert(line != NULL);
    unsigned best = a, lv = 0U;
    for (unsigned x = a; x < b; ++x) if (level_at(line, x) > lv) { lv = level_at(line, x); best = x; }
    return best;
}

/* Pixel of the trace maximum in [a, b): exact where the levels saturate. */
static unsigned trace_peak(unsigned a, unsigned b)
{
    const int16_t *t = SPEC_Trace();
    assert(t != NULL);
    unsigned best = a;
    for (unsigned x = a; x < b; ++x) if (t[x] > t[best]) best = x;
    return best;
}

static int expected_x(uint32_t hz)
{
    spec_view_t v;
    SPEC_GetView(&v);
    return (int)lrintf((float)SPEC_CENTER + (float)(int32_t)(hz - v.centre_hz) / v.hz_per_px);
}

static void setup(void)
{
    memset(&config, 0, sizeof(config));
    config.rx_i_gain = config.rx_q_gain = 1.0f;
    config.lpf_upper_hz[0] = 30000000UL;
    config.bpf_upper_hz[0] = 30000000UL;
    state = RADIO_RECEIVE;
    dial = 14074000UL;
    plan_dial();
    n_carriers = 0U;
    SPEC_ConfigDefaults();
    SPEC_Open();
    SPEC_Attach(hist, 40U, trace, peak);
}

static void test_wide(void)
{
    setup();
    carriers[0].hz = 14100000UL; carriers[0].amp = 0.01f;
    carriers[1].hz = 14060000UL; carriers[1].amp = 0.00003f; /* -90 dBFS: mid-scale */
    n_carriers = 2U;
    assert(profile == 1U);
    run_ms(600U);
    spec_view_t v;
    SPEC_GetView(&v);
    printf("wide: centre %lu Hz, %.1f Hz/px, %u lines at %u.%u/s, scale %d..%d dBFS\n",
           (unsigned long)v.centre_hz, v.hz_per_px, v.lines, v.rate10 / 10U, v.rate10 % 10U, v.bottom_db, v.top_db);
    assert(v.valid && v.receiving && !v.scanning);
    assert(fabsf(v.hz_per_px - fs / 512.0f) < 0.01f);
    assert(abs((int)v.centre_hz - (int)lo_hz()) <= 1);
    assert(v.lines >= 4U && v.rate10 >= 80U && v.rate10 <= 120U); /* 10 lines/s default */
    const int x0 = expected_x(14100000UL), x1 = expected_x(14060000UL);
    const unsigned b0 = trace_peak((unsigned)x0 - 20U, (unsigned)x0 + 20U), b1 = brightest((unsigned)x1 - 20U, (unsigned)x1 + 20U);
    SPEC_Latch();
    const uint8_t *line = SPEC_HistoryLine(0U);
    printf("wide: tones expected at x=%d and x=%d, found %u (level %u) and %u (level %u), quiet x=250 level %u\n",
           x0, x1, b0, level_at(line, b0), b1, level_at(line, b1), level_at(line, 250U));
    assert(abs((int)b0 - x0) <= 1 && abs((int)b1 - x1) <= 1);
    assert(level_at(line, b0) == 15U && level_at(line, b1) >= 6U && level_at(line, b1) <= 12U);
    assert(level_at(line, 250U) <= 4U);
    /* The trace shows the tone at its level: -40 dBFS within the window's scalloping. */
    const int16_t *t = SPEC_Trace();
    assert(t != NULL);
    int16_t top = INT16_MIN;
    for (int x = x0 - 1; x <= x0 + 1; ++x) if (t[x] > top) top = t[x];
    printf("wide: tone at x=%d reads %.1f dBFS\n", x0, top / 8.0);
    assert(top > -8 * 42 && top < -8 * 38);
    /* the dial marker sits at the dial */
    assert(abs(v.dial_x - expected_x(dial)) <= 1 && v.pass_lo_x > v.dial_x && v.pass_hi_x > v.pass_lo_x);
    SPEC_Close();
    assert(profile == 0U && !armed);
}

static void test_zoom_and_shift(void)
{
    setup();
    SPEC_Config()->span = SPEC_SPAN_9K5;
    SPEC_ConfigChanged();
    carriers[0].hz = dial + 2000U; carriers[0].amp = 0.01f;
    n_carriers = 1U;
    run_ms(800U);
    spec_view_t v;
    SPEC_GetView(&v);
    printf("zoom 9.5k: %.2f Hz/px, halvings %u, %u lines\n", v.hz_per_px, arm_halvings, v.lines);
    assert(arm_source == RADIO_SPEC_SRC_CHANNEL && arm_halvings == 2U);
    assert(fabsf(v.hz_per_px - 9500.0f / 304.0f) < 0.01f && v.centre_hz == dial && v.dial_x == SPEC_CENTER);
    assert(v.lines >= 2U);
    const unsigned x = trace_peak(0U, SPEC_W);
    printf("zoom 9.5k: tone expected at x=%d, trace peak at %u, brightest %u\n", expected_x(dial + 2000U), x, brightest(0U, SPEC_W));
    assert(abs((int)x - expected_x(dial + 2000U)) <= 1);
    /* Retune by 1 kHz: the history moves 32 px left, the carrier stays put
     * on the old lines until new ones come. */
    const unsigned lines_before = v.lines, bx = brightest(0U, SPEC_W);
    dial += 1000U;
    (void)SPEC_Service(tick);
    SPEC_GetView(&v);
    assert(v.centre_hz == dial || abs((int)v.centre_hz - (int)dial) < 16);
    SPEC_Latch();
    const uint8_t *old = SPEC_HistoryLine(0U);
    unsigned lv = 0U, at = 0U;
    for (unsigned k = 0U; k < SPEC_W; ++k) if (level_at(old, k) > lv) { lv = level_at(old, k); at = k; }
    printf("zoom 9.5k: after +1 kHz the old carrier is at x=%u (was %u)\n", at, bx);
    assert((int)at == (int)bx - 32 && v.lines == lines_before);
    SPEC_Close();
}

static void test_scan(void)
{
    setup();
    SPEC_Config()->mode = SPEC_MODE_SCAN;
    SPEC_ConfigChanged();
    carriers[0].hz = 14010000UL; carriers[0].amp = 0.01f;
    carriers[1].hz = 14300000UL; carriers[1].amp = 0.01f;
    carriers[2].hz = 14175000UL; carriers[2].amp = 0.003f;
    n_carriers = 3U;
    hops = 0U;
    uint8_t saw_mute = 0U;
    for (unsigned t = 0U; t < 400U; ++t) { run_ms(1U); if (muted) saw_mute = 1U; }
    spec_view_t v;
    SPEC_GetView(&v);
    printf("scan: centre %lu, %.0f Hz/px, %u lines, %lu hops, LO back at dial: %s\n",
           (unsigned long)v.centre_hz, v.hz_per_px, v.lines, (unsigned long)hops, period == dial_period ? "yes" : "no");
    assert(saw_mute && !muted && period == dial_period && !v.scanning);
    assert(v.centre_hz == 14175000UL && fabsf(v.hz_per_px - 350000.0f / 304.0f) < 0.5f);
    assert(v.lines >= 1U && hops >= 4U);
    /* every carrier is found where it is, across different hops */
    const int xa = expected_x(14010000UL), xb = expected_x(14300000UL), xc = expected_x(14175000UL);
    assert(abs((int)brightest(0U, 60U) - xa) <= 1);
    assert(abs((int)brightest(250U, SPEC_W) - xb) <= 1);
    assert(abs((int)brightest(120U, 190U) - xc) <= 1);
    /* LISTEN: nothing hops while the receiver listens at the dial. */
    const uint32_t h = hops;
    run_ms(1000U);
    assert(hops == h && period == dial_period);
    /* A receiver restart in the middle of a sweep: the LO goes back to the
     * dial and the receiver restarts on it. */
    SPEC_ScanTrigger();
    run_ms(2U);
    assert(period != dial_period || hops > h);
    const uint32_t s0 = suspends;
    ++generation;
    run_ms(2U);
    printf("scan: restart mid-sweep -> LO at dial %s, receiver restarted %s\n",
           period == dial_period ? "yes" : "no", suspends > s0 ? "yes" : "no");
    assert(period == dial_period && !muted);
    /* TX takes the receiver: the sweep ends, the engine idles. */
    SPEC_ScanTrigger();
    run_ms(2U);
    state = RADIO_TRANSMIT;
    run_ms(5U);
    SPEC_GetView(&v);
    assert(!v.scanning && !v.receiving && !muted);
    state = RADIO_RECEIVE;
    SPEC_Close();
}

static void test_guard(void)
{
    setup();
    run_ms(300U);
    assert(SPEC_TxAllowed());
    load_pct = 96U;
    run_ms(300U);
    assert(!SPEC_TxAllowed());
    load_pct = 70U;
    run_ms(1000U);
    assert(!SPEC_TxAllowed()); /* held two seconds */
    run_ms(1500U);
    assert(SPEC_TxAllowed());
    ++overruns; /* a missed deadline on a settled receiver */
    run_ms(300U);
    assert(!SPEC_TxAllowed());
    SPEC_Close();
    assert(SPEC_TxAllowed()); /* a closed page never holds TX */
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_wide();
    test_zoom_and_shift();
    test_scan();
    test_guard();
    puts("spectrum_engine: ok");
    return 0;
}
