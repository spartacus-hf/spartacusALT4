#include "radio_spectrum.h"
#include "radio_spectrum_dsp.h"
#include "radio_audio.h"
#include "radio_clocks.h"
#include "radio.h"
#include "modulator.h"
#include "main.h"
#include <math.h>
#include <string.h>

/* The engine's bookkeeping -- a line, an axis, a sweep step at most a few
 * dozen times a second, in the main loop -- is built for size; the FFT, the
 * fold into pixels and the receive worker's capture stay at -O2. */
#define SPEC_SMALL __attribute__((optimize("Os")))

/* The waterfall engine: see radio_spectrum.h. Everything below runs in the
 * main loop, one captured block per call at most, so the loop never waits on
 * more than one FFT (~0.25 ms) at a time. */

#define NODATA            INT16_MIN
/* Raw bins used: the PCM1821 is flat to +-0.3 fS (+-152 bins); a scan cell
 * may reach +-0.414 fS, still well inside its alias-free band, where the
 * droop is a few dB -- the price of covering 10 m, whose LO grid is 144 kHz. */
#define RAW_VIEW_BINS     152
#define RAW_SCAN_BINS     212
/* Zoom captures: +-19 kHz of the 48 kHz channel is usable (radio_rx_front.c:
 * -0.3 dB at 18 kHz, -1.3 at 19),
 * the half-bands keep 0.396 of their rate. */
#define CHANNEL_BINS      203
/* After a hop: the half in flight, one more, and the PCM1821's 17-sample
 * group delay rounded up to a whole half. */
#define SCAN_SETTLE_HALVES 3U
#define SCAN_STEP_MS      60U   /* a step that has not delivered by then is abandoned */
#define SCAN_RETRY_MS     250U
#define SCAN_MAX_STEPS    48U
#define METER_MS          250U

enum { SCAN_IDLE = 0, SCAN_CAPTURE };

typedef struct {
    int16_t acc[SPEC_W];          /* the line being made, dBFS x 8 */
    int16_t win[SPEC_WIN_HALF];   /* the window, Q15 half table */
} spec_scratch_t;
_Static_assert(sizeof(spec_scratch_t) <= RADIO_SPEC_SCRATCH_BYTES,
               "the waterfall scratch must fit RADIO_SPEC_SCRATCH_BYTES");

const uint8_t spec_rates[SPEC_RATE_COUNT] = SPEC_RATES;
const uint8_t spec_averages[SPEC_AVG_COUNT] = SPEC_AVERAGES;
const uint8_t spec_ranges[SPEC_RANGE_COUNT] = SPEC_RANGES;
const uint16_t spec_listens[SPEC_LISTEN_COUNT] = SPEC_LISTEN_MS;
const uint8_t spec_scan_avgs[SPEC_SCAN_AVG_COUNT] = SPEC_SCAN_AVGS;
static const uint16_t zoom_hz[SPEC_SPAN_COUNT] = {0U, 38000U, 19000U, 9500U};
/* Band edges in kHz for SCAN BAND, amateur and broadcast, widest allocation
 * of the three regions; the first band holding the dial wins. */
static const uint16_t band_khz[][2] = {
    {1800U, 2000U}, {2300U, 2495U}, {3200U, 3400U}, {3500U, 4000U}, {5250U, 5450U},
    {5900U, 6200U}, {7000U, 7300U}, {7300U, 7450U}, {9400U, 9900U}, {10100U, 10150U},
    {11600U, 12100U}, {13570U, 13870U}, {14000U, 14350U}, {15100U, 15800U}, {17480U, 17900U},
    {18068U, 18168U}, {21000U, 21450U}, {21450U, 21850U}, {24890U, 24990U}, {26965U, 27405U},
    {28000U, 29700U},
};

static const spec_config_t defaults = {
    .mode = SPEC_MODE_VIEW, .span = SPEC_SPAN_WIDE, .layout = SPEC_LAYOUT_WF,
    .rate = 2U /* 10 lines/s */, .average = 2U /* 4 FFTs */, .window = SPEC_WIN_BLACKMAN,
    .detector = SPEC_DET_AVG, .scale = SPEC_SCALE_AUTO, .range = 2U /* 50 dB */,
    .palette = 0U, .fill = 1U, .smooth = 1U, .peak_hold = SPEC_PEAK_OFF, .dc_notch = 1U,
    .iq_correct = 1U, .markers = SPEC_MARK_PASS, .scan_range = SPEC_SCAN_BAND,
    .scan_audio = SPEC_SCANA_LISTEN, .listen = 3U /* 2 s */, .scan_avg = 1U /* 2 FFTs */,
    .floor_offset = -6, .ref_dbfs = -30,
};

static spec_config_t cfg = {
    .rate = 2U, .average = 2U, .window = SPEC_WIN_BLACKMAN, .range = 2U, .fill = 1U, .smooth = 1U,
    .dc_notch = 1U, .iq_correct = 1U, .markers = SPEC_MARK_PASS, .listen = 3U, .scan_avg = 1U,
    .floor_offset = -6, .ref_dbfs = -30,
};

static struct {
    uint8_t open, profile_was, receiving, armed, acc_n, win_kind, axis_valid, axis_key;
    uint8_t *hist;
    int16_t *trace;
    uint8_t *peak;
    uint8_t slots, head, lines, show_head, show_lines, guard, load_rx, load_wf, scan_state, scan_request;
    uint8_t scan_step, scan_steps, scan_blocks, cfg_dirty;
    uint32_t win_gen, arm_gen, arm_at, line_due, last_line;
    uint32_t hist_centre;         /* frequency at SPEC_CENTER the history is drawn for */
    float px_hz, floor8, peak_credit;
    int32_t bottom8, span8;
    uint16_t rate10;
    uint32_t meter_at, cycles, overruns, rx_since, guard_until, version;
    /* scan */
    uint64_t hr;
    uint32_t scan_lo, scan_hi, scan_dial, scan_gen, dial_period, hop_period, step_deadline, listen_until;
} sp;

static inline uint32_t cycles_now(void)
{
#if defined(__arm__)
    return DWT->CYCCNT;
#else
    return 0U;
#endif
}

spec_config_t *SPEC_Config(void) { return &cfg; }
__attribute__((cold)) void SPEC_ConfigDefaults(void) { cfg = defaults; SPEC_ConfigChanged(); }
void SPEC_ConfigChanged(void) { sp.cfg_dirty = 1U; }
uint8_t SPEC_IsOpen(void) { return sp.open; }
uint32_t SPEC_Version(void) { return sp.version; }

/* ---- History ---------------------------------------------------------------- */

static uint8_t nibble(const uint8_t *line, unsigned x) { return (uint8_t)((line[x >> 1] >> ((x & 1U) * 4U)) & 15U); }
static void set_nibble(uint8_t *line, unsigned x, uint8_t v)
{
    const unsigned s = (x & 1U) * 4U;
    line[x >> 1] = (uint8_t)((line[x >> 1] & ~(15U << s)) | ((unsigned)v << s));
}

SPEC_SMALL static void clear_history(void)
{
    sp.lines = sp.show_lines = 0U;
    sp.head = (uint8_t)(sp.slots ? sp.slots - 1U : 0U);
    if (sp.hist) memset(sp.hist, 0, (size_t)sp.slots * SPEC_LINE_BYTES);
    if (sp.trace) for (unsigned x = 0U; x < SPEC_W; ++x) sp.trace[x] = NODATA;
    if (sp.peak) memset(sp.peak, 0, SPEC_W);
    ++sp.version;
}

/* line[x] = old line[x + n], vacated pixels empty (level 0). */
SPEC_SMALL static void shift_line(uint8_t *line, int32_t n)
{
    if ((n & 1) == 0) {
        const int32_t b = n / 2;
        if (b > 0) {
            memmove(line, line + b, (size_t)(SPEC_LINE_BYTES - (uint32_t)b));
            memset(line + SPEC_LINE_BYTES - b, 0, (size_t)b);
        } else {
            memmove(line - b, line, (size_t)(SPEC_LINE_BYTES - (uint32_t)-b));
            memset(line, 0, (size_t)-b);
        }
        return;
    }
    if (n > 0) {
        for (int32_t x = 0; x < (int32_t)SPEC_W; ++x)
            set_nibble(line, (unsigned)x, (x + n < (int32_t)SPEC_W) ? nibble(line, (unsigned)(x + n)) : 0U);
    } else {
        for (int32_t x = (int32_t)SPEC_W - 1; x >= 0; --x)
            set_nibble(line, (unsigned)x, (x + n >= 0) ? nibble(line, (unsigned)(x + n)) : 0U);
    }
}

/* The dial moved by n whole pixels: move everything drawn with it, so a
 * signal stays where it is on the waterfall while the frequency scale moves. */
SPEC_SMALL static void shift_history(int32_t n)
{
    if (n >= (int32_t)SPEC_W || n <= -(int32_t)SPEC_W) { clear_history(); return; }
    for (unsigned s = 0U; s < sp.slots && sp.hist; ++s) shift_line(&sp.hist[s * SPEC_LINE_BYTES], n);
    const uint32_t keep = SPEC_W - (uint32_t)(n > 0 ? n : -n);
    if (sp.trace) {
        if (n > 0) { memmove(sp.trace, sp.trace + n, keep * 2U); for (uint32_t x = keep; x < SPEC_W; ++x) sp.trace[x] = NODATA; }
        else { memmove(sp.trace - n, sp.trace, keep * 2U); for (int32_t x = 0; x < -n; ++x) sp.trace[x] = NODATA; }
    }
    if (sp.peak) {
        if (n > 0) { memmove(sp.peak, sp.peak + n, keep); memset(sp.peak + keep, 0, (size_t)n); }
        else { memmove(sp.peak - n, sp.peak, keep); memset(sp.peak, 0, (size_t)-n); }
    }
    ++sp.version;
}

void SPEC_Attach(uint8_t *hist, uint8_t slots, int16_t *trace, uint8_t *peak)
{
    sp.hist = (hist && slots >= 2U) ? hist : NULL;
    sp.slots = sp.hist ? slots : 0U;
    sp.trace = trace;
    sp.peak = peak;
    clear_history();
    sp.axis_valid = 0U;
}

void SPEC_Detach(void) { SPEC_Attach(NULL, 0U, NULL, NULL); }

void SPEC_Latch(void)
{
    sp.show_head = sp.head;
    sp.show_lines = sp.lines;
}

const uint8_t *SPEC_HistoryLine(uint8_t age)
{
    if (!sp.hist || age >= sp.show_lines) return NULL;
    const unsigned slots = sp.slots;
    const unsigned slot = ((unsigned)sp.show_head + slots - ((unsigned)age % slots)) % slots;
    return &sp.hist[slot * SPEC_LINE_BYTES];
}

const int16_t *SPEC_Trace(void) { return (sp.trace && sp.lines) ? sp.trace : NULL; }
const uint8_t *SPEC_Peak(void) { return (sp.peak && sp.lines && cfg.peak_hold != SPEC_PEAK_OFF) ? sp.peak : NULL; }

/* ---- One block into the line --------------------------------------------- */

static int32_t iceil(float v) { const int32_t i = (int32_t)v; return i + (((float)i < v) ? 1 : 0); }

static void acc_reset(spec_scratch_t *sc)
{
    for (unsigned x = 0U; x < SPEC_W; ++x) sc->acc[x] = NODATA;
    sp.acc_n = 0U;
}

/* Fold a transformed block into pixels [x0, x1): pixel x is `f0 + x * px`
 * from the capture's own centre and takes the strongest bin whose centre
 * lies inside it (the nearest one when the pixel is narrower than a bin), so
 * no narrow signal falls between pixels. Bins past +-kmax are not trusted. */
static void fold(spec_scratch_t *sc, const float *x, float f0, float bin_hz, int32_t kmax,
                 unsigned x0, unsigned x1, uint8_t notch)
{
    const float inv = 1.0f / bin_hz, half = 0.5f * sp.px_hz;
    const int16_t db_off = spec_windows[cfg.window].db8;
    const uint8_t peak = (uint8_t)(cfg.detector == SPEC_DET_PEAK);
    /* The DC notch: the LO's own leak and the converter's offsets live in
     * bins -1..1 of a raw block; show what their neighbours show. */
    const float dc = notch ? 0.5f * (spec_power(x, -2) + spec_power(x, 2)) : 0.0f;
    for (unsigned px = x0; px < x1; ++px) {
        const float f = f0 + (float)px * sp.px_hz;
        int32_t ka = iceil((f - half) * inv), kb = iceil((f + half) * inv) - 1;
        if (ka > kb) ka = kb = (int32_t)lrintf(f * inv);
        if (ka < -kmax) ka = -kmax;
        if (kb > kmax) kb = kmax;
        if (ka > kb) continue;
        float p = 0.0f;
        for (int32_t k = ka; k <= kb; ++k) {
            const float v = (notch && k >= -1 && k <= 1) ? dc : spec_power(x, (int)k);
            if (v > p) p = v;
        }
        int32_t d = (int32_t)spec_db8(p) - db_off;
        if (d < -2000) d = -2000;
        int16_t *a = &sc->acc[px];
        if (*a == NODATA) *a = (int16_t)d;
        else if (peak) { if (d > *a) *a = (int16_t)d; }
        else *a = (int16_t)(*a + d);
    }
}

/* Window, correct (raw streams) and transform the block in place. */
static void transform(spec_scratch_t *sc, float *x, uint8_t raw)
{
    if (raw) {
        const radio_config_t *c = RADIO_Config();
        spec_iq_t iq = {1.0f, 1.0f, c->rx_iq_invert ? -1.0f : 1.0f, 0.0f, 0U, 0U};
        if (cfg.iq_correct) {
            const float skew = c->rx_skew_samples;
            iq.i_gain = c->rx_i_gain;
            iq.q_gain = c->rx_q_gain;
            if (isfinite(skew) && fabsf(skew) >= 0.00001f && fabsf(skew) <= RADIO_IQ_SKEW_MAX) {
                iq.skew = fabsf(skew);
                iq.delay = 1U;
                iq.delay_q = (uint8_t)(skew > 0.0f);
            }
        }
        spec_prepare(x, &iq, sc->win);
        spec_fft512(x);
        spec_iq_delay_bins(x, &iq);
    } else {
        spec_prepare(x, NULL, sc->win);
        spec_fft512(x);
    }
}

/* ---- One finished line ------------------------------------------------------ */

SPEC_SMALL static void colour_scale(void)
{
    const int32_t range8 = (int32_t)spec_ranges[cfg.range < SPEC_RANGE_COUNT ? cfg.range : 2U] * 8;
    if (cfg.scale == SPEC_SCALE_FIXED) sp.bottom8 = (int32_t)cfg.ref_dbfs * 8 - range8;
    else sp.bottom8 = (int32_t)lrintf(sp.floor8) + (int32_t)cfg.floor_offset * 8;
    sp.span8 = range8;
}

/* Noise floor of a line: its 20th percentile, in 2 dB steps. Signals take a
 * small share of any band, so the quiet fifth is the noise. */
SPEC_SMALL static void track_floor(const int16_t *v)
{
    uint16_t buckets[96];
    memset(buckets, 0, sizeof(buckets));
    unsigned valid = 0U;
    for (unsigned x = 0U; x < SPEC_W; ++x) {
        if (v[x] == NODATA) continue;
        int32_t b = ((int32_t)v[x] + 180 * 8) / 16;
        if (b < 0) b = 0;
        if (b > 95) b = 95;
        ++buckets[b];
        ++valid;
    }
    if (!valid) return;
    unsigned seen = 0U, b = 0U;
    for (; b < 95U; ++b) if ((seen += buckets[b]) * 5U >= valid) break;
    const float f = (float)(((int32_t)b * 2 - 180) * 8 + 8);
    if (sp.lines == 0U) sp.floor8 = f;
    else sp.floor8 += 0.2f * (f - sp.floor8);
}

SPEC_SMALL static void emit_line(spec_scratch_t *sc, unsigned n, uint32_t now)
{
    int16_t *v = sc->acc;
    if (cfg.detector == SPEC_DET_AVG && n > 1U)
        for (unsigned x = 0U; x < SPEC_W; ++x) if (v[x] != NODATA) v[x] = (int16_t)(v[x] / (int32_t)n);
    track_floor(v);
    colour_scale();
    if (sp.hist) {
        sp.head = (uint8_t)((sp.head + 1U) % sp.slots);
        uint8_t *line = &sp.hist[sp.head * SPEC_LINE_BYTES];
        for (unsigned x = 0U; x < SPEC_W; x += 2U) {
            const uint8_t a = (v[x] == NODATA) ? 0U : spec_level(v[x], sp.bottom8, sp.span8);
            const uint8_t b = (v[x + 1U] == NODATA) ? 0U : spec_level(v[x + 1U], sp.bottom8, sp.span8);
            line[x >> 1] = (uint8_t)(a | (b << 4));
        }
    }
    if (sp.trace) {
        const unsigned s = cfg.smooth < SPEC_SMOOTH_COUNT ? cfg.smooth : 0U;
        for (unsigned x = 0U; x < SPEC_W; ++x) {
            if (v[x] == NODATA) continue;
            if (!s || sp.trace[x] == NODATA) sp.trace[x] = v[x];
            else sp.trace[x] = (int16_t)(sp.trace[x] + ((v[x] - sp.trace[x]) >> s));
        }
    }
    if (sp.peak && cfg.peak_hold != SPEC_PEAK_OFF) {
        /* FAST falls 20 dB/s, SLOW 5 dB/s, HOLD never, in whole dB. */
        static const uint8_t fall[SPEC_PEAK_COUNT] = {0U, 20U, 5U, 0U};
        const float rate = sp.rate10 ? (float)sp.rate10 * 0.1f : (float)spec_rates[cfg.rate];
        sp.peak_credit += (float)fall[cfg.peak_hold] / rate;
        const uint8_t drop = (uint8_t)sp.peak_credit;
        sp.peak_credit -= (float)drop;
        for (unsigned x = 0U; x < SPEC_W; ++x) {
            uint8_t p = sp.peak[x];
            p = (p > drop + 1U) ? (uint8_t)(p - drop) : (p ? 1U : 0U);
            if (v[x] != NODATA) {
                int32_t d = v[x] / 8 + 160;
                if (d < 1) d = 1;
                if (d > 255) d = 255;
                if ((uint8_t)d > p) p = (uint8_t)d;
            }
            sp.peak[x] = p;
        }
    }
    if (sp.hist ? (sp.lines + 1U < sp.slots) : (sp.lines == 0U)) ++sp.lines;
    if (sp.last_line && now != sp.last_line) {
        const uint32_t r = 10000U / (now - sp.last_line);
        sp.rate10 = (uint16_t)(sp.rate10 ? (sp.rate10 * 3U + (r > 999U ? 999U : r)) / 4U : r);
    }
    sp.last_line = now;
    acc_reset(sc);
    ++sp.version;
}

/* ---- Axis --------------------------------------------------------------------- */

/* Frequency at SPEC_CENTER and hertz per pixel for the view; the history
 * follows a moving centre by whole pixels and restarts on anything else. */
SPEC_SMALL static uint8_t set_axis(uint8_t key, uint32_t centre, float px)
{
    if (!sp.axis_valid || key != sp.axis_key || fabsf(px - sp.px_hz) > 0.001f * sp.px_hz) {
        sp.axis_valid = 1U;
        sp.axis_key = key;
        sp.px_hz = px;
        sp.hist_centre = centre;
        clear_history();
        return 1U;
    }
    const int32_t n = (int32_t)lrintf((float)(int32_t)(centre - sp.hist_centre) / sp.px_hz);
    if (n == 0) return 0U;
    shift_history(n);
    sp.hist_centre += (uint32_t)(int32_t)lrintf((float)n * sp.px_hz);
    return 1U;
}

/* ---- Guard -------------------------------------------------------------------- */

SPEC_SMALL static uint8_t meter(uint32_t now)
{
    const uint32_t dt = now - sp.meter_at;
    if (dt < METER_MS) return 0U;
    sp.meter_at = now;
    const uint8_t old_rx = sp.load_rx, old_wf = sp.load_wf, old_guard = sp.guard;
    sp.load_rx = RADIO_AudioLoadPct();
    /* The page's own share: sp.cycles is wall time in thread mode, and the
     * receive worker preempts it at the share load_rx already counts. Counted
     * twice, the two read 93 % with S-CTRL attached (USB, 164 MHz; bench,
     * v8.11) and the guard held TX for as long as the page was open. */
    const uint32_t per_ms = SystemCoreClock / 1000U, cycles = sp.cycles;
    sp.cycles = 0U;
    const uint32_t wf = (per_ms && dt < 60000U) ? (cycles / per_ms) * (100U - sp.load_rx) / dt : 0U;
    sp.load_wf = (uint8_t)(wf > 100U ? 100U : wf);
    const uint32_t overruns = RADIO_AudioOverruns();
    /* A missed deadline counts only on a receiver that has been running for a
     * second: starting and retuning are not the waterfall's doing. */
    const uint8_t missed = (uint8_t)(overruns != sp.overruns && sp.receiving &&
                                     (int32_t)(now - sp.rx_since) > 1000);
    sp.overruns = overruns;
    if (missed || (uint32_t)sp.load_rx + sp.load_wf >= SPEC_GUARD_PCT) sp.guard_until = now + SPEC_GUARD_MS;
    sp.guard = (uint8_t)((int32_t)(now - sp.guard_until) < 0);
    return (uint8_t)(sp.guard != old_guard || sp.load_rx != old_rx || sp.load_wf != old_wf);
}

uint8_t SPEC_TxAllowed(void)
{
    return (uint8_t)!(sp.open && (int32_t)(HAL_GetTick() - sp.guard_until) < 0);
}

/* ---- View --------------------------------------------------------------------- */

SPEC_SMALL static uint8_t view_service(uint32_t now, spec_scratch_t *sc, uint32_t gen)
{
    const uint8_t raw = (uint8_t)(cfg.span == SPEC_SPAN_WIDE);
    const uint8_t halvings = raw ? 0U : (uint8_t)(cfg.span - SPEC_SPAN_38K);
    const float bin = raw ? RADIO_AudioRate() / (float)SPEC_FFT_N : 48000.0f / (float)(SPEC_FFT_N << halvings);
    const float px = raw ? bin : (float)zoom_hz[cfg.span] / (float)SPEC_W;
    const uint32_t cap_centre = raw ? RADIO_RxClockHz() : MOD_GetCarrierRequestedHz();
    uint8_t redraw = set_axis(cfg.span, cap_centre, px);
    if (sp.armed && sp.arm_gen != gen) sp.armed = 0U; /* a restart took the pending block */
    if (!sp.armed) {
        if ((int32_t)(now - sp.arm_at) < 0) return redraw;
        RADIO_AudioSpecArm(raw ? RADIO_SPEC_SRC_RAW : RADIO_SPEC_SRC_CHANNEL, halvings, 0U);
        sp.armed = 1U;
        sp.arm_gen = gen;
        return redraw;
    }
    float *x = RADIO_AudioSpecTake();
    if (!x) return redraw;
    sp.armed = 0U;
    transform(sc, x, raw);
    const float f0 = (float)(int32_t)(sp.hist_centre - cap_centre) - (float)SPEC_CENTER * px;
    fold(sc, x, f0, bin, raw ? RAW_VIEW_BINS : CHANNEL_BINS, 0U, SPEC_W, (uint8_t)(raw && cfg.dc_notch));
    const unsigned n = spec_averages[cfg.average < SPEC_AVG_COUNT ? cfg.average : 0U];
    if (++sp.acc_n < n) { sp.arm_at = now; return redraw; }
    emit_line(sc, n, now);
    /* Pace the next line: its blocks are captured back to back, starting so
     * the line lands on time, and nothing runs in between. */
    const uint32_t period = 1000U / spec_rates[cfg.rate < SPEC_RATE_COUNT ? cfg.rate : 2U];
    const uint32_t block_ms = (uint32_t)(1000.0f / bin) + 1U; /* a block lasts 1 / bin */
    sp.line_due += period;
    if ((int32_t)(now - sp.line_due) >= 0 || (int32_t)(sp.line_due - now) > (int32_t)period) sp.line_due = now + period;
    sp.arm_at = sp.line_due - n * block_ms;
    if ((int32_t)(sp.arm_at - now) < 0) sp.arm_at = now;
    return 1U;
}

/* ---- Scan --------------------------------------------------------------------- */

/* The receive filter slot around `hz` (radio_config.c covering_slot): the
 * lowest configured upper edge at or above it, and the highest below. */
SPEC_SMALL static void slot_limits(const uint32_t *upper, unsigned n, uint32_t hz, uint32_t *lo, uint32_t *hi)
{
    uint32_t top = 0xFFFFFFFFUL, below = 0U;
    for (unsigned i = 0U; i < n; ++i) {
        const uint32_t u = upper[i];
        if (!u) continue;
        if (u >= hz && u < top) top = u;
        if (u < hz && u > below) below = u;
    }
    if (below + 1U > *lo) *lo = below + 1U;
    if (top < *hi) *hi = top;
}

SPEC_SMALL static void scan_limits(uint32_t dial, uint32_t *lo_out, uint32_t *hi_out)
{
    const radio_config_t *c = RADIO_Config();
    uint32_t flo = 1000000UL, fhi = RADIO_RX_MAX_HZ;
    slot_limits(c->lpf_upper_hz, 5U, dial, &flo, &fhi);
    slot_limits(c->bpf_upper_hz, 4U, dial, &flo, &fhi);
    uint32_t lo = flo, hi = fhi, half = 0U;
    switch (cfg.scan_range) {
        case SPEC_SCAN_250K: half = 125000UL; break;
        case SPEC_SCAN_500K: half = 250000UL; break;
        case SPEC_SCAN_1M:   half = 500000UL; break;
        case SPEC_SCAN_2M:   half = 1000000UL; break;
        case SPEC_SCAN_FILTER: break;
        case SPEC_SCAN_BAND:
        default:
            half = 250000UL;
            for (unsigned i = 0U; i < sizeof(band_khz) / sizeof(band_khz[0]); ++i) {
                const uint32_t blo = band_khz[i][0] * 1000UL, bhi = band_khz[i][1] * 1000UL;
                if (dial >= blo && dial <= bhi) { lo = blo; hi = bhi; half = 0U; break; }
            }
            break;
    }
    if (half) { lo = dial > half ? dial - half : 0U; hi = dial + half; }
    if (lo < flo) lo = flo;
    if (hi > fhi) hi = fhi;
    if (hi < lo + 20000UL) { lo = dial - 10000UL; hi = dial + 10000UL; }
    *lo_out = lo;
    *hi_out = hi;
}

SPEC_SMALL static uint32_t scan_period(unsigned i)
{
    const uint64_t span = (uint64_t)(sp.scan_hi - sp.scan_lo);
    const uint64_t t = sp.scan_lo + (span * (2U * i + 1U)) / (2U * sp.scan_steps);
    return (uint32_t)((sp.hr + t / 2U) / t);
}

static uint32_t period_hz(uint32_t p) { return (uint32_t)((sp.hr + p / 2U) / p); }

/* Pixels [x0, x1) of step i: those nearer its LO than any other step's. */
SPEC_SMALL static void scan_cell(unsigned i, unsigned *x0, unsigned *x1)
{
    const uint32_t lo = period_hz(scan_period(i));
    *x0 = 0U;
    *x1 = SPEC_W;
    if (i > 0U) {
        const float m = 0.5f * (float)(period_hz(scan_period(i - 1U)) + lo);
        const int32_t x = iceil((m - (float)sp.hist_centre) / sp.px_hz + (float)SPEC_CENTER);
        *x0 = (unsigned)(x < 0 ? 0 : (x > (int32_t)SPEC_W ? (int32_t)SPEC_W : x));
    }
    if (i + 1U < sp.scan_steps) {
        const float m = 0.5f * (float)(period_hz(scan_period(i + 1U)) + lo);
        const int32_t x = iceil((m - (float)sp.hist_centre) / sp.px_hz + (float)SPEC_CENTER);
        *x1 = (unsigned)(x < 0 ? 0 : (x > (int32_t)SPEC_W ? (int32_t)SPEC_W : x));
    }
}

/* Put the dial's LO back if the running one is still a hop of ours. A
 * receiver that restarted meanwhile planned its IF on the hopped LO, so it
 * is restarted once more on the right one. */
SPEC_SMALL static void scan_restore(uint8_t restarted)
{
    radio_rx_geometry_t g;
    RADIO_RxClockGeometry(&g);
    if (RADIO_RxClockHz() && sp.hop_period && sp.hop_period != sp.dial_period && g.period == sp.hop_period &&
        MOD_GetCarrierRequestedHz() == sp.scan_dial) {
        (void)RADIO_RxLoHop(sp.dial_period);
        if (restarted) RADIO_AudioSuspend();
    }
    RADIO_AudioSpecStop(); /* also ends the scan mute */
    sp.hop_period = 0U;
    sp.armed = 0U;
    sp.scan_state = SCAN_IDLE;
}

SPEC_SMALL static void scan_abort(uint32_t now, uint8_t restarted)
{
    scan_restore(restarted);
    sp.listen_until = now + SCAN_RETRY_MS;
    sp.acc_n = 0U;
}

SPEC_SMALL static void scan_hop(uint32_t now)
{
    const uint32_t p = scan_period(sp.scan_step);
    uint16_t skip = 0U;
    if (p != sp.hop_period) {
        if (!RADIO_RxLoHop(p)) { scan_abort(now, 0U); return; }
        sp.hop_period = p;
        skip = SCAN_SETTLE_HALVES;
    }
    RADIO_AudioScanMute(1U);
    RADIO_AudioSpecArm(RADIO_SPEC_SRC_RAW, 0U, skip);
    sp.armed = 1U;
    sp.scan_blocks = 0U;
    sp.step_deadline = now + SCAN_STEP_MS;
}

SPEC_SMALL static void scan_start(uint32_t now, uint32_t gen, spec_scratch_t *sc)
{
    radio_rx_geometry_t g;
    RADIO_RxClockGeometry(&g);
    if (!RADIO_RxClockHz() || !g.period) return;
    sp.dial_period = sp.hop_period = g.period;
    sp.scan_dial = MOD_GetCarrierRequestedHz();
    sp.scan_gen = gen;
    sp.hr = RADIO_HrClockHz();
    /* Steps of 0.58 fS: two cells of the flat +-0.3 fS with a little overlap. */
    const float step = 0.58f * RADIO_AudioRate();
    uint32_t n = (uint32_t)ceilf((float)(sp.scan_hi - sp.scan_lo) / (step > 1000.0f ? step : 100000.0f));
    if (n < 1U) n = 1U;
    if (n > SCAN_MAX_STEPS) n = SCAN_MAX_STEPS;
    sp.scan_steps = (uint8_t)n;
    sp.scan_step = 0U;
    acc_reset(sc);
    sp.scan_state = SCAN_CAPTURE;
    scan_hop(now);
}

SPEC_SMALL static uint8_t scan_service(uint32_t now, spec_scratch_t *sc, uint32_t gen)
{
    uint8_t redraw = 0U;
    if (sp.scan_state == SCAN_CAPTURE) {
        if (gen != sp.scan_gen || MOD_GetCarrierRequestedHz() != sp.scan_dial) {
            scan_abort(now, (uint8_t)(gen != sp.scan_gen));
            return 1U;
        }
        float *x = RADIO_AudioSpecTake();
        if (!x) {
            if ((int32_t)(now - sp.step_deadline) > 0) { scan_abort(now, 0U); return 1U; }
            return 0U;
        }
        const uint32_t lo_hz = period_hz(sp.hop_period);
        unsigned x0, x1;
        scan_cell(sp.scan_step, &x0, &x1);
        transform(sc, x, 1U);
        const float f0 = (float)(int32_t)(sp.hist_centre - lo_hz) - (float)SPEC_CENTER * sp.px_hz;
        fold(sc, x, f0, RADIO_AudioRate() / (float)SPEC_FFT_N, RAW_SCAN_BINS, x0, x1, cfg.dc_notch);
        const unsigned n = spec_scan_avgs[cfg.scan_avg < SPEC_SCAN_AVG_COUNT ? cfg.scan_avg : 0U];
        if (++sp.scan_blocks < n) {
            RADIO_AudioSpecArm(RADIO_SPEC_SRC_RAW, 0U, 0U);
            sp.step_deadline = now + SCAN_STEP_MS;
            return 0U;
        }
        if (++sp.scan_step < sp.scan_steps) { scan_hop(now); return 1U; }
        scan_restore(0U);
        emit_line(sc, n, now);
        sp.listen_until = now + ((cfg.scan_audio == SPEC_SCANA_LISTEN) ? spec_listens[cfg.listen < SPEC_LISTEN_COUNT ? cfg.listen : 3U] : 0U);
        return 1U;
    }
    /* Between sweeps: the range follows the dial, the history restarts when
     * it changes. */
    const uint32_t dial = MOD_GetCarrierRequestedHz();
    uint32_t lo, hi;
    scan_limits(dial, &lo, &hi);
    sp.scan_lo = lo;
    sp.scan_hi = hi;
    const uint8_t key = (uint8_t)(0x80U | cfg.scan_range);
    redraw = set_axis(key, lo + (hi - lo) / 2U, (float)(hi - lo) / (float)SPEC_W);
    if (redraw && cfg.scan_audio == SPEC_SCANA_SINGLE) sp.scan_request = 1U;
    if (cfg.scan_audio == SPEC_SCANA_SINGLE ? !sp.scan_request
                                            : ((int32_t)(now - sp.listen_until) < 0 && !sp.scan_request))
        return redraw;
    sp.scan_request = 0U;
    scan_start(now, gen, sc);
    return 1U;
}

void SPEC_ScanTrigger(void) { sp.scan_request = 1U; }

/* ---- Service ------------------------------------------------------------------ */

__attribute__((cold)) void SPEC_Open(void)
{
    if (sp.open) return;
    const uint32_t now = HAL_GetTick();
    sp.open = 1U;
    sp.armed = 0U;
    sp.receiving = 0U;
    sp.guard = 0U;
    sp.guard_until = now;
    sp.meter_at = now;
    sp.cycles = 0U;
    sp.overruns = RADIO_AudioOverruns();
    sp.win_gen = 0U;
    sp.win_kind = 0xFFU;
    sp.arm_at = now;
    sp.line_due = now;
    sp.last_line = 0U;
    sp.rate10 = 0U;
    sp.scan_state = SCAN_IDLE;
    sp.scan_request = 1U;
    sp.listen_until = now;
    sp.axis_valid = 0U;
    /* The guard needs the receive worker's own cycle count; it is one DWT
     * read per half, and the console's setting comes back on close. The
     * diagnostic build has no such counter (RADIO_AudioLoadPct reads 0). */
#if !MOD_DEEP_DIAG_ENABLE
    sp.profile_was = RADIO_AudioProfileEnabled();
    RADIO_AudioProfileEnable(1U);
#endif
}

__attribute__((cold)) void SPEC_Close(void)
{
    if (!sp.open) return;
    if (sp.scan_state == SCAN_CAPTURE) scan_restore(0U);
    RADIO_AudioSpecStop();
#if !MOD_DEEP_DIAG_ENABLE
    RADIO_AudioProfileEnable(sp.profile_was);
#endif
    sp.open = 0U;
    sp.armed = 0U;
    sp.guard = 0U;
    SPEC_Detach();
}

SPEC_SMALL uint8_t SPEC_Service(uint32_t now)
{
    if (!sp.open) return 0U;
    const uint32_t c0 = cycles_now();
    uint8_t redraw = meter(now);
    spec_scratch_t *sc = (spec_scratch_t *)RADIO_AudioSpecScratch();
    const uint8_t rx = (uint8_t)(RADIO_State() == RADIO_RECEIVE && RADIO_RxClockHz() != 0U && sc != NULL);
    if (!rx || (!sp.hist && !sp.trace)) {
        if (sp.scan_state == SCAN_CAPTURE) scan_abort(now, 0U);
        if (sp.receiving) { sp.receiving = 0U; redraw = 1U; }
        sp.armed = 0U;
        return redraw;
    }
    if (!sp.receiving) { sp.receiving = 1U; sp.rx_since = now; redraw = 1U; }
    const uint32_t gen = RADIO_AudioGeneration();
    if (sp.cfg_dirty) {
        /* Pacing, averaging and detector changes start a fresh line; a scan in
         * flight ends (the mode, range or averaging it ran on may be gone). */
        sp.cfg_dirty = 0U;
        if (sp.scan_state == SCAN_CAPTURE) scan_abort(now, 0U);
        sp.armed = 0U;
        sp.arm_at = now;
        sp.listen_until = now;
        sp.scan_request = 1U;
        sp.win_kind = 0xFFU;
        if (sp.peak && cfg.peak_hold == SPEC_PEAK_OFF) memset(sp.peak, 0, SPEC_W);
        colour_scale();
        ++sp.version;
        redraw = 1U;
    }
    if (gen != sp.win_gen || sp.win_kind != cfg.window) {
        /* A receiver start zeroed the scratch: the window table and a line
         * in progress with it. */
        spec_window_table(&spec_windows[cfg.window < SPEC_WIN_COUNT ? cfg.window : 0U], sc->win);
        sp.win_gen = gen;
        sp.win_kind = cfg.window;
        acc_reset(sc);
    }
    if (cfg.mode == SPEC_MODE_SCAN) redraw |= scan_service(now, sc, gen);
    else redraw |= view_service(now, sc, gen);
    sp.cycles += cycles_now() - c0;
    return redraw;
}

SPEC_SMALL void SPEC_GetView(spec_view_t *v)
{
    memset(v, 0, sizeof(*v));
    const uint32_t dial = MOD_GetCarrierRequestedHz();
    v->centre_hz = sp.hist_centre;
    v->hz_per_px = sp.px_hz;
    v->dial_hz = dial;
    v->valid = (uint8_t)(sp.axis_valid && sp.px_hz > 0.0f);
    v->receiving = sp.receiving;
    v->scanning = (uint8_t)(sp.scan_state == SCAN_CAPTURE);
    v->sweep_pct = sp.scan_steps ? (uint8_t)((100U * sp.scan_step) / sp.scan_steps) : 0U;
    v->lines = sp.lines;
    v->load_rx = sp.load_rx;
    v->load_wf = sp.load_wf;
    v->guard = sp.guard;
    v->bottom_db = (int16_t)(sp.bottom8 / 8);
    v->top_db = (int16_t)((sp.bottom8 + sp.span8) / 8);
    v->rate10 = sp.rate10;
    if (v->valid) {
        const float x = (float)SPEC_CENTER + (float)(int32_t)(dial - sp.hist_centre) / sp.px_hz;
        v->dial_x = (int16_t)(x < -1000.0f ? -1000 : (x > 1000.0f ? 1000 : (int32_t)lrintf(x)));
        /* The receive channel as the planner and the filters have it. */
        int32_t lo = 0, hi = 0;
        RADIO_RxChannelSpan((radio_rx_mode_t)RADIO_Config()->rx_mode, &lo, &hi);
        v->pass_lo_x = (int16_t)(v->dial_x + (int32_t)lrintf((float)lo / sp.px_hz));
        v->pass_hi_x = (int16_t)(v->dial_x + (int32_t)lrintf((float)hi / sp.px_hz));
    }
}
