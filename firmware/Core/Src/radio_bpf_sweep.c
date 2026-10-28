#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "radio_bpf_sweep.h"
#include "card_text.h"
#include "radio.h"
#include "radio_audio.h"
#include "radio_clocks.h"
#include "board_rev1.h"
#include "mod_sdcard_service.h"
#include "main.h"
#include "modulator.h"
#include <stdio.h>
#include <string.h>

/* Chop settling covers the PCM1821 group delay after every BPF switch and LO
 * restart; 128 halves (~46 ms) average about 3000 samples per slot. */
#define SWEEP_SETTLE_HALVES   40U
#define SWEEP_MEASURE_HALVES  128U
#define SWEEP_LO_SETTLE_MS    25U
#define SWEEP_LPF_SETTLE_MS   40U
#define SWEEP_RESULT_TIMEOUT  1000U

enum { PHASE_POINT, PHASE_SLOT, PHASE_WAIT };
#define CSV_HEADER_LINES 5U

static struct {
    uint8_t active, status, phase, point, slot, lpf, bpf, inject, clipped, fit_ok, floor_code, measured;
    uint8_t axis, slots;
    uint32_t centre, at;
    const char *message;
} sweep;
static uint8_t levels[RADIO_SWEEP_POINTS][RADIO_SWEEP_SLOTS];
static uint32_t fitted[4];
static char export_name[24];

uint32_t RADIO_SweepPointHz(unsigned k)
{
    return RADIO_SweepGridHz(sweep.axis, sweep.centre, k);
}

__attribute__((cold)) void RADIO_SweepBegin(uint32_t now, uint8_t axis)
{
    memset(&sweep, 0, sizeof(sweep));
    memset(levels, 0, sizeof(levels));
    export_name[0] = '\0';
    sweep.active = 1U;
    sweep.status = RADIO_SWEEP_RUNNING;
    sweep.lpf = 255U;
    sweep.at = now;
    sweep.axis = axis > RADIO_SWEEP_BAND ? RADIO_SWEEP_BPF : axis;
    /* Five PE2..PE6 filters on the LPF axis, four U10 channels otherwise. */
    sweep.slots = sweep.axis == RADIO_SWEEP_LPF ? 5U : 4U;
    sweep.centre = MOD_GetCarrierRequestedHz();
    sweep.message = "MEASURING";
    RADIO_AudioChopStart(0U); /* first result is the no-injection floor */
}

/* Level code as dBFS with one decimal, or an empty CSV field. */
__attribute__((cold)) static int db_field(char *buf, size_t cap, uint8_t code)
{
    if (!code) return snprintf(buf, cap, ",");
    int half = (int)code - 255; /* 0.5 dB units, <= 0 */
    return snprintf(buf, cap, ",-%d.%d", -half / 2, (-half % 2) ? 5 : 0);
}

__attribute__((cold)) static uint8_t slot_for(const uint32_t *edges, uint32_t hz)
{
    radio_config_t c = *RADIO_Config();
    memcpy(c.bpf_upper_hz, edges, sizeof(c.bpf_upper_hz));
    uint8_t lpf, bpf;
    (void)RADIO_ConfigFilters(&c, hz, &lpf, &bpf);
    return bpf;
}

__attribute__((cold)) static uint16_t csv_line(uint16_t index, char *buf, uint16_t cap)
{
    const uint32_t *e = RADIO_Config()->bpf_upper_hz;
    int n = 0;
    if (index == 0U) {
        /* The file goes to the card, so its header comes from the card's
         * text pack (card_text.def) rather than flash. */
        n = (int)UI_CardText(sweep.axis == RADIO_SWEEP_LPF ? TXT_CSV_LPF_SWEEP : TXT_CSV_BPF_SWEEP, buf, cap);
        n += snprintf(buf + n, cap - (uint16_t)n, "\r\n");
    } else if (index == 1U) {
        uint8_t fc = sweep.floor_code ? sweep.floor_code : 1U;
        n = snprintf(buf, cap, "# axis %s, centre_hz %lu, status %s, sysclk %lu, clipped %u, floor",
                     sweep.axis == RADIO_SWEEP_LPF ? "lpf" : sweep.axis == RADIO_SWEEP_BAND ? "band" : "bpf",
                     (unsigned long)sweep.centre, sweep.message ? sweep.message : "",
                     (unsigned long)SystemCoreClock, (unsigned)sweep.clipped);
        n += db_field(buf + n, cap - (uint16_t)n, fc);
        n += snprintf(buf + n, cap - (uint16_t)n, "\r\n");
    } else if (index == 2U) {
        n = snprintf(buf, cap, "# edges_hz_in_use,%lu,%lu,%lu,%lu\r\n",
                     (unsigned long)e[0], (unsigned long)e[1], (unsigned long)e[2], (unsigned long)e[3]);
    } else if (index == 3U) {
        if (sweep.fit_ok)
            n = snprintf(buf, cap, "# edges_hz_fitted,%lu,%lu,%lu,%lu\r\n", (unsigned long)fitted[0],
                         (unsigned long)fitted[1], (unsigned long)fitted[2], (unsigned long)fitted[3]);
        else
            n = snprintf(buf, cap, "# edges_hz_fitted,none\r\n");
    } else if (index == 4U) {
        n = sweep.axis == RADIO_SWEEP_LPF
            ? snprintf(buf, cap, "hz,lpf0_dbfs,lpf1_dbfs,lpf2_dbfs,lpf3_dbfs,lpf4_dbfs\r\n")
            : snprintf(buf, cap, "hz,bpf0_dbfs,bpf1_dbfs,bpf2_dbfs,bpf3_dbfs,bpf_in_use,bpf_fitted\r\n");
    } else {
        unsigned k = index - CSV_HEADER_LINES;
        uint32_t hz = RADIO_SweepPointHz(k);
        n = snprintf(buf, cap, "%lu", (unsigned long)hz);
        for (unsigned slot = 0U; slot < sweep.slots; ++slot)
            n += db_field(buf + n, cap - (uint16_t)n, levels[k][slot]);
        if (sweep.axis == RADIO_SWEEP_LPF) {
            n += snprintf(buf + n, cap - (uint16_t)n, "\r\n");
        } else {
            uint8_t now = slot_for(e, hz), fit = sweep.fit_ok ? slot_for(fitted, hz) : 255U;
            n += snprintf(buf + n, cap - (uint16_t)n, now < 4U ? ",%u" : ",", (unsigned)now);
            n += snprintf(buf + n, cap - (uint16_t)n, fit < 4U ? ",%u\r\n" : ",\r\n", (unsigned)fit);
        }
    }
    return n < 0 ? 0U : (n >= (int)cap ? (uint16_t)(cap - 1U) : (uint16_t)n);
}

static void sweep_abort(const char *message)
{
    RADIO_AudioChopStop();
    sweep.active = 0U;
    sweep.status = RADIO_SWEEP_STOPPED;
    sweep.message = message;
}

/* Main-loop end of a run: keep the curves on SD for comparison across
 * re-tuning runs (no card, USB disk mode or a busy card just skips it). */
__attribute__((cold)) static uint8_t sweep_end(uint8_t status, const char *message)
{
    RADIO_AudioChopStop();
    RADIO_TestClockStop();
    sweep.active = 0U;
    sweep.status = status;
    sweep.message = message;
    if (sweep.measured &&
        !SD_ExportLines(sweep.axis == RADIO_SWEEP_LPF ? "LPF" : sweep.axis == RADIO_SWEEP_BAND ? "RFS" : "BPF",
                        export_name, CSV_HEADER_LINES + RADIO_SWEEP_POINTS, csv_line))
        export_name[0] = '\0';
    return 0U;
}

__attribute__((cold)) static uint8_t sweep_finish(void)
{
    uint8_t top = 0U;
    for (unsigned k = 0U; k < RADIO_SWEEP_POINTS; ++k)
        for (unsigned s = 0U; s < sweep.slots; ++s)
            if (levels[k][s] > top) top = levels[k][s];
    if (!sweep.measured) return sweep_end(RADIO_SWEEP_FAILED, "NO LPF COVERS THIS RANGE");
    /* The carrier must stand 20 dB clear of the detector floor. */
    if ((unsigned)top < (unsigned)sweep.floor_code + 40U) return sweep_end(RADIO_SWEEP_FAILED, "NO TEST SIGNAL");
    /* Only the whole-range BPF axis fits crossovers: the LPF axis measures a
     * different set of filters, and the band axis does not span them. */
    if (sweep.axis != RADIO_SWEEP_BPF)
        return sweep_end(RADIO_SWEEP_DONE, sweep.clipped ? "DONE (ADC CLIPPED)" : "DONE");
    sweep.fit_ok = RADIO_SweepFitEdges(levels, RADIO_Config()->bpf_upper_hz, fitted);
    return sweep_end(sweep.fit_ok ? RADIO_SWEEP_DONE : RADIO_SWEEP_FAILED,
                     sweep.fit_ok ? (sweep.clipped ? "DONE (ADC CLIPPED)" : "DONE") : "SET 2+ BPF EDGES");
}

__attribute__((cold)) uint8_t RADIO_SweepService(uint32_t now)
{
    if (!sweep.active) return 0U;
    if (sweep.phase == PHASE_POINT) {
        if (sweep.point >= RADIO_SWEEP_POINTS) return sweep_finish();
        uint32_t hz = RADIO_SweepPointHz(sweep.point);
        uint8_t lpf = 255U, bpf = 255U;
        (void)RADIO_ConfigFilters(RADIO_Config(), hz, &lpf, &bpf);
        if (bpf >= 4U) { ++sweep.point; return 1U; } /* nothing can hear here */
        uint32_t settle = SWEEP_LO_SETTLE_MS;
        if (sweep.axis == RADIO_SWEEP_LPF) {
            /* The filter under test changes per slot below. The band-pass is
             * fixed for the whole point, so the five curves differ only by the
             * low-pass filter in front of it. */
            sweep.bpf = bpf;
            sweep.lpf = 255U; /* force a select on the first slot */
        } else {
            if (lpf >= 5U) { ++sweep.point; return 1U; } /* no receive path here */
            if (lpf != sweep.lpf) {
                RADIO_SelectFilters(lpf, bpf);
                sweep.lpf = lpf;
                settle = SWEEP_LPF_SETTLE_MS;
            }
        }
        if (!RADIO_MeasureClocksStart(hz)) return sweep_end(RADIO_SWEEP_FAILED, "CLOCK BLOCKED");
        BOARD_SetRx(1U);
        sweep.slot = 0U;
        sweep.phase = PHASE_SLOT;
        sweep.at = now + settle;
        return 1U;
    }
    if (sweep.phase == PHASE_SLOT) {
        if ((int32_t)(now - sweep.at) < 0) return 1U;
        if (sweep.axis == RADIO_SWEEP_LPF) {
            /* RADIO_SelectFilters clears RX_EN before it moves the one-hot
             * select, so the PIN diodes are never hot-switched; RX_EN goes
             * back on here and the diodes get their own settling time before
             * anything is measured. */
            if (sweep.slot != sweep.lpf) {
                RADIO_SelectFilters(sweep.slot, sweep.bpf);
                sweep.lpf = sweep.slot;
                BOARD_SetRx(1U);
                sweep.at = now + SWEEP_LPF_SETTLE_MS;
                return 1U;
            }
        } else {
            RADIO_SelectBpf(sweep.slot);
        }
        RADIO_AudioChopMeasure(SWEEP_SETTLE_HALVES, SWEEP_MEASURE_HALVES);
        sweep.phase = PHASE_WAIT;
        sweep.at = now;
        return 1U;
    }
    float dbfs;
    uint8_t clipped;
    if (!RADIO_AudioChopResult(&dbfs, &clipped)) {
        if (now - sweep.at > SWEEP_RESULT_TIMEOUT) return sweep_end(RADIO_SWEEP_FAILED, "NO RECEIVE DATA");
        return 1U;
    }
    if (!sweep.inject) {
        sweep.floor_code = RADIO_SweepCode(dbfs);
        sweep.inject = 1U;
        RADIO_AudioChopStart(1U);
    } else {
        sweep.clipped |= clipped;
        sweep.measured = 1U;
        levels[sweep.point][sweep.slot] = RADIO_SweepCode(dbfs);
        if (++sweep.slot >= sweep.slots) { ++sweep.point; sweep.phase = PHASE_POINT; return 1U; }
    }
    sweep.phase = PHASE_SLOT;
    sweep.at = now;
    return 1U;
}

void RADIO_SweepStop(void)
{
    RADIO_AudioChopStop();
    if (sweep.active) (void)sweep_end(RADIO_SWEEP_STOPPED, "STOPPED");
}

void RADIO_SweepInfo(radio_sweep_info_t *info)
{
    /* A protection trip clears calibration without a normal stop. */
    if (sweep.active && RADIO_CalActive() != RADIO_CAL_BPF_SWEEP) sweep_abort("STOPPED BY PROTECTION");
    info->status = sweep.status;
    info->point = sweep.point;
    info->clipped = sweep.clipped;
    info->fit_ok = sweep.fit_ok;
    info->axis = sweep.axis;
    info->slots = sweep.slots ? sweep.slots : 4U;
    info->centre_hz = sweep.centre;
    info->floor_dbfs = sweep.floor_code ? RADIO_SweepCodeDb(sweep.floor_code) : -127.5f;
    info->message = sweep.message ? sweep.message : "NOT RUN";
    info->export_name = export_name[0] ? export_name : NULL;
}

uint8_t RADIO_SweepLevel(unsigned point, unsigned slot)
{
    return (point < RADIO_SWEEP_POINTS && slot < RADIO_SWEEP_SLOTS) ? levels[point][slot] : 0U;
}

const uint32_t *RADIO_SweepEdges(void)
{
    return (sweep.status == RADIO_SWEEP_DONE && sweep.fit_ok) ? fitted : NULL;
}
