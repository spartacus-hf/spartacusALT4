#pragma GCC optimize ("Os")

/* Live diagnostics in words an operator can act on: the six-cell summary on
 * the home screen and the five full-screen pages behind it (receive,
 * transmit, power, audio, system). Every value is read fresh for each frame;
 * the tone of a cell says whether it is fine, worth a look, or wrong. */

#include "ui_state.h"
#include "ui_gfx.h"
#include "ui_lcd.h"
#include "ui_format.h"
#include "ui_labels_priv.h"
#include "mod_sdcard.h"
#include "radio.h"
#include "radio_analog.h"
#include "radio_audio.h"
#include "radio_digi.h"
#include "radio_clocks.h"
#include "clock_plan.h"
#include "radio_pd.h"
#include "board_rev1.h"
#include "main.h"
#include <math.h>
#include <string.h>

typedef struct {
    ui_diag_cell_t *cells;
    uint8_t         n;
    uint8_t         max;
} diag_t;

/* Next cell with its label and tone; returns its (empty) value text. */
static char *put(diag_t *d, const char *label, uint8_t tone)
{
    if (d->n >= d->max) {
        static char sink[20];
        sink[0] = 0;
        return sink;
    }
    ui_diag_cell_t *c = &d->cells[d->n++];
    c->label = label;
    c->tone = tone;
    c->value[0] = 0;
    return c->value;
}

static void put_s(diag_t *d, const char *label, const char *text, uint8_t tone)
{
    strncat(put(d, label, tone), text, 19U);
}

static void put_u(diag_t *d, const char *label, uint32_t v, const char *unit, uint8_t tone)
{
    strcat(UI_AppendU(put(d, label, tone), v, 1U), unit);
}

static void put_volts(diag_t *d, const char *label, uint16_t mv, uint8_t tone)
{
    strcat(UI_AppendF(put(d, label, tone), (float)mv * 0.001f, 2U), " V");
}

static void put_pct(diag_t *d, const char *label, float fraction)
{
    put_u(d, label, (uint32_t)(fraction * 100.0f + 0.5f), "%", UI_TONE_TEXT);
}

/* ---- Shared judgements -------------------------------------------------- */

static const char *carrier_state(const modulator_status_t *st, uint8_t *tone)
{
    *tone = UI_TONE_OK;
    if (st->retune_fault_latched) { *tone = UI_TONE_ALERT; return "FAILED"; }
    if (st->retune_in_progress || st->carrier_apply_pending) { *tone = UI_TONE_WARN; return "RETUNING"; }
    if (!st->carrier_tx_ready) { *tone = UI_TONE_WARN; return "TX HOLD"; }
    return "LOCKED";
}

static const char *fault_name(uint32_t faults)
{
    static const char *const names[8] = {"ADC", "LOW SUPPLY", "5V RAIL", "PIN BIAS",
                                         "HIGH SWR", "ENVELOPE", "DMA", "CLOCK"};
    for (uint8_t i = 0U; i < 8U; ++i) if (faults & (1UL << i)) return names[i];
    return "NONE";
}

/* The console's range for VIN: 6 V up to the over-voltage trip. */
static uint8_t supply_tone(uint16_t vin_mv)
{
    if (vin_mv > RADIO_Config()->vin_limit_mv) return UI_TONE_ALERT;
    return (vin_mv < 6000U) ? UI_TONE_WARN : UI_TONE_OK;
}

/* "PD 9V", "USB-C", "DC JACK": what powers the radio. */
static void source_text(char *dst, uint8_t *tone)
{
    radio_pd_info_t p;
    *tone = UI_TONE_OK;
    switch (RADIO_PdSupply()) {
        case RADIO_SUPPLY_USB_PD:
            RADIO_PdSnapshot(&p);
            strcat(UI_AppendU(strcpy(dst, "PD ") + 3, (p.mv + 500U) / 1000U, 1U), "V");
            break;
        case RADIO_SUPPLY_USB_C:  strcpy(dst, "USB-C"); *tone = UI_TONE_WARN; break;
        case RADIO_SUPPLY_USB_5V: strcpy(dst, "USB 5V"); *tone = UI_TONE_WARN; break;
        case RADIO_SUPPLY_DC_JACK: strcpy(dst, "DC JACK"); break;
        case RADIO_SUPPLY_UNKNOWN:
        default:                   strcpy(dst, "UNKNOWN"); *tone = UI_TONE_DIM; break;
    }
}

/* Receive overruns and transmit underruns: "CLEAN" unless one happened in
 * the last ten seconds. */
static uint8_t audio_glitch_recent(const modulator_status_t *st, uint32_t now)
{
    static uint32_t last_total, last_change;
    static uint8_t seen;
    const uint32_t total = RADIO_AudioOverruns() + st->render_underruns;
    if (!seen) { seen = 1U; last_total = total; last_change = now - 60000U; }
    if (total != last_total) { last_total = total; last_change = now; }
    return (uint8_t)((now - last_change) < 10000U);
}

static const char *tx_state_text(const modulator_status_t *st, uint8_t *tone)
{
    const char *block = RADIO_ConfigTxBlock(RADIO_Config(), MOD_GetCarrierRequestedHz());
    *tone = UI_TONE_OK;
    if (RADIO_State() == RADIO_TRANSMIT) { *tone = UI_TONE_ALERT; return "ON AIR"; }
    if (MOD_GetMuted()) { *tone = UI_TONE_WARN; return "MUTED"; }
    if (block != NULL) { *tone = UI_TONE_WARN; return block; }
    if (!st->carrier_tx_ready) { *tone = UI_TONE_WARN; return "WAIT"; }
    return "READY";
}

void UI_DiagSummary(ui_diag_cell_t cells[UI_DIAG_SUMMARY], const modulator_status_t *st, uint32_t now)
{
    diag_t d = {cells, 0U, UI_DIAG_SUMMARY};
    radio_telemetry_t t;
    uint8_t tone;
    RADIO_GetTelemetry(&t);
    put_volts(&d, "SUPPLY", t.vin_mv, supply_tone(t.vin_mv));
    char *v = put(&d, "SOURCE", UI_TONE_OK);
    source_text(v, &tone);
    cells[1].tone = tone;
    const char *c = carrier_state(st, &tone);
    put_s(&d, "CARRIER", c, tone);
    if (audio_glitch_recent(st, now)) put_s(&d, "AUDIO", "GLITCHES", UI_TONE_WARN);
    else put_s(&d, "AUDIO", "CLEAN", UI_TONE_OK);
    c = tx_state_text(st, &tone);
    put_s(&d, "TRANSMIT", c, tone);
    if (t.faults) put_s(&d, "FAULTS", fault_name(t.faults), UI_TONE_ALERT);
    else if (st->pipeline.soft_fault_latched) put_s(&d, "FAULTS", "RF PIPELINE", UI_TONE_WARN);
    else put_s(&d, "FAULTS", "NONE", UI_TONE_OK);
}

/* ---- DIGI --------------------------------------------------------------- */

/* What WSJT-X receives: the stream's level, or why there is none. Too low a
 * level is still decodable (FT8 shrugs off 16-bit quantisation at the radio's
 * fixed gain, bench 2026-10-05), so only a near-full-scale peak warns. */
static void digi_host(diag_t *d, const radio_digi_stats_t *s, const radio_digi_status_t *g, uint32_t now)
{
    static uint32_t last_clips, clip_at;
    static uint8_t seen;
    if (!seen || s->cap_clips < last_clips) { seen = 1U; last_clips = s->cap_clips; clip_at = now - 60000U; }
    if (s->cap_clips != last_clips) { last_clips = s->cap_clips; clip_at = now; }
    if (!g->host_live) { put_s(d, "HOST", "NO STREAM", UI_TONE_DIM); return; }
    const uint8_t clipped = (uint8_t)((now - clip_at) < 10000U);
    const uint8_t tone = clipped ? UI_TONE_ALERT : (s->peak_dbfs > -3.0f ? UI_TONE_WARN : UI_TONE_OK);
    strcat(UI_AppendI(put(d, "HOST", tone), (int32_t)(s->level_dbfs - 0.5f)), clipped ? " CLIP" : " dBFS");
}

static void digi_slot(diag_t *d, const radio_digi_status_t *g)
{
    if (g->slot_ms < 0) { put_s(d, "SLOT", "LEARNING", UI_TONE_DIM); return; }
    char *v = put(d, "SLOT", UI_TONE_TEXT);
    v = UI_AppendU(v, (uint32_t)g->slot_ms / 1000U, 2U);
    strcpy(v, (g->slot_locked == 2U) ? " s TX" : " s");
}

/* REF LOCAL on REV1's 8 MHz DSC1003 lets the MEMS's own wander through: it
 * chirped tens of Hz a slot as the board warmed, 0.8 decodes a slot against
 * 21 following the host (bench, 2026-10-05). A TCXO (16-40 MHz) is fine. */
#define DIGI_REF_MEMS (CLOCK_HSE_NOMINAL_HZ == 8000000UL)

static void digi_ref(diag_t *d, const radio_digi_status_t *g)
{
    if (!g->follow) {
        put_s(d, "REF", DIGI_REF_MEMS ? "LOCAL MEMS" : "LOCAL", DIGI_REF_MEMS ? UI_TONE_ALERT : UI_TONE_TEXT);
        return;
    }
    strcat(UI_AppendF(strcpy(put(d, "REF", UI_TONE_TEXT), "HOST ") + 5, g->ref_ppm, 1U), "ppm");
}

/* The tracker following the host: locked, or why not. A re-acquisition steps
 * the frequency (and the slot's signals with it). */
static void digi_track(diag_t *d, const radio_digi_status_t *g)
{
    if (!g->follow) { put_s(d, "TRACKER", "OFF", UI_TONE_DIM); return; }
    if (g->ref_state == 0U) { put_s(d, "TRACKER", "NO HOST", UI_TONE_WARN); return; }
    if (g->ref_state == 1U) { put_s(d, "TRACKER", "ACQUIRING", UI_TONE_WARN); return; }
    if (!g->reacq_10min) { put_s(d, "TRACKER", "LOCKED", UI_TONE_OK); return; }
    strcat(UI_AppendU(put(d, "TRACKER", UI_TONE_WARN), g->reacq_10min, 1U), " STEP/10M");
}

static void digi_gaps(diag_t *d, const radio_digi_status_t *g)
{
    if (!g->gaps_10min) { put_s(d, "GAPS", "CLEAN", UI_TONE_OK); return; }
    strcat(UI_AppendU(put(d, "GAPS", UI_TONE_WARN), g->gaps_10min, 1U), " /10 MIN");
}

/* The home screen's six cells in DIGI: the stream WSJT-X gets, its gaps, the
 * slot clock, the reference and its tracker, and transmit (or a fault). */
void UI_DigiSummary(ui_diag_cell_t cells[UI_DIAG_SUMMARY], const modulator_status_t *st, uint32_t now)
{
    diag_t d = {cells, 0U, UI_DIAG_SUMMARY};
    radio_digi_stats_t s;
    radio_digi_status_t g;
    radio_telemetry_t t;
    uint8_t tone;
    RADIO_DigiStats(&s);
    RADIO_DigiStatus(&g, now);
    RADIO_GetTelemetry(&t);
    digi_host(&d, &s, &g, now);
    digi_gaps(&d, &g);
    digi_slot(&d, &g);
    digi_ref(&d, &g);
    digi_track(&d, &g);
    if (t.faults) put_s(&d, "FAULTS", fault_name(t.faults), UI_TONE_ALERT);
    else {
        const char *c = tx_state_text(st, &tone);
        put_s(&d, "TRANSMIT", c, tone);
    }
}

static void page_digi(diag_t *d, uint32_t now)
{
    radio_digi_stats_t s;
    radio_digi_status_t g;
    RADIO_DigiStats(&s);
    RADIO_DigiStatus(&g, now);
    digi_host(d, &s, &g, now);
    strcat(UI_AppendI(put(d, "Host peak", UI_TONE_TEXT), (int32_t)(s.peak_dbfs - 0.5f)), " dBFS");
    put_u(d, "Clips", s.cap_clips, "", s.cap_clips ? UI_TONE_WARN : UI_TONE_TEXT);
    digi_gaps(d, &g);
    put_u(d, "Gaps slot", g.gaps_slot, "", g.gaps_slot ? UI_TONE_WARN : UI_TONE_TEXT);
    put_u(d, "Restarts", s.restarts, "", UI_TONE_TEXT);
    char *v = put(d, "ADC peak", s.adc_clips ? UI_TONE_WARN : UI_TONE_TEXT);
    strcat(UI_AppendI(v, (int32_t)(s.adc_peak_dbfs - 0.5f)), " dBFS");
    digi_slot(d, &g);
    v = put(d, "Activity", (g.slot_ms >= 0) ? UI_TONE_TEXT : UI_TONE_DIM);
    if (g.slot_depth_db > -50.0f) strcat(UI_AppendF(strcpy(v, "+") + 1, g.slot_depth_db, 1U), " dB");
    else strcpy(v, "-");
    digi_ref(d, &g);
    digi_track(d, &g);
    v = put(d, "IF / load", UI_TONE_TEXT);
    const uint32_t lo = RADIO_RxClockHz();
    strcat(UI_AppendI(v, lo ? (int32_t)MOD_GetCarrierRequestedHz() - (int32_t)lo : 0), "Hz ");
    strcat(UI_AppendU(v + strlen(v), g.load_pct, 1U), "%");
}

/* ---- Pages ------------------------------------------------------------------ */

static void page_receive(diag_t *d, const modulator_status_t *st)
{
    char *v;
    uint8_t tone, lpf = 255U, bpf = 255U;
    const uint32_t hz = MOD_GetCarrierRequestedHz();
    const radio_config_t *c = RADIO_Config();
    const mod_band_t *b = MOD_GetBand(MOD_GetActiveBandIndex());
    (void)UI_FormatFreq(hz, put(d, "Frequency", UI_TONE_TEXT), 20U);
    put_s(d, "Mode", UI_UModeLabel(UI_ModeCurrent()), UI_TONE_TEXT);
    put_s(d, "Band", (b != NULL) ? b->name : "-", UI_TONE_TEXT);
    strcat(UI_AppendI(put(d, "Signal", UI_TONE_TEXT), (int32_t)RADIO_AudioRxLevelDbfs()), " dBFS");
    if (c->volume) put_u(d, "Volume", c->volume, "%", UI_TONE_TEXT);
    else put_s(d, "Volume", "Muted", UI_TONE_TEXT);
    put_s(d, "Receiver", UI_RxModeLabel(c->rx_mode), UI_TONE_TEXT);
    const char *state = carrier_state(st, &tone);
    put_s(d, "Carrier", state, tone);
    /* The HRTIM carrier grid's offset from the dial. The TX NCO removes it,
     * so it is not a frequency error; what it costs is the NCO's images at
     * +-192 kHz, shown beside it. */
    const int32_t images = MOD_GetCarrierImageDbc();
    v = put(d, "TX grid", images > -40 ? UI_TONE_WARN : UI_TONE_TEXT);
    strcat(UI_AppendI(v, st->carrier_error_hz), "Hz ");
    strcat(UI_AppendI(v + strlen(v), images), "dBc");
    (void)RADIO_ConfigFilters(c, hz, &lpf, &bpf);
    v = put(d, "Filters", (lpf < 5U) ? UI_TONE_TEXT : UI_TONE_WARN);
    strcpy(v, "LPF ");
    v[4] = (lpf < 5U) ? (char)('0' + lpf) : '-';
    strcpy(v + 5, " BPF ");
    v[10] = (bpf < 4U) ? (char)('0' + bpf) : '-';
    v[11] = 0;
    put_u(d, "Audio rate", (uint32_t)(RADIO_AudioRate() + 0.5f), " Hz", UI_TONE_TEXT);
    v = put(d, "IQ gain", UI_TONE_TEXT);
    strcat(UI_AppendF(v, c->rx_i_gain, 2U), "/");
    (void)UI_AppendF(v + strlen(v), c->rx_q_gain, 2U);
    put_u(d, "Overruns", RADIO_AudioOverruns(), "", UI_TONE_TEXT);
}

static void page_transmit(diag_t *d, const modulator_status_t *st)
{
    static const char *const states[4] = {"RECEIVE", "ON AIR", "SETTLING", "OFF"};
    radio_telemetry_t t;
    uint8_t tone;
    const radio_config_t *c = RADIO_Config();
    const uint8_t cw = (uint8_t)(st->mod_type == MOD_TYPE_CW);
    RADIO_GetTelemetry(&t);
    const radio_state_t rs = RADIO_State();
    const uint8_t si = (rs == RADIO_TRANSMIT) ? 1U : ((rs == RADIO_TX_PREPARE) ? 2U : ((rs == RADIO_RECEIVE) ? 0U : 3U));
    put_s(d, "State", states[si], (si == 1U) ? UI_TONE_ALERT : UI_TONE_TEXT);
    const char *ready = tx_state_text(st, &tone);
    put_s(d, "TX", ready, tone);
    put_pct(d, "Power", cw ? MOD_GetCwDrive() : MOD_GetDrive());
    put_s(d, "Input", (c->mic_source == RADIO_SOURCE_USB) ? "USB AUDIO" : "MIC", UI_TONE_TEXT);
    put_u(d, "Input peak", ((uint32_t)st->meter.peak_hold * 100U) / 32768U, "%",
          st->cessb_clipping ? UI_TONE_WARN : UI_TONE_TEXT);
    strcat(UI_AppendI(put(d, "Compressor", UI_TONE_TEXT), (int32_t)st->comp_gain_db), " dB");
    strcat(UI_AppendI(put(d, "Limiter", UI_TONE_TEXT), (int32_t)st->alc_gain_db), " dB");
    /* The meter's last reading (radio_rf_meter.h) beside the detectors'
     * live mV. As on QMX's diagnostics, reflected shows red when its
     * detector reads above the forward one. */
    rf_meter_reading_t m;
    const uint8_t have = RADIO_MeterRecent(0U, 0xFFFFFFFFUL, &m);
    char *v = put(d, "Forward", UI_TONE_TEXT);
    if (have) { v = UI_AppendF(v, m.pep_w, 2U); strcpy(v, "W "); v += 2; }
    strcat(UI_AppendU(v, t.fwd_mv, 1U), "mV");
    v = put(d, "Reflected", (t.rev_mv > t.fwd_mv) ? UI_TONE_ALERT : UI_TONE_TEXT);
    if (have) { v = UI_AppendF(v, m.ref_w, 2U); strcpy(v, "W "); v += 2; }
    strcat(UI_AppendU(v, t.rev_mv, 1U), "mV");
    v = put(d, "SWR", UI_TONE_TEXT);
    if (have && m.swr_valid) {
        v = UI_AppendF(v, m.swr, 2U);
        strcpy(v, " raw ");
        (void)UI_AppendF(v + 5, m.swr_raw, 2U);
        if (m.swr * 100.0f >= (float)c->swr_limit_x100) d->cells[d->n - 1U].tone = UI_TONE_ALERT;
    } else {
        strcpy(v, "-");
    }
    put_u(d, "PIN bias", (uint32_t)(RADIO_BiasCurrentMa(t.bias_mv, t.rail5_mv) + 0.5f), " mA", UI_TONE_TEXT);
    v = put(d, "Outphasing", st->outphase_limited ? UI_TONE_WARN : UI_TONE_TEXT);
    strcat(UI_AppendI(v, (int32_t)st->outphase_eff_theta_deg), GFX_SYM_DEG);
    if (st->outphase_limited) strcat(v, " LIM");
}

static void page_power(diag_t *d)
{
    radio_telemetry_t t;
    radio_pd_info_t p;
    uint8_t tone;
    RADIO_GetTelemetry(&t);
    RADIO_PdSnapshot(&p);
    put_volts(d, "Supply", t.vin_mv, supply_tone(t.vin_mv));
    char *v = put(d, "Source", UI_TONE_OK);
    source_text(v, &tone);
    d->cells[d->n - 1U].tone = tone;
    v = put(d, "Contract", p.contract ? UI_TONE_TEXT : UI_TONE_DIM);
    if (p.mv) {
        strcat(UI_AppendF(v, (float)p.mv * 0.001f, 1U), "V ");
        strcat(UI_AppendF(v + strlen(v), (float)p.ma * 0.001f, 1U), "A");
    } else {
        strcpy(v, "NONE");
    }
    put_volts(d, "PD request", p.target_mv, UI_TONE_TEXT);
    const char *why = RADIO_PdWhyDetail();
    put_s(d, "PD state", why, strcmp(why, "TARGET OK") ? UI_TONE_WARN : UI_TONE_OK);
    put_volts(d, "5V rail", t.rail5_mv, UI_TONE_TEXT);
    put_u(d, "Bias duty", (t.bias_duty * 100U + 512U) / 1024U, "%", UI_TONE_TEXT);
    put_u(d, "Envelope A", t.env_a_mv, " mV", UI_TONE_TEXT);
    put_u(d, "Envelope B", t.env_b_mv, " mV", UI_TONE_TEXT);
    if (MOD_GetVrefReady()) put_volts(d, "Analog ref", MOD_GetVrefMillivolts(), UI_TONE_TEXT);
    else put_s(d, "Analog ref", "-", UI_TONE_DIM);
    put_s(d, "Faults", fault_name(t.faults), t.faults ? UI_TONE_ALERT : UI_TONE_OK);
    put_u(d, "PD failures", p.failures, "", p.failures ? UI_TONE_WARN : UI_TONE_TEXT);
}

static void page_audio(diag_t *d, const modulator_status_t *st, uint32_t now)
{
    char *v = put(d, "Buffer", (st->render_count <= 2U) ? UI_TONE_WARN : UI_TONE_TEXT);
    strcat(UI_AppendU(v, st->render_count, 1U), "/");
    (void)UI_AppendU(v + strlen(v), st->render_capacity, 1U);
    put_u(d, "Lowest", st->render_low_water, "", UI_TONE_TEXT);
    put_u(d, "Dropouts", st->render_underruns, "", st->render_underruns ? UI_TONE_WARN : UI_TONE_TEXT);
    put_u(d, "Overflows", st->render_overflows, "", UI_TONE_TEXT);
    put_u(d, "Resyncs", st->transport_resyncs, "", UI_TONE_TEXT);
    put_u(d, "PCM drops", st->pcm_drop_samples, "", st->pcm_drop_samples ? UI_TONE_WARN : UI_TONE_TEXT);
    put_s(d, "USB stream", stream_state(st, now), UI_TONE_TEXT);
    put_u(d, "USB packets", st->usb_packets, "", UI_TONE_TEXT);
    put_u(d, "Missed", st->usb_missing_frames, "", st->usb_missing_frames ? UI_TONE_WARN : UI_TONE_TEXT);
    put_pct(d, "USB gain", st->usb_gain);
    put_u(d, "USB clips", st->usb_clip_count, "", st->usb_clip_count ? UI_TONE_WARN : UI_TONE_TEXT);
    put_u(d, "I2S retries", RADIO_AudioI2sFallbacks(), "", UI_TONE_TEXT);
}

static void page_system(diag_t *d, const modulator_status_t *st, uint32_t now)
{
    const uint32_t s = now / 1000U;
    char *v;
    put_s(d, "Firmware", FW_VERSION_STRING, UI_TONE_TEXT);
    put_s(d, "Build", (MOD_GetBuildFlavor() == MOD_BUILD_FLAVOR_DIAG) ? "DIAGNOSTIC" : "PRODUCTION", UI_TONE_TEXT);
    v = put(d, "Uptime", UI_TONE_TEXT);
    strcat(UI_AppendU(v, s / 3600U, 1U), "h ");
    strcat(UI_AppendU(v + strlen(v), (s / 60U) % 60U, 2U), "m ");
    strcat(UI_AppendU(v + strlen(v), s % 60U, 2U), "s");
    put_s(d, "Last reset", MOD_GetResetReasonLabel(), UI_TONE_TEXT);
    put_s(d, "Settings", MOD_GetStorageStatusLabel(), UI_TONE_TEXT);
    put_s(d, "SD", SD_GetStatusString(), UI_TONE_TEXT);
    if (st->pipeline.soft_fault_latched)
        put_s(d, "RF pipeline", MOD_PipelineReasonName(st->pipeline.last_reason), UI_TONE_WARN);
    else put_s(d, "RF pipeline", "HEALTHY", UI_TONE_OK);
    v = put(d, "Retunes", st->retune_failed_count ? UI_TONE_WARN : UI_TONE_TEXT);
    strcat(UI_AppendU(v, st->retune_applied_count, 1U), " ok ");
    strcat(UI_AppendU(v + strlen(v), st->retune_failed_count, 1U), " bad");
    v = put(d, "CPU clock", UI_TONE_TEXT);
    strcat(UI_AppendF(v, (float)st->hrtim_input_hz * 0.000001f, 2U), "MHz ");
    strcat(UI_AppendF(v + strlen(v), (float)RADIO_Config()->ref_ppb * 0.001f, 2U), "ppm");
    put_s(d, "Clock plan", MOD_GetCarrierClockPolicyLabel((mod_clock_policy_t)st->clock_policy), UI_TONE_TEXT);
    v = put(d, "Screen", UI_TONE_TEXT);
    strcat(UI_AppendU(v, LCDUI_Rotation() ? 270U : 90U, 1U), GFX_SYM_DEG " ");
    strcat(UI_AppendU(v + strlen(v), LCDUI_FlushMs(), 1U), "ms");
    v = put(d, "FUNC key", UI_TONE_TEXT);
    if (BOARD_FuncKeyPin() == 0U) strcpy(v, "OFF");
    else UI_FormatSettingValue(UI_GetSetting(SET_UI_FKEY_PIN), (float)RADIO_ConfigFuncKeyPin(RADIO_Config()), v);
}

uint8_t UI_DiagPage(uint8_t page, ui_diag_cell_t cells[UI_DIAG_CELLS], const modulator_status_t *st, uint32_t now)
{
    diag_t d = {cells, 0U, UI_DIAG_CELLS};
    switch (page) {
        case 0U:  page_receive(&d, st); break;
        case 1U:  page_transmit(&d, st); break;
        case 2U:  page_power(&d); break;
        case 3U:  page_audio(&d, st, now); break;
        case UI_DIAG_PAGE_DIGI: page_digi(&d, now); break;
        default:  page_system(&d, st, now); break;
    }
    return d.n;
}

const char *UI_DiagPageTitle(uint8_t page)
{
    static const char *const titles[UI_DIAG_PAGES] = {"RECEIVE", "TRANSMIT", "POWER", "AUDIO", "SYSTEM", "DIGI"};
    return titles[(page < UI_DIAG_PAGES) ? page : (UI_DIAG_PAGES - 1U)];
}

uint8_t UI_ToneColor(uint8_t tone)
{
    static const uint8_t colors[5] = {GFX_TEXT, GFX_OK, GFX_WARN, GFX_ALERT, GFX_MUTED};
    return colors[(tone < 5U) ? tone : 0U];
}
