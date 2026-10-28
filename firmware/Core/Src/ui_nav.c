#pragma GCC optimize ("Os")

/* Front-panel navigation for the landscape UI.
 *
 * The knob is the primary control (turn, click, hold, hold-and-turn); FUNC
 * is BACK; the panel buttons are secondary. Home is three zones: the knob
 * tunes and a click cycles the step; hold lifts it to a zone picker
 * (frequency, settings row, diagnostics), and hold again opens the menu. In
 * lists the knob moves (clockwise = down), click selects, hold goes back;
 * UP/DN/SEL do the same. See docs/REV1.md "Front-panel controls". */

#include "ui_state.h"
#include "ui_trainer.h"
#include "ui_app.h"
#include "ui_sd.h"
#include "ui_lcd.h"
#include "ui_format.h"
#include "ui_waterfall.h"
#include "radio_spectrum.h"
#include "mod_selftest.h"
#include "radio.h"
#include "radio_analog.h"
#include "board_rev1.h"
#include "radio_audio.h"
#include "sctrl_link.h"
#include "main.h"
#include <string.h>

#define UI_PICK_TIMEOUT_MS   5000U
#define UI_SLIDE_TIMEOUT_MS  10000U
#define UI_MODE_DWELL_MS     400U
#define UI_FLASH_MS          1500U
#define UI_FREQ_DIGITS       8U     /* 10 MHz .. 1 Hz on the entry page */

/* ---- Small helpers -------------------------------------------------------- */

void UI_NavFlash(const char *text)
{
    ui.flash_text = text;
    ui.flash_until = HAL_GetTick() + UI_FLASH_MS;
    ui.dirty = 1U;
}

static uint8_t io_allowed(void)
{
    if (MOD_BackgroundIoAllowed(HAL_GetTick()) != 0U) return 1U;
    UI_NavFlash("AUDIO BUSY");
    return 0U;
}

static int8_t sign8(int32_t v) { return (int8_t)((v > 0) - (v < 0)); }

static uint8_t wrap_index(int32_t v, uint8_t count)
{
    if (count == 0U) return 0U;
    v %= (int32_t)count;
    if (v < 0) v += count;
    return (uint8_t)v;
}

/* ---- Modes that own the transmitter (TX test, trainer) -------------------- */

static mod_config_t tx_test_prev_mode = MOD_CFG_PROD_SSB_14MHZ;
static uint8_t tx_test_prev_valid;

static void tx_test_exit_restore(void)
{
    if (ui.page != PAGE_TX_TEST) return;
    MOD_TxCarrierTestStop();
    MOD_TxTestDiagPreset(0U);
    MOD_CarrierUpgradeHold(0U);
    if (tx_test_prev_valid) {
        MOD_SetTestConfig(tx_test_prev_mode);
        MOD_ApplyRequestedConfigNow();
        tx_test_prev_valid = 0U;
    }
}

static void set_page(ui_page_t page)
{
    if (!ui_debug_visible && ((page == PAGE_RUNTIME) || (page == PAGE_TESTS))) page = PAGE_MENU;
    if ((ui.page == PAGE_SD) && (page != PAGE_SD)) UI_SDExit();
    if ((ui.page == PAGE_WATERFALL) && (page != PAGE_WATERFALL)) UI_WaterfallExit();
    if (page != PAGE_CW_TRAINER) UI_TrainerLeave(); /* the radio back as it was */
    if (page != PAGE_TX_TEST) tx_test_exit_restore();
    if ((page != PAGE_HOME) && (page != PAGE_MENU)) ui.page_back = (ui.page == PAGE_HOME) ? PAGE_HOME : PAGE_MENU;
    ui.page = page;
    ui.cursor = 0U;
    ui.list_first = 0U;
    ui.page_edit = 0U;
    ui.editing = 0U;
    ui.home_level = (uint8_t)UI_HOME_TUNE;
    ui.dirty = 1U;
}

/* A trainer activity (mtrain_act_t, or UI_TRAINER_GUIDED) on its page. */
static void trainer_open(uint8_t act)
{
    if (!io_allowed()) return;
    tx_test_exit_restore();
    if (UI_TrainerBegin(act, HAL_GetTick())) set_page(PAGE_CW_TRAINER);
}

void UI_EnterCwTrainerPage(void) { trainer_open(MTRAIN_Prefs()->act); }
_Static_assert((int)UI_ACT_TRAIN_PADDLE - (int)UI_ACT_TRAIN_ECHO == (int)MTRAIN_PADDLE - (int)MTRAIN_ECHO,
               "the trainer's actions run in mtrain_act_t's order");

static void tx_test_enter(void)
{
    if (!io_allowed()) return;
    mod_config_t prev = MOD_GetTestConfig();
    if (prev == MOD_CFG_TX_CARRIER_TEST) prev = MOD_CFG_PROD_SSB_14MHZ;
    tx_test_prev_mode = prev;
    tx_test_prev_valid = 1U;
    MOD_TxCarrierTestStop();
    MOD_SetTestConfig(MOD_CFG_TX_CARRIER_TEST);
    MOD_ApplyRequestedConfigNow();
    set_page(PAGE_TX_TEST);
    MOD_CarrierUpgradeHold(1U);
}

void UI_NavOpenDiag(uint8_t diag_page)
{
    set_page(PAGE_DIAG);
    ui.cursor = (diag_page < UI_DIAG_PAGES) ? diag_page : 0U;
}

void UI_NavOpenPage(ui_page_t page)
{
    switch (page) {
        case PAGE_TX_TEST:    tx_test_enter(); break;
        case PAGE_CW_TRAINER: UI_EnterCwTrainerPage(); break;
        case PAGE_SD:         set_page(PAGE_SD); UI_SDEnter(); break;
        case PAGE_WATERFALL:
            if (ui.page != PAGE_WATERFALL) { set_page(PAGE_WATERFALL); UI_WaterfallEnter(); }
            break;
        case PAGE_FREQ:       set_page(PAGE_FREQ); ui.cursor = 4U; break;
        case PAGE_DIAG: {
            /* From the menu it stands in for ABOUT: open on SYSTEM; from
             * the home screen in DIGI, on the DIGI page. */
            const uint8_t from_menu = (uint8_t)(ui.page == PAGE_MENU);
            set_page(PAGE_DIAG);
            ui.cursor = from_menu ? (uint8_t)UI_DIAG_PAGE_SYSTEM
                      : (UI_ModeCurrent() == UI_UMODE_DIGI) ? (uint8_t)UI_DIAG_PAGE_DIGI : 0U;
            break;
        }
        default:              set_page(page); break;
    }
}

static void go_home(void)
{
    set_page(PAGE_HOME);
}

static void open_menu(void)
{
    if (RADIO_CalActive()) RADIO_CalStop();
    set_page(PAGE_MENU);
    ui.menu_level = 0U;
    ui.menu_group = AUDIO_GROUP_NONE;
    if (ui.menu_cat >= UI_MenuCategoryCount()) ui.menu_cat = 0U;
}

static void open_menu_category(uint8_t cat)
{
    open_menu();
    ui.menu_cat = cat;
    ui.menu_level = 1U;
    ui.menu_row = UI_MenuSelectable(cat, AUDIO_GROUP_NONE, ui.cat_row[cat], 1);
}

/* ---- Unified mode --------------------------------------------------------- */

/* A setting written as the console writes it, if it differs. */
static void setting_write(setting_id_t id, float v)
{
    const setting_item_t *it = UI_GetSetting(id);
    if (!it || !it->set) return;
    v = UI_ClampSettingValue(it, v);
    if (it->get && (it->get() == v)) return;
    it->set(v);
}

/* DIGI is a policy rather than a transmit configuration (UI_UModePlan):
 * entering it sets DIGI CLEAN and USB audio as the TX input, as CAT MD9
 * does; USB or LSB put the voice processing back and leave the input
 * alone, as CAT MD2/MD1 do. CW, AM and FM leave both (0xFF). */
static void mode_digi_apply(uint8_t digi)
{
    if (digi == 0xFFU) return;
    setting_write(SET_USB_CLEAN, digi ? (float)USB_AUDIO_POLICY_DIGI_CLEAN : (float)USB_AUDIO_POLICY_VOICE_PROC);
    if (digi) setting_write(SET_REV1_MIC, (float)RADIO_SOURCE_USB);
}

static uint8_t mode_apply(ui_umode_t mode, uint8_t lsb)
{
    if ((ui.page == PAGE_TX_TEST) || UI_TrainerOwns() ||
        (UI_SDPlayerPageActive() != 0U) || (RADIO_CalActive() != 0U)) return 0U;
    if (!io_allowed()) return 0U;
    const mod_config_t cfg = MOD_GetTestConfig();
    ui_umode_plan_t plan;
    UI_UModePlan(mode, cfg, MOD_GetModeType(cfg), MOD_GetSideband(), &plan);
    if (lsb) {
        plan.sideband = SIDEBAND_LSB;
        plan.change_sb = (uint8_t)(MOD_GetSideband() != SIDEBAND_LSB);
        plan.rx_mode = RADIO_RX_LSB;
    }
    if (plan.change_cfg) MOD_SetTestConfig(plan.cfg);
    if (plan.change_sb) MOD_SetSideband(plan.sideband);
    mode_digi_apply(plan.digi);
    (void)RADIO_SetRxMode(plan.rx_mode);
    ui.dirty = 1U;
    return 1U;
}

uint8_t UI_ModeApply(ui_umode_t mode) { return mode_apply(mode, 0U); }
uint8_t UI_ModeApplyDigi(sideband_t sideband)
{
    return mode_apply(UI_UMODE_DIGI, (uint8_t)(sideband == SIDEBAND_LSB));
}

ui_umode_t UI_ModeCurrent(void)
{
    const mod_config_t cfg = MOD_GetTestConfig();
    return UI_UModeFromTx(MOD_GetModeType(cfg), MOD_GetSideband(), RADIO_DigiReceive());
}

static ui_umode_t mode_shown(void)
{
    return (ui.mode_pending < (uint8_t)UI_UMODE_COUNT) ? (ui_umode_t)ui.mode_pending : UI_ModeCurrent();
}

static void mode_step(int8_t detents, uint32_t now)
{
    const int32_t m = (int32_t)mode_shown() + detents;
    ui.mode_pending = wrap_index(m, (uint8_t)UI_UMODE_COUNT);
    ui.mode_due = now + UI_MODE_DWELL_MS;
    ui.dirty = 1U;
}

static void mode_service(uint32_t now)
{
    if ((ui.mode_pending >= (uint8_t)UI_UMODE_COUNT) || ((int32_t)(now - ui.mode_due) < 0)) return;
    if (MOD_BackgroundIoAllowed(now) == 0U) return; /* shown as HOLD until it can go */
    if (UI_ModeApply((ui_umode_t)ui.mode_pending)) ui.mode_pending = (uint8_t)UI_UMODE_COUNT;
}

/* ---- Tuning ---------------------------------------------------------------- */

/* The band the title names: a band just chosen while its retune is pending,
 * otherwise the active one. A tune by the knob is pending for up to 100 ms
 * a detent; naming ui.band_index then showed whatever band was last chosen
 * from the list ("10M FT8" while tuning 20 m, bench 2026-10-01). */
static uint8_t band_index_now(void)
{
    return (ui.tune.pending && ui.band_pending) ? ui.band_index : MOD_GetActiveBandIndex();
}

void UI_NavSelectBand(uint8_t index)
{
    const mod_band_t *b = MOD_GetBand(index);
    if (b == NULL) return;
    ui.band_index = index;
    ui.band_pending = 1U;
    UI_TuneTarget(&ui.tune, UI_BandPresetHz(b->mhz), HAL_GetTick());
    ui.dirty = 1U;
}

static void band_step(int8_t detents)
{
    const uint8_t count = MOD_GetBandCount();
    if (count == 0U) return;
    UI_NavSelectBand(wrap_index((int32_t)band_index_now() + detents, count));
}

uint8_t UI_NavStepDigit(void) { return ui.tune_digit; }

void UI_NavSetStepDigit(uint8_t digit)
{
    ui.tune_digit = (digit < UI_TUNE_DIGITS) ? digit : (uint8_t)UI_TUNE_DIGIT_DEF;
    ui.dirty = 1U;
}

static void step_change(int8_t detents)
{
    int32_t d = (int32_t)ui.tune_digit + detents;
    if (d < 0) d = 0;
    if (d >= (int32_t)UI_TUNE_DIGITS) d = (int32_t)UI_TUNE_DIGITS - 1;
    UI_NavSetStepDigit((uint8_t)d);
}

static uint32_t tune_base(void)
{
    return ui.tune.pending ? ui.tune.target_hz : MOD_GetCarrierRequestedHz();
}

uint32_t UI_NavTuneHz(void) { return tune_base(); }

uint8_t UI_NavTuneTo(uint32_t hz)
{
    if ((hz < UI_TUNE_MIN_HZ) || (hz > UI_TUNE_MAX_HZ)) return 0U;
    if ((ui.page == PAGE_TX_TEST) || UI_TrainerOwns()) return 0U;
    ui.band_pending = 0U;
    UI_TuneTarget(&ui.tune, hz, HAL_GetTick());
    ui.dirty = 1U;
    return 1U;
}

ui_umode_t UI_NavModeShown(void) { return mode_shown(); }

/* Microseconds the last and the longest carrier apply took, and when
 * (console `knob`): every detent pays it in the main loop. */
uint32_t ui_tune_apply_us, ui_tune_apply_max_us, ui_tune_turn_tick, ui_tune_apply_tick;

static void tune_turn(int8_t detents, uint32_t now)
{
    uint8_t accel = UI_TuneAccel(now - ui.last_turn_ms, (uint32_t)((detents < 0) ? -detents : detents));
    ui.last_turn_ms = now;
    if (ui.tune_digit >= 5U) accel = 1U; /* no 10x jumps on 10 kHz and coarser */
    ui.band_pending = 0U;
    ui_tune_turn_tick = now;
    UI_TuneTarget(&ui.tune, UI_TuneMove(tune_base(), (int32_t)detents * accel, ui.tune_digit), now);
    ui.dirty = 1U;
}

/* The carrier follows the knob at a bounded rate: see UI_TuneDue. Held
 * only while transmitting (realtime-critical): MOD_SetCarrierFreqHz defers
 * the TX geometry itself while a stream is up, and the receiver follows the
 * requested frequency at once. Behind MOD_BackgroundIoAllowed, as before,
 * an unmuted radio tuned 200 ms late -- that gate also refuses for recent
 * microphone or USB audio, which has nothing to do with tuning. */
static void tune_service(uint32_t now)
{
    if (!ui.tune.pending) return;
    if (!UI_TuneDue(&ui.tune, now, (uint8_t)(MOD_AudioRealtimeCritical(now) == 0U))) return;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    const uint32_t t0 = DWT->CYCCNT;
    MOD_SetCarrierFreqHz(ui.tune.target_hz);
    ui_tune_apply_us = (DWT->CYCCNT - t0) / (SystemCoreClock / 1000000U);
    ui_tune_apply_tick = now;
    if (ui_tune_apply_us > ui_tune_apply_max_us) ui_tune_apply_max_us = ui_tune_apply_us;
    UI_TuneApplied(&ui.tune, now);
    ui.band_pending = 0U;
    ui.tune_applied = 1U; /* UI_AppServiceDisplay lets the receiver go first */
    ui.dirty = 1U;
}

/* ---- Settings ------------------------------------------------------------- */

/* Direct-write trims apply on every detent, so the operator hears or sees the
 * change while turning; everything else applies on confirm. */
static uint8_t setting_live(setting_id_t id)
{
    switch (id) {
        case SET_REV1_VOLUME:
        case SET_REV1_BACKLIGHT:
        case SET_REV1_MIC_GAIN:
        case SET_USB_GAIN:
        case SET_FM_GAIN:
        case SET_DRIVE:
        case SET_CW_DRIVE:
        case SET_CW_WPM:
        case SET_UI_STEP:
            return 1U;
        default:
            return 0U;
    }
}

/* The edit value is what the screen shows, so it has to stop where the
 * setter stops (a REV1 value such as VOLUME used to run on past 100 % on
 * screen while the radio sat at 100): UI_ClampSettingValue holds every
 * row's limits. */
static float setting_clamp(setting_id_t id, const setting_item_t *it, float value)
{
    (void)id;
    return UI_ClampSettingValue(it, value);
}

static float setting_step(setting_id_t id, const setting_item_t *it, float value,
                          int32_t detents, uint8_t accel)
{
    if (UI_SettingIsDiscrete(it->style)) {
        return (float)wrap_index((int32_t)(value + 0.5f) + detents,
                                 (uint8_t)UI_SettingDiscreteCount(it->style));
    }
    if (id == SET_REV1_MIC_GAIN) return UI_MicGainStep(value, detents);
    return setting_clamp(id, it, value + (float)(detents * (int32_t)accel) * UI_SettingStep(it));
}

/* ---- Editing one setting ----------------------------------------------------
 * The menu and the home settings row share this: turn (or UP/DN) changes the
 * value, live settings apply on every detent, the rest on click. */

static void edit_begin(setting_id_t id)
{
    const setting_item_t *it = UI_GetSetting(id);
    if ((it == NULL) || (it->set == NULL)) return;
    ui.editing = 1U;
    ui.edit_id = (uint8_t)id;
    ui.edit_orig = setting_clamp(id, it, it->get ? it->get() : 0.0f);
    ui.edit_value = ui.edit_orig;
    ui.dirty = 1U;
}

/* Leave an edit without setting it; a live setting goes back to where the
 * edit started. */
static void edit_cancel(void)
{
    const setting_id_t id = (setting_id_t)ui.edit_id;
    const setting_item_t *it = UI_GetSetting(id);
    if (ui.editing && setting_live(id) && (it != NULL) && (it->set != NULL)) it->set(ui.edit_orig);
    ui.editing = 0U;
    ui.dirty = 1U;
}

static __attribute__((noinline)) void edit_events(uint32_t now)
{
    const setting_id_t id = (setting_id_t)ui.edit_id;
    const setting_item_t *it = UI_GetSetting(id);
    if ((it == NULL) || (it->set == NULL)) { ui.editing = 0U; ui.dirty = 1U; return; }
    const int32_t delta = ui_ev.turn + ui_ev.up - ui_ev.dn;
    const int32_t coarse = ui_ev.turn_held + ui_ev.up_long - ui_ev.dn_long;
    if ((delta != 0) || (coarse != 0)) {
        uint8_t accel = UI_TuneAccel(now - ui.last_turn_ms, (uint32_t)((delta < 0) ? -delta : delta));
        ui.last_turn_ms = now;
        if (accel > 5U) accel = 5U;
        float v = setting_step(id, it, ui.edit_value, delta, accel);
        if (!UI_SettingIsDiscrete(it->style)) v = setting_step(id, it, v, coarse, 10U);
        ui.edit_value = v;
        if (setting_live(id)) it->set(v);
        ui.dirty = 1U;
    }
    if (ui_ev.click || ui_ev.sel) {
        if (!setting_live(id) && !io_allowed()) return;
        it->set(ui.edit_value);
        ui.editing = 0U;
        ui.dirty = 1U;
    } else if (ui_ev.hold || ui_ev.sel_long || ui_ev.fkey) {
        edit_cancel();
    }
}

/* ---- Home ------------------------------------------------------------------- */

static void ab_swap(void)
{
    MOD_SwapWithLastMode();
    (void)RADIO_SetRxMode(UI_UModeRxMode(UI_ModeCurrent()));
}

static void home_level(ui_home_level_t level, uint32_t now)
{
    ui.home_level = (uint8_t)level;
    ui.home_until = now + ((level == UI_HOME_PICK) ? UI_PICK_TIMEOUT_MS : UI_SLIDE_TIMEOUT_MS);
    ui.dirty = 1U;
}

static uint8_t slide_items(uint8_t out[UI_SLIDE_MAX])
{
    return UI_SlideItems(mode_shown(), (uint8_t)(RADIO_Config()->mic_source == RADIO_SOURCE_USB), out);
}

/* The settings row, open on `id` (the first item when the mode has none). */
static void slide_open(setting_id_t id, uint32_t now)
{
    uint8_t items[UI_SLIDE_MAX];
    const uint8_t n = slide_items(items);
    if (ui.page != PAGE_HOME) set_page(PAGE_HOME);
    ui.slide_focus = 0U;
    for (uint8_t i = 0U; i < n; ++i) if (items[i] == (uint8_t)id) ui.slide_focus = i;
    home_level(UI_HOME_SLIDE, now);
}

static void run_shortcut(ui_shortcut_action_t action, uint32_t now)
{
    switch (action) {
        case UI_SHORTCUT_AB_SWAP: ab_swap(); UI_NavFlash("A/B SWAP"); break;
        case UI_SHORTCUT_QUICK:   UI_NavOpenPage(PAGE_FREQ); break;
        case UI_SHORTCUT_BAND:    slide_open(SET_UI_BAND, now); break;
        case UI_SHORTCUT_MODES:   slide_open(SET_UI_MODE, now); break;
        case UI_SHORTCUT_SYSTEM:  open_menu_category(UI_CAT_SYSTEM); break;
        case UI_SHORTCUT_MENU:    open_menu(); break;
        case UI_SHORTCUT_NONE:
        default:                  break;
    }
}

/* Tuning: turn tunes, click cycles the step, hold picks a zone. FUNC has
 * nothing to go back from here; FUNC + turn changes band. */
/* The operator's one action after reading a fault: clear the latched
 * protection fault and the RF pipeline lockout with it. Before, the notice's
 * hold only re-armed the pipeline and FAULT_DMA stayed latched, so the radio
 * sat in PROTECTION until a power cycle. Refused with a key down, which would
 * key the transmitter straight back into whatever tripped it. */
static void fault_clear_request(void)
{
    const modulator_status_t *st = UI_FrameStatus();
    if (st->cw_paddle_dit_active || st->cw_paddle_dah_active || RADIO_Ptt()) {
        UI_NavFlash("RELEASE THE KEY FIRST");
        return;
    }
    if (RADIO_State() == RADIO_FAULT) RADIO_ClearFault();
    else MOD_RequestPipelineFaultAcknowledge();
    UI_NavFlash("FAULT CLEARED");
}

static __attribute__((noinline)) void home_tune_events(uint32_t now)
{
    /* PROTECTION: the first hold clears it (home_message says so). */
    if (ui_ev.hold && (RADIO_State() == RADIO_FAULT)) { fault_clear_request(); return; }
    if (ui_ev.hold) { ui.zone = (uint8_t)UI_ZONE_SETTINGS; home_level(UI_HOME_PICK, now); return; }
    if (ui_ev.sel_long) { open_menu(); return; }
    if (ui_ev.click) UI_NavSetStepDigit(UI_StepCycle(ui.tune_digit));
    if (ui_ev.turn != 0) tune_turn(ui_ev.turn, now);
    if (ui_ev.turn_held != 0) step_change(ui_ev.turn_held);
    if (ui_ev.fkey_turn != 0) band_step(ui_ev.fkey_turn);
    if (ui_ev.up) band_step(1);
    if (ui_ev.dn) band_step(-1);
    if (ui_ev.sel) mode_step(1, now);
    if (ui_ev.up_long) run_shortcut(MOD_GetUiShortcutUp(), now);
    if (ui_ev.dn_long) run_shortcut(MOD_GetUiShortcutDown(), now);
}

/* Zone picker: turn moves between the three zones, click enters one, hold
 * opens the menu, FUNC returns to tuning. */
static __attribute__((noinline)) void home_pick_events(uint32_t now)
{
    if (ui_ev.hold || ui_ev.sel_long) { open_menu(); return; }
    if (ui_ev.fkey) { home_level(UI_HOME_TUNE, now); return; }
    int32_t z = (int32_t)ui.zone + ui_ev.turn + ui_ev.dn - ui_ev.up;
    if (z < 0) z = 0;
    if (z >= (int32_t)UI_ZONE_COUNT) z = (int32_t)UI_ZONE_COUNT - 1;
    ui.zone = (uint8_t)z;
    home_level(UI_HOME_PICK, now);
    if (!(ui_ev.click || ui_ev.sel)) return;
    switch ((ui_zone_t)ui.zone) {
        case UI_ZONE_SETTINGS: home_level(UI_HOME_SLIDE, now); break;
        case UI_ZONE_DIAG:     UI_NavOpenPage(PAGE_DIAG); break;
        case UI_ZONE_TUNE:
        default:               home_level(UI_HOME_TUNE, now); break;
    }
}

/* Settings row: turn slides, click edits (or runs SAVE), hold goes back up
 * to the picker, FUNC back to tuning. */
static __attribute__((noinline)) void home_slide_events(uint32_t now)
{
    uint8_t items[UI_SLIDE_MAX];
    const uint8_t n = slide_items(items);
    home_level(UI_HOME_SLIDE, now);
    if (n == 0U) { home_level(UI_HOME_TUNE, now); return; }
    if (ui.slide_focus >= n) ui.slide_focus = (uint8_t)(n - 1U);
    if (ui.editing) { edit_events(now); return; }
    if (ui_ev.hold || ui_ev.sel_long) { ui.zone = (uint8_t)UI_ZONE_SETTINGS; home_level(UI_HOME_PICK, now); return; }
    if (ui_ev.fkey) { home_level(UI_HOME_TUNE, now); return; }
    int32_t f = (int32_t)ui.slide_focus + ui_ev.turn + ui_ev.dn - ui_ev.up;
    if (f < 0) f = 0;
    if (f >= (int32_t)n) f = (int32_t)n - 1;
    ui.slide_focus = (uint8_t)f;
    if (!(ui_ev.click || ui_ev.sel)) return;
    const setting_id_t id = (setting_id_t)items[ui.slide_focus];
    if ((uint8_t)id == UI_SLIDE_MEMORIES) {
        open_menu_category(UI_CAT_CW);
        ui.menu_parent_row = 1U; /* CW's MEMORIES link */
        ui.menu_group = AUDIO_GROUP_CW_MEMORIES;
        ui.menu_level = 2U;
        ui.menu_row = 0U;
    } else if (UI_SlideFormat((uint8_t)id) == UI_SLIDE_FMT_PAGE) {
        UI_NavOpenPage(PAGE_WATERFALL);
    } else if (id != SET_SAVE && UI_SlideFormat((uint8_t)id) == UI_SLIDE_FMT_ACTION) {
        UI_GetSetting(id)->set(0.0f); /* TRAINER: the last activity */
    } else if (UI_SlideFormat((uint8_t)id) == UI_SLIDE_FMT_ACTION) {
        if (io_allowed()) UI_NavFlash(MOD_SaveSettings() ? "SAVED" : "SAVE FAILED");
    } else {
        edit_begin(id);
    }
}

static __attribute__((noinline)) void home_events(uint32_t now)
{
    if (ui_ev.fkey_long) {
        /* FUNC held: back to tuning from anywhere on home. */
        if (ui.editing) edit_cancel();
        home_level(UI_HOME_TUNE, now);
        return;
    }
    switch ((ui_home_level_t)ui.home_level) {
        case UI_HOME_PICK:  home_pick_events(now); break;
        case UI_HOME_SLIDE: home_slide_events(now); break;
        case UI_HOME_TUNE:
        default:            home_tune_events(now); break;
    }
    ui.dirty = 1U;
}

/* ---- Menu ------------------------------------------------------------------- */

static uint8_t menu_cat(void) { return UI_MenuCategoryAt(ui.menu_cat); }
static uint8_t menu_group(void) { return (ui.menu_level == 2U) ? ui.menu_group : (uint8_t)AUDIO_GROUP_NONE; }

static uint8_t menu_row(ui_row_t *row)
{
    return UI_MenuRowAt(menu_cat(), menu_group(), ui.menu_row, row);
}

static void menu_move(int32_t detents)
{
    const uint8_t cat = menu_cat(), group = menu_group();
    const uint8_t total = UI_MenuRowCount(cat, group);
    int32_t row = ui.menu_row;
    const int8_t dir = sign8(detents);
    while ((detents != 0) && (total != 0U)) {
        row = wrap_index(row + dir, total);
        row = UI_MenuSelectable(cat, group, (uint8_t)row, dir);
        detents -= dir;
    }
    ui.menu_row = (uint8_t)row;
}

/* Hold-and-turn (or a held UP/DN) jumps a whole section. */
static void menu_jump(int8_t dir)
{
    const uint8_t cat = menu_cat(), group = menu_group();
    const uint8_t total = UI_MenuRowCount(cat, group);
    ui_row_t row;
    for (int16_t i = (int16_t)(ui.menu_row + dir); (i >= 0) && (i < (int16_t)total); i = (int16_t)(i + dir)) {
        if (UI_MenuRowAt(cat, group, (uint8_t)i, &row) && (row.kind == UI_ROW_HEADER)) {
            ui.menu_row = UI_MenuSelectable(cat, group, (uint8_t)(i + 1), 1);
            return;
        }
    }
    ui.menu_row = UI_MenuSelectable(cat, group, (dir > 0) ? (uint8_t)(total - 1U) : 0U, (int8_t)-dir);
}

static void menu_back(void)
{
    if (ui.menu_level == 2U) {
        ui.menu_level = 1U;
        ui.menu_group = AUDIO_GROUP_NONE;
        ui.menu_row = ui.menu_parent_row;
    } else if (ui.menu_level == 1U) {
        ui.cat_row[menu_cat()] = ui.menu_row;
        ui.menu_level = 0U;
    } else {
        go_home();
    }
    ui.dirty = 1U;
}

const char *UI_NavAction(ui_action_t action)
{
    switch (action) {
        case UI_ACT_SAVE:
            if (!io_allowed()) return NULL;
            return MOD_SaveSettings() ? "SAVED" : "SAVE FAILED";
        case UI_ACT_LOAD:
            if (!io_allowed()) return NULL;
            if (!MOD_LoadSettings()) return "LOAD FAILED";
            ui_debug_visible = (MOD_GetRuntimeDiagEnable() || MOD_GetCpuDiagEnable()) ? 1U : 0U;
            return "LOADED";
        case UI_ACT_FACTORY:
            if (!io_allowed()) return NULL;
            MOD_FactoryReset();
            return "FACTORY DEFAULTS";
        case UI_ACT_AB_SWAP:
            ab_swap();
            return "A/B SWAP";
        case UI_ACT_DEBUG_UI:
#if MOD_DEEP_DIAG_ENABLE
            ui_debug_visible = ui_debug_visible ? 0U : 1U;
            if (ui.menu_cat >= UI_MenuCategoryCount()) ui.menu_cat = 0U;
            return ui_debug_visible ? "DEBUG UI ON" : "DEBUG UI OFF";
#else
            return "DIAGNOSTIC IMAGE ONLY";
#endif
        case UI_ACT_TRAIN_GUIDED:
            trainer_open(UI_TRAINER_GUIDED);
            return NULL;
        case UI_ACT_TRAIN_ADD:
            return io_allowed() ? UI_TrainerAddCharacter() : NULL;
        case UI_ACT_TRAIN_DELETE: {
            /* A second click within 3 s: nothing goes by accident. */
            static uint32_t armed = 0x80000000UL;
            if (!io_allowed()) return NULL;
            if (HAL_GetTick() - armed > 3000U) {
                armed = HAL_GetTick();
                return "CLICK AGAIN TO DELETE";
            }
            armed = 0x80000000UL;
            return MOD_TrainerRecordDelete() ? "DELETED" : "NO CARD";
        }
        case UI_ACT_TRAIN_ECHO:
        case UI_ACT_TRAIN_LISTEN:
        case UI_ACT_TRAIN_LEARN:
        case UI_ACT_TRAIN_FREE:
        case UI_ACT_TRAIN_PADDLE:
            trainer_open((uint8_t)(MTRAIN_ECHO + (action - UI_ACT_TRAIN_ECHO)));
            return NULL;
        case UI_ACT_COUNT:
        default:
            return NULL;
    }
}

static void menu_activate_setting(setting_id_t id)
{
    const setting_item_t *it = UI_GetSetting(id);
    if (it == NULL) return;
    if (it->style == STY_ACTION) {
        if (!io_allowed()) return;
        if (it->set != NULL) it->set(1.0f);
        else UI_NavFlash(MOD_SaveSettings() ? "SAVED" : "SAVE FAILED");
    } else {
        edit_begin(id);
    }
}

static __attribute__((noinline)) void menu_activate(void)
{
    ui_row_t row;
    if (!menu_row(&row)) return;
    switch (row.kind) {
        case UI_ROW_SETTING:
            menu_activate_setting((setting_id_t)row.id);
            break;
        case UI_ROW_GROUP:
            ui.menu_parent_row = ui.menu_row;
            ui.menu_group = (uint8_t)row.id;
            ui.menu_level = 2U;
            ui.menu_row = 0U;
            break;
        case UI_ROW_PAGE:
            UI_NavOpenPage((ui_page_t)row.id);
            break;
        case UI_ROW_ACTION: {
            const char *text = UI_NavAction((ui_action_t)row.id);
            if (text != NULL) UI_NavFlash(text);
            break;
        }
        default:
            break;
    }
    ui.dirty = 1U;
}

static __attribute__((noinline)) void menu_events(uint32_t now)
{
    if (ui.editing) { edit_events(now); return; }
    if (ui_ev.hold || ui_ev.sel_long) { menu_back(); return; }
    const int32_t move = ui_ev.turn + ui_ev.dn - ui_ev.up;
    const int32_t jump = ui_ev.turn_held + ui_ev.dn_long - ui_ev.up_long;
    const uint8_t select = (uint8_t)(ui_ev.click || ui_ev.sel);
    if (ui.menu_level == 0U) {
        if (move != 0) ui.menu_cat = wrap_index((int32_t)ui.menu_cat + move, UI_MenuCategoryCount());
        if (select) {
            ui.menu_level = 1U;
            ui.menu_row = UI_MenuSelectable(menu_cat(), AUDIO_GROUP_NONE, ui.cat_row[menu_cat()], 1);
        }
    } else {
        if (move != 0) menu_move(move);
        if (jump != 0) menu_jump(sign8(jump));
        if (select) menu_activate();
    }
    ui.dirty = 1U;
}

/* ---- Pages ------------------------------------------------------------------ */

static void page_back(void)
{
    if (RADIO_CalActive()) RADIO_CalStop();
    if (ui.page_back == PAGE_MENU) set_page(PAGE_MENU);
    else go_home();
}

static __attribute__((noinline)) void freq_events(uint32_t now)
{
    static const uint32_t steps[UI_FREQ_DIGITS] = {
        10000000UL, 1000000UL, 100000UL, 10000UL, 1000UL, 100UL, 10UL, 1UL
    };
    if (ui_ev.hold || ui_ev.sel_long) { page_back(); return; }
    if (ui.cursor >= UI_FREQ_DIGITS) ui.cursor = 4U;
    const int32_t delta = ui_ev.turn + ui_ev.up - ui_ev.dn;
    if (delta != 0) {
        const int64_t hz = (int64_t)tune_base() + (int64_t)delta * (int64_t)steps[ui.cursor];
        ui.band_pending = 0U;
        UI_TuneTarget(&ui.tune, UI_TuneClamp((hz < 0) ? 0U : (uint32_t)hz), now);
    }
    if (ui_ev.turn_held != 0) ui.cursor = wrap_index((int32_t)ui.cursor + ui_ev.turn_held, UI_FREQ_DIGITS);
    if (ui_ev.click || ui_ev.sel) ui.cursor = wrap_index((int32_t)ui.cursor + 1, UI_FREQ_DIGITS);
    ui.dirty = 1U;
}

static void cal_edit(int32_t delta)
{
    float v;
    if (ui.cursor == UI_CAL_SKEW_ITEM) v = MOD_GetCalibrationDifferentialSkew();
    else if (ui.cursor < UI_CAL_THETA_FIRST) v = MOD_GetCalibrationCommonPhaseBin((uint8_t)(ui.cursor - UI_CAL_COMMON_FIRST));
    else v = MOD_GetCalibrationThetaBin((uint8_t)(ui.cursor - UI_CAL_THETA_FIRST));
    if (!io_allowed()) return;
    v += (float)delta * 0.0005f;
    if (ui.cursor == UI_CAL_SKEW_ITEM) MOD_SetCalibrationDifferentialSkew(v);
    else if (ui.cursor < UI_CAL_THETA_FIRST) MOD_SetCalibrationCommonPhaseBin((uint8_t)(ui.cursor - UI_CAL_COMMON_FIRST), v);
    else MOD_SetCalibrationThetaBin((uint8_t)(ui.cursor - UI_CAL_THETA_FIRST), v);
}

static __attribute__((noinline)) void cal_events(void)
{
    const uint8_t select = (uint8_t)(ui_ev.click || ui_ev.sel);
    if (ui_ev.hold || ui_ev.sel_long) {
        if (ui.page_edit) ui.page_edit = 0U;
        else page_back();
        return;
    }
    if (ui.page_edit) {
        const int32_t delta = ui_ev.turn + ui_ev.up - ui_ev.dn;
        if (delta != 0) cal_edit(delta);
        if (select) ui.page_edit = 0U;
        return;
    }
    const int32_t move = ui_ev.turn + ui_ev.dn - ui_ev.up;
    if (move != 0) ui.cursor = wrap_index((int32_t)ui.cursor + move, (uint8_t)UI_CAL_ITEM_COUNT);
    if (!select) return;
    if (ui.cursor != UI_CAL_STATUS_ITEM) { ui.page_edit = 1U; return; }
    if (MOD_GetCalibrationApply() == 0U) MOD_SetCalibrationApply(1U);
    else if (MOD_GetCalibrationValidated() == 0U) MOD_SetCalibrationValidated(1U);
    else MOD_SetCalibrationApply(0U);
}

static void trainer_events(uint32_t now)
{
    if (UI_TrainerEvents(now) == UI_TRAINER_EXIT) page_back();
}

static void tx_test_events(void)
{
    const int32_t move = ui_ev.turn + ui_ev.up - ui_ev.dn;
    /* PROTECTION (an SWR trip at the key-down): the knob's hold clears it in
     * place, as the badge says, so the next key-down tries again. */
    if (ui_ev.hold && !ui_ev.fkey && (RADIO_State() == RADIO_FAULT)) { fault_clear_request(); return; }
    if (ui_ev.hold || ui_ev.sel_long || ui_ev.click || ui_ev.sel) { page_back(); return; }
    if (move == 0) return;
    if (!io_allowed()) return;
    MOD_TxCarrierTestStop();
    /* The band presets, then the grid presets (GRID/WORST per band). */
    const uint8_t bands = MOD_GetBandCount();
    const uint8_t diag = MOD_TxTestDiagPresetActive();
    const int32_t at = diag ? (int32_t)bands + (int32_t)diag - 1 : (int32_t)MOD_GetActiveBandIndex();
    const uint8_t next = wrap_index(at + sign8(move), (uint8_t)(bands + MOD_TxTestDiagPresetCount()));
    if (next < bands) {
        MOD_TxTestDiagPreset(0U);
        MOD_SelectBand(next);
    } else {
        MOD_TxTestDiagPreset((uint8_t)(next - bands + 1U));
    }
}

#if MOD_DEEP_DIAG_ENABLE
static void tests_events(void)
{
    const int32_t move = ui_ev.turn + ui_ev.dn - ui_ev.up;
    if (ui_ev.hold || ui_ev.sel_long) { page_back(); return; }
    if (move != 0) ui.cursor = wrap_index((int32_t)ui.cursor + move, UI_GetTestModeCount());
    if ((ui_ev.click || ui_ev.sel) && io_allowed()) {
        MOD_SetTestConfig(UI_GetTestModeAt(ui.cursor));
        go_home();
    }
}
#endif

/* The bench pages that run a routine: select or back stops it and returns,
 * turning scrolls results or flips the detail pane. */
static void bench_events(void)
{
    const int32_t move = ui_ev.turn + ui_ev.dn - ui_ev.up;
    if (ui_ev.hold || ui_ev.sel_long || ui_ev.click || ui_ev.sel) { page_back(); return; }
    if (move == 0) return;
    if (ui.page == PAGE_SELFTEST) {
        int32_t c = (int32_t)ui.cursor + sign8(move);
        if (c < 0) c = 0;
        if (c >= (int32_t)SELFTEST_COUNT) c = (int32_t)SELFTEST_COUNT - 1;
        ui.cursor = (uint8_t)c;
    } else {
        ui.cursor ^= 1U;
    }
}

/* Diagnostics: turn (or UP/DN) flips the page, any press goes back. */
static void diag_events(void)
{
    const int32_t move = ui_ev.turn + ui_ev.dn - ui_ev.up;
    if (ui_ev.hold || ui_ev.sel_long || ui_ev.click || ui_ev.sel) { page_back(); return; }
    if (move != 0) ui.cursor = wrap_index((int32_t)ui.cursor + move, (uint8_t)UI_DIAG_PAGES);
}

/* The SD pages keep their own button grammar; the knob is mapped onto it one
 * detent per poll so a spun knob cannot queue a burst of card reads. */
static __attribute__((noinline)) void sd_events(void)
{
    static int8_t pending;
    /* FUNC is always a safe back here: the SD grammar's long select would
     * confirm a delete, format or firmware install. */
    if (ui_ev.fkey) { if (!UI_SDBack()) page_back(); return; }
    if ((ui_ev.hold || ui_ev.sel_long) && UI_SDAtHub()) { page_back(); return; }
    int32_t p = pending + ui_ev.turn;
    if (p > 24) p = 24;
    if (p < -24) p = -24;
    uint8_t up = ui_ev.up, down = ui_ev.dn;
    if (p > 0) { down = 1U; p--; }
    else if (p < 0) { up = 1U; p++; }
    pending = (int8_t)p;
    UI_SDHandle(up, down, (uint8_t)(ui_ev.click || ui_ev.sel),
                (uint8_t)(ui_ev.up_long || (ui_ev.turn_held < 0)),
                (uint8_t)(ui_ev.dn_long || (ui_ev.turn_held > 0)),
                (uint8_t)(ui_ev.hold || ui_ev.sel_long));
}

/* WATERFALL: tuning as on home -- turn tunes, UP/DN change band -- with a
 * click for the settings list, hold-and-turn (or FUNC + turn) to zoom, SEL
 * to step (or to sweep when scanning), and hold (FUNC) back home. */
static __attribute__((noinline)) void waterfall_events(uint32_t now)
{
    if (UI_WaterfallSetupOpen()) { UI_WaterfallSetupEvents(); return; }
    if (ui_ev.hold || ui_ev.sel_long) { page_back(); return; }
    if (ui_ev.click) { UI_WaterfallSetupShow(); return; }
    if (ui_ev.turn != 0) tune_turn(ui_ev.turn, now);
    if (ui_ev.turn_held != 0) UI_WaterfallZoom(ui_ev.turn_held);
    if (ui_ev.up) band_step(1);
    if (ui_ev.dn) band_step(-1);
    if (ui_ev.sel) {
        if (SPEC_Config()->mode == SPEC_MODE_SCAN) SPEC_ScanTrigger();
        else UI_NavSetStepDigit(UI_StepCycle(ui.tune_digit));
    }
}

static __attribute__((noinline)) void page_events(uint32_t now)
{
    switch (ui.page) {
        case PAGE_FREQ:       freq_events(now); break;
        case PAGE_WATERFALL:  waterfall_events(now); break;
        case PAGE_CAL:        cal_events(); break;
        case PAGE_CW_TRAINER: trainer_events(now); break;
        case PAGE_TX_TEST:    tx_test_events(); break;
#if MOD_DEEP_DIAG_ENABLE
        case PAGE_TESTS:      tests_events(); break;
#endif
        case PAGE_SD:         sd_events(); break;
        case PAGE_PD:
        case PAGE_BPF_SWEEP:
        case PAGE_AF_SWEEP:
        case PAGE_IQ_SCOPE:
        case PAGE_SELFTEST:   bench_events(); break;
        case PAGE_DIAG:       diag_events(); break;
        case PAGE_RUNTIME:
        default:              if (UI_EventsPending()) page_back(); break;
    }
    ui.dirty = 1U;
}

/* SW4/SW5 actions latched by RADIO_Service. */
static __attribute__((noinline)) void panel_request(uint8_t request, uint32_t now)
{
    switch (request) {
        case RADIO_PANEL_BAND_UP:   band_step(1); if (ui.page != PAGE_HOME) go_home(); break;
        case RADIO_PANEL_BAND_DOWN: band_step(-1); if (ui.page != PAGE_HOME) go_home(); break;
        case RADIO_PANEL_MENU:      if (ui.page == PAGE_MENU) go_home(); else open_menu(); break;
        case RADIO_PANEL_MUTE:
            MOD_SetMuted(MOD_GetMuted() ? 0U : 1U);
            UI_NavFlash(MOD_GetMuted() ? "TX MUTED" : "TX UNMUTED");
            break;
        case RADIO_PANEL_MODE_NEXT: mode_step(1, now); ui.mode_due = now; break;
        default: break;
    }
}

static __attribute__((noinline)) void home_timers(uint32_t now)
{
    if ((ui.page == PAGE_HOME) && (ui.home_level != (uint8_t)UI_HOME_TUNE) &&
        ((int32_t)(now - ui.home_until) >= 0)) {
        /* Left alone: a live value stays where it was turned to, anything
         * else is left unapplied, and the knob tunes again. */
        ui.editing = 0U;
        ui.home_level = (uint8_t)UI_HOME_TUNE;
        ui.dirty = 1U;
    }
    mode_service(now);
    tune_service(now);
}

void UI_AppHandle(void)
{
    const uint32_t now = HAL_GetTick();
    const uint8_t request = RADIO_TakePanelRequest();
    if (request != RADIO_PANEL_NONE) panel_request(request, now);
    home_timers(now);
    if (!UI_EventsPending()) return;

    if (ui.boot_hold) {
        /* The first key only puts the boot log away. */
        ui.boot_hold = 0U;
        ui.dirty = 1U;
    } else if (MOD_PipelineNotificationPending() != 0U) {
        if (ui_ev.hold || ui_ev.sel_long || ui_ev.fkey_long) fault_clear_request();
        ui.dirty = 1U;
    } else if (ui.page == PAGE_HOME) {
        home_events(now);
    } else if (ui_ev.fkey_long) {
        /* FUNC held: straight home from anywhere. */
        if ((ui.page == PAGE_MENU) && ui.editing) edit_cancel();
        go_home();
    } else {
        /* Away from home FUNC is BACK, and FUNC + turn is hold-and-turn. */
        ui_ev.turn_held = (int8_t)(ui_ev.turn_held + ui_ev.fkey_turn);
        if (ui_ev.fkey && (ui.page != PAGE_SD)) ui_ev.hold = 1U;
        if (ui.page == PAGE_MENU) menu_events(now);
        else page_events(now);
    }
    UI_AppConsumeEvents();
    if (ui.dirty) LCDUI_Preempt();
    /* A detent just turned is applied in this pass, not the next: between
     * them sat a whole frame build. */
    tune_service(now);
}

/* ---- Home view -------------------------------------------------------------- */

void UI_SlideValue(uint8_t id, float value, char *dst)
{
    const uint32_t whole = (value > 0.0f) ? (uint32_t)(value + 0.5f) : 0U;
    const uint32_t pct = (value > 0.0f) ? (uint32_t)(value * 100.0f + 0.5f) : 0U;
    const setting_item_t *it = UI_GetSetting((setting_id_t)id);
    dst[0] = 0;
    switch (UI_SlideFormat(id)) {
        case UI_SLIDE_FMT_PCT:
            /* Silence says so; "0%" reads like a fault. */
            if ((id == SET_REV1_VOLUME) && (whole == 0U)) strcpy(dst, "MUTED");
            else strcat(UI_AppendU(dst, whole, 1U), "%");
            break;
        case UI_SLIDE_FMT_PCT100: strcat(UI_AppendU(dst, pct, 1U), "%"); break;
        case UI_SLIDE_FMT_MIC_DB: strcat(UI_AppendI(dst, UI_MicGainDb(value)), "dB"); break;
        case UI_SLIDE_FMT_WPM:    strcat(UI_AppendU(dst, whole, 1U), " WPM"); break;
        case UI_SLIDE_FMT_ACTION: strcpy(dst, id == SET_SAVE ? "SAVE" : "START"); break;
        case UI_SLIDE_FMT_PAGE:
            if (id == UI_SLIDE_MEMORIES) strcpy(dst, MOD_CwMemoryBusy() ? "SENDING" : "SEND");
            else UI_WaterfallSummary(dst);
            break;
        case UI_SLIDE_FMT_SETTING:
        default:                  if (it != NULL) UI_FormatSettingValue(it, value, dst); break;
    }
}

/* 0..100 for the cell's bar, 255 for settings that are choices. */
static uint8_t slide_level(uint8_t id, float value)
{
    float lo = 0.0f, hi = 0.0f;
    const ui_slide_fmt_t fmt = UI_SlideFormat(id);
    if (fmt == UI_SLIDE_FMT_MIC_DB) return (uint8_t)((UI_MicGainDb(value) + 20) * 100 / 60);
    if ((fmt == UI_SLIDE_FMT_SETTING) || (fmt == UI_SLIDE_FMT_ACTION) || (fmt == UI_SLIDE_FMT_PAGE)) return 255U;
    if (!UI_SettingRange((setting_id_t)id, &lo, &hi) || !(hi > lo)) return 255U;
    float pos = (value - lo) / (hi - lo);
    if (pos < 0.0f) pos = 0.0f;
    if (pos > 1.0f) pos = 1.0f;
    return (uint8_t)(pos * 100.0f + 0.5f);
}

static void slide_view(ui_home_view_t *v)
{
    uint8_t items[UI_SLIDE_MAX];
    const uint8_t n = slide_items(items);
    const uint8_t focus = (ui.slide_focus < n) ? ui.slide_focus : (uint8_t)(n ? n - 1U : 0U);
    const uint8_t active = (uint8_t)(ui.home_level == (uint8_t)UI_HOME_SLIDE);
    ui.slide_first = UI_SlideFirst(active ? ui.slide_first : 0U, active ? focus : 0U, n);
    v->slide_count = n;
    v->slide_first = ui.slide_first;
    v->slide_focus = active ? focus : 255U;
    v->editing = (uint8_t)(active && ui.editing);
    for (uint8_t i = 0U; i < UI_SLIDE_VISIBLE; ++i) {
        const uint8_t index = (uint8_t)(ui.slide_first + i);
        v->slide_level[i] = 255U;
        if (index >= n) continue;
        const setting_item_t *it = UI_GetSetting((setting_id_t)items[index]);
        float value = ((it != NULL) && (it->get != NULL)) ? it->get() : 0.0f;
        if (v->editing && (index == focus)) value = ui.edit_value;
        v->slide_id[i] = items[index];
        UI_SlideValue(items[index], value, v->slide_value[i]);
        v->slide_level[i] = slide_level(items[index], value);
    }
    if (v->editing) {
        const setting_item_t *it = UI_GetSetting((setting_id_t)ui.edit_id);
        v->edit_name = (it != NULL) ? UI_SettingName((setting_id_t)ui.edit_id) : NULL;
    }
}

static ui_txs_t tx_state(void)
{
    switch (RADIO_State()) {
        case RADIO_TRANSMIT:   return UI_TXS_TX;
        case RADIO_TX_PREPARE: return UI_TXS_WAIT;
        case RADIO_RECEIVE:    return UI_TXS_RX;
        default:               return UI_TXS_OFF;
    }
}

/* "PROTECTION ADC DMA - HOLD TO CLEAR": which faults, and the way out. */
const char *UI_NavFaultMessage(void)
{
    static char text[48];
    radio_telemetry_t t;
    RADIO_GetTelemetry(&t);
    strcpy(text, "PROTECTION");
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
        if (!(t.faults & (1UL << bit))) continue;
        const char *name = MOD_RadioFaultName(bit);
        if (strlen(text) + strlen(name) + 2U > 30U) break;
        strcat(text, " ");
        strcat(text, name);
    }
    strcat(text, " - HOLD TO CLEAR");
    return text;
}

static void home_message(ui_home_view_t *v, const modulator_status_t *st, uint32_t now)
{
    const char *status = RADIO_Status();
    const radio_state_t rs = RADIO_State();
    v->message_level = UI_MSG_NONE;
    if ((ui.flash_text != NULL) && ((int32_t)(now - ui.flash_until) < 0)) {
        v->message = ui.flash_text;
        v->message_level = UI_MSG_INFO;
    } else if (st->retune_fault_latched) {
        v->message = "CARRIER RETUNE FAILED";
        v->message_level = UI_MSG_ALERT;
    } else if (rs == RADIO_FAULT) {
        v->message = UI_NavFaultMessage();
        v->message_level = UI_MSG_ALERT;
    } else if ((rs != RADIO_RECEIVE) && (rs != RADIO_TRANSMIT) && (rs != RADIO_TX_PREPARE)) {
        v->message = status;
        v->message_level = UI_MSG_WARN;
    } else if (strcmp(status, "RECEIVE") && strcmp(status, "TRANSMIT") && strcmp(status, "TX SETTLING")) {
        v->message = status;
        v->message_level = UI_MSG_WARN;
    } else if (MOD_StorageWarning() != NULL) {
        /* The settings store is the card: the radio keeps working without
         * it, but nothing it changes is kept, and a band whose calibration
         * could not be read keeps TX muted. */
        v->message = MOD_StorageWarning();
        v->message_level = MOD_CalNotLoaded() ? UI_MSG_ALERT : UI_MSG_WARN;
    } else if (st->pipeline.soft_fault_latched) {
        v->message = MOD_PipelineReasonName(st->pipeline.last_reason);
        v->message_level = UI_MSG_WARN;
    } else if (UI_ModeCurrent() == UI_UMODE_CW && !MOD_GetMuted() && MOD_CwBootArmStage() < 3U) {
        /* CW sends nothing after power-up until the key is held to arm it
         * (mod_cw.c); say how, and say when a key is closed from the start. */
        static const char *const arm[3] = {
            "CW KEY IS CLOSED - OPEN IT", "HOLD THE KEY 1 S TO ARM CW", "RELEASE THE KEY",
        };
        v->message = arm[MOD_CwBootArmStage()];
        v->message_level = MOD_CwBootArmStage() ? UI_MSG_INFO : UI_MSG_WARN;
    }
    static uint8_t arm_seen;
    if (MOD_CwBootArmStage() == 3U && !arm_seen) {
        arm_seen = 1U;
        if (UI_ModeCurrent() == UI_UMODE_CW) UI_NavFlash("CW ARMED");
    }
}

uint8_t UI_TxMeter(ui_tx_meter_t *m, uint32_t now)
{
    const radio_config_t *c = RADIO_Config();
    rf_meter_reading_t r;
    memset(m, 0, sizeof(*m));
    m->tune_pct = RADIO_ConfigTunePct(c);
    m->seq = rf_meter.seq;
    const uint8_t tx = (uint8_t)((RADIO_State() == RADIO_TRANSMIT) || BOARD_RfActive());
    /* In TX only this over's readings (a few intervals old at most); after
     * it, the last one for the hang. */
    const uint32_t age = tx ? 3U * RADIO_ConfigMeterUpdateMs(c) : RADIO_ConfigMeterHangMs(c);
    m->valid = RADIO_MeterRecent(now, age, &r);
    if (m->valid) {
        m->pep_w = r.pep_w;
        m->ref_w = r.ref_w;
        m->swr = r.swr;
        m->drain_v = r.drain_v;
        m->vin_v = r.vin_v;
        m->swr_valid = r.swr_valid;
        m->swr_alert = (uint8_t)(r.swr_valid && r.swr * 100.0f >= (float)c->swr_limit_x100);
    }
    return (uint8_t)(RADIO_ConfigTxPanel(c) && (tx || m->valid));
}

void UI_HomeView(ui_home_view_t *v, const modulator_status_t *st, uint32_t now)
{
    const uint8_t flags = MOD_GetUiHomeFlags();
    const mod_band_t *band;
    memset(v, 0, sizeof(*v));

    v->freq_hz = tune_base();
    v->step_digit = ui.tune_digit;
    v->tune_hold = (uint8_t)(ui.tune.pending || MOD_CarrierFrequencyApplyPending() ||
                             (ui.mode_pending < (uint8_t)UI_UMODE_COUNT));
    v->txs = (uint8_t)tx_state();
    v->umode = (uint8_t)mode_shown();
    v->rx_mode = RADIO_Config()->rx_mode;
    /* Against the applied mode: a pending change is not a mismatch. */
    v->rx_mismatch = (uint8_t)(UI_UModeRxMode(UI_ModeCurrent()) != v->rx_mode);
    v->tx_muted = MOD_GetMuted();
    v->pc_link = SCTRL_LinkBadge();
    v->usb_audio = (uint8_t)(st->usb_audio_seen && ((now - st->last_usb_audio_tick_ms) < 1000U));
    band = MOD_GetBand(band_index_now());
    v->band = (band != NULL) ? band->name : "";
    v->meter_tx = (uint8_t)((v->txs == UI_TXS_TX) || (v->txs == UI_TXS_WAIT));
    v->rx_dbfs = (int16_t)RADIO_AudioRxLevelDbfs();
    v->tx_rms = (uint8_t)((st->meter.rms >= 1.0f) ? 100U : (uint32_t)(st->meter.rms * 100.0f));
    v->tx_peak = (uint8_t)(((uint32_t)st->meter.peak_hold * 100U) / 32768U);
    v->clip = st->cessb_clipping;
    v->show_meter = (uint8_t)((flags & UI_HOME_FLAG_METER) != 0U);
    v->tx_panel = UI_TxMeter(&v->txm, now);
    v->show_hints = (uint8_t)((flags & UI_HOME_FLAG_HINTS) != 0U);
    v->has_fkey = (uint8_t)(BOARD_FuncKeyPin() != 0U);
    v->home_level = ui.home_level;
    v->zone = ui.zone;
    slide_view(v);
    if (v->umode == (uint8_t)UI_UMODE_DIGI) UI_DigiSummary(v->diag, st, now);
    else UI_DiagSummary(v->diag, st, now);
    home_message(v, st, now);
    if (UI_CwDecodeShown()) {
        uint8_t locked = 0U;
        float offset = 0.0f;
        v->cw_decode = 1U;
        RADIO_CwDecodeStatus(&v->cw_wpm, &offset, NULL, &locked);
        v->cw_state = !RADIO_CwDecodeActive() ? UI_CW_PAUSED : (locked ? UI_CW_LOCKED : UI_CW_LISTENING);
        if (v->cw_state != UI_CW_LOCKED) v->cw_wpm = 0U;
        v->cw_offset_hz = (int16_t)(offset < 0.0f ? offset - 0.5f : offset + 0.5f);
        /* The zone's cells are hidden: the worst warning among them stays. */
        uint8_t worst = UI_TONE_WARN - 1U;
        for (uint8_t i = 0U; i < UI_DIAG_SUMMARY; ++i) {
            const uint8_t tone = v->diag[i].tone;
            if ((tone == UI_TONE_WARN || tone == UI_TONE_ALERT) && tone > worst) { worst = tone; v->cw_alert = (uint8_t)(i + 1U); }
        }
        UI_CwTeletype(&v->cw_line[0], &v->cw_line[1]);
    }
}

/* ---- Decoded CW ---------------------------------------------------------- */

uint8_t UI_CwDecodeShown(void)
{
    const radio_config_t *c = RADIO_Config();
    return (uint8_t)((c->rx_mode == RADIO_RX_CW) && RADIO_ConfigCwDecode(c));
}

static struct {
    char line[2][UI_CW_LINE_CHARS + 1U];
    uint32_t seen; /* RADIO_CwDecodeCount already on the lines */
} tty;

/* One decoded character onto the newer line. */
static void tty_put(char c)
{
    char *cur = tty.line[1];
    size_t n = strlen(cur);
    if (c == ' ') {
        /* None at a line's start, after another, or past its end. */
        if (n && (n < UI_CW_LINE_CHARS) && (cur[n - 1U] != ' ')) { cur[n] = ' '; cur[n + 1U] = 0; }
        return;
    }
    if (n >= UI_CW_LINE_CHARS) {
        /* Full: the line moves up, and the word being written comes down
         * with its next letter -- unless it fills the line alone. */
        const char *space = strrchr(cur, ' ');
        const size_t keep = space ? (size_t)(space - cur) : n;
        const size_t moved = space ? n - keep - 1U : 0U;
        memcpy(tty.line[0], cur, keep);
        tty.line[0][keep] = 0;
        memmove(cur, cur + n - moved, moved);
        n = moved;
    }
    cur[n] = c;
    cur[n + 1U] = 0;
}

void UI_CwTeletype(const char **older, const char **newer)
{
    const uint32_t count = RADIO_CwDecodeCount();
    const char *text = RADIO_CwDecodeText();
    const size_t len = strlen(text);
    size_t from = len - (size_t)((count - tty.seen) < len ? (count - tty.seen) : len);
    if ((count < tty.seen) || (count - tty.seen > len)) {
        /* Cleared, or more came than the decoder keeps (a page away): start
         * again from its text, at a whole word if its start was cut. */
        tty.line[0][0] = tty.line[1][0] = 0;
        from = 0U;
        if (count > len) {
            const char *space = strchr(text, ' ');
            from = space ? (size_t)(space - text) + 1U : len;
        }
    }
    for (size_t i = from; i < len; ++i) tty_put(text[i]);
    tty.seen = count;
    *older = tty.line[0];
    *newer = tty.line[1];
}
