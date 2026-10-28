#ifndef UI_TUNE_H
#define UI_TUNE_H

/* Operator arithmetic behind the front panel: the unified operating mode,
 * tuning steps and their acceleration, the rate at which a spun knob is
 * allowed to retune the carrier, and the contents of the home screen's
 * settings row. No hardware, no globals: the host suite ui_tune_test runs it as is. */

#include <stdint.h>
#include "modulator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Unified operating mode ------------------------------------------ */
/* One MODE moves the transmit configuration, the SSB sideband and the
 * receiver together, like any transceiver. */
typedef enum {
    UI_UMODE_USB = 0,
    UI_UMODE_LSB,
    UI_UMODE_CW,
    UI_UMODE_AM,
    UI_UMODE_FM,
    /* QMX's Digi: the SSB receiver with the digital-mode passband and fixed
     * gain to the USB sound card, USB audio as the transmit source sent as a
     * measured tone (DIGI TONE). It is USB/LSB with USB AUDIO POLICY = DIGI
     * CLEAN and TX INPUT = USB (RADIO_DigiReceive), so CAT MD9/MD6 (WSJT-X's
     * Data/Pkt) land in it too. */
    UI_UMODE_DIGI,
    UI_UMODE_COUNT
} ui_umode_t;

typedef struct {
    mod_config_t cfg;        /* transmit configuration to request */
    uint8_t      change_cfg; /* 0 when cfg is already the active one */
    uint8_t      change_sb;  /* MOD_SetSideband reloads calibration: only if it differs */
    sideband_t   sideband;
    uint8_t      rx_mode;    /* RADIO_RX_* */
    /* USB/LSB/DIGI only: 1 to set DIGI CLEAN and USB input, 0 to put the
     * voice processing back; 0xFF leaves them (CW, AM, FM). */
    uint8_t      digi;
} ui_umode_plan_t;

/* What to call for `mode`, given the present transmit configuration, its
 * modulation type and sideband. An SSB configuration that is already active
 * (including the diagnostic SSB variants) is kept when only the sideband
 * changes. */
void UI_UModePlan(ui_umode_t mode, mod_config_t current_cfg, mod_type_t current_type,
                  sideband_t current_sb, ui_umode_plan_t *out);
/* The mode the transmitter is in now; a TEST configuration reads as USB.
 * `digi` is RADIO_DigiReceive(): SSB with it is DIGI, on either sideband. */
ui_umode_t UI_UModeFromTx(mod_type_t type, sideband_t sb, uint8_t digi);
/* RADIO_RX_* that belongs to `mode`. */
uint8_t UI_UModeRxMode(ui_umode_t mode);
ui_umode_t UI_UModeNext(ui_umode_t mode, int8_t direction);
const char *UI_UModeLabel(ui_umode_t mode);
const char *UI_RxModeLabel(uint8_t rx_mode);

/* ---- Tuning ------------------------------------------------------------ */
#define UI_TUNE_MIN_HZ      1000000UL
#define UI_TUNE_MAX_HZ      28333333UL /* RADIO_RX_MAX_HZ */
/* Tuning steps, finest first: 1, 10, 100, 500 Hz, 1, 10, 100 kHz, 1 MHz.
 * 500 Hz is the cursor on the separator between the kHz and the 100 Hz
 * digit, as QMX/QMX+ have it (operating manual 1.03, "tuning rate"). */
#define UI_TUNE_DIGITS      8U         /* step positions */
#define UI_TUNE_DIGIT_DEF   2U         /* 100 Hz */
#define UI_TUNE_STEP_500    3U         /* the separator dot */

uint32_t UI_TuneStepHz(uint8_t digit);
/* Move `hz` by `detents` steps of 10^digit. An off-grid frequency lands on
 * the grid first: one detent up goes to the next multiple, one detent down to
 * the one below. Clamped to the receive range. */
uint32_t UI_TuneMove(uint32_t hz, int32_t detents, uint8_t digit);
uint32_t UI_TuneClamp(uint32_t hz);
/* Detents multiplier for a quick spin: `detents` arrived `interval_ms`
 * after the previous motion. 1 at hand speed, up to 10 for a hard spin. */
uint8_t UI_TuneAccel(uint32_t interval_ms, uint32_t detents);
/* Band preset in MHz (float in the band table) to whole kHz in Hz, so 28.074
 * is 28074000 and not the 28073999 a float truncation gives. */
uint32_t UI_BandPresetHz(float mhz);

/* A knob can outrun the carrier: every retune rebuilds the clock solution.
 * The target moves at knob speed; the carrier follows on the first detent
 * after a pause, at most every UI_TUNE_APPLY_PERIOD_MS while turning, and
 * once more UI_TUNE_SETTLE_MS after the knob stops. Since v8.08 the receiver
 * follows in place (no restart per retune), so the pace is set by feel:
 * 100 ms, sized when each apply restarted the receiver, read as lag (bench,
 * 2026-10-01). */
#define UI_TUNE_APPLY_PERIOD_MS 25U
#define UI_TUNE_SETTLE_MS       20U
typedef struct {
    uint32_t target_hz;
    uint32_t moved_ms;
    uint32_t applied_ms;
    uint8_t  pending;
} ui_tune_state_t;
void UI_TuneTarget(ui_tune_state_t *s, uint32_t hz, uint32_t now);
uint8_t UI_TuneDue(const ui_tune_state_t *s, uint32_t now, uint8_t io_allowed);
void UI_TuneApplied(ui_tune_state_t *s, uint32_t now);

/* "14.074.000", "7.074.000": MHz, then kHz and Hz groups. Returns length. */
uint8_t UI_FormatFreq(uint32_t hz, char *out, uint8_t cap);

/* Microphone gain is a linear factor 0.1..100 (-20..+40 dB). A detent is
 * one dB either way. */
float UI_MicGainStep(float gain, int32_t detents);
int32_t UI_MicGainDb(float gain);

/* ---- Home screen settings row ---------------------------------------- */
/* The sliding row under the frequency: the settings an operator changes
 * most, most used first, filtered to what the mode can use. Items are
 * setting_id_t values; the knob edits them through the settings map. */
#define UI_SLIDE_MAX 24U
typedef enum {
    UI_SLIDE_FMT_SETTING = 0, /* the settings map's own text */
    UI_SLIDE_FMT_PCT,         /* 0..100 as "45%" */
    UI_SLIDE_FMT_PCT100,      /* 0..1 as "45%" */
    UI_SLIDE_FMT_MIC_DB,      /* linear microphone gain as "+12dB" */
    UI_SLIDE_FMT_WPM,
    UI_SLIDE_FMT_ACTION,      /* runs on a click */
    UI_SLIDE_FMT_PAGE         /* opens its page on a click */
} ui_slide_fmt_t;
/* The row's cells are setting_id_t values except these, which are no
 * setting: WATERFALL opens its page, and it is the only way there; CW
 * MEMORIES opens CW > MEMORIES. */
#define UI_SLIDE_WATERFALL 0xFEU
#define UI_SLIDE_MEMORIES  0xFDU
/* Items for `mode`; `usb_source` swaps the microphone gain for the USB
 * input gain. Returns how many were written. */
uint8_t UI_SlideItems(ui_umode_t mode, uint8_t usb_source, uint8_t out[UI_SLIDE_MAX]);
const char *UI_SlideCaption(uint8_t id);
ui_slide_fmt_t UI_SlideFormat(uint8_t id);
/* Position of the four visible cells over `count` items so `focus` shows
 * with a neighbour on each side while there is one. */
uint8_t UI_SlideFirst(uint8_t first, uint8_t focus, uint8_t count);
#define UI_SLIDE_VISIBLE 4U

/* The knob click on the tuning screen: 10 Hz, 100 Hz, 500 Hz, 1 kHz, 10 kHz,
 * then round again. A step set elsewhere (1 Hz, 100 kHz, 1 MHz) goes to
 * 10 Hz. */
uint8_t UI_StepCycle(uint8_t digit);

#ifdef __cplusplus
}
#endif

#endif
