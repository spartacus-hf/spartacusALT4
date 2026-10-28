/* Size over speed: nothing here runs in the RF or audio path. */
#pragma GCC optimize ("Os")

#include "mod_selftest.h"
#include "card_text.h"
#include "radio_rx_cal.h"
#include "radio_af_sweep.h"
#include "radio.h"
#include "radio_analog.h"
#include "radio_audio.h"
#include "radio_clocks.h"
#include "radio_rx_front.h"
#include "radio_bpf_sweep.h"
#include "radio_pd.h"
#include "board_rev1.h"
#include "modulator.h"
#include "mod_sdcard_service.h"
#include "mod_stack_watermark.h"
#include "main.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Timing. The LPF numbers come from C73 (470 uF): falling through the two
 * 33 ohm feeds is about 20 ms per time constant, rising through R74/R77 in
 * parallel about 235 ms, so a deselect needs far longer to settle than a
 * select does. See the model in mod_selftest_logic.h. */
#define ST_RIPPLE_MS        200U
#define ST_ADC_RATE_MS      100U
#define ST_LPF_SETTLE_MS    400U
#define ST_LPF_OFF_MS       800U
#define ST_CONTROLS_MS    60000U
#define ST_RX_SETTLE_MS     300U
#define ST_BIAS_MS         2500U
#define ST_ENV_STEP_MS       25U
#define ST_ENV_STEP_CODE    256U
#define ST_ENV_MAX_POINTS    16U
/* Instantaneous steps run back to back, but never so many that one main-loop
 * pass stops servicing the display and the watchdog. */
#define ST_STEPS_PER_TICK     6U

/* Control rows, in the order MOD_SelfTestNote* reports them. */
#define ST_CTRL_BITS 10U

static struct {
    uint8_t  active, mask, step, phase, armed, onehot_bad, bias_slot;
    uint16_t controls_hit;  /* ST_CTRL_BITS is 10: this must be wider than a byte */
    uint8_t  env_points;
    uint32_t at, started, finished, seq0, tick0, bias_at;
    uint16_t lo_mv, hi_mv;
    uint16_t env_code[ST_ENV_MAX_POINTS], env_a_mv[ST_ENV_MAX_POINTS], env_b_mv[ST_ENV_MAX_POINTS];
    uint32_t overruns0;
    float    bias_ma;
    const char *status, *status_suffix; /* literals either side of status_n */
    char     export_name[24];  /* EXPORTS/STnnn.CSV of the last finished run */
    uint8_t  status_n;
} st;

/* The results outlive the run and are read only by the foreground -- the UI
 * page, the console and the SD export. That makes them CPU-only data, so they
 * live in CCM and leave the 648 bytes of SRAM they would otherwise hold for
 * the USB console's endpoint buffers, which must be in normal SRAM. */
static selftest_result_t st_row[SELFTEST_COUNT] __attribute__((section(".ccmram_bss")));

static uint8_t pending_mask = ST_GROUP_DEFAULT;

/* ---- result plumbing ---- */

/* The limits a row was judged against live in flash (MOD_SelfTestLimits), so
 * a row stores only what the run produced. Its detail is a card text
 * (card_text.def), or what another module words at run time (record_s). */
static void row_set(uint8_t id, uint8_t verdict, int32_t value, const char *detail, card_text_t text)
{
    selftest_result_t *r = &st_row[id];
    r->verdict = verdict;
    r->value = value;
    r->detail = detail;
    r->text = (uint16_t)text;
}

static void record(uint8_t id, uint8_t verdict, int32_t value, card_text_t text)
{
    row_set(id, verdict, value, NULL, text);
}

static void record_s(uint8_t id, uint8_t verdict, int32_t value, const char *detail)
{
    row_set(id, verdict, value, detail ? detail : "", TXT_NONE);
}

static void record_offer(uint8_t id, uint8_t verdict, int32_t value, card_text_t text)
{
    record(id, verdict, value, text);
    st_row[id].flags |= ST_FLAG_OFFERS;
}

static void skip(uint8_t id, card_text_t why) { record(id, ST_SKIP, 0, why); }
static void skip_s(uint8_t id, const char *why) { record_s(id, ST_SKIP, 0, why); }

/* The PCM1821 rate window: SYSCLK / 1792 (ADC at ~96 kHz) or SYSCLK / 960,
 * SYSCLK anywhere in 165-170 MHz. */
static void pcm_window(uint8_t adc96, int32_t *lo, int32_t *hi)
{
    *lo = adc96 ? 91000 : 171000;
    *hi = adc96 ? 96000 : 178000;
}

/* The status line is a prefix literal, an optional number and a suffix
 * literal. Kept in that form rather than as a format string so the strict
 * build can still check every format, and so no string buffer lives here:
 * MOD_SelfTestStatus assembles it on the caller's stack. */
static void status(const char *text) { st.status = text; st.status_suffix = NULL; st.status_n = 0U; }
static void status_n(const char *text, uint8_t n, const char *suffix)
{
    st.status = text;
    st.status_n = n;
    st.status_suffix = suffix;
}

/* The RF path may not be touched on a board whose assembly is undeclared. */
static const char *rf_block(void) { return RADIO_ConfigRxBlock(RADIO_Config()); }

/* ---- POWER ---- */

__attribute__((cold, noinline)) static uint8_t step_power(uint8_t id, uint32_t now)
{
    radio_telemetry_t t;
    RADIO_GetTelemetry(&t);
    const radio_config_t *c = RADIO_Config();
    switch (id) {
        case ST_VREF: {
            const int32_t mv = (int32_t)MOD_GetVrefMillivolts();
            if (!MOD_GetVrefReady()) record(id, ST_FAIL, mv,
                   TXT_ST_VREFINT_NEVER_MEASURED);
            else record(id, MOD_SelfTestRange(mv, 2800, 3000), mv,
                   TXT_ST_INTERNAL_VREFBUF_SCALE);
            return 1U;
        }
        case ST_VIN: {
            const int32_t mv = (int32_t)t.vin_mv;
            const int32_t high = (int32_t)c->vin_limit_mv;
            record(id, MOD_SelfTestRange(mv, 6000, high), mv,
                   mv < 6000 ? TXT_ST_BELOW_THE_LM1084 : TXT_ST_PB2_ADC2_IN12);
            return 1U;
        }
        case ST_RAIL5: {
            const int32_t mv = (int32_t)t.rail5_mv;
            /* A low rail is the documented USB-C 5 V cold start: it inhibits
             * TX but is not a defect, so it warns. High is a regulator fault. */
            const uint8_t verdict = mv > 5500 ? (uint8_t)ST_FAIL : MOD_SelfTestRangeWarn(mv, 4400, 5500);
            record(id, verdict, mv,
                   mv < 4400 ? TXT_ST_LOW_USB_C
                             : (mv > 5500 ? TXT_ST_OVERVOLTAGE_LM1084_5 : TXT_ST_LM1084_5_VIA));
            return 1U;
        }
        case ST_RAIL_RIPPLE:
            if (!st.phase) {
                st.lo_mv = 0xFFFFU; st.hi_mv = 0U;
                st.at = now + ST_RIPPLE_MS;
                st.phase = 1U;
                status("SAMPLING THE 5 V RAIL");
            }
            if (t.rail5_mv < st.lo_mv) st.lo_mv = t.rail5_mv;
            if (t.rail5_mv > st.hi_mv) st.hi_mv = t.rail5_mv;
            if ((int32_t)(now - st.at) < 0) return 0U;
            {
                const int32_t spread = (int32_t)st.hi_mv - (int32_t)st.lo_mv;
                record(id, MOD_SelfTestRangeWarn(spread, 0, 150), spread,
                   TXT_ST_PEAK_TO_PEAK);
            }
            return 1U;
        case ST_ADC_SCAN:
            if (!st.phase) {
                st.seq0 = t.sequence; st.tick0 = now;
                st.at = now + ST_ADC_RATE_MS;
                st.phase = 1U;
                status("TIMING THE ADC2 DMA SCAN");
                return 0U;
            }
            if ((int32_t)(now - st.at) < 0) return 0U;
            {
                const uint32_t elapsed = now - st.tick0;
                const int32_t hz = elapsed ? (int32_t)((t.sequence - st.seq0) * 1000UL / elapsed) : 0;
                /* TIM7 triggers a 7-channel sequence at 4 kHz and the DMA
                 * half-completes every sequence, so telemetry advances at
                 * 4 kHz. A stalled scan is the ADC fault the radio trips on. */
                record(id, MOD_SelfTestRange(hz, 3600, 4400), hz,
                       hz ? TXT_ST_TIM7_TRGO_AT : TXT_ST_NO_DMA_COMPLETIONS);
            }
            return 1U;
        case ST_FAULTS:
            record(id, t.faults ? (uint8_t)ST_FAIL : (uint8_t)ST_PASS, (int32_t)t.faults,
                   t.faults ? TXT_ST_LATCHED_SEE_THE : TXT_ST_NO_PROTECTION_FAULT);
            return 1U;
        case ST_RESET: {
            const uint32_t csr = MOD_GetResetCsr();
            const uint8_t watchdog = (csr & (RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF)) != 0U;
            if (watchdog) record(id, ST_WARN, (int32_t)(csr >> 24U), TXT_ST_LAST_BOOT_WATCHDOG);
            else record_s(id, ST_PASS, (int32_t)(csr >> 24U), MOD_GetResetReasonLabel());
            return 1U;
        }
        case ST_PD: {
            radio_pd_info_t pd;
            RADIO_PdSnapshot(&pd);
            /* The reason string carries the verdict's evidence, so the two
             * cannot drift apart the way "RUNNING ON DC" did on a board whose
             * CC analog PHY never came up. */
            const char *why = RADIO_PdWhyDetail();
            if (pd.why == (uint8_t)RADIO_PD_WHY_DISABLED || pd.supply == (uint8_t)RADIO_SUPPLY_DC_JACK ||
                pd.supply == (uint8_t)RADIO_SUPPLY_USB_5V || pd.supply == (uint8_t)RADIO_SUPPLY_UNKNOWN)
                skip_s(id, why);
            else if (!pd.contract)
                record_s(id, pd.why == (uint8_t)RADIO_PD_WHY_PROBING ||
                           pd.why == (uint8_t)RADIO_PD_WHY_NEGOTIATING
                           ? (uint8_t)ST_WARN : (uint8_t)ST_FAIL, (int32_t)pd.mv, why);
            else
                record_s(id, pd.mv < pd.target_mv ? (uint8_t)ST_WARN
                           : MOD_SelfTestRange((int32_t)pd.mv, 4500, 14000), (int32_t)pd.mv, why);
            return 1U;
        }
        default: skip(id, TXT_ST_NOT_A_SUPPLY); return 1U;
    }
}

/* ---- CLOCK ---- */

__attribute__((cold, noinline)) static int32_t rx_lo_coverage(void)
{
    const uint64_t hr = (uint64_t)MOD_GetHrtimInputClockHz() * 32ULL;
    const uint8_t bands = MOD_GetBandCount();
    unsigned usable = 0U, tried = 0U;
    for (uint8_t i = 0U; i < bands; ++i) {
        const mod_band_t *b = MOD_GetBand(i);
        if (!b) continue;
        const uint32_t hz = (uint32_t)(b->mhz * 1000000.0f);
        if (hz < 1000000UL || hz > RADIO_RX_MAX_HZ) continue;
        uint32_t period = 0UL;
        int32_t offset = 0;
        ++tried;
        if (RADIO_RxPlanLo(hr, hz, (radio_rx_mode_t)RADIO_Config()->rx_mode, &period, &offset) >= 2U) ++usable;
    }
    return tried ? (int32_t)(usable * 100U / tried) : -1;
}

__attribute__((cold, noinline)) static uint8_t step_clock(uint8_t id, uint32_t now)
{
    (void)now;
    switch (id) {
        case ST_SYSCLK: {
            const int32_t khz = (int32_t)(SystemCoreClock / 1000UL);
            record(id, MOD_SelfTestRange(khz, 165000, 170000), khz,
                   TXT_ST_PLL_FROM_THE);
            return 1U;
        }
        case ST_HRTIM: {
            const int32_t khz = (int32_t)(MOD_GetHrtimInputClockHz() / 1000UL);
            record(id, MOD_SelfTestRange(khz, 100000, 1000000), khz,
                   TXT_ST_HRTIM_DLL_LOCKED);
            return 1U;
        }
        case ST_RF_GEOM: {
            const int32_t hz = (int32_t)MOD_GetActualCarrierFreqHz();
            const uint8_t ok = MOD_GetCarrierPeriodTicks() > 0U &&
                               hz >= 1000000 && hz <= 28333333;
            record(id, ok ? (uint8_t)ST_PASS : (uint8_t)ST_FAIL, hz,
                   ok ? TXT_ST_A_LEGAL_HRTIM
                      : TXT_ST_NO_LEGAL_CARRIER);
            return 1U;
        }
        case ST_CORDIC: {
            const uint8_t ok = MOD_RunCordicSelfTest();
            const uint8_t hw = MOD_GetCordicEnabled();
            record(id, ok && hw ? (uint8_t)ST_PASS : (uint8_t)ST_WARN, hw ? 1 : 0,
                   ok && hw ? TXT_ST_HARDWARE_CORDIC_AGREES
                            : TXT_ST_RUNNING_THE_SOFTWARE);
            return 1U;
        }
        case ST_MATH: {
            /* The same three identities main.c checks at boot, so a broken
             * FPU or libm shows up here as well as in the boot log. */
            const float s = sinf(0.5235987756f), c = cosf(1.0471975512f), h = sqrtf(0.25f);
            const uint8_t ok = fabsf(s - 0.5f) < 0.0005f && fabsf(c - 0.5f) < 0.0005f &&
                               fabsf(h - 0.5f) < 0.0005f;
            record(id, ok ? (uint8_t)ST_PASS : (uint8_t)ST_FAIL, ok ? 1 : 0,
                   ok ? TXT_ST_SINF_COSF_AND : TXT_ST_FPU_OR_LIBM);
            return 1U;
        }
        case ST_RX_LO_COVER: {
            const int32_t pct = rx_lo_coverage();
            if (pct < 0) { skip(id, TXT_ST_NO_BAND_PRESETS); return 1U; }
            /* An integer HRTIM period cannot reach every frequency near the
             * top of HF; about a fifth of 10 m has no DC-free IF. That is a
             * known receiver limit, not a board fault, so it warns. */
            record(id, MOD_SelfTestRangeWarn(pct, 90, 100), pct,
                   pct >= 90 ? TXT_ST_EVERY_BAND_PRESET
                             : TXT_ST_SOME_PRESETS_HAVE);
            return 1U;
        }
        case ST_CPU: {
            mod_cpu_diag_snapshot_t cpu;
            MOD_GetCpuDiagSnapshot(&cpu);
            if (!cpu.enabled || !cpu.dwt_available) { skip(id, TXT_ST_CPU_DIAGNOSTICS_OFF); return 1U; }
            const int32_t cycles = (int32_t)cpu.section[MOD_CPU_DIAG_SERVICE_BG].max_cycles;
            record(id, ST_PASS, cycles,
                   TXT_ST_WORST_BACKGROUND_SERVICE);
            return 1U;
        }
        case ST_STACK: {
            const int32_t unused = (int32_t)MOD_StackWatermarkUnusedBytes();
            if (!unused) { skip(id, TXT_ST_STACK_WATERMARK_NOT); return 1U; }
            record(id, MOD_SelfTestRange(unused, 1024, 0x7FFFFFFF), unused,
                   TXT_ST_UNTOUCHED_BYTES_LEFT);
            return 1U;
        }
        default: skip(id, TXT_ST_NOT_A_CLOCK); return 1U;
    }
}

/* ---- CONTROLS ----
 * QMX's diagnostics screen lists every button and both encoder directions and
 * clears each row as the operator works it (operating manual 8.6.10). The
 * whole group is one waiting step: rows clear in any order, the step ends as
 * soon as everything has been seen, and whatever is left times out as SKIP. */

static const uint8_t control_ids[ST_CTRL_BITS] = {
    ST_BTN_UP, ST_BTN_DN, ST_BTN_SEL, ST_BTN_SW4, ST_BTN_SW5,
    ST_ENC_CW, ST_ENC_CCW, ST_ENC_CLICK, ST_ENC_LONG, ST_BTN_POL
};

void MOD_SelfTestNoteButton(uint8_t index)
{
    if (st.active && index < 5U) st.controls_hit |= (uint16_t)(1U << index);
}

void MOD_SelfTestNoteEncoder(int8_t steps, uint8_t click, uint8_t long_click)
{
    if (!st.active) return;
    if (steps > 0) st.controls_hit |= (uint16_t)(1U << 5U);
    if (steps < 0) st.controls_hit |= (uint16_t)(1U << 6U);
    if (click) st.controls_hit |= (uint16_t)(1U << 7U);
    if (long_click) st.controls_hit |= (uint16_t)(1U << 8U);
}

__attribute__((cold, noinline)) static uint8_t step_controls(uint8_t id, uint32_t now)
{
    if (id != control_ids[0]) return 1U; /* the group is finished in one step */
    if (!st.phase) {
        st.controls_hit = 0U;
        st.at = now + ST_CONTROLS_MS;
        st.phase = 1U;
    }
    /* Buttons are also sampled directly, so a press still counts on a screen
     * that is not forwarding UI events. */
    for (uint8_t i = 0U; i < 5U; ++i)
        if (BOARD_ButtonPressed(i)) st.controls_hit |= (uint16_t)(1U << i);
    {
        uint8_t learned = 0U;
        if (BOARD_ButtonService(now, &learned)) st.controls_hit |= (uint16_t)(1U << 9U);
    }
    {
        int8_t steps = 0;
        uint8_t click = 0U, long_click = 0U;
        if (BOARD_EncoderRead(&steps, &click, &long_click)) MOD_SelfTestNoteEncoder(steps, click, long_click);
    }
    const uint16_t all = (1U << ST_CTRL_BITS) - 1U;
    const uint8_t done = (st.controls_hit & all) == all || (int32_t)(now - st.at) >= 0;
    {
        uint8_t left = 0U;
        for (uint8_t b = 0U; b < ST_CTRL_BITS; ++b) if (!(st.controls_hit & (1U << b))) ++left;
        status_n("WORK EVERY CONTROL: ", left, " LEFT");
    }
    if (!done) return 0U;
    for (uint8_t b = 0U; b < ST_CTRL_BITS; ++b) {
        const uint8_t cid = control_ids[b];
        const uint8_t hit = (st.controls_hit & (1U << b)) != 0U;
        if (cid == ST_BTN_POL) {
            uint8_t learned = 0U;
            (void)BOARD_ButtonService(now, &learned);
            record(cid, hit ? (uint8_t)ST_PASS : (uint8_t)ST_SKIP, learned ? 1 : 0,
                   !hit ? TXT_ST_POLARITY_NOT_LEARNED
                        : (learned ? TXT_ST_LEARNED_ACTIVE_HIGH
                                   : TXT_ST_LEARNED_ACTIVE_LOW));
        } else {
            record(cid, hit ? (uint8_t)ST_PASS : (uint8_t)ST_SKIP, hit ? 1 : 0,
                   hit ? TXT_ST_SEEN : TXT_ST_NOT_EXERCISED_WITHIN);
        }
    }
    return 1U;
}

/* ---- STORAGE ---- */

#define ST_SD_PATH  "DIAG/SELFTST.BIN"
#define ST_SD_BYTES 4096U

__attribute__((cold, noinline)) static uint8_t sd_pattern_byte(uint32_t i)
{
    /* A cheap non-repeating pattern: a stuck address line or a short read
     * shows up as a mismatch rather than as more of the same byte. */
    return (uint8_t)((i * 31U) ^ (i >> 8U) ^ 0xA5U);
}

__attribute__((cold, noinline)) static uint8_t step_storage(uint8_t id, uint32_t now)
{
    switch (id) {
        case ST_SD_MOUNT: {
            const int32_t mib = (int32_t)SD_ServiceCapacityMiB();
            if (!SD_IsMounted()) { record_s(id, ST_WARN, mib,
                   SD_ServiceStatusString()); return 1U; }
            record(id, MOD_SelfTestRange(mib, 1, 0x7FFFFFFF), mib,
                   TXT_ST_FAT_MOUNTED_OVER);
            return 1U;
        }
        case ST_SD_RW: {
            uint8_t buffer[256]; /* stack: not worth 256 B of permanent BSS */
            if (!SD_IsMounted()) { skip(id, TXT_ST_NO_CARD_MOUNTED); return 1U; }
            if (!MOD_BackgroundIoAllowed(now)) return 0U; /* wait for a quiet moment */
            if (!st.phase) { st.tick0 = now; st.phase = 1U; status("WRITING THE SD TEST PATTERN"); }
            (void)SD_DiagMkdir("DIAG");
            uint32_t written = 0U;
            uint8_t ok = 1U;
            while (ok && written < ST_SD_BYTES) {
                for (unsigned i = 0U; i < sizeof(buffer); ++i) buffer[i] = sd_pattern_byte(written + i);
                ok = SD_DiagWriteFile(ST_SD_PATH, written, buffer, sizeof(buffer), written == 0U);
                written += sizeof(buffer);
            }
            uint32_t verified = 0U;
            while (ok && verified < ST_SD_BYTES) {
                uint32_t got = 0U;
                ok = SD_DiagReadFile(ST_SD_PATH, verified, buffer, sizeof(buffer), &got) && got == sizeof(buffer);
                for (unsigned i = 0U; ok && i < sizeof(buffer); ++i)
                    if (buffer[i] != sd_pattern_byte(verified + i)) ok = 0U;
                verified += sizeof(buffer);
            }
            st.seq0 = now - st.tick0; /* elapsed, reused by ST_SD_SPEED */
            (void)SD_DiagDelete(ST_SD_PATH);
            record(id, ok ? (uint8_t)ST_PASS : (uint8_t)ST_FAIL, ok ? (int32_t)ST_SD_BYTES : 0,
                   ok ? TXT_ST_4_KIB_WRITTEN
                      : TXT_ST_MISMATCH_OR_I);
            return 1U;
        }
        case ST_SD_SPEED: {
            if (st_row[ST_SD_RW].verdict != ST_PASS) { skip(id, TXT_ST_NO_SUCCESSFUL_READ); return 1U; }
            /* Two passes over the file, so the rate covers both directions. */
            const uint32_t ms = st.seq0 ? st.seq0 : 1U;
            const int32_t kibs = (int32_t)(2U * ST_SD_BYTES * 1000UL / (ms * 1024U));
            record(id, MOD_SelfTestRangeWarn(kibs, 16, 0x7FFFFFFF), kibs,
                   TXT_ST_COMBINED_WRITE_AND);
            return 1U;
        }
        case ST_SETTINGS: {
            const char *label = MOD_GetStorageStatusLabel();
            if (label && label[0]) record_s(id, ST_PASS, 0, label);
            else record(id, ST_WARN, 0, TXT_ST_STORAGE_REPORTS_NOTHING);
            return 1U;
        }
        default: skip(id, TXT_ST_NOT_A_STORAGE); return 1U;
    }
}

/* ---- FILTERS ---- */

static uint8_t lpf_fitted(uint8_t slot)
{
    return slot < 5U && RADIO_Config()->lpf_upper_hz[slot] != 0UL;
}

__attribute__((cold, noinline)) static uint8_t step_filters(uint8_t id, uint32_t now)
{
    radio_telemetry_t t;
    RADIO_GetTelemetry(&t);
    if (id >= ST_LPF_OFF && id <= ST_LPF_ONEHOT && rf_block()) { skip_s(id, rf_block()); return 1U; }
    if (id == ST_LPF_OFF || (id >= ST_LPF0 && id <= ST_LPF4)) {
        const uint8_t slot = id == ST_LPF_OFF ? 255U : (uint8_t)(id - ST_LPF0);
        const uint8_t fitted = id == ST_LPF_OFF ? 1U : lpf_fitted(slot);
        if (!fitted) { skip(id, TXT_ST_NO_UPPER_EDGE); return 1U; }
        if (!st.phase) {
            /* The buck stays off: this measures the passive R74/R77 receive
             * bias, which is what makes the test safe and antenna-free. */
            RADIO_BiasEnable(0U);
            RADIO_SelectFilters(slot, 0U);
            st.at = now + (id == ST_LPF_OFF ? ST_LPF_OFF_MS : ST_LPF_SETTLE_MS);
            st.phase = 1U;
            if (id == ST_LPF_OFF) status("SETTLING WITH NO FILTER SELECTED");
            else status_n("MEASURING LPF ", slot, " BIAS");
            return 0U;
        }
        if ((int32_t)(now - st.at) < 0) return 0U;
        if (MOD_SelfTestOneHot(RADIO_SelectedLpfBits(), slot) != ST_PASS) st.onehot_bad = 1U;
        const int32_t mv = (int32_t)t.bias_mv;
        const uint8_t expect = (uint8_t)(id == ST_LPF_OFF ? 0U : 1U);
        record(id, MOD_SelfTestLpfVerdict(mv, expect, 1U), mv,
               (card_text_t)MOD_SelfTestLpfDetail(mv, expect, 1U));
        return 1U;
    }
    if (id == ST_LPF_ONEHOT) {
        RADIO_SelectFilters(255U, 0U); /* leave nothing biased behind us */
        record(id, st.onehot_bad ? (uint8_t)ST_FAIL : (uint8_t)ST_PASS, st.onehot_bad ? 0 : 1,
               st.onehot_bad ? TXT_ST_PE2_PE6_DID
                             : TXT_ST_PE2_PE6_READ);
        return 1U;
    }
    if (id == ST_BPF_SWEEP) {
        /* QMX warns that its RF sweep needs a dummy load (operating manual
         * 8.6.2), and a 20 s sweep does not belong in every run. Report the
         * last one instead; BPF SWEEP ~20S runs a fresh one on demand. */
        radio_sweep_info_t info;
        RADIO_SweepInfo(&info);
        if (info.status != RADIO_SWEEP_DONE) skip(id, TXT_ST_NOT_SWEPT_THIS);
        else if (info.fit_ok) record(id, ST_PASS, 4, TXT_ST_LAST_SWEEP_FITTED);
        else record_s(id, ST_WARN, 0, info.message);
        return 1U;
    }
    skip(id, TXT_ST_NOT_A_FILTER);
    return 1U;
}

/* ---- RX ---- */

__attribute__((cold, noinline)) static uint8_t step_rx(uint8_t id, uint32_t now)
{
    if (rf_block()) { skip_s(id, rf_block()); return 1U; }
    switch (id) {
        case ST_PCM: {
            const int32_t hz = (int32_t)RADIO_AudioRate();
            const uint32_t fallbacks = RADIO_AudioI2sFallbacks();
            if (RADIO_State() != RADIO_RECEIVE) { skip(id, TXT_ST_NOT_RECEIVING_TUNE); return 1U; }
            /* SYSCLK / 960 in the 176.4 kHz family, or SYSCLK / 1792 when the
             * receive plan has the ADC at ~96 kHz (since the DIGI work). The
             * carrier clock planner (PLL assist, and the dynamic clock
             * policies) runs SYSCLK anywhere in 165-170 MHz, i.e. 171.9-177.1
             * or 92.1-94.9 kHz here; the PCM1821 delivers valid I/Q across
             * both (measured at 172.2 and 177.1). */
            const uint8_t adc96 = RADIO_RxAdc96();
            int32_t lo, hi;
            pcm_window(adc96, &lo, &hi);
            record(id, fallbacks ? (uint8_t)ST_WARN : MOD_SelfTestRange(hz, lo, hi), hz,
                   fallbacks ? TXT_ST_THE_PCM1821_REJECTED
                             : adc96 ? TXT_ST_I2S3_SLAVE_96 : TXT_ST_I2S3_SLAVE_AT);
            return 1U;
        }
        case ST_RX_FLOOR: {
            if (RADIO_State() != RADIO_RECEIVE) { skip(id, TXT_ST_NOT_RECEIVING); return 1U; }
            if (!st.phase) { st.at = now + ST_RX_SETTLE_MS; st.phase = 1U; status("READING THE RECEIVE FLOOR"); return 0U; }
            if ((int32_t)(now - st.at) < 0) return 0U;
            {
                const int32_t ddb = (int32_t)(RADIO_AudioRxLevelDbfs() * 10.0f);
                /* A dead converter reads full scale or digital silence; a
                 * working one sits somewhere in between even with no antenna. */
                record(id, MOD_SelfTestRangeWarn(ddb, -1300, -50), ddb,
                       ddb <= -1300 ? TXT_ST_DIGITAL_SILENCE_SUSPECT
                                    : (ddb > -50 ? TXT_ST_AT_FULL_SCALE : TXT_ST_CHANNEL_LEVEL_WITH));
            }
            return 1U;
        }
        case ST_PHONES:
            if (!st.phase) {
                st.overruns0 = RADIO_AudioOverruns();
#if !MOD_DEEP_DIAG_ENABLE
                RADIO_AudioPhoneStatsBegin(32U); /* ~25 ms of phone halves */
#endif
                st.at = now + ST_RX_SETTLE_MS;
                st.phase = 1U;
                return 0U;
            }
            if ((int32_t)(now - st.at) < 0) return 0U;
            {
                /* The ring in RAM can look perfect while the comparators hold
                 * something else; read the timer itself first. */
                if (!RADIO_AudioPhoneCompareOk()) {
                    record(id, (uint8_t)ST_FAIL, 0,
                           TXT_ST_TIM2_COMPARE_BEYOND);
                    return 1U;
                }
                const int32_t delta = (int32_t)(RADIO_AudioOverruns() - st.overruns0);
                if (delta) {
                    record(id, (uint8_t)ST_FAIL, delta,
                           TXT_ST_THE_HEADPHONE_PWM);
                    return 1U;
                }
                /* Meeting the deadline only proves the DMA ran. With the
                 * volume up, what reached the comparators has to look like
                 * audio as well: centred on half the PWM reload, and moving.
                 * The deep-diagnostic flavour does not build the read-back. */
#if MOD_DEEP_DIAG_ENABLE
                record(id, (uint8_t)ST_WARN, 0,
                       TXT_ST_NO_DMA_OVERRUNS);
#else
                uint32_t mean = 0U, swing = 0U, top = 0U;
                if (!RADIO_Config()->volume) {
                    record(id, (uint8_t)ST_WARN, 0,
                           TXT_ST_NO_DMA_OVERRUNS_2);
                } else if (!RADIO_AudioPhoneStats(&mean, &swing, &top) || !top) {
                    record(id, (uint8_t)ST_WARN, 0, TXT_ST_NO_DMA_OVERRUNS_3);
                } else {
                    const int32_t offset = (int32_t)mean - (int32_t)(top / 2U);
                    const uint8_t centred = (uint32_t)(offset < 0 ? -offset : offset) <= top / 16U;
                    const uint8_t moving = swing > top / 16U;
                    record(id, centred && moving ? (uint8_t)ST_PASS : (uint8_t)ST_WARN,
                           (int32_t)swing,
                           !moving ? TXT_ST_PWM_CODES_SAT
                                   : (!centred ? TXT_ST_PWM_CODES_ARE
                                               : TXT_ST_PWM_CODES_CENTRED));
                }
#endif
            }
            return 1U;
        case ST_MIC1: case ST_MIC2: case ST_MIC3: {
            const uint8_t source = (uint8_t)(RADIO_SOURCE_MIC1 + (id - ST_MIC1));
            radio_mic_probe_t probe;
            if (!RADIO_MicProbe(source, &probe)) { skip(id, TXT_ST_MIC_ADC_BUSY); return 1U; }
            /* A floating or dead input sits pinned at a rail; a live bias
             * network sits mid-scale and moves with room noise. Raw ADC codes
             * throughout: a millivolt scale rounds away the single-code
             * excursions an electret on this passive network produces. */
            const int32_t mid = (int32_t)((probe.min + probe.max) / 2U);
            const uint8_t biased = mid > 250 && mid < 3845;
            record(id, biased ? (uint8_t)ST_PASS : (uint8_t)ST_WARN,
                   (int32_t)probe.rms_x16,
                   biased ? TXT_ST_BIASED_MID_SCALE
                          : TXT_ST_PINNED_AT_A);
            return 1U;
        }
        case ST_IQ_AMP: case ST_IQ_PHASE: case ST_IMAGE: case ST_OPP_SB: {
            /* QMX states plainly that its I/Q, image and audio sweeps need a
             * dummy load for valid results (operating manual 8.6.1, 8.6.3,
             * 8.6.7), and an unattended run cannot assume one is fitted. So
             * these report the last `rxcal` rather than starting one, the way
             * ST_BPF_SWEEP reports the last sweep. */
#if MOD_DEEP_DIAG_ENABLE
            /* The receive bench is not built into this flavour at all. */
            skip(id, TXT_ST_NOT_BUILT_INTO);
#else
            radio_iqcal_info_t iq;
            RADIO_IqCalInfo(&iq);
            if (!iq.have_result || !iq.measured.valid) {
                skip(id, TXT_ST_NOT_MEASURED_THIS);
                return 1U;
            }
            if (id == ST_IQ_AMP) {
                const int32_t ddb = (int32_t)(iq.measured.amp_db * 10.0f);
                record(id, MOD_SelfTestRangeWarn(ddb, -10, 10), ddb,
                       TXT_ST_QSD_AMPLITUDE_IMBALANCE);
            } else if (id == ST_IQ_PHASE) {
                const int32_t ddeg = (int32_t)(iq.measured.phase_deg * 10.0f);
                record(id, MOD_SelfTestRangeWarn(ddeg, -50, 50), ddeg,
                       TXT_ST_QSD_DEPARTURE_FROM);
            } else if (id == ST_IMAGE) {
                /* What the measured coefficients achieve, measured with them
                 * applied -- not what the raw hardware gives. QMX quotes
                 * 60-75 dB with no correction at all. */
                const int32_t ddb = (int32_t)(iq.corrected_image_db * 10.0f);
                record(id, MOD_SelfTestRange(ddb, 400, ST_LIMIT_NONE), ddb,
                       ddb < 400 ? TXT_ST_LOW_AN_ANTENNA
                                 : TXT_ST_IMAGE_REJECTION_WITH);
            } else {
                /* An AF sweep measures the ratio at 50 Hz steps across the
                 * passband and reports the worst of them, which is what QMX
                 * plots (8.6.1); `rxcal sb` measures one pair at +-1650 Hz.
                 * Prefer the sweep when there is one, and say which was read
                 * so a good number cannot be mistaken for the other test. */
                radio_af_info_t af;
                RADIO_AfSweepInfo(&af);
                const uint8_t swept = af.status == RADIO_AF_DONE && !af.image_mode &&
                                      af.worst_ratio_db > 0.0f;
                if (!swept && !iq.sideband_valid) {
                    skip(id, TXT_ST_NOT_MEASURED_THIS_2);
                    return 1U;
                }
                const float db = swept ? af.worst_ratio_db : iq.opposite_db;
                const int32_t ddb = (int32_t)(db * 10.0f);
                record(id, MOD_SelfTestRange(ddb, 400, ST_LIMIT_NONE), ddb,
                       swept ? TXT_ST_WORST_WANTED_OVER
                             : TXT_ST_AUDIO_CHANNEL_AT);
            }
#endif
            return 1U;
        }
        default: skip(id, TXT_ST_NOT_A_RECEIVE); return 1U;
    }
}

/* ---- ANALOG ---- */

static uint8_t first_fitted_lpf(void)
{
    for (uint8_t s = 0U; s < 5U; ++s) if (lpf_fitted(s)) return s;
    return 255U;
}

__attribute__((cold, noinline)) static uint8_t step_analog(uint8_t id, uint32_t now)
{
    radio_telemetry_t t;
    RADIO_GetTelemetry(&t);
    const radio_config_t *c = RADIO_Config();
    if (rf_block()) { skip_s(id, rf_block()); return 1U; }
    switch (id) {
        case ST_FWD_ZERO: case ST_REV_ZERO: {
            const int32_t mv = id == ST_FWD_ZERO ? (int32_t)t.fwd_mv : (int32_t)t.rev_mv;
            record_offer(id, MOD_SelfTestRange(mv, 0, 200), mv,
                         mv <= 200 ? TXT_ST_DETECTOR_REST_LEVEL
                                   : TXT_ST_HIGH_WITH_NO);
            return 1U;
        }
        case ST_BIAS_BUCK: case ST_BIAS_SETTLE: {
            if (!st.armed) { skip(id, TXT_ST_ANALOG_GROUP_NOT); return 1U; }
            if (c->bias_ma <= RADIO_BIAS_MA_OFF) { skip(id, TXT_ST_PIN_BIAS_MA); return 1U; }
            if (id == ST_BIAS_SETTLE) {
                const int32_t ms = (int32_t)st.bias_at;
                if (st_row[ST_BIAS_BUCK].verdict == ST_SKIP) { skip(id, TXT_ST_THE_BUCK_TEST); return 1U; }
                record(id, ms ? MOD_SelfTestRange(ms, 1, 100) : (uint8_t)ST_FAIL, ms,
                       ms ? TXT_ST_TIME_FOR_THE
                          : TXT_ST_NEVER_REACHED_THE);
                return 1U;
            }
            if (!st.phase) {
                st.bias_slot = first_fitted_lpf();
                if (st.bias_slot >= 5U) { skip(id, TXT_ST_NO_FITTED_LPF); return 1U; }
                RADIO_SelectFilters(st.bias_slot, 0U);
                st.tick0 = now;
                st.bias_at = 0U;
                st.at = now + ST_BIAS_MS;
                st.phase = 1U;
                status("SETTLING THE PIN BIAS BUCK");
                return 0U;
            }
            if (st.phase == 1U) {
                /* Give the filter the same settling the continuity test uses
                 * before asking the buck to regulate into it. */
                if ((int32_t)(now - (st.tick0 + ST_LPF_SETTLE_MS)) < 0) return 0U;
                RADIO_BiasEnable(1U);
                st.tick0 = now;
                st.at = now + ST_BIAS_MS;
                st.phase = 2U;
                return 0U;
            }
            st.bias_ma = RADIO_BiasCurrentMa(t.bias_mv, t.rail5_mv);
            if (!st.bias_at && fabsf(st.bias_ma - (float)c->bias_ma) <= 3.0f) st.bias_at = now - st.tick0;
            if ((int32_t)(now - st.at) < 0 && !t.faults) return 0U;
            RADIO_BiasEnable(0U);
            RADIO_SelectFilters(255U, 0U);
            {
                const int32_t dma = (int32_t)(st.bias_ma * 10.0f + 0.5f);
                const int32_t target = (int32_t)c->bias_ma * 10;
                record(id, t.faults ? (uint8_t)ST_FAIL : MOD_SelfTestRange(dma, target - 30, target + 30),
                       dma,
                       t.faults ? TXT_ST_TRIPPED_THE_PERFORMANCE
                                : TXT_ST_MODELLED_PER_DIODE);
            }
            return 1U;
        }
        case ST_ENV_A: case ST_ENV_B: case ST_ENV_LIN: {
            if (!st.armed) { skip(id, TXT_ST_ANALOG_GROUP_NOT); return 1U; }
            if (id != ST_ENV_A) return 1U; /* one ramp fills all three rows */
            if (!st.phase) {
                st.env_points = 0U;
                RADIO_EnvelopeSetCodes(0U, 0U);
                st.at = now + ST_ENV_STEP_MS;
                st.phase = 1U;
                status("RAMPING THE ENVELOPE DACS");
                return 0U;
            }
            if ((int32_t)(now - st.at) < 0) return 0U;
            {
                /* The ceiling is the smaller of the configured envelope limit
                 * and 95 % of VIN, exactly as RADIO_EnvelopeFill clamps it. */
                const uint16_t ceiling = (uint16_t)fminf((float)c->envelope_limit_mv, (float)t.vin_mv * 0.95f);
                const uint16_t code = (uint16_t)(st.env_points * ST_ENV_STEP_CODE);
                st.env_code[st.env_points] = code;
                st.env_a_mv[st.env_points] = t.env_a_mv;
                st.env_b_mv[st.env_points] = t.env_b_mv;
                ++st.env_points;
                const uint8_t at_ceiling = t.env_a_mv >= ceiling || t.env_b_mv >= ceiling;
                const uint16_t next = (uint16_t)(st.env_points * ST_ENV_STEP_CODE);
                if (!at_ceiling && st.env_points < ST_ENV_MAX_POINTS && next <= 4095U) {
                    RADIO_EnvelopeSetCodes(next, next);
                    st.at = now + ST_ENV_STEP_MS;
                    return 0U;
                }
            }
            RADIO_EnvelopeSetCodes(0U, 0U);
            {
                selftest_fit_t a, b;
                MOD_SelfTestFit(st.env_code, st.env_a_mv, st.env_points, &a);
                MOD_SelfTestFit(st.env_code, st.env_b_mv, st.env_points, &b);
                const selftest_fit_t *f[2] = {&a, &b};
                const uint8_t rid[2] = {ST_ENV_A, ST_ENV_B};
                for (unsigned k = 0U; k < 2U; ++k) {
                    if (!f[k]->valid) {
                        record(rid[k], ST_FAIL, 0,
                               TXT_ST_NO_RISING_TRANSFER);
                        continue;
                    }
                    const int32_t cpv = (int32_t)(f[k]->counts_per_volt + 0.5f);
                    record_offer(rid[k], MOD_SelfTestRange(cpv, ST_ENV_CPV_MIN, ST_ENV_CPV_MAX),
                                 cpv,
                                 TXT_ST_DAC_COUNTS_PER);
                }
                {
                    const int32_t worst = a.r2_ppt < b.r2_ppt ? a.r2_ppt : b.r2_ppt;
                    record(ST_ENV_LIN, MOD_SelfTestRange(worst, ST_ENV_R2_MIN_PPT, 1000), worst,
                           worst >= ST_ENV_R2_MIN_PPT ? TXT_ST_BOTH_BRANCHES_RAMP
                                                      : TXT_ST_A_BRANCH_BENDS);
                }
            }
            return 1U;
        }
        default: skip(id, TXT_ST_NOT_AN_ANALOG); return 1U;
    }
}

/* ---- run control ---- */

__attribute__((cold, noinline)) static uint8_t run_step(uint8_t id, uint32_t now)
{
    switch (MOD_SelfTestGroup(id)) {
        case ST_GROUP_POWER:    return step_power(id, now);
        case ST_GROUP_CLOCK:    return step_clock(id, now);
        case ST_GROUP_CONTROLS: return step_controls(id, now);
        case ST_GROUP_STORAGE:  return step_storage(id, now);
        case ST_GROUP_FILTERS:  return step_filters(id, now);
        case ST_GROUP_RX:       return step_rx(id, now);
        case ST_GROUP_ANALOG:   return step_analog(id, now);
        default:                skip(id, TXT_ST_NO_GROUP); return 1U;
    }
}

__attribute__((cold)) void MOD_SelfTestBegin(uint32_t now)
{
    memset(&st, 0, sizeof(st));
    st.active = 1U;
    st.mask = pending_mask;
    st.started = now;
    st.armed = 0U; /* re-armed below only if the caller armed this run */
    for (uint8_t id = 0U; id < SELFTEST_COUNT; ++id) {
        /* st_row is outside the struct memset above, so clear it in full:
         * a stale value or offer flag from the previous run must not survive. */
        st_row[id].verdict = ST_PENDING;
        st_row[id].detail = "";
        st_row[id].text = TXT_NONE;
        st_row[id].value = 0;
        st_row[id].flags = 0U;
    }
    status("STARTING");
}

__attribute__((cold)) uint8_t MOD_SelfTestStart(uint8_t mask)
{
    const uint8_t armed = st.armed;
    pending_mask = mask ? (uint8_t)(mask & ST_GROUP_ALL) : (uint8_t)ST_GROUP_DEFAULT;
    if (!RADIO_CalStart(RADIO_CAL_SELFTEST, 0U)) return 0U;
    st.armed = armed; /* MOD_SelfTestBegin cleared it; arming survives one run */
    return 1U;
}

void MOD_SelfTestStop(void) { RADIO_CalStop(); }

__attribute__((cold)) void MOD_SelfTestAbort(void)
{
    if (!st.active) return;
    /* Leave nothing energised behind a run that was cut short. RADIO_CalStop
     * stops the analog side itself, but the filter select is ours. */
    RADIO_EnvelopeSetCodes(0U, 0U);
    RADIO_BiasEnable(0U);
    st.active = 0U;
    st.armed = 0U;
    st.finished = HAL_GetTick();
    status("STOPPED");
}

uint8_t MOD_SelfTestService(uint32_t now)
{
    if (!st.active) return 0U;
    for (unsigned n = 0U; n < ST_STEPS_PER_TICK; ++n) {
        if (st.step >= SELFTEST_COUNT) {
            st.active = 0U;
            st.armed = 0U;
            st.finished = now;
            status("COMPLETE");
            /* Keep the run on the card for comparison across sessions, the
             * way the BPF sweep keeps its curves. No card, USB disk mode or a
             * busy card simply skips it. */
            if (MOD_BackgroundIoAllowed(now) && !MOD_SelfTestExport(st.export_name))
                st.export_name[0] = '\0';
            return 0U;
        }
        const uint8_t id = st.step;
        if (!(MOD_SelfTestGroup(id) & st.mask)) { ++st.step; continue; }
        if (!run_step(id, now)) return 1U;
        ++st.step;
        st.phase = 0U;
    }
    return 1U;
}

uint8_t MOD_SelfTestActive(void) { return st.active; }
const char *MOD_SelfTestExportName(void) { return st.export_name[0] ? st.export_name : NULL; }
uint8_t MOD_SelfTestMask(void) { return st.mask; }
uint8_t MOD_SelfTestCurrent(void) { return st.active ? st.step : (uint8_t)SELFTEST_COUNT; }
__attribute__((cold)) void MOD_SelfTestStatus(char *out, uint16_t cap)
{
    if (!out || !cap) return;
    if (!st.status) { out[0] = '\0'; return; }
    if (!st.status_suffix) (void)snprintf(out, cap, "%s", st.status);
    else (void)snprintf(out, cap, "%s%u%s", st.status, (unsigned)st.status_n, st.status_suffix);
}

/* The window a row was judged against. Two tests are judged against live
 * configuration rather than a fixed number, so they are substituted here
 * where the config is in reach. */
__attribute__((cold)) void MOD_SelfTestRowLimits(uint8_t id, int32_t *low, int32_t *high)
{
    int32_t lo = 0, hi = 0;
    MOD_SelfTestLimits(id, &lo, &hi);
    if (id == ST_VIN && hi == ST_LIMIT_DYNAMIC) hi = (int32_t)RADIO_Config()->vin_limit_mv;
    if (id == ST_PCM) pcm_window((uint8_t)(st_row[id].value > 0 && st_row[id].value < 120000), &lo, &hi);
    if (id == ST_BIAS_BUCK) {
        const int32_t target = (int32_t)RADIO_Config()->bias_ma * 10;
        lo = target - 30;
        hi = target + 30;
    }
    if (low) *low = lo;
    if (high) *high = hi;
}
uint8_t MOD_SelfTestProgress(void) { return (uint8_t)((uint32_t)st.step * 100UL / SELFTEST_COUNT); }
uint8_t MOD_SelfTestOverall(void) { return MOD_SelfTestRollup(st_row, SELFTEST_COUNT); }
uint32_t MOD_SelfTestElapsedMs(void)
{
    return (st.active ? HAL_GetTick() : st.finished) - st.started;
}

const selftest_result_t *MOD_SelfTestResult(uint8_t id)
{
    return id < SELFTEST_COUNT ? &st_row[id] : NULL;
}

__attribute__((cold)) const char *MOD_SelfTestDetail(uint8_t id, char *buf, uint16_t cap)
{
    if (cap) buf[0] = '\0';
    if (id >= SELFTEST_COUNT) return buf;
    if (st_row[id].detail) return st_row[id].detail;
    (void)UI_CardText((card_text_t)st_row[id].text, buf, cap);
    return buf;
}

__attribute__((cold)) const char *MOD_SelfTestDetailShown(uint8_t id)
{
    if (id >= SELFTEST_COUNT) return "";
    if (st_row[id].detail) return st_row[id].detail;
    return UI_CardTextCached((card_text_t)st_row[id].text);
}

void MOD_SelfTestArmAnalog(void) { st.armed = 1U; }
uint8_t MOD_SelfTestAnalogArmed(void) { return st.armed; }

/* ---- offered measurements ---- */

__attribute__((cold)) const char *MOD_SelfTestOffer(uint8_t id)
{
    if (id >= SELFTEST_COUNT || !(st_row[id].flags & ST_FLAG_OFFERS)) return NULL;
    switch (id) {
        case ST_FWD_ZERO: return "SET FWD DET ZERO V TO THE MEASURED REST LEVEL";
        case ST_REV_ZERO: return "SET REV DET ZERO V TO THE MEASURED REST LEVEL";
        case ST_ENV_A:    return "SET ENV A DAC/V TO THE FITTED SLOPE";
        case ST_ENV_B:    return "SET ENV B DAC/V TO THE FITTED SLOPE";
        default:          return NULL;
    }
}

__attribute__((cold)) uint8_t MOD_SelfTestApply(uint8_t id)
{
    radio_config_t c;
    if (!MOD_SelfTestOffer(id)) return 0U;
    /* Only a result the run actually stood behind may be applied. */
    if (st_row[id].verdict != ST_PASS) return 0U;
    RADIO_GetConfig(&c);
    switch (id) {
        case ST_FWD_ZERO: c.swr_offset[0] = (float)st_row[id].value * 0.001f; break;
        case ST_REV_ZERO: c.swr_offset[1] = (float)st_row[id].value * 0.001f; break;
        case ST_ENV_A:    c.envelope_dac_per_volt[0] = (float)st_row[id].value; break;
        case ST_ENV_B:    c.envelope_dac_per_volt[1] = (float)st_row[id].value; break;
        default: return 0U;
    }
    if (!RADIO_ConfigValid(&c)) return 0U;
    RADIO_SetConfig(&c);
    return 1U;
}

/* ---- SD export ---- */

__attribute__((cold, noinline)) static uint16_t export_line(uint16_t index, char *buf, uint16_t cap)
{
    int n;
    if (index == 0U) {
        n = snprintf(buf, cap, "# REV1 self test, %s, %lu ms, groups %02X\r\n",
                     MOD_SelfTestVerdictName(MOD_SelfTestOverall()),
                     (unsigned long)MOD_SelfTestElapsedMs(), (unsigned)st.mask);
    } else if (index == 1U) {
        n = snprintf(buf, cap, "id,group,test,verdict,value,unit,low,high,detail\r\n");
    } else {
        const uint8_t id = (uint8_t)(index - 2U);
        const selftest_result_t *r = &st_row[id];
        if (id >= SELFTEST_COUNT || r->verdict == ST_PENDING) return 0U;
        int32_t low = 0, high = 0;
        MOD_SelfTestRowLimits(id, &low, &high);
        char detail[80];
        n = snprintf(buf, cap, "%u,%s,%s,%s,%ld,%s,%ld,%ld,%s\r\n", (unsigned)id,
                     MOD_SelfTestGroupName(MOD_SelfTestGroup(id)), MOD_SelfTestName(id),
                     MOD_SelfTestVerdictName(r->verdict), (long)r->value, MOD_SelfTestUnit(id),
                     (long)low, (long)high, MOD_SelfTestDetail(id, detail, sizeof(detail)));
    }
    return n < 0 ? 0U : (n >= (int)cap ? (uint16_t)(cap - 1U) : (uint16_t)n);
}

__attribute__((cold)) uint8_t MOD_SelfTestExport(char *name)
{
    if (st.active || !st.finished) return 0U;
    return SD_ExportLines("ST", name, (uint16_t)(SELFTEST_COUNT + 2U), export_line);
}
