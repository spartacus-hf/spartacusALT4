/* Host driver for tools/ui_preview.py: runs the production UI (navigation,
 * menu model, renderers, graphics core) against a small simulated radio,
 * feeds it knob and button events, and writes every frame as a PPM.
 *
 * Only the values the screens depend on are simulated here; every other
 * firmware function the UI links against is a generated zero stub. */

#include "ui_state.h"
#include "ui_gfx.h"
#include "ui_sd.h"
#include "ui_app.h"
#include "ui_lcd.h"
#include "board_rev1.h"
#include "radio.h"
#include "radio_analog.h"
#include "radio_audio.h"
#include "radio_pd.h"
#include "mod_sdcard.h"
#include "mod_selftest.h"
#include "radio_clocks.h"
#include "radio_digi.h"
#include "radio_rx_front.h"
#include "radio_spectrum.h"
#include "ui_waterfall.h"
#include "ui_trainer.h"
#include "card_text.h"
#include "mod_morse_trainer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* ---- Simulated radio --------------------------------------------------- */

static uint32_t g_tick = 10000U;
static modulator_status_t g_st;
static radio_telemetry_t g_tel;
static radio_config_t g_cfg;
static radio_state_t g_rs = RADIO_RECEIVE;
static const char *g_status = "RECEIVE";
static float g_dbfs = -87.0f;
static mod_config_t g_mode = MOD_CFG_PROD_SSB_14MHZ;
static sideband_t g_sb = SIDEBAND_USB;
static uint32_t g_hz = 14074000UL;
static uint8_t g_muted, g_flags = UI_HOME_FLAGS_DEFAULT;
static float g_drive = 0.5f, g_cw_drive = 0.8f, g_wpm = 18.0f, g_usb_gain = 1.0f;
static tx_bw_preset_t g_txbw = TX_BW_300_2700;

uint32_t HAL_GetTick(void) { return g_tick; }
void MOD_GetStatus(modulator_status_t *s) { *s = g_st; }
void RADIO_GetTelemetry(radio_telemetry_t *t) { *t = g_tel; }
const radio_config_t *RADIO_Config(void) { return &g_cfg; }

/* The CW decoder as radio_audio.c presents it: the last 40 characters and
 * how many there have been. cw_say appends, as the decoder would. */
static char g_cw_text[41];
static uint32_t g_cw_count;
static uint8_t g_cw_active = 1U, g_cw_locked;
static float g_cw_offset = -27.6f;
uint8_t RADIO_CwDecodeActive(void) { return g_cw_active; }
const char *RADIO_CwDecodeText(void) { return g_cw_text; }
uint32_t RADIO_CwDecodeCount(void) { return g_cw_count; }
void RADIO_CwDecodeStatus(uint8_t *wpm, float *offset_hz, float *snr_db, uint8_t *locked)
{
    if (wpm) *wpm = g_cw_locked ? 20U : 0U;
    if (offset_hz) *offset_hz = g_cw_locked ? g_cw_offset : 0.0f;
    if (snr_db) *snr_db = 14.0f;
    if (locked) *locked = g_cw_locked;
}
static void cw_say(const char *s)
{
    for (; *s; ++s) {
        size_t n = strlen(g_cw_text);
        if (n == 40U) { memmove(g_cw_text, g_cw_text + 1, 40U); n = 39U; }
        g_cw_text[n] = *s;
        g_cw_text[n + 1U] = 0;
        ++g_cw_count;
        const char *a, *b;
        UI_CwTeletype(&a, &b); /* the UI reads it every frame */
    }
}
void RADIO_GetConfig(radio_config_t *c) { *c = g_cfg; }
void RADIO_SetConfig(const radio_config_t *c) { g_cfg = *c; }
void RADIO_SetVolume(uint8_t v) { g_cfg.volume = v; }
void RADIO_SetMicGain(float g) { g_cfg.mic_gain = g; }
void RADIO_SetBacklight(uint8_t v) { g_cfg.backlight = v; }
uint8_t RADIO_SetRxMode(uint8_t m) { g_cfg.rx_mode = m; return 1U; }
void RADIO_SetBootFlag(uint16_t mask, uint8_t on)
{
    g_cfg.boot_flags = (uint16_t)(on ? (g_cfg.boot_flags | mask) : (g_cfg.boot_flags & ~mask));
}
radio_state_t RADIO_State(void) { return g_rs; }
/* The power/SWR meter: a 4.5 W carrier into a 1.18:1 load unless a frame
 * sets otherwise. */
radio_meter_t rf_meter;
static uint8_t g_meter_on;
static float g_meter_swr = 1.18f;
uint8_t RADIO_MeterRecent(uint32_t now, uint32_t max_age_ms, rf_meter_reading_t *out)
{
    (void)now; (void)max_age_ms;
    if (!g_meter_on) return 0U;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->pep_w = out->peak_w = out->plateau_w = 4.52f;
        out->swr = g_meter_swr;
        out->rho = (g_meter_swr - 1.0f) / (g_meter_swr + 1.0f);
        out->ref_w = out->pep_w * out->rho * out->rho;
        out->drain_v = 8.3f;
        out->vin_v = 8.62f;
        out->swr_valid = 1U;
    }
    return 1U;
}
const char *RADIO_Status(void) { return g_status; }
float RADIO_AudioRxLevelDbfs(void) { return g_dbfs; }
const char *RADIO_PdStatus(void) { return "USB-C PD 12000 mV 3000 mA"; }
const char *SD_GetStatusString(void) { return "SDHC 29.7 GB FAT32"; }
const char *SD_ServiceStatusString(void) { return "MOUNTED"; }
uint32_t SD_ServiceCapacityMiB(void) { return 30436U; }

mod_config_t MOD_GetTestConfig(void) { return g_mode; }
void MOD_SetTestConfig(mod_config_t c) { g_mode = c; }
mod_type_t MOD_GetModeType(mod_config_t c)
{
    if (c == MOD_CFG_CW_14MHZ) return MOD_TYPE_CW;
    if (c == MOD_CFG_AM_14MHZ) return MOD_TYPE_AM;
    if (c == MOD_CFG_FM_NARROW_14MHZ) return MOD_TYPE_FM;
    return MOD_TYPE_SSB;
}
const char *MOD_GetModeLabel(mod_config_t c)
{
    switch (MOD_GetModeType(c)) {
        case MOD_TYPE_CW: return "CW";
        case MOD_TYPE_AM: return "AM";
        case MOD_TYPE_FM: return "FM NARROW";
        default:          return "PROD SSB";
    }
}
sideband_t MOD_GetSideband(void) { return g_sb; }
void MOD_SetSideband(sideband_t sb) { g_sb = sb; }
uint32_t MOD_GetCarrierRequestedHz(void) { return g_hz; }
void MOD_SetCarrierFreqHz(uint32_t hz) { g_hz = hz; }
uint32_t MOD_GetActualCarrierFreqHz(void) { return g_hz + 3U; }
int32_t MOD_GetCarrierErrorHz(void) { return 3; }
/* TX TEST's grid readout at its widest: the 10 m WORST plan, k = +8 of 9. */
uint16_t MOD_GetCarrierPeriodTicks(void) { return 192U; }
uint8_t MOD_GetCarrierFraction(int8_t *extra) { if (extra) *extra = 8; return 9U; }
int64_t MOD_GetCarrierErrorMilliHz(void) { return -8047000; }
uint32_t MOD_GetHrtimInputClockHz(void) { return 170000000UL; }
uint32_t MOD_TxPhaseUpdateHz(void) { return 192000UL; }
int32_t MOD_GetCarrierImageDbc(void) { return -27; }
const char *MOD_TxTestDiagLabel(void) { return "10M WORST"; }
uint8_t MOD_BackgroundIoAllowed(uint32_t now) { (void)now; return 1U; }
uint8_t MOD_BlockingIoAllowed(uint32_t now) { (void)now; return 1U; }

/* The SD card: none, except for the trainer's frames, whose texts are the
 * card's -- then HELP.BIN from the tree (tools/gen_help_text.py). */
static uint8_t g_card;
uint8_t SD_IsMounted(void) { return g_card; }
static FILE *help_at(const char *path, uint32_t offset)
{
    FILE *f = (g_card && strcmp(path, "HELP.BIN") == 0) ? fopen(HELP_BIN_PATH, "rb") : NULL;
    if (f && fseek(f, (long)offset, SEEK_SET) != 0) { fclose(f); f = NULL; }
    return f;
}
uint8_t SD_DiagReadFile(const char *path, uint32_t offset, void *data, uint32_t len, uint32_t *out_len)
{
    FILE *f = help_at(path, offset);
    if (out_len) *out_len = 0U;
    if (!f) return 0U;
    const size_t got = fread(data, 1U, len, f);
    fclose(f);
    if (out_len) *out_len = (uint32_t)got;
    return 1U;
}
uint8_t SD_DiagReadSpans(const char *path, uint32_t off_a, void *a, uint32_t len_a,
                         uint32_t off_b, void *b, uint32_t len_b, uint32_t *got_b)
{
    uint32_t got = 0U;
    if (got_b) *got_b = 0U;
    if (!SD_DiagReadFile(path, off_a, a, len_a, &got) || got != len_a) return 0U;
    return SD_DiagReadFile(path, off_b, b, len_b, got_b);
}
uint8_t SD_DiagFileFnv1a(const char *path, uint32_t offset, uint32_t *hash)
{
    FILE *f = help_at(path, offset);
    uint32_t h = 0x811C9DC5UL;
    int c;
    if (!f) return 0U;
    while ((c = fgetc(f)) != EOF) h = (h ^ (uint32_t)c) * 0x01000193UL;
    fclose(f);
    *hash = h;
    return 1U;
}

/* The Morse trainer as the course would show it (mod_morse_trainer.c is
 * not linked: its view is set per frame). */
static mtrain_view_t g_tv;
static mtrain_prefs_t g_tp;
static mtrain_progress_t g_tpr;
const mtrain_view_t *MTRAIN_View(void) { return &g_tv; }
uint8_t MTRAIN_Active(void) { return (uint8_t)(g_tv.state != MTRAIN_OFF); }
mtrain_prefs_t *MTRAIN_Prefs(void) { return &g_tp; }
mtrain_progress_t *MTRAIN_Progress(void) { return &g_tpr; }
uint8_t MTRAIN_Start(uint8_t guided, uint32_t now) { (void)guided; (void)now; return 1U; }
uint8_t MOD_AudioRealtimeCritical(uint32_t now) { (void)now; return 0U; }
uint8_t MOD_PipelineNotificationPending(void) { return g_st.pipeline.sticky_latched; }
uint8_t MOD_GetMuted(void) { return g_muted; }
uint8_t MOD_CwBootArmStage(void) { return 3U; } /* armed: the prompt is not what these frames show */
void MOD_SetMuted(uint8_t m) { g_muted = m; }
float MOD_GetDrive(void) { return g_drive; }
void MOD_SetDrive(float d) { g_drive = d; }
float MOD_GetCwDrive(void) { return g_cw_drive; }
void MOD_SetCwDrive(float d) { g_cw_drive = d; }
float MOD_GetCwWpm(void) { return g_wpm; }
void MOD_SetCwWpm(float w) { g_wpm = w; }
float MOD_GetUsbInputGain(void) { return g_usb_gain; }
void MOD_SetUsbInputGain(float g) { g_usb_gain = g; }
tx_bw_preset_t MOD_GetTxBwPreset(void) { return g_txbw; }
void MOD_SetTxBwPreset(tx_bw_preset_t v) { g_txbw = v; }
uint8_t MOD_GetUiHomeFlags(void) { return g_flags; }
radio_supply_t RADIO_PdSupply(void) { return RADIO_SUPPLY_USB_PD; }
void RADIO_PdSnapshot(radio_pd_info_t *p)
{
    memset(p, 0, sizeof(*p));
    p->attached = p->contract = 1U;
    p->mv = p->target_mv = 9000U;
    p->ma = 3000U;
}
const char *RADIO_PdWhyDetail(void) { return "TARGET OK"; }
float RADIO_AudioRate(void) { return 177083.33f; } /* the PCM1821's rate at SYSCLK 170 MHz */
/* DIGI: a healthy WSJT-X session, or one on the MEMS reference with gaps. */
static uint8_t g_digi, g_digi_bad;
uint8_t RADIO_DigiReceive(void) { return g_digi; }
void RADIO_DigiStats(radio_digi_stats_t *o)
{
    memset(o, 0, sizeof(*o));
    o->streaming = 1U;
    o->cap_sent = 2880000U;
    o->restarts = 3U;
    o->level_dbfs = -38.4f;
    o->peak_dbfs = g_digi_bad ? -2.1f : -17.6f;
    o->adc_peak_dbfs = -56.3f;
}
void RADIO_DigiStatus(radio_digi_status_t *o, uint32_t now)
{
    (void)now;
    memset(o, 0, sizeof(*o));
    o->host_live = 1U;
    o->slot_ms = 7300;
    o->slot_locked = 1U;
    o->slot_depth_db = 9.8f;
    o->gaps_10min = g_digi_bad ? 4U : 0U;
    o->gaps_slot = g_digi_bad ? 1U : 0U;
    o->follow = g_digi_bad ? 0U : 1U;
    o->ref_ppm = 17.8f;
    o->ref_state = 2U;
    o->reacq_10min = 0U;
    o->load_pct = 69U;
}
uint8_t BOARD_FuncKeyPin(void) { return 6U; }
uint16_t LCDUI_FlushMs(void) { return 31U; }
uint8_t LCDUI_BootWorst(void) { return 1U; }
ui_shortcut_action_t MOD_GetUiShortcutUp(void) { return UI_SHORTCUT_AB_SWAP; }
ui_shortcut_action_t MOD_GetUiShortcutDown(void) { return UI_SHORTCUT_BAND; }
mod_build_flavor_t MOD_GetBuildFlavor(void) { return MOD_BUILD_FLAVOR_PROD; }
const char *MOD_GetStorageStatusLabel(void) { return "FLASH OK"; }
const char *MOD_GetResetReasonLabel(void) { return "POWER ON"; }
const char *MOD_PipelineStageName(uint8_t s) { (void)s; return "USB AUDIO IN"; }
const char *MOD_PipelineReasonName(uint8_t r) { (void)r; return "3 FRAMES LOST"; }
const char *MOD_GetCarrierClockPolicyLabel(mod_clock_policy_t p) { (void)p; return "FIXED 170"; }
float MOD_GetCalibrationThetaBin(uint8_t i) { return 0.002f * (float)((int)(i % 7U) - 3); }
float MOD_GetCalibrationCommonPhaseBin(uint8_t i) { return 0.003f * (float)((int)(i % 5U) - 2); }
uint8_t MOD_SelfTestStart(uint8_t mask) { (void)mask; return 1U; }

static const mod_band_t bands[] = {
    {"160M", 1.840f}, {"80M", 3.750f}, {"60M", 5.357f}, {"40M", 7.150f}, {"30M", 10.130f},
    {"20M", 14.225f}, {"17M", 18.118f}, {"15M", 21.300f}, {"12M", 24.940f}, {"10M FT8", 28.074f}
};
uint8_t MOD_GetBandCount(void) { return (uint8_t)(sizeof(bands) / sizeof(bands[0])); }
const mod_band_t *MOD_GetBand(uint8_t i) { return (i < MOD_GetBandCount()) ? &bands[i] : NULL; }
uint8_t MOD_GetActiveBandIndex(void)
{
    uint8_t best = 0U;
    for (uint8_t i = 0U; i < MOD_GetBandCount(); ++i)
        if ((float)g_hz * 1e-6f >= bands[i].mhz - 0.5f) best = i;
    return best;
}
void MOD_SelectBand(uint8_t i) { if (i < MOD_GetBandCount()) g_hz = (uint32_t)(bands[i].mhz * 1e6f + 0.5f); }

/* ---- Simulated receiver for the waterfall engine ------------------------ */

/* Capture blocks are synthesised from carriers at absolute frequencies
 * around whatever LO (raw) or dial (channel) they are taken at, as in
 * spectrum_engine_test.c: a 20 m evening with FT8, CW, a beacon and SSB. */
uint32_t SystemCoreClock = 170000000UL;
static const uint64_t sim_hr = 170000000ULL * 32ULL;
static uint32_t sim_period, sim_planned_hz;
static uint8_t sim_armed, sim_source, sim_halvings, sim_profile;
static uint32_t sim_arm_tick, sim_skip;
static uint32_t sim_scratch[RADIO_SPEC_SCRATCH_BYTES / 4U];
static float sim_block[2U * RADIO_SPEC_N];

static void sim_plan(void)
{
    /* The dial's LO sits 12 kHz below it, as the receive planner puts it. */
    if (sim_planned_hz != g_hz) { sim_planned_hz = g_hz; sim_period = (uint32_t)(sim_hr / (g_hz - 12000U)); }
}
static uint32_t sim_lo_hz(void) { sim_plan(); return (uint32_t)((sim_hr + sim_period / 2U) / sim_period); }
uint64_t RADIO_HrClockHz(void) { return sim_hr; }
uint32_t RADIO_RxClockHz(void) { return sim_lo_hz(); }
void RADIO_RxClockGeometry(radio_rx_geometry_t *g) { sim_plan(); memset(g, 0, sizeof(*g)); g->period = sim_period; }
uint8_t RADIO_RxLoHop(uint32_t p)
{
    if (g_rs != RADIO_RECEIVE) return 0U;
    sim_plan();
    sim_period = p;
    return 1U;
}
uint32_t RADIO_AudioGeneration(void) { return 1U; }
uint32_t RADIO_AudioOverruns(void) { return 0U; }
uint8_t RADIO_AudioLoadPct(void) { return 71U; }
void RADIO_AudioProfileEnable(uint8_t on) { sim_profile = on; }
uint8_t RADIO_AudioProfileEnabled(void) { return sim_profile; }
void *RADIO_AudioSpecScratch(void) { return sim_scratch; }
void RADIO_AudioSpecArm(uint8_t source, uint8_t halvings, uint16_t skip)
{
    sim_armed = 1U;
    sim_arm_tick = g_tick;
    sim_skip = skip;
    sim_source = source;
    sim_halvings = halvings;
}
void RADIO_AudioSpecStop(void) { sim_armed = 0U; }

typedef struct { uint32_t hz; float dbfs; uint16_t on_ms, period_ms, phase_ms; } sim_carrier_t;
static const sim_carrier_t sim_air[] = {
    /* FT8: 15 s slots, even and odd, 50 Hz apart above 14.074 */
    {14074450U, -62.0f, 12600U, 30000U, 0U},     {14074900U, -71.0f, 12600U, 30000U, 15000U},
    {14075300U, -58.0f, 12600U, 30000U, 0U},     {14075750U, -77.0f, 12600U, 30000U, 15000U},
    {14076200U, -66.0f, 12600U, 30000U, 0U},     {14076650U, -74.0f, 12600U, 30000U, 15000U},
    {14076950U, -69.0f, 12600U, 30000U, 0U},
    /* CW: keyed at about 20 WPM, and a slower one */
    {14031500U, -55.0f, 60U, 150U, 0U},          {14040200U, -73.0f, 180U, 420U, 90U},
    {14055800U, -80.0f, 60U, 130U, 30U},
    /* the NCDXF beacon on 14.100, ten seconds in every three minutes */
    {14100000U, -64.0f, 10000U, 12000U, 0U},
    /* SSB: a voice's comb of tones, syllables of a few hundred ms */
    {14107300U, -52.0f, 300U, 520U, 0U},         {14107750U, -56.0f, 300U, 520U, 40U},
    {14108200U, -60.0f, 240U, 520U, 90U},        {14108900U, -63.0f, 200U, 520U, 150U},
    {14109400U, -68.0f, 180U, 520U, 210U},
};

float *RADIO_AudioSpecTake(void)
{
    if (!sim_armed) return NULL;
    const double rate = sim_source ? 48000.0 / (double)(1U << sim_halvings) : (double)RADIO_AudioRate();
    const double ms = 1000.0 * ((double)RADIO_SPEC_N / rate + (double)sim_skip * 64.0 / (double)RADIO_AudioRate());
    if ((double)(g_tick - sim_arm_tick) < ms) return NULL;
    sim_armed = 0U;
    const double centre = sim_source ? (double)g_hz : (double)sim_lo_hz();
    static uint32_t seed = 1U;
    const double noise = pow(10.0, -103.0 / 20.0);
    for (unsigned n = 0U; n < RADIO_SPEC_N; ++n) {
        double i = 0.0, q = 0.0;
        for (unsigned c = 0U; c < sizeof(sim_air) / sizeof(sim_air[0]); ++c) {
            const sim_carrier_t *s = &sim_air[c];
            if (((g_tick + s->phase_ms) % s->period_ms) >= s->on_ms) continue;
            const double f = ((double)s->hz - centre) / rate;
            if (fabs(f) >= 0.5) continue;
            const double a = pow(10.0, s->dbfs / 20.0);
            i += a * cos(2.0 * M_PI * f * n);
            q += a * sin(2.0 * M_PI * f * n);
        }
        seed = seed * 1664525U + 1013904223U;
        i += noise * ((double)(seed >> 8) / 16777216.0 - 0.5) * 3.46;
        seed = seed * 1664525U + 1013904223U;
        q += noise * ((double)(seed >> 8) / 16777216.0 - 0.5) * 3.46;
        sim_block[2U * n] = (float)i;
        sim_block[2U * n + 1U] = (float)q;
    }
    return sim_block;
}

/* ---- Frames and events -------------------------------------------------- */

static const char *out_dir = ".";
static unsigned frame_no;

/* The frame as the panel is sent it: GFX_PrepareLines, colour band included. */
static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) { perror(path); exit(1); }
    fprintf(f, "P6\n%u %u\n255\n", (unsigned)GFX_W, (unsigned)GFX_H);
    static uint8_t line[GFX_W * 2U];
    for (uint16_t y = 0U; y < GFX_H; ++y) {
        GFX_PrepareLines(y, 1U, line);
        for (uint16_t x = 0U; x < GFX_W; ++x) {
            const uint16_t c = (uint16_t)((line[2U * x] << 8) | line[2U * x + 1U]);
            const uint8_t rgb[3] = {(uint8_t)((c >> 8) & 0xF8U), (uint8_t)((c >> 3) & 0xFCU), (uint8_t)((c << 3) & 0xF8U)};
            fwrite(rgb, 1U, 3U, f);
        }
    }
    fclose(f);
}

static void frame(const char *name)
{
    char path[512];
    UI_AppBuildFrame(NULL, g_tick);
    (void)snprintf(path, sizeof(path), "%s/%02u_%s.ppm", out_dir, frame_no++, name);
    write_ppm(path);
}

static void step(uint32_t ms) { g_tick += ms; UI_AppHandle(); }
static void turn(int8_t n) { ui_ev.turn = n; UI_AppHandle(); g_tick += 50U; }
static void turn_held(int8_t n) { ui_ev.turn_held = n; UI_AppHandle(); g_tick += 50U; }
static void click(void) { ui_ev.click = 1U; UI_AppHandle(); g_tick += 50U; }
static void hold(void) { ui_ev.hold = 1U; UI_AppHandle(); g_tick += 50U; }
static void sel(void) { ui_ev.sel = 1U; UI_AppHandle(); g_tick += 50U; }
static void fkey(void) { ui_ev.fkey = 1U; UI_AppHandle(); g_tick += 50U; }

static void menu_to_category(ui_cat_t cat)
{
    while (ui.page != PAGE_HOME) hold();
    fkey();                                  /* tuning level */
    hold(); hold();                          /* picker, then menu */
    while (UI_MenuCategoryAt(ui.menu_cat) != (uint8_t)cat) turn(1);
}

static void scenario_home(void)
{
    ui.boot_hold = 0U;
    frame("home_rx_usb");
    turn(3);                                 /* tune up 3 x 100 Hz */
    frame("home_tuning_hold");
    step(200U);
    click();                                 /* step 100 Hz -> 500 Hz (the dot) */
    frame("home_step_click");
    click(); click(); click(); click();      /* 1 kHz, 10 kHz, 10 Hz, 100 Hz */
    hold();                                  /* zone picker, on SETTINGS */
    frame("home_pick_settings");
    turn(1);
    frame("home_pick_diag");
    turn(-1);
    click();                                 /* into the settings row */
    frame("home_slide_volume");
    click();                                 /* edit VOLUME */
    turn(4);
    frame("home_slide_edit_volume");
    click();
    turn(5);                                 /* slide along */
    frame("home_slide_scrolled");
    turn(-3);                                /* MODE (WATERFALL is the second cell) */
    click();
    turn(2);                                 /* USB -> CW, applies on click */
    frame("home_slide_edit_mode");
    click();
    fkey();                                  /* back to tuning */
    frame("home_cw");                        /* decoder listening */
    g_cw_locked = 1U;
    cw_say("CQ CQ DE DL1ABC DL1ABC K ");
    frame("home_cw_decode");
    cw_say("GE OM UR RST 579 579 NAME HANS QTH MUNICH HW? DL1ABC DE ");
    frame("home_cw_decode_wrap");
    g_cw_offset = 4.2f;                      /* tuned in: ZERO */
    frame("home_cw_decode_zero");
    g_cw_active = 0U;                        /* a key-down pauses it */
    g_muted = 1U;
    g_st.meter.rms = 0.55f;
    g_st.meter.peak_hold = 24000U;
    g_st.usb_audio_seen = 1U;
    g_st.last_usb_audio_tick_ms = g_tick;
    frame("home_tx_cw_waiting");             /* TX, no reading yet */
    g_meter_on = 1U;
    ++rf_meter.seq;
    g_rs = RADIO_TRANSMIT;
    frame("home_tx_cw");
    g_meter_swr = 3.4f;
    ++rf_meter.seq;
    frame("home_tx_swr_alert");
    g_meter_swr = 1.18f;
    g_meter_on = 0U;
    frame("home_tx_hang_over");              /* back to receive: the zone returns */
    g_cw_active = 1U;
    g_rs = RADIO_RECEIVE;
    g_muted = 0U;
    g_st.usb_audio_seen = 0U;
    g_mode = MOD_CFG_PROD_SSB_14MHZ;
    g_cfg.rx_mode = RADIO_RX_USB;
    g_hz = 7074000UL;
    g_status = "NO LPF FOR FREQ";
    frame("home_warning");
    g_status = "RECEIVE";
    g_hz = 14074000UL;
    hold(); turn(1); click();                /* picker -> DIAGNOSTICS */
    for (uint8_t p = 0U; p < UI_DIAG_PAGES; ++p) {
        static const char *const names[UI_DIAG_PAGES] = {"diag_receive", "diag_transmit", "diag_power",
                                                         "diag_audio", "diag_system", "diag_digi"};
        frame(names[p]);
        turn(1);
    }
    click();                                 /* back to tuning */
    g_digi = 1U;                             /* WSJT-X sent MD9 */
    frame("home_digi");
    g_digi_bad = 1U;
    frame("home_digi_alert");
    g_digi = g_digi_bad = 0U;
}

static void scenario_menu(void)
{
    hold(); hold();
    frame("menu_top");
    turn(1);
    frame("menu_top_audio");
    click();
    frame("menu_audio");
    click();                                 /* edit HEADPHONE PCT */
    turn(5);
    frame("menu_edit_volume");
    click();
    hold();                                  /* back to categories */
    menu_to_category(UI_CAT_OPERATE);
    click();
    click();                                 /* edit MODE */
    turn(1);
    frame("menu_edit_mode");
    hold();                                  /* cancel */
    hold();
    menu_to_category(UI_CAT_CW);
    click();
    frame("menu_cw");                        /* TRAINER, MEMORIES, the everyday ones */
    turn(1);
    click();
    frame("menu_cw_memories");               /* MEMORY 1 shows its line (the stub's) */
    hold();
    hold();
    menu_to_category(UI_CAT_VOICE);
    click();
    frame("menu_voice_sections");
    turn_held(1);                            /* jump to next section */
    frame("menu_voice_jump");
    menu_to_category(UI_CAT_DISPLAY);
    click();
    click();                                 /* SCREEN ROTATION */
    frame("menu_edit_rotation");
    hold();
    menu_to_category(UI_CAT_BOARD);
    click();
    frame("menu_board");
    turn(1);
    click();                                 /* CONTROLS */
    frame("menu_board_controls");
    menu_to_category(UI_CAT_SYSTEM);
    click();
    frame("menu_system");
}

/* A finished self test: the rows the page lists, a detail as the card has it. */
static selftest_result_t g_strow[SELFTEST_COUNT];
const selftest_result_t *MOD_SelfTestResult(uint8_t id) { return id < SELFTEST_COUNT ? &g_strow[id] : NULL; }
const char *MOD_SelfTestDetailShown(uint8_t id)
{
    if (id < SELFTEST_COUNT && g_strow[id].detail) return g_strow[id].detail;
    return id == ST_RAIL5 ? "LOW: USB-C 5 V CONTRACT OR VIN BELOW DROPOUT" : "PB2 ADC2 IN12 VIA THE 5.7:1 DIVIDER";
}
static void selftest_results(void)
{
    static const struct { uint8_t id, verdict; int32_t value; } rows[] = {
        {ST_VREF, ST_PASS, 2902}, {ST_VIN, ST_PASS, 12040}, {ST_RAIL5, ST_WARN, 4310},
        {ST_RAIL_RIPPLE, ST_PASS, 38}, {ST_ADC_SCAN, ST_PASS, 4000}, {ST_FAULTS, ST_PASS, 0},
        {ST_RESET, ST_PASS, 4}, {ST_PD, ST_SKIP, 0}, {ST_SYSCLK, ST_PASS, 168000}, {ST_HRTIM, ST_PASS, 336000},
    };
    for (unsigned k = 0U; k < sizeof(rows) / sizeof(rows[0]); ++k) {
        g_strow[rows[k].id].verdict = rows[k].verdict;
        g_strow[rows[k].id].value = rows[k].value;
    }
    g_strow[ST_PD].detail = "NO USB-PD SOURCE: 5 V ONLY";
}

static void page(ui_page_t p, const char *name)
{
    while (ui.page != PAGE_HOME) hold();
    fkey();
    UI_NavOpenPage(p);
    frame(name);
}

/* A course frame: the view set, the card text given time to load (the
 * help cache waits for the cursor to rest), then drawn. */
static void trainer_frame(const char *name)
{
    UI_AppBuildFrame(NULL, g_tick);
    g_tick += 300U;
    UI_AppBuildFrame(NULL, g_tick);
    g_tick += 300U;
    frame(name);
}

static void set_text(mtrain_view_t *t, const char *s)
{
    t->prompt_len = (uint8_t)strlen(s);
    memcpy(t->prompt, s, t->prompt_len + 1U);
}

static void scenario_trainer(void)
{
    g_tp.listen_wpm = 20U;
    g_tp.pace_wpm = 10U;
    g_tp.key_wpm = 15U;
    g_card = 1U;
    while (ui.page != PAGE_HOME) hold();
    fkey();
    UI_NavOpenPage(PAGE_CW_TRAINER);
    /* START LEARNING, the keyer tried: held dits done, dahs pressed. */
    memset(&g_tv, 0, sizeof(g_tv));
    g_tv.state = MTRAIN_KEYING; g_tv.act = MTRAIN_FREE; g_tv.guided = 2U; g_tv.levers = 2U;
    g_tv.text = (uint8_t)(TXT_TR_EXPLORE - TXT_TR_PADDLE_DIT + 1);
    g_tv.listen_wpm = 20U; g_tv.pace_wpm = 10U;
    memcpy(g_tv.line, "S O", 4U); g_tv.line_len = 3U;
    trainer_frame("course_explore");
    /* Lesson 3's new character. */
    g_tv.state = MTRAIN_THINK; g_tv.act = MTRAIN_LEARN; g_tv.guided = 3U; g_tv.levers = 0U;
    g_tv.lesson = 3U; g_tv.lesson_max = MORSE_SYMBOLS; g_tv.newest = 'R'; g_tv.reveal = 1U;
    g_tv.text = (uint8_t)(TXT_TR_LEARN - TXT_TR_PADDLE_DIT + 1);
    set_text(&g_tv, "R");
    trainer_frame("course_intro");
    /* Familiarisation: listening, nothing shown yet. */
    g_tv.state = MTRAIN_PLAY; g_tv.act = MTRAIN_LISTEN; g_tv.guided = 4U; g_tv.reveal = 0U;
    g_tv.text = (uint8_t)(TXT_TR_C_FAMILIAR - TXT_TR_PADDLE_DIT + 1);
    set_text(&g_tv, "RMK");
    trainer_frame("course_listen");
    /* The sending drill, by sight. */
    g_tv.state = MTRAIN_THINK; g_tv.act = MTRAIN_ECHO; g_tv.guided = 5U; g_tv.reveal = 1U;
    g_tv.text = (uint8_t)(TXT_TR_C_DRILL - TXT_TR_PADDLE_DIT + 1);
    set_text(&g_tv, "RRRRR");
    trainer_frame("course_drill");
    /* Mastery: a wrong answer, the score so far. */
    g_tv.state = MTRAIN_THINK; g_tv.guided = 8U; g_tv.reveal = 0U;
    g_tv.text = (uint8_t)(TXT_TR_C_MASTER - TXT_TR_PADDLE_DIT + 1);
    g_tv.done = 31U; g_tv.right = 28U; g_tv.window = 44U; g_tpr.win_n = 48U; g_tv.weakest = 'R';
    set_text(&g_tv, "R");
    trainer_frame("course_master");
    g_tv.state = MTRAIN_FEEDBACK; g_tv.verdict = MTRAIN_V_WRONG; g_tv.reveal = 1U;
    g_tv.answer[0] = 'K'; g_tv.answer[1] = 0; g_tv.answer_len = 1U; g_tv.wrong = 1U;
    g_tv.expected = 'R'; g_tv.heard = 'K';
    trainer_frame("course_wrong");
    g_tv.state = MTRAIN_STEP_DONE; g_tv.verdict = MTRAIN_V_RIGHT;
    trainer_frame("course_step_done");
    memset(&g_tv, 0, sizeof(g_tv));
    g_card = 0U;
    hold();
    /* The TRAINER list down to its end: SAVE PROGRESS, DELETE PROGRESS. */
    menu_to_category(UI_CAT_CW);
    click();
    while (ui.menu_row != 0U) turn(-1);      /* TRAINER, the first row */
    click();
    frame("menu_trainer");
    turn(-1);                                /* round to the end */
    frame("menu_trainer_end");
}

static void scenario_pages(void)
{
    page(PAGE_FREQ, "page_freq");
    page(PAGE_CAL, "page_cal");
    turn(20);
    frame("page_cal_theta");
    g_meter_on = 1U;
    page(PAGE_TX_TEST, "page_tx_test");
    g_meter_on = 0U;
    page(PAGE_CW_TRAINER, "page_trainer");
    g_tel.vin_mv = 12040U; g_tel.rail5_mv = 5010U; g_tel.bias_mv = 712U; g_tel.fwd_mv = 3U; g_tel.env_a_mv = 11980U;
    page(PAGE_PD, "page_pd");
    page(PAGE_BPF_SWEEP, "page_bpf_sweep");
    page(PAGE_AF_SWEEP, "page_af_sweep");
    page(PAGE_IQ_SCOPE, "page_iq_scope");
    page(PAGE_SELFTEST, "page_selftest");
    selftest_results();
    turn(2);                                 /* the 5 V rail: its detail below */
    frame("page_selftest_results");
    page(PAGE_SD, "page_sd_hub");
    turn(3);
    frame("page_sd_hub_moved");
    hold();
}

/* The engine as the main loop runs it (UI_AppHandle, then
 * UI_AppServiceDisplay), ms by ms: a new line or the page's 250 ms refresh
 * rebuilds the frame. */
static void run_waterfall(uint32_t ms)
{
    for (uint32_t t = 0U; t < ms; ++t) {
        ++g_tick;
        UI_AppHandle();
        uint8_t dirty = UI_WaterfallService(g_tick);
        dirty |= UI_WaterfallService(g_tick);
        if (dirty || (g_tick % 250U) == 0U) UI_AppBuildFrame(NULL, g_tick);
    }
}

/* WATERFALL: the home row's second cell opens it; its settings list, the
 * split and spectrum layouts, a zoom and the experimental scanner. */
static void scenario_waterfall(void)
{
    while (ui.page != PAGE_HOME) hold();
    fkey();
    g_mode = MOD_CFG_PROD_SSB_14MHZ;
    g_cfg.rx_mode = RADIO_RX_USB;
    g_hz = 14074000UL;
    hold(); click();                         /* settings row */
    turn(-30);
    turn(1);                                 /* second cell: WATERFALL */
    frame("wf_home_cell");
    click();                                 /* opens the page */
    frame("wf_open");
    run_waterfall(9000U);
    frame("wf_default");
    click();                                 /* settings list */
    run_waterfall(600U);
    frame("wf_setup");
    turn(2);                                 /* LAYOUT */
    click();
    turn(1);                                 /* SPLIT */
    run_waterfall(300U);
    frame("wf_setup_edit_layout");
    click();
    turn(12);                                /* PEAK HOLD */
    click();
    turn(2);                                 /* SLOW */
    click();
    hold();                                  /* close the list */
    run_waterfall(6000U);
    frame("wf_split");
    turn_held(3);                            /* zoom to 9.5 kHz */
    run_waterfall(6000U);
    frame("wf_split_zoom");
    g_mode = MOD_CFG_CW_14MHZ;               /* CW: the decoder's line over the scale */
    g_cfg.rx_mode = RADIO_RX_CW;
    run_waterfall(6000U);
    frame("wf_split_cw");
    g_mode = MOD_CFG_PROD_SSB_14MHZ;
    g_cfg.rx_mode = RADIO_RX_USB;
    click();
    turn(-30);                               /* MODE */
    click();
    turn(1);                                 /* SCAN */
    click();
    turn(2);                                 /* LAYOUT */
    click();
    turn(-1);                                /* WATERFALL only */
    click();
    hold();
    run_waterfall(12000U);
    frame("wf_scan");
    g_rs = RADIO_TRANSMIT;
    run_waterfall(200U);
    frame("wf_scan_tx");
    g_rs = RADIO_RECEIVE;
    click();
    turn(30);                                /* DEFAULTS */
    click();
    hold();
    turn(-2);                                /* tune down 200 Hz */
    run_waterfall(4000U);
    frame("wf_defaults_tuned");
    hold();                                  /* back home */
    frame("wf_back_home");
}

/* The limits: VOLUME and BACKLIGHT stop at 100 %, VOLUME at 0 reads MUTED,
 * BACKLIGHT stops at its 5 % floor, and the row carries the on-air settings. */
static void scenario_limits(void)
{
    while (ui.page != PAGE_HOME) hold();
    fkey();
    g_mode = MOD_CFG_PROD_SSB_14MHZ;
    g_cfg.rx_mode = RADIO_RX_USB;
    hold(); click();                         /* settings row */
    turn(-30);                               /* first cell: VOLUME */
    click();                                 /* edit VOLUME */
    for (uint8_t i = 0U; i < 12U; ++i) turn(20);
    frame("limit_volume_top");
    for (uint8_t i = 0U; i < 12U; ++i) turn(-20);
    frame("limit_volume_muted");
    click();
    turn(8);                                 /* MIC AGC, NOISE GATE, TX EQ ... */
    frame("limit_row_voice");
    turn(20);                                /* to the end: BACKLIGHT, SETTINGS */
    turn(-1);
    click();                                 /* edit BACKLIGHT */
    for (uint8_t i = 0U; i < 8U; ++i) turn(-20);
    frame("limit_backlight_floor");
    click();
    fkey();
    g_mode = MOD_CFG_CW_14MHZ;
    hold(); click();
    turn(-30);
    turn(8);                                 /* HANG, WEIGHT, PADL SWAP, PRACTICE */
    frame("limit_row_cw");
    fkey();
    g_cfg.volume = 0U;
    frame("limit_home_muted");
    g_cfg.volume = 35U;
    g_cfg.backlight = 70U;
}

int main(int argc, char **argv)
{
    if (argc > 1) out_dir = argv[1];
    /* The UI times a retune with DWT->CYCCNT (ui_nav.c): give the Cortex-M
     * system control space a page of plain memory to land in. */
    if (mmap((void *)(uintptr_t)0xE0000000UL, 0x100000U, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED) {
        perror("mmap 0xE0000000");
        return 1;
    }
    RADIO_ConfigDefaults(&g_cfg);
    g_cfg.confirmed = 1U;
    g_cfg.volume = 35U;
    g_cfg.mic_source = RADIO_SOURCE_MIC2;
    g_cfg.mic_gain = 4.0f;
    g_tel.vin_mv = 12400U;

    UI_DrawBootHeader();
    static const char *const steps[][2] = {
        {"CLOCK", "SYS 170M"}, {"GPIO/DMA/SPI", "LCD OK"}, {"BOOT IMAGE", "CRC OK"}, {"TIM6", "48K BASE"},
        {"HRTIM DLL", "LOCKED"}, {"VREFINT", "2.901V"}, {"SD CARD", "SDHC"}, {"SUPPLY", "USB-C PD"},
        {"SETTINGS/RF", "GEOM OK"}, {"CORDIC", "HW OK"}, {"MATH", "DSP OK"}, {"UI", "HOME READY"},
        {"USB AUDIO", "48K READY"}, {"RF DMA", "PARKED"}, {"BOOT", "DONE"}
    };
    for (uint8_t i = 0U; i < sizeof(steps) / sizeof(steps[0]); ++i)
        UI_DrawBootRow(i, steps[i][0], (i == 6U) ? 2U : 1U, steps[i][1]);
    {
        /* frame() rebuilds from the UI; dump the boot log as drawn. */
        char path[512];
        (void)snprintf(path, sizeof(path), "%s/%02u_boot.ppm", out_dir, frame_no++);
        write_ppm(path);
    }

    UI_AppInit();
    UI_SDInit();
    scenario_home();
    scenario_limits();
    scenario_menu();
    scenario_pages();
    scenario_trainer();
    scenario_waterfall();
    g_st.pipeline.sticky_latched = 1U;
    g_st.pipeline.hard_lockout = 1U;
    frame("fault");
    printf("%u frames\n", frame_no);
    return 0;
}
