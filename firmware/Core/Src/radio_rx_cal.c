#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "radio_rx_cal.h"
#include "radio.h"
#include "radio_audio.h"
#include "radio_clocks.h"
#include "radio_config.h"
#include "radio_dsp.h"
#include "modulator.h"
#include "main.h"
#include <string.h>

/* Not built into the deep-diagnostic flavour. That image carries its own
 * pipeline instrumentation and its FLASH_APP region is full: the receive bench
 * costs about 4 KB it does not have, and a receiver is calibrated on the
 * shipping images. The console says so rather than pretending the command
 * exists, the same way the full-screen `ui` view does. */
#if MOD_DEEP_DIAG_ENABLE

void RADIO_IqCalBegin(uint32_t now, uint8_t with_sideband) { (void)now; (void)with_sideband; }
uint8_t RADIO_IqCalService(uint32_t now) { (void)now; return 0U; }
void RADIO_IqCalStop(void) {}
uint8_t RADIO_IqCalApply(void) { return 0U; }
void RADIO_IqCalInfo(radio_iqcal_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->message = "NOT BUILT INTO THIS FLAVOUR";
}

#else

/* 65536 samples at 177 kHz is 370 ms of coherent integration and a 2.7 Hz
 * detector bandwidth, which puts the bin noise far below any imbalance worth
 * correcting. Three passes plus settling come to about 1.4 s. */
#define IQ_SAMPLES          65536UL
#define IQ_SETTLE_HALVES    40U    /* PCM1821 group delay, as the sweep uses */
#define IQ_GATE_SETTLE_MS   25U    /* BPF and the QSD node RCs after gating */
#define IQ_SB_SETTLE_MS     250U   /* channel filter and the level averager */
#define IQ_RESULT_TIMEOUT   2000U
#define IQ_MIN_DELTA_HZ     6000.0f
#define IQ_MAX_DELTA_HZ     45000.0f
/* The same margin the BPF sweep demands of its carrier over the floor. */
#define IQ_SIGNAL_MARGIN_DB 20.0f
/* Where the audio channel is centred, so a fixed carrier can be put in the
 * wanted sideband and then in the opposite one. RADIO_DspInit uses the same
 * numbers. Only USB and LSB have an opposite sideband to reject: CW, AM and FM
 * have symmetric channel filters, where the image rejection above is the whole
 * story. */
#define IQ_SB_CENTRE_HZ     1650.0f

enum { PASS_FLOOR, PASS_RAW, PASS_VERIFY, PASS_SB_WANTED, PASS_SB_OPPOSITE, PASS_END };
enum { STEP_GATE, STEP_SETTLE, STEP_COLLECT };

static struct {
    uint8_t active, pass, step, sideband, retuned;
    uint32_t at;
    float cycles;               /* delta / fS, exactly */
    radio_iqcal_info_t info;
} cal;

static uint8_t iqcal_end(uint8_t status, const char *message)
{
    RADIO_AudioIqCalStop();
    RADIO_TestOutput(0U);
    RADIO_TestClockStop();
    /* Put the demodulator back where receive expects it, but only if a
     * sideband pass actually moved it: this runs from RADIO_CalStart's
     * critical section too, and RADIO_DspInit is not free. */
    if (cal.retuned) RADIO_AudioRetuneDsp(0.0f);
    cal.active = 0U;
    cal.info.status = status;
    cal.info.message = message;
    return 0U;
}

__attribute__((cold)) void RADIO_IqCalBegin(uint32_t now, uint8_t with_sideband)
{
    const uint8_t mode = RADIO_Config()->rx_mode;
    memset(&cal, 0, sizeof(cal));
    cal.info.status = RADIO_IQCAL_RUNNING;
    cal.info.message = "MEASURING";
    cal.info.rf_hz = MOD_GetCarrierRequestedHz();
    cal.info.lo_hz = RADIO_RxClockHz();
    cal.sideband = (uint8_t)(with_sideband &&
                             (mode == RADIO_RX_USB || mode == RADIO_RX_LSB));

    radio_rx_geometry_t g;
    RADIO_RxClockGeometry(&g);
    const uint32_t divider = RADIO_AudioI2sDivider();
    const float rate = RADIO_AudioRate();
    cal.info.lo_period = g.period;
    if (!g.period || !divider || !(rate > 0.0f)) {
        (void)iqcal_end(RADIO_IQCAL_FAILED, "RECEIVER NOT RUNNING");
        return;
    }
    uint32_t test_period = 0U;
    if (!RADIO_IqChoosePeriod(g.period, divider, rate,
                              IQ_MIN_DELTA_HZ, IQ_MAX_DELTA_HZ, &test_period)) {
        (void)iqcal_end(RADIO_IQCAL_FAILED, "NO TEST OFFSET AT THIS FREQUENCY");
        return;
    }
    cal.cycles = RADIO_IqDeltaCycles(g.period, test_period, divider);
    cal.info.test_period = test_period;
    cal.info.delta_hz = (int32_t)(cal.cycles * rate + (cal.cycles > 0.0f ? 0.5f : -0.5f));
    /* Programmed once and gated per pass, so every pass sees the same carrier
     * and the floor pass sees the same receiver with it switched off. */
    if (!RADIO_TestClockStartPeriod(test_period)) {
        (void)iqcal_end(RADIO_IQCAL_FAILED, "CLOCK BLOCKED");
        return;
    }
    RADIO_TestOutput(0U);
    cal.active = 1U;
    cal.pass = PASS_FLOOR;
    cal.step = STEP_GATE;
    cal.at = now;
}

/* One correlated pass: gate the injection, let the path settle, integrate. */
__attribute__((cold, noinline)) static uint8_t service_correlate(uint32_t now)
{
    if (cal.step == STEP_GATE) {
        RADIO_TestOutput(cal.pass != PASS_FLOOR);
        cal.at = now + IQ_GATE_SETTLE_MS;
        cal.step = STEP_SETTLE;
        return 1U;
    }
    if (cal.step == STEP_SETTLE) {
        if ((int32_t)(now - cal.at) < 0) return 1U;
        /* The verification pass corrects the stream before correlating, so
         * what it reports is a measured residual and not a prediction. */
        const uint8_t verify = cal.pass == PASS_VERIFY;
        RADIO_AudioIqCalStart(cal.cycles, IQ_SAMPLES, IQ_SETTLE_HALVES,
                              verify ? cal.info.measured.inverted : 0U,
                              verify ? cal.info.measured.q_gain : 1.0f,
                              verify ? cal.info.measured.skew_samples : 0.0f);
        cal.step = STEP_COLLECT;
        cal.at = now;
        return 1U;
    }
    radio_iq_corr_t corr;
    if (!RADIO_AudioIqCalResult(&corr)) {
        if (now - cal.at > IQ_RESULT_TIMEOUT) return iqcal_end(RADIO_IQCAL_FAILED, "NO RECEIVE DATA");
        return 1U;
    }
    radio_iq_result_t result;
    const uint8_t solved = RADIO_IqSolve(&corr, &result);
    if (cal.pass == PASS_FLOOR) {
        /* Nothing injected: whichever bin is higher is what the measurement
         * has to stand clear of. */
        cal.info.floor_dbfs = result.wanted_dbfs > result.image_dbfs
                            ? result.wanted_dbfs : result.image_dbfs;
    } else if (cal.pass == PASS_RAW) {
        if (!solved) return iqcal_end(RADIO_IQCAL_FAILED, "NO CARRIER IN THE RECEIVER");
        if (result.wanted_dbfs < cal.info.floor_dbfs + IQ_SIGNAL_MARGIN_DB)
            return iqcal_end(RADIO_IQCAL_FAILED, "NO TEST SIGNAL");
        cal.info.measured = result;
        cal.info.have_result = 1U;
    } else {
        cal.info.corrected_image_db = result.image_db;
    }
    cal.pass = (uint8_t)(cal.pass + 1U);
    cal.step = STEP_GATE;
    return 1U;
}

/* The carrier stays where it is; the demodulator moves, so the same tone is
 * read once in the wanted sideband and once in the opposite one. */
__attribute__((cold, noinline)) static uint8_t service_sideband(uint32_t now)
{
    if (cal.step == STEP_GATE) {
        const float centre = RADIO_Config()->rx_mode == RADIO_RX_LSB
                           ? -IQ_SB_CENTRE_HZ : IQ_SB_CENTRE_HZ;
        /* Where the injected carrier sits in the 48 kHz stream: the receiver
         * has already shifted the tuned frequency to DC. */
        const float tone = (float)cal.info.delta_hz -
                           ((float)cal.info.rf_hz - (float)cal.info.lo_hz);
        RADIO_AudioRetuneDsp(cal.pass == PASS_SB_WANTED ? tone - centre : tone + centre);
        cal.retuned = 1U;
        cal.at = now + IQ_SB_SETTLE_MS;
        cal.step = STEP_SETTLE;
        return 1U;
    }
    if ((int32_t)(now - cal.at) < 0) return 1U;
    const float level = RADIO_AudioRxLevelDbfs();
    if (cal.pass == PASS_SB_WANTED) cal.info.in_band_dbfs = level;
    else {
        cal.info.opposite_dbfs = level;
        cal.info.opposite_db = cal.info.in_band_dbfs - level;
        cal.info.sideband_valid = 1U;
    }
    cal.pass = (uint8_t)(cal.pass + 1U);
    cal.step = STEP_GATE;
    return 1U;
}

__attribute__((cold)) uint8_t RADIO_IqCalService(uint32_t now)
{
    if (!cal.active) return 0U;
    if (cal.pass <= PASS_VERIFY) return service_correlate(now);
    if (cal.sideband && cal.pass <= PASS_SB_OPPOSITE) return service_sideband(now);
    return iqcal_end(RADIO_IQCAL_DONE,
                     cal.info.measured.clipped ? "DONE (ADC CLIPPED)" : "DONE");
}

void RADIO_IqCalStop(void)
{
    if (cal.active) (void)iqcal_end(RADIO_IQCAL_STOPPED, "STOPPED");
}

void RADIO_IqCalInfo(radio_iqcal_info_t *info)
{
    /* A protection trip clears calibration without a normal stop. */
    if (cal.active && RADIO_CalActive() != RADIO_CAL_RX_IQ)
        (void)iqcal_end(RADIO_IQCAL_STOPPED, "STOPPED BY PROTECTION");
    *info = cal.info;
    if (!info->message) info->message = "NOT RUN";
}

__attribute__((cold)) uint8_t RADIO_IqCalApply(void)
{
    radio_config_t c;
    if (cal.active || !cal.info.have_result || !cal.info.measured.valid) return 0U;
    const radio_iq_result_t *r = &cal.info.measured;
    RADIO_GetConfig(&c);
    c.rx_iq_invert = r->inverted;
    c.rx_i_gain = r->i_gain;
    c.rx_q_gain = r->q_gain;
    c.rx_skew_samples = r->skew_samples;
    /* RADIO_ConfigValid bounds the gains and fractional sample skew.
     * A measurement outside that is not an imbalance to trim out, so it is
     * reported and refused rather than clamped into looking reasonable. */
    if (!RADIO_ConfigValid(&c)) return 0U;
    RADIO_SetConfig(&c);
    return 1U;
}

#endif /* MOD_DEEP_DIAG_ENABLE */
