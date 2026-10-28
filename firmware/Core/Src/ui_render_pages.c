#pragma GCC optimize ("Os")

/* Every page that is not home, menu or SD card: frequency entry, SSB
 * calibration, TX test, the REV1 bench pages and plots, the self test,
 * About, and the debug-UI pages (the Morse trainer draws its own page,
 * ui_trainer.c). Each takes the 320 x 240 frame:
 * title band, content, footer with what the knob and buttons do here. */

#include "ui_state.h"
#include "ui_trainer.h"
#include "ui_gfx.h"
#include "ui_format.h"
#include "ui_labels_priv.h"
#include "ui_waterfall.h"
#include "mod_selftest.h"
#include "mod_sdcard.h"
#include "radio.h"
#include "radio_audio.h"
#include "radio_bpf_sweep.h"
#include "radio_af_sweep.h"
#include "radio_iq_fit.h"
#include "radio_pd.h"
#include "main.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *band_name(void)
{
    const mod_band_t *b = MOD_GetBand(MOD_GetActiveBandIndex());
    return (b != NULL) ? b->name : "";
}

static void format_mhz(char *dst, size_t cap, uint32_t hz)
{
    (void)snprintf(dst, cap, "%lu.%06lu", (unsigned long)(hz / 1000000UL), (unsigned long)(hz % 1000000UL));
}

static const char *retune_label(uint8_t state, uint8_t fault, uint8_t pending)
{
    if (fault) return "FAIL";
    if (pending && (state == (uint8_t)MOD_RETUNE_IDLE)) return "HOLD";
    switch ((mod_retune_state_t)state) {
        case MOD_RETUNE_PENDING:          return "PEND";
        case MOD_RETUNE_WAITING_FOR_IDLE: return "WAIT";
        case MOD_RETUNE_APPLYING:         return "APPLY";
        case MOD_RETUNE_FAILED:           return "ERR";
        case MOD_RETUNE_FAILED_RECOVERED: return "RECOV";
        case MOD_RETUNE_FAILED_FALLBACK:  return "FAIL";
        case MOD_RETUNE_IDLE:
        default:                          return "OK";
    }
}

/* ---- Frequency entry ------------------------------------------------------ */

#define FREQ_X0  36U
#define FREQ_Y   48U

static uint16_t freq_slot_x(uint8_t slot) /* slot 0 = 10 MHz digit */
{
    uint16_t x = (uint16_t)(FREQ_X0 + slot * GFX_CharWidth(GFX_FONT_FREQ));
    if (slot >= 2U) x = (uint16_t)(x + 12U);
    if (slot >= 5U) x = (uint16_t)(x + 12U);
    return x;
}

static void page_freq(uint32_t now)
{
    static const char *const steps[8] = {"10 MHz", "1 MHz", "100 kHz", "10 kHz", "1 kHz", "100 Hz", "10 Hz", "1 Hz"};
    char digits[12], line[48];
    const uint32_t hz = ui.tune.pending ? ui.tune.target_hz : MOD_GetCarrierRequestedHz();
    const uint8_t cur = (ui.cursor < 8U) ? ui.cursor : 4U;
    UI_DrawTitle("FREQUENCY ENTRY", band_name());
    (void)snprintf(digits, sizeof(digits), "%08lu", (unsigned long)hz);
    for (uint8_t i = 0U; i < 8U; ++i) {
        const char d[2] = {digits[i], 0};
        const uint8_t lead = (uint8_t)((i == 0U) && (digits[0] == '0'));
        (void)GFX_Text(freq_slot_x(i), FREQ_Y, GFX_FONT_FREQ, lead ? GFX_LINE : GFX_FREQ, d, GFX_W);
    }
    (void)GFX_Text((uint16_t)(freq_slot_x(1U) + 28U), FREQ_Y, GFX_FONT_FREQ, GFX_FREQ, ".", GFX_W);
    (void)GFX_Text((uint16_t)(freq_slot_x(4U) + 28U), FREQ_Y, GFX_FONT_FREQ, GFX_FREQ, ".", GFX_W);
    GFX_Rect((uint16_t)(freq_slot_x(cur) + 2U), 90U, 24U, 3U, ui.tune.pending ? GFX_WARN : GFX_ACCENT);
    (void)GFX_TextCenter(0U, GFX_W, 104U, GFX_FONT_BODY, GFX_ACCENT, steps[cur]);

    format_mhz(line, sizeof(line), MOD_GetActualCarrierFreqHz());
    UI_DrawPair(GFX_MARGIN, 136U, "ACTUAL", line, GFX_TEXT);
    line[0] = 0;
    (void)UI_AppendI(line, MOD_GetCarrierErrorHz());
    strcat(line, " Hz");
    UI_DrawPair(184U, 136U, "GRID", line, GFX_TEXT); /* removed by the TX NCO */
    UI_DrawPair(GFX_MARGIN, 152U, "CLOCK", MOD_GetCarrierClockPolicyLabel(MOD_GetCarrierClockPolicy()), GFX_TEXT);
    UI_DrawPair(184U, 152U, "RETUNE",
                retune_label(MOD_GetCarrierRetuneState(), 0U, MOD_CarrierFrequencyApplyPending()), GFX_TEXT);
    (void)UI_DrawWrapped(GFX_MARGIN, 180U, (uint16_t)(GFX_W - 2U * GFX_MARGIN), 2U, GFX_MUTED,
                         "Turn or UP/DN changes the marked digit; the radio follows.|"
                         "Click or SEL moves to the next digit.");
    UI_DrawFooter(GFX_SYM_TURN " CHANGE   " GFX_SYM_PRESS " NEXT DIGIT   HOLD DONE", "HOLD+" GFX_SYM_TURN " DIGIT", now);
}

/* ---- SSB calibration ------------------------------------------------------ */

static void cal_state(char *line)
{
    strcpy(line, "STATE ");
    if (MOD_GetCalibrationLegacyPending() != 0U) strcat(line, MOD_GetCalibrationApply() ? "RECHECK / ON" : "RECHECK / OFF");
    else if (MOD_GetCalibrationValidated() == 0U) strcat(line, MOD_GetCalibrationApply() ? "EDITED / ON" : "NEW / OFF");
    else strcat(line, MOD_GetCalibrationApply() ? "VALID / ON" : "VALID / OFF");
    if (MOD_GetDebugCalBypass() != 0U) strcat(line, "  BYPASS");
}

static void page_cal(uint32_t now)
{
    enum { CX = 32U, CY = 72U, CH = 120U, PITCH = 8U };
    char line[48], right[32];
    const uint8_t theta = (uint8_t)((ui.cursor >= UI_CAL_THETA_FIRST) && (ui.cursor < UI_CAL_STATUS_ITEM));
    uint8_t bin = 0U;
    if ((ui.cursor >= UI_CAL_COMMON_FIRST) && (ui.cursor < UI_CAL_THETA_FIRST)) bin = (uint8_t)(ui.cursor - UI_CAL_COMMON_FIRST);
    else if (theta) bin = (uint8_t)(ui.cursor - UI_CAL_THETA_FIRST);
    (void)snprintf(right, sizeof(right), "%s %s %s", band_name(), (MOD_GetSideband() == SIDEBAND_LSB) ? "LSB" : "USB",
                   analytic_label(MOD_GetSsbAnalyticBackend()));
    UI_DrawTitle("SSB CALIBRATION", right);
    cal_state(line);
    (void)GFX_Text(GFX_MARGIN, 30U, GFX_FONT_SMALL, GFX_DIM, line, GFX_W);

    if (ui.cursor == UI_CAL_SKEW_ITEM) {
        strcpy(line, "DIFFERENTIAL SKEW  ");
        (void)UI_AppendF(line + strlen(line), MOD_GetCalibrationDifferentialSkew(), 4U);
    } else if (ui.cursor == UI_CAL_STATUS_ITEM) {
        strcpy(line, "STATUS: CLICK = APPLY / VALIDATE / OFF");
    } else {
        (void)snprintf(line, sizeof(line), "%s BIN %u  ", theta ? "THETA" : "COMMON PHASE", (unsigned)bin);
        (void)UI_AppendF(line + strlen(line), theta ? MOD_GetCalibrationThetaBin(bin)
                                                    : MOD_GetCalibrationCommonPhaseBin(bin), 4U);
    }
    UI_DrawRow(0U, (uint16_t)(GFX_W - 8U), 44U, ui.page_edit ? UI_ROW_EDIT : UI_ROW_FOCUS, line, NULL);

    const uint16_t mid = (uint16_t)(CY + CH / 2U);
    GFX_HLine(16U, mid, 8U, GFX_LINE);
    GFX_HLine((uint16_t)(CX + 32U * PITCH + 8U), mid, 8U, GFX_LINE);
    for (uint8_t i = 0U; i < PHASE_CAL_BINS; ++i) {
        const float v = theta ? MOD_GetCalibrationThetaBin(i) : MOD_GetCalibrationCommonPhaseBin(i);
        uint16_t h = (uint16_t)(fabsf(v) * 3200.0f);
        const uint16_t x = (uint16_t)(CX + i * PITCH + 1U);
        if (h > CH / 2U - 2U) h = (uint16_t)(CH / 2U - 2U);
        if (h == 0U) h = 1U;
        GFX_Rect(x, (v >= 0.0f) ? (uint16_t)(mid - h) : mid, 5U, h,
                 ((ui.cursor != UI_CAL_SKEW_ITEM) && (ui.cursor != UI_CAL_STATUS_ITEM) && (i == bin)) ? GFX_WHITE : GFX_ACCENT);
    }
    (void)GFX_Text(GFX_MARGIN, (uint16_t)(CY + 2U), GFX_FONT_SMALL, GFX_MUTED, "+", 16U);
    (void)GFX_Text(GFX_MARGIN, (uint16_t)(CY + CH - 14U), GFX_FONT_SMALL, GFX_MUTED, "-", 16U);
    (void)GFX_Text(CX, (uint16_t)(CY + CH + 6U), GFX_FONT_SMALL, GFX_MUTED,
                   theta ? "THETA BINS, LOW TO HIGH ENVELOPE" : "COMMON PHASE BINS, LOW TO HIGH ENVELOPE", GFX_W);
    UI_DrawFooter(ui.page_edit ? GFX_SYM_TURN " VALUE   " GFX_SYM_PRESS " DONE"
                               : GFX_SYM_TURN " ITEM   " GFX_SYM_PRESS " EDIT   HOLD BACK", NULL, now);
}

/* ---- TX carrier test ------------------------------------------------------ */

/* While the paddle keys the carrier the display service is held off
 * (MOD_AudioRealtimeCritical), and the page went on saying READY through
 * every key-down (bench, 2026-10-01). UI_AppServiceTxCue redraws the badge
 * row and the RF lamp alone, in place, at the positions page_tx_test uses. */
void UI_RenderTxTestCue(uint8_t on)
{
    GFX_Fill(GFX_MARGIN, 32U, (uint16_t)(GFX_W - 2U * GFX_MARGIN), (uint16_t)(GFX_FontHeight(GFX_FONT_BODY) + 2U), GFX_BG);
    (void)GFX_Badge(GFX_MARGIN, 32U, GFX_FONT_BODY, on ? GFX_ALERT : GFX_OK, on ? "CARRIER ON" : "READY - KEY PADDLE");
    uint16_t x = GFX_MARGIN;
    x = (uint16_t)(x + ((GFX_TextWidth(GFX_FONT_BODY, "DIT") + 10U + 7U) & ~7U) + 8U);
    x = (uint16_t)(x + ((GFX_TextWidth(GFX_FONT_BODY, "DAH") + 10U + 7U) & ~7U) + 8U);
    (void)GFX_Badge(x, 144U, GFX_FONT_BODY, on ? GFX_ALERT : GFX_MUTED, "RF");
}

/* The live meter on TX TEST, which is QMX's Tune SWR tool here: the carrier
 * runs at TUNE % of the drain. Drawn by the page and, while keyed, by the TX
 * cue alone. */
void UI_RenderTxTestMeter(const ui_tx_meter_t *m)
{
    char line[48];
    GFX_ClearPixels(GFX_MARGIN, 160U, (uint16_t)(GFX_W - 2U * GFX_MARGIN), 40U);
    GFX_Fill(GFX_MARGIN, 160U, (uint16_t)(GFX_W - 2U * GFX_MARGIN), 40U, GFX_BG);
    strcpy(line, "FWD ");
    if (m->valid) strcat(UI_AppendF(line + strlen(line), m->pep_w, 2U), " W");
    else strcat(line, "--");
    uint16_t x = GFX_Text(GFX_MARGIN, 168U, GFX_FONT_BODY, GFX_TEXT, line, 168U);
    x = (uint16_t)(((x + 7U) & ~7U) + 16U);
    strcpy(line, "SWR ");
    const uint8_t swr = (uint8_t)(m->valid && m->swr_valid);
    if (swr) (void)UI_AppendF(line + strlen(line), m->swr, 2U);
    else strcat(line, "--");
    (void)GFX_Text(x, 168U, GFX_FONT_BODY,
                   !swr ? GFX_MUTED : (m->swr_alert ? GFX_ALERT : (m->swr >= 2.0f ? GFX_WARN : GFX_OK)),
                   line, (uint16_t)(GFX_W - GFX_MARGIN));
    strcpy(line, "REF ");
    if (swr) strcat(UI_AppendF(line + strlen(line), m->ref_w, 2U), " W");
    else strcat(line, "-");
    strcat(line, "  DRAIN ");
    if (m->valid) strcat(UI_AppendF(line + strlen(line), m->drain_v, 1U), " V");
    else strcat(line, "-");
    strcat(line, "  VIN ");
    if (m->valid) strcat(UI_AppendF(line + strlen(line), m->vin_v, 2U), " V");
    else strcat(line, "-");
    strcat(line, "  TUNE ");
    strcat(UI_AppendU(line + strlen(line), m->tune_pct, 1U), " %");
    (void)GFX_Text(GFX_MARGIN, 186U, GFX_FONT_SMALL, GFX_DIM, line, (uint16_t)(GFX_W - GFX_MARGIN));
}

static void page_tx_test(uint32_t now)
{
    const modulator_status_t *st = UI_FrameStatus();
    char line[48];
    const char *grid_preset = MOD_TxTestDiagLabel();
    UI_DrawTitle("TX CARRIER TEST", grid_preset ? grid_preset : band_name());
    /* Why the radio will not transmit here, if it will not: it does not key. */
    const char *why = RADIO_ConfigTxBlock(RADIO_Config(), MOD_GetCarrierRequestedHz());
    if (st->tx_state == TX_STATE_TX) (void)GFX_Badge(GFX_MARGIN, 32U, GFX_FONT_BODY, GFX_ALERT, "CARRIER ON");
    /* A protection trip at the key-down leaves the page up; READY there hid
     * it once the trip no longer came with an RF PIPELINE FAULT screen. */
    else if (RADIO_State() == RADIO_FAULT) (void)GFX_Badge(GFX_MARGIN, 32U, GFX_FONT_BODY, GFX_ALERT, UI_NavFaultMessage());
    else if (why) (void)GFX_Badge(GFX_MARGIN, 32U, GFX_FONT_BODY, GFX_WARN, why);
    else if (st->cw_paddle_armed) (void)GFX_Badge(GFX_MARGIN, 32U, GFX_FONT_BODY, GFX_OK, "READY - KEY PADDLE");
    else (void)GFX_Badge(GFX_MARGIN, 32U, GFX_FONT_BODY, GFX_WARN, "LOCKED");
    format_mhz(line, sizeof(line), MOD_GetCarrierRequestedHz());
    UI_DrawPair(GFX_MARGIN, 64U, "REQUESTED", line, GFX_FREQ);
    format_mhz(line, sizeof(line), MOD_GetActualCarrierFreqHz());
    strcat(line, "  GRID ");
    (void)UI_AppendI(line + strlen(line), MOD_GetCarrierErrorHz());
    UI_DrawPair(GFX_MARGIN, 80U, "ACTUAL", line, GFX_TEXT);
    {
        /* The carrier fraction, k ticks per N periods, after the period. */
        int8_t extra = 0;
        const uint8_t cycles = MOD_GetCarrierFraction(&extra);
        char frac[12] = "";
        if (extra != 0) (void)snprintf(frac, sizeof(frac), "%+d/%u", (int)extra, (unsigned)cycles);
        (void)snprintf(line, sizeof(line), "%lu%s TICKS  %lu MHz  %s  %s",
                       (unsigned long)MOD_GetCarrierPeriodTicks(), frac,
                       (unsigned long)(MOD_GetHrtimInputClockHz() / 1000000UL),
                       MOD_GetCarrierClockPolicyLabel(MOD_GetCarrierClockPolicy()),
                       retune_label(st->retune_state, st->retune_fault_latched, st->carrier_apply_pending));
    }
    UI_DrawPair(GFX_MARGIN, 96U, "PERIOD", line, GFX_TEXT);
    {
        /* The TX NCO's phase step at every update: the grid residual over
         * the update rate, in ticks and degrees, and the strongest image the
         * grid leaves (the NCO's, or the fraction's nudge). */
        const int64_t err_mhz = MOD_GetCarrierErrorMilliHz();
        const int64_t rate_mhz = (int64_t)MOD_TxPhaseUpdateHz() * 1000;
        const int64_t tenths = rate_mhz ? err_mhz * 10 * (int64_t)MOD_GetCarrierPeriodTicks() / rate_mhz : 0;
        const int64_t deg = rate_mhz ? err_mhz * 360 / rate_mhz : 0;
        const uint32_t at = (uint32_t)(tenths < 0 ? -tenths : tenths);
        (void)snprintf(line, sizeof(line), "%s%lu.%lu T  %ld DEG  @%luK  IMG %ld",
                       (tenths < 0) ? "-" : "", (unsigned long)(at / 10U), (unsigned long)(at % 10U),
                       (long)deg, (unsigned long)(MOD_TxPhaseUpdateHz() / 1000U), (long)MOD_GetCarrierImageDbc());
        UI_DrawPair(GFX_MARGIN, 112U, "NCO STEP", line, GFX_TEXT);
    }
    UI_DrawPair(GFX_MARGIN, 128U, "PADDLE", cw_paddle_guard_label(st->cw_paddle_guard_state), GFX_TEXT);
    uint16_t x = GFX_MARGIN;
    x = (uint16_t)(GFX_Badge(x, 144U, GFX_FONT_BODY, st->cw_paddle_dit_active ? GFX_OK : GFX_MUTED, "DIT") + 8U);
    x = (uint16_t)(GFX_Badge(x, 144U, GFX_FONT_BODY, st->cw_paddle_dah_active ? GFX_OK : GFX_MUTED, "DAH") + 8U);
    (void)GFX_Badge(x, 144U, GFX_FONT_BODY, st->cw_rf_active ? GFX_ALERT : GFX_MUTED, "RF");
    ui_tx_meter_t m;
    (void)UI_TxMeter(&m, now);
    UI_RenderTxTestMeter(&m);
    (void)UI_DrawWrapped(GFX_MARGIN, 200U, (uint16_t)(GFX_W - 2U * GFX_MARGIN), 1U, GFX_MUTED,
                         "Arm: paddles open, press both, release.");
    UI_DrawFooter(GFX_SYM_TURN " BAND   " GFX_SYM_PRESS " EXIT", NULL, now);
}

/* ---- USB-C power --------------------------------------------------------------- */

static void pd_offer_line(char *line, size_t cap, unsigned index, uint32_t pdo)
{
    radio_pd_offer_t offer;
    if (!RADIO_PdDecode(pdo, &offer)) {
        (void)snprintf(line, cap, "%u  UNSUPPORTED PDO", index);
    } else if (offer.type == RADIO_PD_FIXED) {
        (void)snprintf(line, cap, "%u  FIXED %u mV  %u mA %s", index, (unsigned)offer.max_mv,
                       (unsigned)offer.max_ma, offer.selectable ? " *" : "");
    } else {
        (void)snprintf(line, cap, "%u  %s %u-%u mV  %u %s%s", index,
                       (offer.type == RADIO_PD_PPS) ? "PPS" : ((offer.type == RADIO_PD_BATTERY) ? "BAT" : "VAR"),
                       (unsigned)offer.min_mv, (unsigned)offer.max_mv,
                       (unsigned)((offer.type == RADIO_PD_BATTERY) ? offer.max_mw : offer.max_ma),
                       (offer.type == RADIO_PD_BATTERY) ? "mW" : "mA", offer.selectable ? " *" : "");
    }
}

static void pd_detail(const radio_pd_info_t *p)
{
    char line[48];
    if (p->attached) {
        (void)snprintf(line, sizeof(line), "CC%u   PD %u.0   RP %s", (unsigned)p->cc, (unsigned)p->revision,
                       (p->rp == 3U) ? "3A" : ((p->rp == 2U) ? "1.5A" : ((p->rp == 1U) ? "DEFAULT" : "NONE")));
    } else {
        (void)snprintf(line, sizeof(line), "SUPPLY %s   PROBE %s", RADIO_PdSupplyName(), RADIO_PdProbeName());
    }
    (void)GFX_Text(GFX_MARGIN, 72U, GFX_FONT_SMALL, GFX_TEXT, line, GFX_W);
    if (p->identity_valid)
        (void)snprintf(line, sizeof(line), "VID %04X  PID %04X  %u W  FW %u HW %u", (unsigned)p->vid, (unsigned)p->pid,
                       (unsigned)p->source_watts, (unsigned)p->fw_revision, (unsigned)p->hw_revision);
    else strcpy(line, "CHARGER ID UNAVAILABLE");
    (void)GFX_Text(GFX_MARGIN, 88U, GFX_FONT_SMALL, GFX_TEXT, line, GFX_W);
    if (p->status_valid) (void)snprintf(line, sizeof(line), "PPS REPORT %u mV %u mA", (unsigned)p->source_mv, (unsigned)p->source_ma);
    else strcpy(line, "PPS STATUS UNAVAILABLE");
    (void)GFX_Text(GFX_MARGIN, 104U, GFX_FONT_SMALL, GFX_TEXT, line, GFX_W);
    (void)snprintf(line, sizeof(line), "EVENT %lu   FAIL %lu   RESET %lu", (unsigned long)p->last_event,
                   (unsigned long)p->failures, (unsigned long)p->resets);
    (void)GFX_Text(GFX_MARGIN, 120U, GFX_FONT_SMALL, GFX_TEXT, line, GFX_W);
    (void)GFX_Text(GFX_MARGIN, 144U, GFX_FONT_SMALL, GFX_MUTED, "POWER U18 CC1 PB6 / CC2 PB4   DATA U17 PA11/PA12", GFX_W);
    (void)UI_DrawWrapped(GFX_MARGIN, 164U, (uint16_t)(GFX_W - 2U * GFX_MARGIN), 3U, GFX_DIM, RADIO_PdWhyDetail());
}

static void page_pd(uint32_t now)
{
    radio_pd_info_t p;
    char line[48];
    RADIO_PdSnapshot(&p);
    UI_DrawTitle("POWER SUPPLY", ui.cursor ? "DETAIL" : "OFFERS");
    (void)GFX_Text(GFX_MARGIN, 30U, GFX_FONT_SMALL, GFX_ACCENT, RADIO_PdStatus(), GFX_W);
    (void)snprintf(line, sizeof(line), "CONTRACT %u mV  %u mA", (unsigned)p.mv, (unsigned)p.ma);
    (void)GFX_Text(GFX_MARGIN, 46U, GFX_FONT_BODY, GFX_TEXT, line, GFX_W);
    if (ui.cursor) {
        pd_detail(&p);
    } else {
        (void)GFX_Text(GFX_MARGIN, 72U, GFX_FONT_SMALL, GFX_MUTED, "SOURCE OFFERS  (* = SELECTABLE)", GFX_W);
        for (unsigned i = 0U; (i < p.count) && (i < 7U); ++i) {
            pd_offer_line(line, sizeof(line), i + 1U, p.pdo[i]);
            (void)GFX_Text(GFX_MARGIN, (uint16_t)(88U + i * 14U), GFX_FONT_SMALL, GFX_TEXT, line, GFX_W);
        }
        if (!p.count) (void)GFX_Text(GFX_MARGIN, 88U, GFX_FONT_SMALL, GFX_WARN, "NO PD CAPABILITIES RECEIVED", GFX_W);
        (void)snprintf(line, sizeof(line), "REQUEST %u mV   MAX %u mV   %s", (unsigned)p.target_mv,
                       (unsigned)p.max_mv, RADIO_PdWhyName());
        (void)GFX_Text(GFX_MARGIN, 192U, GFX_FONT_SMALL, GFX_DIM, line, GFX_W);
    }
    UI_DrawFooter(GFX_SYM_TURN " OFFERS / DETAIL   " GFX_SYM_PRESS " BACK", NULL, now);
}

/* ---- Plots --------------------------------------------------------------------- */

static void format_code_db(char *dst, size_t cap, uint8_t code)
{
    const unsigned half = 255U - code; /* code is 0.5 dB steps below 0 dBFS */
    (void)snprintf(dst, cap, "-%u.%u", half / 2U, (half & 1U) ? 5U : 0U);
}

/* Axes of a plot area [x0, x0+w) x [y0, y0+h): dotted left edge, solid base. */
static void plot_axes(uint16_t x0, uint16_t y0, uint16_t w, uint16_t h)
{
    for (uint16_t y = y0; y < y0 + h; y = (uint16_t)(y + 2U)) GFX_Pixels((uint16_t)(x0 - 3U), y, 1U, 1U);
    GFX_Pixels((uint16_t)(x0 - 3U), (uint16_t)(y0 + h), (uint16_t)(w + 3U), 1U);
    GFX_Ink((uint16_t)(x0 - 3U), y0, 3U, (uint16_t)(h + 1U), GFX_MUTED);
    GFX_Ink(x0, (uint16_t)(y0 + h), w, 1U, GFX_MUTED);
}

static uint8_t bpf_cover(const uint32_t *edge, uint32_t hz)
{
    uint8_t best = 255U;
    for (uint8_t s = 0U; s < 4U; ++s)
        if (edge[s] && (hz <= edge[s]) && ((best == 255U) || (edge[s] < edge[best]))) best = s;
    return best;
}

static void sweep_legend(const radio_sweep_info_t *info, const uint32_t *fit, const uint8_t *peak,
                         const uint8_t *at, unsigned slots)
{
    static const char *const bpf_band[4] = {"15M", "20/17M", "30M", "60/40M"};
    static const char *const lpf_band[5] = {"160M", "80/60M", "40/30M", "20-15M", "12/10M"};
    const uint32_t *now_edge = RADIO_Config()->bpf_upper_hz;
    char line[64], db[12];
    for (unsigned s = 0U; s < slots; ++s) {
        const uint32_t edge = (info->axis == RADIO_SWEEP_LPF) ? RADIO_Config()->lpf_upper_hz[s] : now_edge[s];
        (void)snprintf(line, sizeof(line), "%s%u %-6s %2lu.%02lu", (info->axis == RADIO_SWEEP_LPF) ? "LPF" : "BPF",
                       (unsigned)s, (info->axis == RADIO_SWEEP_LPF) ? lpf_band[s] : bpf_band[s & 3U],
                       (unsigned long)(edge / 1000000UL), (unsigned long)((edge % 1000000UL) / 10000UL));
        if (fit) (void)snprintf(line + strlen(line), sizeof(line) - strlen(line), " FIT %2lu.%02lu",
                                (unsigned long)(fit[s] / 1000000UL), (unsigned long)((fit[s] % 1000000UL) / 10000UL));
        if (peak[s]) {
            const uint32_t hz = RADIO_SweepPointHz(at[s]);
            format_code_db(db, sizeof(db), peak[s]);
            (void)snprintf(line + strlen(line), sizeof(line) - strlen(line), "  PK %s @ %lu.%02lu", db,
                           (unsigned long)(hz / 1000000UL), (unsigned long)((hz % 1000000UL) / 10000UL));
        }
        (void)GFX_Text(GFX_MARGIN, (uint16_t)(160U + s * 12U), GFX_FONT_SMALL, GFX_TEXT, line, GFX_W);
    }
}

static __attribute__((noinline, cold)) void page_bpf_sweep(uint32_t now)
{
    enum { X0 = 40U, STEP = 4U, Y0 = 56U, H = 88U };
    radio_sweep_info_t info;
    char line[48], db[12];
    RADIO_SweepInfo(&info);
    const uint8_t lpf_axis = (uint8_t)(info.axis == RADIO_SWEEP_LPF);
    const unsigned slots = (info.slots > RADIO_SWEEP_SLOTS) ? RADIO_SWEEP_SLOTS : info.slots;
    const uint32_t *fit = (info.axis == RADIO_SWEEP_BPF) ? RADIO_SweepEdges() : NULL;
    const uint32_t *shown = fit ? fit : RADIO_Config()->bpf_upper_hz;
    UI_DrawTitle(lpf_axis ? "LPF SWEEP" : ((info.axis == RADIO_SWEEP_BAND) ? "RF SWEEP" : "BPF SWEEP"),
                 (info.status == RADIO_SWEEP_RUNNING) ? "MEASURING" : info.message);
    format_code_db(db, sizeof(db), RADIO_SweepCode(info.floor_dbfs));
    (void)snprintf(line, sizeof(line), "POINT %u/%u   FLOOR %s dBFS   SD %s", (unsigned)info.point,
                   (unsigned)RADIO_SWEEP_POINTS, db, info.export_name ? info.export_name + 8 : "-");
    (void)GFX_Text(GFX_MARGIN, 30U, GFX_FONT_SMALL, GFX_DIM, line, GFX_W);

    uint8_t top = 0U, peak[RADIO_SWEEP_SLOTS] = {0U}, at[RADIO_SWEEP_SLOTS] = {0U};
    for (unsigned k = 0U; k < RADIO_SWEEP_POINTS; ++k)
        for (unsigned s = 0U; s < slots; ++s) {
            const uint8_t c = RADIO_SweepLevel(k, s);
            if (c > peak[s]) { peak[s] = c; at[s] = (uint8_t)k; }
            if (c > top) top = c;
        }
    plot_axes(X0, Y0, (uint16_t)(STEP * RADIO_SWEEP_POINTS), H);
    uint8_t previous = 255U;
    for (unsigned k = 0U; top && (k < RADIO_SWEEP_POINTS); ++k) {
        const uint16_t x = (uint16_t)(X0 + STEP * k);
        const uint8_t selected = lpf_axis ? 255U : bpf_cover(shown, RADIO_SweepPointHz(k));
        if (k && (selected != previous) && !lpf_axis)
            for (unsigned y = Y0; y < Y0 + H; y += 4U) GFX_Pixels((uint16_t)(x - 1U), (uint16_t)y, 1U, 1U);
        previous = selected;
        for (unsigned s = 0U; s < slots; ++s) {
            const uint8_t c = RADIO_SweepLevel(k, s);
            const unsigned dy = 2U * (unsigned)(top - c);
            if (!c || (dy >= H)) continue;
            if (s == selected) GFX_Pixels(x, (uint16_t)(Y0 + dy), STEP, 2U);
            else GFX_Pixels((uint16_t)(x + 1U), (uint16_t)(Y0 + dy), 2U, 1U);
        }
    }
    GFX_Ink(X0, Y0, (uint16_t)(STEP * RADIO_SWEEP_POINTS), H, GFX_ACCENT);
    if (top) {
        format_code_db(db, sizeof(db), top);
        (void)GFX_Text(GFX_MARGIN, Y0, GFX_FONT_SMALL, GFX_MUTED, db, (uint16_t)(X0 - 4U));
    }
    for (unsigned i = 0U; i < 5U; ++i) {
        const unsigned k = i * (RADIO_SWEEP_POINTS - 1U) / 4U;
        const uint32_t hz = RADIO_SweepPointHz(k);
        (void)snprintf(line, sizeof(line), "%lu.%lu", (unsigned long)(hz / 1000000UL), (unsigned long)((hz % 1000000UL) / 100000UL));
        (void)GFX_TextCenter((uint16_t)(X0 + STEP * k - 16U), 32U, (uint16_t)(Y0 + H + 3U), GFX_FONT_SMALL, GFX_MUTED, line);
    }
    sweep_legend(&info, fit, peak, at, slots);
    UI_DrawFooter(GFX_SYM_PRESS " STOP / BACK", "MHz", now);
}

static void af_ratio_line(const radio_af_info_t *info)
{
    char line[48];
    const float r = info->worst_ratio_db;
    const int tenths = (int)((r < 0.0f ? -r : r) * 10.0f + 0.5f);
    (void)snprintf(line, sizeof(line), info->image_mode ? "WANTED OVER IMAGE %s%d.%d dB" : "WORST OPPOSITE SIDEBAND %s%d.%d dB",
                   (r < 0.0f) ? "-" : "", tenths / 10, tenths % 10);
    (void)GFX_Text(GFX_MARGIN, 196U, GFX_FONT_BODY, GFX_TEXT, line, GFX_W);
    (void)GFX_Text(GFX_MARGIN, 212U, GFX_FONT_SMALL, GFX_MUTED,
                   info->image_mode ? "WANTED/IMAGE 2xDELTA APART; QMX QUOTES 60-75 dB"
                                    : "FLAT 150 Hz-3.2 kHz; QMX OPPOSITE ~60 dB DOWN", GFX_W);
}

static __attribute__((noinline, cold)) void page_af_sweep(uint32_t now)
{
    enum { X0 = 48U, STEP = 3U, Y0 = 52U, H = 124U };
    radio_af_info_t info;
    char line[80], db[12];
    RADIO_AfSweepInfo(&info);
    const unsigned curves = info.image_mode ? 1U : RADIO_AF_CURVES;
    UI_DrawTitle(info.image_mode ? "IMAGE SWEEP" : "AUDIO SWEEP",
                 (info.status == RADIO_AF_RUNNING) ? "MEASURING" : info.message);
    format_code_db(db, sizeof(db), RADIO_SweepCode(info.floor_dbfs));
    (void)snprintf(line, sizeof(line), "RF %lu.%03lu  DELTA %ld  TONE %ld Hz  FLOOR %s dB",
                   (unsigned long)(info.rf_hz / 1000000UL), (unsigned long)((info.rf_hz % 1000000UL) / 1000UL),
                   (long)info.delta_hz, (long)info.tone_hz, db);
    (void)GFX_Text(GFX_MARGIN, 30U, GFX_FONT_SMALL, GFX_DIM, line, GFX_W);
    uint8_t top = 0U;
    for (unsigned k = 0U; k < RADIO_AF_POINTS; ++k)
        for (unsigned c = 0U; c < curves; ++c) {
            const uint8_t code = RADIO_AfSweepLevel(k, c);
            if (code > top) top = code;
        }
    plot_axes(X0, Y0, (uint16_t)(STEP * RADIO_AF_POINTS), H);
    /* 1 px per 0.5 dB: 62 dB below the strongest point fits. */
    for (unsigned k = 0U; top && (k < RADIO_AF_POINTS); ++k) {
        const uint16_t x = (uint16_t)(X0 + STEP * k);
        for (unsigned c = 0U; c < curves; ++c) {
            const uint8_t code = RADIO_AfSweepLevel(k, c);
            const unsigned dy = (unsigned)(top - code);
            if (!code || (dy >= H)) continue;
            if (c == 0U) GFX_Pixels(x, (uint16_t)(Y0 + dy), STEP, 2U);
            else GFX_Pixels((uint16_t)(x + 1U), (uint16_t)(Y0 + dy), 1U, 1U);
        }
    }
    GFX_Ink(X0, Y0, (uint16_t)(STEP * RADIO_AF_POINTS), H, GFX_ACCENT);
    if (top) {
        format_code_db(db, sizeof(db), top);
        (void)GFX_Text(GFX_MARGIN, Y0, GFX_FONT_SMALL, GFX_MUTED, db, (uint16_t)(X0 - 4U));
    }
    for (unsigned i = 0U; i < 5U; ++i) {
        const unsigned k = i * (RADIO_AF_POINTS - 1U) / 4U;
        if (info.image_mode) (void)snprintf(line, sizeof(line), "%ld", (long)(RADIO_ImagePointHz(info.first_hz, info.step_hz, k) / 1000.0f));
        else (void)snprintf(line, sizeof(line), "%ld", (long)RADIO_AfPointHz(k));
        (void)GFX_TextCenter((uint16_t)(X0 + STEP * k - 16U), 32U, (uint16_t)(Y0 + H + 3U), GFX_FONT_SMALL, GFX_MUTED, line);
    }
    af_ratio_line(&info);
    UI_DrawFooter(GFX_SYM_PRESS " STOP / BACK", info.image_mode ? "OFFSET kHz" : "AUDIO Hz", now);
}

static __attribute__((noinline, cold)) void page_iq_scope(uint32_t now)
{
    enum { X0 = 40U, STEP = 4U, Y0 = 56U, H = 112U, N = 64U };
    int32_t pairs[N][2];
    radio_iq_snapshot_t st;
    char line[80];
    UI_DrawTitle("ADC I/Q SCOPE", "I BOLD  Q DOTTED");
    const unsigned got = RADIO_AudioIqSnapshotRead(&pairs[0][0], 0U, N);
    if (!got) {
        (void)GFX_TextCenter(0U, GFX_W, 104U, GFX_FONT_BODY, GFX_WARN, "NO CAPTURE - RUN THE SCOPE AGAIN");
        UI_DrawFooter(GFX_SYM_PRESS " BACK", NULL, now);
        return;
    }
    RADIO_IqSnapshotStats(&pairs[0][0], got, &st);
    const float a = (st.amp_db < 0.0f) ? -st.amp_db : st.amp_db;
    const float p = (st.phase_deg < 0.0f) ? -st.phase_deg : st.phase_deg;
    (void)snprintf(line, sizeof(line), "%u %s%s   AMP %s%d.%d dB   PHASE %s%d.%d" GFX_SYM_DEG, got,
                   st.valid ? "VALID" : "WEAK", st.clipped ? " CLIP" : "",
                   (st.amp_db < 0.0f) ? "-" : "", (int)a, (int)(a * 10.0f) % 10,
                   (st.phase_deg < 0.0f) ? "-" : "", (int)p, (int)(p * 10.0f) % 10);
    (void)GFX_Text(GFX_MARGIN, 30U, GFX_FONT_SMALL, st.valid ? GFX_DIM : GFX_WARN, line, GFX_W);
    int32_t peak = 1;
    for (unsigned n = 0U; n < got; ++n)
        for (unsigned c = 0U; c < 2U; ++c) {
            const int32_t v = (pairs[n][c] < 0) ? -pairs[n][c] : pairs[n][c];
            if (v > peak) peak = v;
        }
    const uint16_t mid = (uint16_t)(Y0 + H / 2U);
    for (uint16_t x = X0; x < X0 + STEP * got; x = (uint16_t)(x + 4U)) GFX_Pixels(x, mid, 1U, 1U);
    for (unsigned n = 0U; n < got; ++n) {
        const uint16_t x = (uint16_t)(X0 + STEP * n);
        for (unsigned c = 0U; c < 2U; ++c) {
            const int32_t dy = (int32_t)(((int64_t)pairs[n][c] * (H / 2 - 2)) / peak);
            const uint16_t y = (uint16_t)((int32_t)mid - dy);
            if (c == 0U) GFX_Pixels(x, y, 3U, 2U);
            else GFX_Pixels((uint16_t)(x + 1U), y, 1U, 1U);
        }
    }
    GFX_Ink(X0, Y0, (uint16_t)(STEP * got), H, GFX_ACCENT);
    (void)snprintf(line, sizeof(line), "I %+ld / %+ld     Q %+ld / %+ld", (long)(st.i_max >> 16), (long)(st.i_min >> 16),
                   (long)(st.q_max >> 16), (long)(st.q_min >> 16));
    (void)GFX_Text(GFX_MARGIN, 176U, GFX_FONT_SMALL, GFX_TEXT, line, GFX_W);
    (void)UI_DrawWrapped(GFX_MARGIN, 192U, (uint16_t)(GFX_W - 2U * GFX_MARGIN), 2U, GFX_MUTED,
                         "Equal heights and a clean quarter-cycle shift are good. Above 20 m "
                         "the traces get noisier, which is normal.");
    UI_DrawFooter(GFX_SYM_PRESS " BACK", "SHARED SCALE", now);
}

/* ---- Self test ----------------------------------------------------------------- */

static uint8_t verdict_color(uint8_t verdict)
{
    switch (verdict) {
        case ST_PASS: return GFX_OK;
        case ST_WARN: return GFX_WARN;
        case ST_FAIL: return GFX_ALERT;
        default:      return GFX_MUTED;
    }
}

static __attribute__((noinline, cold)) void page_selftest(uint32_t now)
{
    /* Eight rows, and under them the selected row's detail in full (from
     * the card, or the module that worded it). */
    enum { VISIBLE = 8U, TOP = 64U, DETAIL_Y = 197U };
    char line[64], status[48];
    uint8_t shown[SELFTEST_COUNT];
    uint8_t count = 0U;
    unsigned tally[4] = {0U, 0U, 0U, 0U};
    for (uint8_t id = 0U; id < SELFTEST_COUNT; ++id) {
        const selftest_result_t *r = MOD_SelfTestResult(id);
        if ((r == NULL) || (r->verdict == ST_PENDING)) continue;
        shown[count++] = id;
        tally[(r->verdict == ST_PASS) ? 0U : (r->verdict == ST_WARN) ? 1U : (r->verdict == ST_FAIL) ? 2U : 3U]++;
    }
    (void)snprintf(status, sizeof(status), "%u%%  %lu s", (unsigned)MOD_SelfTestProgress(),
                   (unsigned long)(MOD_SelfTestElapsedMs() / 1000UL));
    UI_DrawTitle("SELF TEST", status);
    uint16_t x = GFX_Badge(GFX_MARGIN, 28U, GFX_FONT_BODY, verdict_color((uint8_t)MOD_SelfTestOverall()),
                           MOD_SelfTestVerdictName(MOD_SelfTestOverall()));
    const char *csv = MOD_SelfTestExportName();
    (void)snprintf(line, sizeof(line), "PASS %u  WARN %u  FAIL %u  SKIP %u  SD %s", tally[0], tally[1], tally[2],
                   tally[3], csv ? csv + 8 : "-");
    (void)GFX_Text((uint16_t)(x + 8U), 31U, GFX_FONT_SMALL, GFX_TEXT, line, GFX_W);
    MOD_SelfTestStatus(status, sizeof(status));
    (void)GFX_Text(GFX_MARGIN, 48U, GFX_FONT_SMALL, GFX_DIM, status, GFX_W);
    uint8_t cursor = (ui.cursor < count) ? ui.cursor : (uint8_t)(count ? count - 1U : 0U);
    const uint8_t first = UI_ListFirst(&ui.list_first, cursor, count, VISIBLE);
    for (uint8_t i = 0U; (i < VISIBLE) && ((uint8_t)(first + i) < count); ++i) {
        const uint8_t id = shown[first + i];
        const selftest_result_t *r = MOD_SelfTestResult(id);
        const uint16_t y = (uint16_t)(TOP + i * 16U);
        const uint8_t sel = (uint8_t)(first + i == cursor);
        if (sel) GFX_Fill(0U, y, (uint16_t)(GFX_W - 8U), 16U, GFX_SELECT);
        (void)GFX_Badge(GFX_MARGIN, (uint16_t)(y + 1U), GFX_FONT_SMALL, verdict_color(r->verdict),
                        MOD_SelfTestVerdictName(r->verdict));
        if (r->verdict == ST_SKIP) (void)snprintf(line, sizeof(line), "%s", MOD_SelfTestName(id));
        else (void)snprintf(line, sizeof(line), "%s  %ld %s", MOD_SelfTestName(id), (long)r->value,
                            MOD_SelfTestUnit(id));
        (void)GFX_Text(56U, (uint16_t)(y + 2U), GFX_FONT_SMALL, sel ? GFX_WHITE : GFX_TEXT, line,
                       (uint16_t)(GFX_W - 16U));
    }
    if (count)
        (void)UI_DrawWrapped(GFX_MARGIN, DETAIL_Y, (uint16_t)(GFX_W - 2U * GFX_MARGIN), 2U, GFX_TEXT,
                             MOD_SelfTestDetailShown(shown[cursor]));
    UI_DrawScrollbar((uint16_t)(GFX_W - 6U), TOP, (uint16_t)(VISIBLE * 16U), count, first, VISIBLE);
    if (!count) (void)GFX_Text(GFX_MARGIN, TOP, GFX_FONT_BODY, GFX_MUTED, "NO RESULTS YET", GFX_W);
    UI_DrawFooter(MOD_SelfTestActive() ? GFX_SYM_TURN " SCROLL   " GFX_SYM_PRESS " STOP"
                                       : GFX_SYM_TURN " SCROLL   " GFX_SYM_PRESS " BACK", NULL, now);
}

/* ---- Diagnostics ------------------------------------------------------------------ */

#define DIAG_TAB_Y    16U
#define DIAG_ROWS_Y   40U
#define DIAG_PITCH    24U

/* Tabs for the six pages, then two columns of label and value. */
static void page_diag(uint32_t now)
{
    ui_diag_cell_t cells[UI_DIAG_CELLS];
    char right[8];
    const uint8_t page = (ui.cursor < UI_DIAG_PAGES) ? ui.cursor : 0U;
    const uint8_t n = UI_DiagPage(page, cells, UI_FrameStatus(), now);
    right[0] = 0;
    char *e = UI_AppendU(right, (uint32_t)page + 1U, 1U);
    *e++ = '/';
    (void)UI_AppendU(e, UI_DIAG_PAGES, 1U);
    UI_DrawTitle("DIAGNOSTICS", right);
    /* Six tabs of 53 px (the small font's "TRANSMIT" is 48). */
    const uint16_t tab = (uint16_t)(GFX_W / UI_DIAG_PAGES);
    for (uint8_t t = 0U; t < UI_DIAG_PAGES; ++t) {
        const uint16_t x = (uint16_t)(t * tab);
        if (t == page) GFX_Fill(x, DIAG_TAB_Y, tab, 16U, GFX_SELECT);
        (void)GFX_TextCenter(x, tab, (uint16_t)(DIAG_TAB_Y + 2U), GFX_FONT_SMALL,
                             (t == page) ? GFX_WHITE : GFX_MUTED, UI_DiagPageTitle(t));
    }
    GFX_HLine(0U, (uint16_t)(DIAG_TAB_Y + 16U), GFX_W, GFX_SELECT);
    const uint8_t rows = (uint8_t)((n + 1U) / 2U);
    for (uint8_t i = 0U; i < n; ++i) {
        const uint16_t x0 = (uint16_t)(GFX_MARGIN + (i / rows) * 160U);
        const uint16_t y = (uint16_t)(DIAG_ROWS_Y + (i % rows) * DIAG_PITCH);
        /* Long values drop to the small font before they crowd the label. */
        const uint16_t room = (uint16_t)(136U - GFX_TextWidth(GFX_FONT_SMALL, cells[i].label));
        gfx_font_t font = GFX_FONT_BODY;
        uint16_t vw = GFX_TextWidth(font, cells[i].value);
        if (vw > room) { font = GFX_FONT_SMALL; vw = GFX_TextWidth(font, cells[i].value); }
        const uint16_t vx = (vw < 136U) ? (uint16_t)(x0 + 144U - vw) : (uint16_t)(x0 + 8U);
        (void)GFX_Text(x0, (uint16_t)(y + 2U), GFX_FONT_SMALL, GFX_DIM, cells[i].label,
                       (uint16_t)((vx & ~7U) - 4U));
        (void)GFX_Text(vx, (font == GFX_FONT_SMALL) ? (uint16_t)(y + 2U) : y, font, UI_ToneColor(cells[i].tone),
                       cells[i].value, (uint16_t)(x0 + 144U));
    }
    GFX_VLine(160U, DIAG_ROWS_Y, (uint16_t)(rows * DIAG_PITCH - 8U), GFX_LINE);
    UI_DrawFooter(GFX_SYM_TURN " PAGE   " GFX_SYM_PRESS " BACK", NULL, now);
}

#if MOD_DEEP_DIAG_ENABLE
/* ---- Debug UI pages -------------------------------------------------------------- */

static void page_tests(uint32_t now)
{
    const char *labels[16];
    uint8_t n = UI_GetTestModeCount();
    if (n > 16U) n = 16U;
    for (uint8_t i = 0U; i < n; ++i) labels[i] = MOD_GetModeLabel(UI_GetTestModeAt(i));
    UI_DrawTitle("TEST SIGNALS", MOD_GetModeLabel(MOD_GetTestConfig()));
    UI_DrawList(labels, n, ui.cursor, 11U);
    UI_DrawFooter(GFX_SYM_TURN " MOVE   " GFX_SYM_PRESS " APPLY   HOLD BACK", NULL, now);
}

static void runtime_line(uint8_t row, const char *text)
{
    (void)GFX_Text(GFX_MARGIN, (uint16_t)(28U + row * 15U), GFX_FONT_SMALL, GFX_TEXT, text, (uint16_t)(GFX_W - GFX_MARGIN));
}

static void page_runtime(uint32_t now)
{
    const modulator_status_t *st = UI_FrameStatus();
    char line[64];
    UI_DrawTitle("RUNTIME DIAG", MOD_GetResetReasonLabel());
    (void)snprintf(line, sizeof(line), "USB %s  QUEUE %s", stream_state(st, now), queue_state(st));
    runtime_line(0U, line);
    (void)snprintf(line, sizeof(line), "PCM %lu/%lu  DROP %lu  POP-DROP %lu", (unsigned long)st->pcm_count,
                   (unsigned long)st->pcm_capacity, (unsigned long)st->pcm_drop_samples,
                   (unsigned long)st->pcm_drop_during_pop_samples);
    runtime_line(1U, line);
    (void)snprintf(line, sizeof(line), "RENDER %lu/%lu  LOW %lu  UNDER %lu  OVER %lu  RESYNC %lu",
                   (unsigned long)st->render_count, (unsigned long)st->render_capacity,
                   (unsigned long)st->render_low_water, (unsigned long)st->render_underruns,
                   (unsigned long)st->render_overflows, (unsigned long)st->transport_resyncs);
    runtime_line(2U, line);
    (void)snprintf(line, sizeof(line), "%s %s %s  N%lu  R%lu", st->pipeline.soft_fault_latched ? "LOSS" : "HEALTHY",
                   MOD_PipelineStageName(st->pipeline.last_stage), MOD_PipelineReasonName(st->pipeline.last_reason),
                   (unsigned long)st->pipeline.soft_fault_events, (unsigned long)st->pipeline.controlled_reset_events);
    runtime_line(3U, line);
    (void)snprintf(line, sizeof(line), "CLK %luM  POLICY %s  RETUNE %s%s  A/F %lu/%lu",
                   (unsigned long)(st->hrtim_input_hz / 1000000UL),
                   MOD_GetCarrierClockPolicyLabel((mod_clock_policy_t)st->clock_policy),
                   retune_label(st->retune_state, st->retune_fault_latched, st->carrier_apply_pending),
                   st->carrier_tx_ready ? "" : " !TX", (unsigned long)st->retune_applied_count,
                   (unsigned long)st->retune_failed_count);
    runtime_line(4U, line);
    (void)snprintf(line, sizeof(line), "OUTPHASE %s  HW %d  EFF %d%s",
                   MOD_GetOutphaseCapabilityLabel((mod_capability_t)st->outphase_capability),
                   (int)st->outphase_hw_theta_deg, (int)st->outphase_eff_theta_deg, st->outphase_limited ? "  LIMITED" : "");
    runtime_line(5U, line);
    (void)snprintf(line, sizeof(line), "USB %s  GAIN %d%%  PEAK %lu  CLIP %lu",
                   usb_policy_label((usb_audio_policy_t)st->usb_audio_policy), (int)(st->usb_gain * 100.0f),
                   (unsigned long)st->usb_peak_gain_raw, (unsigned long)st->usb_clip_count);
    runtime_line(6U, line);
    (void)snprintf(line, sizeof(line), "FM DEV %d  LIMIT %lu  SILENT %lu", (int)st->fm_deviation_peak_hz,
                   (unsigned long)st->fm_limiter_count, (unsigned long)st->fm_silent_frame_count);
    runtime_line(7U, line);
    (void)snprintf(line, sizeof(line), "AM %s  CLAMP %lu   CESSB %lu", am_profile_label((am_profile_t)st->am_profile),
                   (unsigned long)st->am_clamp_count, (unsigned long)st->cessb_clip_count);
    runtime_line(8U, line);
    {
        mod_cpu_diag_snapshot_t cpu;
        MOD_GetCpuDiagSnapshot(&cpu);
        (void)snprintf(line, sizeof(line), "CPU SVC %lu/%lu  RENDER %lu/%lu",
                       (unsigned long)cpu.section[MOD_CPU_DIAG_SERVICE_BG].avg_cycles,
                       (unsigned long)cpu.section[MOD_CPU_DIAG_SERVICE_BG].max_cycles,
                       (unsigned long)cpu.section[MOD_CPU_DIAG_RENDER_EXT].avg_cycles,
                       (unsigned long)cpu.section[MOD_CPU_DIAG_RENDER_EXT].max_cycles);
        runtime_line(10U, line);
        (void)snprintf(line, sizeof(line), "DMA %lu/%lu  LCD %lu  UI %lu",
                       (unsigned long)cpu.section[MOD_CPU_DIAG_DMA_POP].avg_cycles,
                       (unsigned long)cpu.section[MOD_CPU_DIAG_DMA_POP].max_cycles,
                       (unsigned long)cpu.section[MOD_CPU_DIAG_LCD_SERVICE].avg_cycles,
                       (unsigned long)cpu.section[MOD_CPU_DIAG_UI_BUILD].avg_cycles);
        runtime_line(11U, line);
    }
    UI_DrawFooter("ANY KEY BACK", NULL, now);
}

#endif /* MOD_DEEP_DIAG_ENABLE: the debug UI exists only in that image */

/* ---- Dispatch ----------------------------------------------------------------------- */

void UI_RenderPage(uint32_t now)
{
    switch (ui.page) {
        case PAGE_FREQ:         page_freq(now); break;
        case PAGE_CAL:          page_cal(now); break;
        case PAGE_TX_TEST:      page_tx_test(now); break;
        case PAGE_CW_TRAINER:   UI_TrainerRender(now); break;
        case PAGE_PD:           page_pd(now); break;
        case PAGE_BPF_SWEEP:    page_bpf_sweep(now); break;
        case PAGE_AF_SWEEP:     page_af_sweep(now); break;
        case PAGE_IQ_SCOPE:     page_iq_scope(now); break;
        case PAGE_WATERFALL:    UI_WaterfallRender(now); break;
        case PAGE_SELFTEST:     page_selftest(now); break;
#if MOD_DEEP_DIAG_ENABLE
        case PAGE_TESTS:        page_tests(now); break;
        case PAGE_RUNTIME:      page_runtime(now); break;
#endif
        case PAGE_DIAG:
        default:                page_diag(now); break;
    }
}
