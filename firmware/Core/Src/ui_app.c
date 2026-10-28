#pragma GCC optimize ("Os")

#include "ui_state.h"
#include "ui_app.h"
#include "ui_lcd.h"
#include "ui_waterfall.h"
#include "radio.h"
#include "radio_analog.h"
#include "board_rev1.h"
#include "main.h"

ui_state_t ui;
uint8_t ui_debug_visible;

#define UI_REFRESH_HOME_MS      120U
#define UI_REFRESH_LIVE_MS      160U
#define UI_REFRESH_MENU_MS      500U
#define UI_REFRESH_DIAG_MS      400U
#define UI_REFRESH_LIVE_DIAG_MS 250U
#define UI_REFRESH_BUSY_MS      300U
#define UI_REFRESH_BUSY_DIAG_MS 500U
/* WATERFALL: new lines ask for their own frames; this keeps the footer's
 * load and state current in between. */
#define UI_REFRESH_WATERFALL_MS 250U
/* How long the boot log stays readable once the main loop runs: a clean
 * boot, one with a warning (no SD card, supply still settling), and one with
 * a failed stage, which waits for a key. Any key puts it away. */
#define UI_BOOT_HOLD_OK_MS      4000U
#define UI_BOOT_HOLD_WARN_MS    12000U

static uint8_t rotation_flag(void)
{
    return (uint8_t)((RADIO_Config()->boot_flags & RADIO_BOOT_FLAG_LCD_ROT270) != 0U);
}

void UI_AppInit(void)
{
#if MOD_DEEP_DIAG_ENABLE
    ui_debug_visible = (MOD_GetRuntimeDiagEnable() || MOD_GetCpuDiagEnable()) ? 1U : 0U;
#else
    ui_debug_visible = 0U;
#endif
    ui.page = PAGE_HOME;
    ui.page_back = PAGE_HOME;
    ui.home_level = (uint8_t)UI_HOME_TUNE;
    ui.zone = (uint8_t)UI_ZONE_SETTINGS;
    ui.boot_hold = 1U;
    ui.tune_digit = UI_TUNE_DIGIT_DEF;
    ui.mode_pending = (uint8_t)UI_UMODE_COUNT;
    ui.menu_group = AUDIO_GROUP_NONE;
    ui.band_index = MOD_GetActiveBandIndex();
    /* Nothing transmits or receives until the assembly is declared, so open
     * straight on the page that declares it; BACK from there lists the rest
     * of the board pages. */
    if (!RADIO_Config()->confirmed) {
        ui.page = PAGE_MENU;
        ui.menu_cat = (uint8_t)UI_CAT_BOARD;
        ui.menu_level = 2U;
        ui.menu_group = AUDIO_GROUP_BOARD_ASSEMBLY;
        ui.menu_parent_row = 0U;
        ui.menu_row = 0U;
    }
    LCDUI_SetRotation(rotation_flag());
    ui.dirty = 1U;
}

static uint16_t idle_refresh_ms(void)
{
    switch (ui.page) {
        case PAGE_HOME:       return UI_REFRESH_HOME_MS;
        case PAGE_MENU:       return UI_REFRESH_MENU_MS;
        case PAGE_FREQ:
        case PAGE_CW_TRAINER:
        case PAGE_TX_TEST:
        case PAGE_SD:         return UI_REFRESH_LIVE_MS;
        case PAGE_DIAG:       return UI_REFRESH_LIVE_DIAG_MS;
        case PAGE_RUNTIME:
        case PAGE_PD:
        case PAGE_BPF_SWEEP:
        case PAGE_AF_SWEEP:
        case PAGE_IQ_SCOPE:
        case PAGE_SELFTEST:   return UI_REFRESH_DIAG_MS;
        case PAGE_WATERFALL:  return UI_REFRESH_WATERFALL_MS;
        default:              return 0U;
    }
}

static uint16_t busy_refresh_ms(void)
{
    switch (ui.page) {
        case PAGE_HOME:
        case PAGE_CW_TRAINER:
        case PAGE_TX_TEST:
        case PAGE_SD:   return UI_REFRESH_BUSY_MS;
        case PAGE_DIAG:
        case PAGE_RUNTIME: return UI_REFRESH_BUSY_DIAG_MS;
        default:        return 0U;
    }
}

/* 1 while the boot log should stay on screen. The first call starts the
 * clock; the knob or any key (UI_AppHandle) ends it early, and so does a
 * pipeline fault, which has a screen of its own. */
static uint8_t boot_hold(uint32_t now)
{
    if (!ui.boot_hold) return 0U;
    if (ui.boot_until == 0U) {
        const uint8_t worst = LCDUI_BootWorst();
        ui.boot_until = now + ((worst == 1U) ? UI_BOOT_HOLD_OK_MS : UI_BOOT_HOLD_WARN_MS);
        if (worst == 0U) ui.boot_until = 0xFFFFFFFFUL; /* a failed stage: until a key */
        if (ui.boot_until == 0U) ui.boot_until = 1U;
    }
    if ((ui.boot_until != 0xFFFFFFFFUL) && ((int32_t)(now - ui.boot_until) >= 0)) ui.boot_hold = 0U;
    if (MOD_PipelineNotificationPending() != 0U) ui.boot_hold = 0U;
    if (!ui.boot_hold) ui.dirty = 1U;
    return ui.boot_hold;
}

/* What UI_AppServiceTxCue last drew over the frame: 0x10 home, 0x20 TX
 * TEST, | 1 for TX (0xFF: nothing since the last full frame, which draws
 * its own). */
static uint8_t cue_shown = 0xFFU;
/* The meter reading (and panel state) UI_AppServiceTxCue last drew, and
 * when it last looked: it looks again for a new reading, or every 100 ms
 * (the panel coming up at key-down, going at the end of the hang). */
static uint32_t cue_meter = 0xFFFFFFFFUL, cue_meter_seq, cue_meter_at;

void UI_AppServiceTxCue(uint32_t now)
{
    uint8_t changed = 0U;
    if (!ui.boot_hold && LCDUI_LightReady() && (ui.page == PAGE_HOME || ui.page == PAGE_TX_TEST)) {
        /* Home: the radio's state (TRANSMIT through a break-in hang, steadier
         * than the keying). TX TEST: the carrier itself, which the paddle
         * keys without leaving RECEIVE. */
        const uint8_t on = (uint8_t)((ui.page == PAGE_HOME) ? (RADIO_State() == RADIO_TRANSMIT) : BOARD_RfActive());
        const uint8_t code = (uint8_t)(((ui.page == PAGE_HOME) ? 0x10U : 0x20U) | on);
        if (code != cue_shown) {
            if (ui.page == PAGE_HOME) UI_RenderStateCue(on ? (uint8_t)UI_TXS_TX : (uint8_t)UI_TXS_RX);
            else UI_RenderTxTestCue(on);
            cue_shown = code;
            changed = 1U;
            ui.dirty = 1U; /* the next full frame takes over */
        }
        /* The live power and SWR: only the meter's own rectangles, once per
         * new reading (METER UPDATE MS), never a frame build -- the CPU in
         * TX belongs to the keyer and the RF path. */
        ui_tx_meter_t m;
        uint8_t panel = 0U;
        uint32_t key = cue_meter;
        if (rf_meter.seq != cue_meter_seq || (uint32_t)(now - cue_meter_at) >= 100U || cue_meter == 0xFFFFFFFFUL) {
            cue_meter_seq = rf_meter.seq;
            cue_meter_at = now;
            panel = UI_TxMeter(&m, now);
            key = (m.seq & 0x3FFFFFFFUL) | ((uint32_t)panel << 31) | ((uint32_t)m.valid << 30);
        }
        if (key != cue_meter) {
            if (ui.page == PAGE_TX_TEST) {
                UI_RenderTxTestMeter(&m);
                changed = 1U;
            } else if (panel) {
                UI_RenderTxPanel(&m);
                if (on && (MOD_GetUiHomeFlags() & UI_HOME_FLAG_METER)) UI_RenderTxMeterRow(&m, 0U);
                changed = 1U;
            }
            if (((cue_meter >> 31) & 1U) != panel) ui.dirty = 1U;
            cue_meter = key;
        }
    }
    LCDUI_LightFlush(changed);
}

void UI_AppServiceDisplay(uint32_t now)
{
    static uint8_t last_notification;
    cue_shown = 0xFFU;
    cue_meter = 0xFFFFFFFFUL;
    /* A tune applied in this pass: the main loop comes back to RADIO_Service,
     * which retunes the receiver, before it spends a frame build. */
    if (ui.tune_applied) { ui.tune_applied = 0U; return; }
    if (boot_hold(now)) return;
    const uint8_t notification = MOD_PipelineNotificationPending();
    if (notification != last_notification) {
        last_notification = notification;
        ui.dirty = 1U;
    }
    if ((ui.flash_text != NULL) && ((int32_t)(now - ui.flash_until) >= 0)) {
        ui.flash_text = NULL;
        ui.dirty = 1U;
    }
    /* The flag can change under the UI (settings load, factory reset, the
     * USB console); following it here keeps every path consistent. */
    LCDUI_SetRotation(rotation_flag());
    if ((ui.page == PAGE_WATERFALL) && UI_WaterfallService(now)) ui.dirty = 1U;
    if (UI_HelpWantsFrame(now)) ui.dirty = 1U;
    LCDUI_Service(now, &ui.dirty, idle_refresh_ms(), busy_refresh_ms(), UI_AppBuildFrame, NULL);
}
