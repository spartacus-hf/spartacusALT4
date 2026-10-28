#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "radio_af_sweep.h"
#include "card_text.h"
#include <math.h>
#include "radio_bpf_sweep.h"
#include "radio_iq_fit.h"
#include "radio.h"
#include "radio_audio.h"
#include "radio_clocks.h"
#include "radio_config.h"
#include "radio_rx_front.h"
#include "board_rev1.h"
#include "mod_sdcard_service.h"
#include "main.h"
#include "modulator.h"
#include <stdio.h>
#include <string.h>

/* A retune rebuilds the channel filters from zero, so each point waits for
 * their impulse response to die away before integrating. 88 halves is about
 * 32 ms at 177 kHz, then 32 halves (11.5 ms, ~550 output samples) are
 * averaged: 43 ms a point, so the 152-point AF sweep runs in about 6.5 s. */
#define AF_SETTLE_HALVES    88U
#define AF_MEASURE_HALVES   32U
#define AF_RESULT_TIMEOUT   1000U
/* The floor pass parks the demodulator well away from the carrier. */
#define AF_FLOOR_OFFSET_HZ  9000.0f
/* An image sweep wants the smallest delta the band offers, so the image stays
 * inside the decimated stream. */
#define AF_IMAGE_MIN_DELTA  6000.0f
#define AF_IMAGE_MAX_DELTA  12000.0f
#define AF_MIN_DELTA        6000.0f
#define AF_MAX_DELTA        45000.0f

#define CSV_HEADER_LINES    5U

#if MOD_DEEP_DIAG_ENABLE

void RADIO_AfSweepBegin(uint32_t now, uint8_t image) { (void)now; (void)image; }
uint8_t RADIO_AfSweepService(uint32_t now) { (void)now; return 0U; }
void RADIO_AfSweepStop(void) { }
void RADIO_AfSweepInfo(radio_af_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->message = "NOT BUILT INTO THE DIAGNOSTIC FLAVOUR";
}
uint8_t RADIO_AfSweepLevel(unsigned point, unsigned curve) { (void)point; (void)curve; return 0U; }

#else

enum { STEP_ARM, STEP_WAIT };

/* Main SRAM, not CCM: this is foreground measurement data touched once every
 * 43 ms, and CCM's reserve is there for the transmit pipeline's buffers. */
static struct {
    uint8_t active, phase, floor_done, measured, curve;
    uint32_t at;
    radio_af_info_t info;
} af;
static uint8_t levels[RADIO_AF_POINTS][RADIO_AF_CURVES];
static uint8_t floor_code;
static char export_name[24];

__attribute__((cold)) static int db_field(char *buf, size_t cap, uint8_t code)
{
    if (!code) return snprintf(buf, cap, ",");
    const int half = (int)code - 255; /* 0.5 dB units, <= 0 */
    return snprintf(buf, cap, ",-%d.%d", -half / 2, (-half % 2) ? 5 : 0);
}

__attribute__((cold)) static uint16_t csv_line(uint16_t index, char *buf, uint16_t cap)
{
    int n = 0;
    if (index == 0U) {
        /* The file goes to the card: its header comes from the card's text
         * pack (card_text.def) rather than flash. */
        n = (int)UI_CardText(af.info.image_mode ? TXT_CSV_IMAGE_SWEEP : TXT_CSV_AF_SWEEP, buf, cap);
        n += snprintf(buf + n, cap - (uint16_t)n, "\r\n");
    } else if (index == 1U) {
        n = snprintf(buf, cap, "# status %s, rf_hz %lu, lo_hz %lu, delta_hz %ld, clipped %u\r\n",
                     af.info.message ? af.info.message : "",
                     (unsigned long)af.info.rf_hz, (unsigned long)af.info.lo_hz,
                     (long)af.info.delta_hz, (unsigned)af.info.clipped);
    } else if (index == 2U) {
        n = snprintf(buf, cap, "# tone_hz %ld, image_hz %ld, mode %s\r\n",
                     (long)af.info.tone_hz, (long)af.info.image_hz,
                     RADIO_Config()->rx_mode == RADIO_RX_LSB ? "LSB" : "USB");
    } else if (index == 3U) {
        n = snprintf(buf, cap, "# floor");
        n += db_field(buf + n, cap - (uint16_t)n, floor_code);
        n += snprintf(buf + n, cap - (uint16_t)n, " dbfs, worst_ratio_ddb %ld\r\n",
                      (long)(af.info.worst_ratio_db * 10.0f));
    } else if (index == 4U) {
        n = snprintf(buf, cap, af.info.image_mode ? "offset_hz,dbfs\r\n"
                                                  : "audio_hz,wanted_dbfs,opposite_dbfs\r\n");
    } else {
        const unsigned k = index - CSV_HEADER_LINES;
        if (af.info.image_mode)
            n = snprintf(buf, cap, "%ld", (long)RADIO_ImagePointHz(af.info.first_hz, af.info.step_hz, k));
        else
            n = snprintf(buf, cap, "%ld", (long)RADIO_AfPointHz(k));
        for (unsigned curve = 0U; curve < af.info.curves; ++curve)
            n += db_field(buf + n, cap - (uint16_t)n, levels[k][curve]);
        n += snprintf(buf + n, cap - (uint16_t)n, "\r\n");
    }
    return n < 0 ? 0U : (n >= (int)cap ? (uint16_t)(cap - 1U) : (uint16_t)n);
}

__attribute__((cold)) static uint8_t af_end(uint8_t status, const char *message)
{
    RADIO_AudioLevelStop();
    RADIO_TestOutput(0U);
    RADIO_TestClockStop();
    /* Put the demodulator back where the operator left it. */
    RADIO_AudioRetuneDsp(0.0f);
    af.active = 0U;
    af.info.status = status;
    af.info.message = message;
    if (af.measured && !SD_ExportLines(af.info.image_mode ? "IMG" : "AFS", export_name,
                                       CSV_HEADER_LINES + RADIO_AF_POINTS, csv_line))
        export_name[0] = '\0';
    af.info.export_name = export_name[0] ? export_name : NULL;
    return 0U;
}

static void af_abort(const char *message)
{
    RADIO_AudioLevelStop();
    af.active = 0U;
    af.info.status = RADIO_AF_STOPPED;
    af.info.message = message;
}

__attribute__((cold)) void RADIO_AfSweepBegin(uint32_t now, uint8_t image)
{
    memset(&af, 0, sizeof(af));
    memset(levels, 0, sizeof(levels));
    floor_code = 0U;
    export_name[0] = '\0';
    af.info.image_mode = image ? 1U : 0U;
    af.info.curves = image ? 1U : RADIO_AF_CURVES;
    af.info.status = RADIO_AF_RUNNING;
    af.info.message = "MEASURING";
    af.info.worst_ratio_db = 0.0f;

    const uint8_t mode = RADIO_Config()->rx_mode;
    if (mode != RADIO_RX_USB && mode != RADIO_RX_LSB) {
        (void)af_end(RADIO_AF_FAILED, "SET USB OR LSB FIRST");
        return;
    }
    radio_rx_geometry_t g;
    RADIO_RxClockGeometry(&g);
    const uint32_t divider = RADIO_AudioI2sDivider();
    const float rate = RADIO_AudioRate();
    af.info.rf_hz = MOD_GetCarrierRequestedHz();
    af.info.lo_hz = RADIO_RxClockHz();
    if (!g.period || !divider || !(rate > 0.0f) || !af.info.lo_hz) {
        (void)af_end(RADIO_AF_FAILED, "RECEIVER NOT RUNNING");
        return;
    }
    uint32_t test_period = 0U;
    if (!RADIO_IqChoosePeriod(g.period, divider, rate,
                              image ? AF_IMAGE_MIN_DELTA : AF_MIN_DELTA,
                              image ? AF_IMAGE_MAX_DELTA : AF_MAX_DELTA, &test_period)) {
        (void)af_end(RADIO_AF_FAILED, image ? "NO SMALL TEST OFFSET ON THIS BAND: USE RXCAL"
                                            : "NO TEST OFFSET AT THIS FREQUENCY");
        return;
    }
    /* The front end has already shifted the tuned frequency to DC, so a
     * carrier delta above the LO sits at delta - (rf - lo) in the 48 kHz
     * stream and its image, the conjugate the QSD imbalance leaves behind,
     * sits at -(delta + (rf - lo)).
     *
     * Which side of the LO the injection goes matters, because those two
     * terms either cancel or add. RADIO_IqChoosePeriod prefers the side above
     * the LO, which is right for rxcal -- it wants the wanted bin positive --
     * but wrong here whenever the planner put the LO above the carrier. On
     * 40 m that makes the intermediate -11.1 kHz and a +9.3 kHz delta lands
     * the tone at +20.4 kHz, where the top of the sweep and the floor pass
     * both fall outside the stream. The mirrored period puts it at +1.8 kHz.
     * So try the other side and keep whichever brings the tone nearer the
     * middle; the period is symmetric about the LO's. */
    const float intermediate = (float)af.info.rf_hz - (float)af.info.lo_hz;
    float cycles = RADIO_IqDeltaCycles(g.period, test_period, divider);
    if (g.period * 2U > test_period) {
        const uint32_t mirror = g.period * 2U - test_period;
        if (mirror >= RADIO_IQ_PERIOD_MIN && mirror <= RADIO_IQ_PERIOD_MAX) {
            const float m_cycles = RADIO_IqDeltaCycles(g.period, mirror, divider);
            const float here = cycles * rate - intermediate;
            const float there = m_cycles * rate - intermediate;
            if (fabsf(there) < fabsf(here)) { test_period = mirror; cycles = m_cycles; }
        }
    }
    const float delta = cycles * rate;
    af.info.delta_hz = (int32_t)(delta + (delta > 0.0f ? 0.5f : -0.5f));
    af.info.tone_hz = delta - intermediate;
    af.info.image_hz = -(delta + intermediate);
    /* Say so plainly rather than letting the run die at the first point with
     * "NO RECEIVE DATA", which is what an offset outside the stream looks
     * like from the demodulator's side. */
    if (!image && !RADIO_AfSpanFits(af.info.tone_hz)) {
        (void)af_end(RADIO_AF_FAILED, "CARRIER TOO FAR OUT FOR AN AF SWEEP ON THIS BAND");
        return;
    }
    if (image && !RADIO_ImageSpan(af.info.tone_hz, af.info.image_hz,
                                  &af.info.first_hz, &af.info.step_hz)) {
        (void)af_end(RADIO_AF_FAILED, "IMAGE OUTSIDE THE 48 KHZ STREAM: USE RXCAL");
        return;
    }
    if (!RADIO_TestClockStartPeriod(test_period)) {
        (void)af_end(RADIO_AF_FAILED, "CLOCK BLOCKED");
        return;
    }
    RADIO_TestOutput(0U); /* floor pass first, as the BPF sweep does */
    af.active = 1U;
    af.phase = STEP_ARM;
    af.at = now;
}

/* Demodulator offset for the point the run is on. */
static float current_offset(void)
{
    /* The floor pass runs with the injection off, so this only has to be a
     * legal place in the stream clear of the carrier's own bin -- and it has
     * to stay inside it, which `tone + offset` does not when the tone is
     * already near the edge. Step toward DC instead. */
    if (!af.floor_done)
        return af.info.tone_hz + (af.info.tone_hz >= 0.0f ? -AF_FLOOR_OFFSET_HZ
                                                          : AF_FLOOR_OFFSET_HZ);
    const uint8_t lsb = RADIO_Config()->rx_mode == RADIO_RX_LSB;
    if (af.info.image_mode)
        return RADIO_AfDemodOffsetHz(
            RADIO_ImagePointHz(af.info.first_hz, af.info.step_hz, af.info.point), lsb);
    return RADIO_AfOffsetHz(af.info.tone_hz, af.info.point, af.curve, lsb);
}

__attribute__((cold)) static uint8_t af_finish(void)
{
    if (!af.measured) return af_end(RADIO_AF_FAILED, "NO RECEIVE DATA");
    uint8_t top = 0U;
    for (unsigned k = 0U; k < RADIO_AF_POINTS; ++k)
        for (unsigned c = 0U; c < af.info.curves; ++c)
            if (levels[k][c] > top) top = levels[k][c];
    af.info.best_dbfs = RADIO_SweepCodeDb(top ? top : 1U);
    /* The carrier has to stand clear of the detector floor or the curve is
     * noise, which is the same qualification the BPF sweep applies. */
    if ((unsigned)top < (unsigned)floor_code + 40U)
        return af_end(RADIO_AF_FAILED, "NO TEST SIGNAL");
    if (af.info.image_mode) {
        /* Wanted over image: the strongest point near each of the two. */
        uint8_t wanted = 0U, image = 0U;
        for (unsigned k = 0U; k < RADIO_AF_POINTS; ++k) {
            const float off = RADIO_ImagePointHz(af.info.first_hz, af.info.step_hz, k);
            const float dw = off - af.info.tone_hz, di = off - af.info.image_hz;
            const float aw = dw < 0.0f ? -dw : dw, ai = di < 0.0f ? -di : di;
            if (aw < ai) { if (levels[k][0] > wanted) wanted = levels[k][0]; }
            else if (levels[k][0] > image) image = levels[k][0];
        }
        af.info.worst_ratio_db = (wanted && image)
            ? RADIO_SweepCodeDb(wanted) - RADIO_SweepCodeDb(image) : 0.0f;
    } else {
        /* Least wanted-over-opposite inside the 300-2700 Hz core of the
         * passband, which is where QMX judges its own curve. */
        float worst = 200.0f;
        uint8_t compared = 0U;
        for (unsigned k = 0U; k < RADIO_AF_POINTS; ++k) {
            const int32_t hz = RADIO_AfPointHz(k);
            if (hz < 300 || hz > 2700) continue;
            if (!levels[k][0] || !levels[k][1]) continue;
            const float ratio = RADIO_SweepCodeDb(levels[k][0]) - RADIO_SweepCodeDb(levels[k][1]);
            if (ratio < worst) worst = ratio;
            compared = 1U;
        }
        af.info.worst_ratio_db = compared ? worst : 0.0f;
    }
    return af_end(RADIO_AF_DONE, af.info.clipped ? "DONE (ADC CLIPPED)" : "DONE");
}

__attribute__((cold)) uint8_t RADIO_AfSweepService(uint32_t now)
{
    if (!af.active) return 0U;
    if (af.phase == STEP_ARM) {
        if (af.floor_done && af.info.point >= RADIO_AF_POINTS) return af_finish();
        RADIO_AudioRetuneDsp(current_offset());
        RADIO_AudioLevelMeasure(AF_SETTLE_HALVES, AF_MEASURE_HALVES);
        af.phase = STEP_WAIT;
        af.at = now;
        return 1U;
    }
    float dbfs = 0.0f;
    uint8_t clipped = 0U;
    if (!RADIO_AudioLevelResult(&dbfs, &clipped)) {
        if (now - af.at > AF_RESULT_TIMEOUT) return af_end(RADIO_AF_FAILED, "NO RECEIVE DATA");
        return 1U;
    }
    if (!af.floor_done) {
        floor_code = RADIO_SweepCode(dbfs);
        af.info.floor_dbfs = RADIO_SweepCodeDb(floor_code);
        af.floor_done = 1U;
        RADIO_TestOutput(1U);
    } else {
        af.info.clipped |= clipped;
        af.measured = 1U;
        /* The two curves are interleaved rather than swept one after the
         * other, so the wanted and opposite readings at a frequency are taken
         * a few tens of milliseconds apart and any drift affects them
         * equally. */
        levels[af.info.point][af.curve] = RADIO_SweepCode(dbfs);
        if (++af.curve >= af.info.curves) { af.curve = 0U; ++af.info.point; }
    }
    af.phase = STEP_ARM;
    af.at = now;
    return 1U;
}

void RADIO_AfSweepStop(void)
{
    if (af.active) (void)af_end(RADIO_AF_STOPPED, "STOPPED");
}

void RADIO_AfSweepInfo(radio_af_info_t *info)
{
    /* A protection trip clears calibration without a normal stop. */
    if (af.active && RADIO_CalActive() != RADIO_CAL_AF_SWEEP) af_abort("STOPPED BY PROTECTION");
    *info = af.info;
    if (!info->message) info->message = "NOT RUN";
    info->export_name = export_name[0] ? export_name : NULL;
}

uint8_t RADIO_AfSweepLevel(unsigned point, unsigned curve)
{
    return (point < RADIO_AF_POINTS && curve < RADIO_AF_CURVES) ? levels[point][curve] : 0U;
}

#endif /* MOD_DEEP_DIAG_ENABLE */
