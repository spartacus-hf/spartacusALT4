#pragma GCC optimize ("Os")

#include "ui_tune.h"
#include "math_pos.h"
#include "ui_priv.h"
#include "radio_config.h"
#include <math.h>

static const char *const umode_labels[UI_UMODE_COUNT] = {"USB", "LSB", "CW", "AM", "FM", "DIGI"};
static const uint8_t umode_rx[UI_UMODE_COUNT] = {
    RADIO_RX_USB, RADIO_RX_LSB, RADIO_RX_CW, RADIO_RX_AM, RADIO_RX_FM, RADIO_RX_USB
};

void UI_UModePlan(ui_umode_t mode, mod_config_t current_cfg, mod_type_t current_type,
                  sideband_t current_sb, ui_umode_plan_t *out)
{
    if ((uint32_t)mode >= (uint32_t)UI_UMODE_COUNT) mode = UI_UMODE_USB;
    out->sideband = current_sb;
    out->change_sb = 0U;
    out->rx_mode = umode_rx[mode];
    out->digi = 0xFFU;
    switch (mode) {
        case UI_UMODE_CW: out->cfg = MOD_CFG_CW_14MHZ; break;
        case UI_UMODE_AM: out->cfg = MOD_CFG_AM_14MHZ; break;
        case UI_UMODE_FM: out->cfg = MOD_CFG_FM_NARROW_14MHZ; break;
        case UI_UMODE_USB:
        case UI_UMODE_LSB:
        case UI_UMODE_DIGI:
        default:
            /* DIGI picked on the panel is upper sideband, as every FT8, FT4
             * and WSPR dial is (QMX's Digi default); CAT MD6 makes it LSB. */
            out->cfg = (current_type == MOD_TYPE_SSB) ? current_cfg : MOD_CFG_PROD_SSB_14MHZ;
            out->sideband = (mode == UI_UMODE_LSB) ? SIDEBAND_LSB : SIDEBAND_USB;
            out->change_sb = (out->sideband != current_sb) ? 1U : 0U;
            out->digi = (mode == UI_UMODE_DIGI) ? 1U : 0U;
            break;
    }
    out->change_cfg = (out->cfg != current_cfg) ? 1U : 0U;
}

ui_umode_t UI_UModeFromTx(mod_type_t type, sideband_t sb, uint8_t digi)
{
    switch (type) {
        case MOD_TYPE_CW: return UI_UMODE_CW;
        case MOD_TYPE_AM: return UI_UMODE_AM;
        case MOD_TYPE_FM: return UI_UMODE_FM;
        case MOD_TYPE_TEST:
            return UI_UMODE_USB;
        case MOD_TYPE_SSB:
        default:
            if (digi) return UI_UMODE_DIGI;
            return (sb == SIDEBAND_LSB) ? UI_UMODE_LSB : UI_UMODE_USB;
    }
}

uint8_t UI_UModeRxMode(ui_umode_t mode)
{
    return ((uint32_t)mode < (uint32_t)UI_UMODE_COUNT) ? umode_rx[mode] : (uint8_t)RADIO_RX_USB;
}

ui_umode_t UI_UModeNext(ui_umode_t mode, int8_t direction)
{
    int32_t m = (int32_t)mode + ((direction < 0) ? -1 : 1);
    if (m < 0) m = (int32_t)UI_UMODE_COUNT - 1;
    if (m >= (int32_t)UI_UMODE_COUNT) m = 0;
    return (ui_umode_t)m;
}

const char *UI_UModeLabel(ui_umode_t mode)
{
    return ((uint32_t)mode < (uint32_t)UI_UMODE_COUNT) ? umode_labels[mode] : "?";
}

const char *UI_RxModeLabel(uint8_t rx_mode)
{
    for (uint8_t i = 0U; i < (uint8_t)UI_UMODE_COUNT; ++i)
        if (umode_rx[i] == rx_mode) return umode_labels[i];
    return "?";
}

uint32_t UI_TuneStepHz(uint8_t digit)
{
    static const uint32_t steps[UI_TUNE_DIGITS] = {1U, 10U, 100U, 500U, 1000U, 10000U, 100000U, 1000000U};
    return steps[(digit < UI_TUNE_DIGITS) ? digit : UI_TUNE_DIGITS - 1U];
}

uint32_t UI_TuneClamp(uint32_t hz)
{
    if (hz < UI_TUNE_MIN_HZ) return UI_TUNE_MIN_HZ;
    if (hz > UI_TUNE_MAX_HZ) return UI_TUNE_MAX_HZ;
    return hz;
}

uint32_t UI_TuneMove(uint32_t hz, int32_t detents, uint8_t digit)
{
    const int64_t step = (int64_t)UI_TuneStepHz(digit);
    int64_t base = ((int64_t)hz / step) * step;
    if ((detents < 0) && (base != (int64_t)hz)) detents++;
    int64_t next = base + (int64_t)detents * step;
    if (next < (int64_t)UI_TUNE_MIN_HZ) next = (int64_t)UI_TUNE_MIN_HZ;
    if (next > (int64_t)UI_TUNE_MAX_HZ) next = (int64_t)UI_TUNE_MAX_HZ;
    return (uint32_t)next;
}

uint8_t UI_TuneAccel(uint32_t interval_ms, uint32_t detents)
{
    if (detents == 0U) return 1U;
    if (interval_ms == 0U) interval_ms = 1U;
    const uint32_t per_second = (detents * 1000U) / interval_ms;
    if (per_second >= 60U) return 10U;
    if (per_second >= 35U) return 5U;
    if (per_second >= 18U) return 2U;
    return 1U;
}

uint32_t UI_BandPresetHz(float mhz)
{
    if (!(mhz > 0.0f)) return UI_TUNE_MIN_HZ;
    const float khz = mhz * 1000.0f + 0.5f;
    return UI_TuneClamp((uint32_t)khz * 1000U);
}

void UI_TuneTarget(ui_tune_state_t *s, uint32_t hz, uint32_t now)
{
    s->target_hz = UI_TuneClamp(hz);
    s->moved_ms = now;
    s->pending = 1U;
}

uint8_t UI_TuneDue(const ui_tune_state_t *s, uint32_t now, uint8_t io_allowed)
{
    if ((s->pending == 0U) || (io_allowed == 0U)) return 0U;
    if ((uint32_t)(now - s->applied_ms) >= UI_TUNE_APPLY_PERIOD_MS) return 1U;
    return ((uint32_t)(now - s->moved_ms) >= UI_TUNE_SETTLE_MS) ? 1U : 0U;
}

void UI_TuneApplied(ui_tune_state_t *s, uint32_t now)
{
    s->applied_ms = now;
    s->pending = 0U;
}

static char *put_group(char *p, uint32_t v)
{
    *p++ = (char)('0' + (v / 100U));
    *p++ = (char)('0' + ((v / 10U) % 10U));
    *p++ = (char)('0' + (v % 10U));
    return p;
}

uint8_t UI_FormatFreq(uint32_t hz, char *out, uint8_t cap)
{
    char tmp[16];
    char *p = tmp;
    uint32_t mhz = hz / 1000000UL;
    if (mhz > 999U) mhz = 999U;
    if (mhz >= 100U) *p++ = (char)('0' + (mhz / 100U));
    if (mhz >= 10U) *p++ = (char)('0' + ((mhz / 10U) % 10U));
    *p++ = (char)('0' + (mhz % 10U));
    *p++ = '.';
    p = put_group(p, (hz / 1000U) % 1000U);
    *p++ = '.';
    p = put_group(p, hz % 1000U);
    const uint8_t n = (uint8_t)(p - tmp);
    if ((out == NULL) || (cap == 0U)) return n;
    uint8_t i = 0U;
    for (; (i < n) && (i + 1U < cap); ++i) out[i] = tmp[i];
    out[i] = 0;
    return i;
}

float UI_MicGainStep(float gain, int32_t detents)
{
    if (!(gain >= 0.1f)) gain = 0.1f;
    if (gain > 100.0f) gain = 100.0f;
    /* Snap to the dB grid so a knob round trip returns the same value. */
    float db = roundf(20.0f * log10f(gain)) + (float)detents;
    if (db < -20.0f) db = -20.0f;
    if (db > 40.0f) db = 40.0f;
    float g = powf_pos(10.0f, db / 20.0f);
    if (g < 0.1f) g = 0.1f;
    if (g > 100.0f) g = 100.0f;
    return g;
}

int32_t UI_MicGainDb(float gain)
{
    if (!(gain >= 0.1f)) gain = 0.1f;
    return (int32_t)lroundf(20.0f * log10f(gain));
}

/* ---- Settings row ------------------------------------------------------ */

#define M_USB   (1U << UI_UMODE_USB)
#define M_LSB   (1U << UI_UMODE_LSB)
#define M_CW    (1U << UI_UMODE_CW)
#define M_AM    (1U << UI_UMODE_AM)
#define M_FM    (1U << UI_UMODE_FM)
#define M_DIGI  (1U << UI_UMODE_DIGI)
#define M_SSB   (M_USB | M_LSB)
#define M_VOICE (M_SSB | M_AM | M_FM)
#define M_ALL   (M_VOICE | M_CW | M_DIGI)
#define M_MIC   0x40U /* only with the microphone as TX input */
#define M_USBIN 0x80U /* only with USB audio as TX input */

typedef struct {
    uint8_t     id;
    uint8_t     when;
    uint8_t     fmt;
    const char *caption;
} slide_t;

_Static_assert(UI_SLIDE_WATERFALL >= SET_COUNT, "the WATERFALL cell must not be a setting id");
_Static_assert(UI_SLIDE_MEMORIES >= SET_COUNT, "the MEMORIES cell must not be a setting id");

/* Most used first: what is on screen without scrolling is volume, the
 * waterfall, mode and band. Then the receiver as the mode uses it -- its
 * filter, AGC and, in CW, pitch and decoder; the FM squelch -- then power (or
 * speed in CW) and what an operator reaches for on the air -- the
 * microphone chain in voice, the memories, the keyer and the trainer in CW
 * (weight, hang and break-in are ADVANCED CW's since v8.10) -- before the station
 * settings (TX input, mute, step, USB I/Q, backlight) and SAVE. Through
 * v8.07 the row held no receive setting at all (bench report, 2026-09-30). */
static const slide_t slides[] = {
    {SET_REV1_VOLUME,    M_ALL & ~M_DIGI, UI_SLIDE_FMT_PCT,     "VOLUME"}, /* DIGI: phones off */
    {UI_SLIDE_WATERFALL, M_ALL,           UI_SLIDE_FMT_PAGE,    "WATERFALL"},
    {SET_UI_MODE,        M_ALL,           UI_SLIDE_FMT_SETTING, "MODE"},
    {SET_UI_BAND,        M_ALL,           UI_SLIDE_FMT_SETTING, "BAND"},
    {SET_CW_RX_BW,       M_CW,            UI_SLIDE_FMT_SETTING, "RX FILTER"},
    {SET_SSB_RX_BW,      M_SSB,           UI_SLIDE_FMT_SETTING, "RX FILTER"},
    {SET_AM_RX_BW,       M_AM,            UI_SLIDE_FMT_SETTING, "RX FILTER"},
    {SET_FM_SQUELCH,     M_FM,            UI_SLIDE_FMT_SETTING, "SQUELCH"},
    {SET_CW_PITCH,       M_CW,            UI_SLIDE_FMT_SETTING, "PITCH"},
    {SET_CW_AGC,         M_CW,            UI_SLIDE_FMT_SETTING, "AGC"},
    {SET_RX_AGC,         M_SSB | M_AM,    UI_SLIDE_FMT_SETTING, "AGC"},
    {SET_CW_DECODE,      M_CW,            UI_SLIDE_FMT_SETTING, "DECODER"},
    {SET_CW_WPM,         M_CW,            UI_SLIDE_FMT_WPM,     "SPEED"},
    {SET_DRIVE,          M_VOICE | M_DIGI, UI_SLIDE_FMT_PCT100, "POWER"},
    {SET_DIGI_TONE,      M_DIGI,          UI_SLIDE_FMT_SETTING, "TONE TX"},
    {SET_USB_VOX,        M_DIGI,          UI_SLIDE_FMT_SETTING, "USB VOX"},
    {SET_CW_DRIVE,       M_CW,            UI_SLIDE_FMT_PCT100,  "POWER"},
    {UI_SLIDE_MEMORIES,  M_CW,            UI_SLIDE_FMT_PAGE,    "MEMORIES"},
    {SET_REV1_MIC_GAIN,  M_VOICE | M_MIC, UI_SLIDE_FMT_MIC_DB,  "MIC GAIN"},
    {SET_USB_GAIN,       M_VOICE | M_USBIN, UI_SLIDE_FMT_PCT100, "USB GAIN"},
    {SET_TX_BW,          M_SSB,           UI_SLIDE_FMT_SETTING, "TX WIDTH"},
    {SET_COMP,           M_SSB,           UI_SLIDE_FMT_SETTING, "COMPRESS"},
    {SET_AM_DEPTH,       M_AM,            UI_SLIDE_FMT_PCT100,  "AM DEPTH"},
    {SET_FM_PRESET,      M_FM,            UI_SLIDE_FMT_SETTING, "FM DEV"},
    {SET_MICAGC,         M_VOICE | M_MIC, UI_SLIDE_FMT_SETTING, "MIC AGC"},
    {SET_NG,             M_VOICE | M_MIC, UI_SLIDE_FMT_SETTING, "NOISE GATE"},
    {SET_EQ,             M_VOICE,         UI_SLIDE_FMT_SETTING, "TX EQ"},
    {SET_EQ_BASS,        M_VOICE,         UI_SLIDE_FMT_SETTING, "BASS DB"},
    {SET_EQ_TREBLE,      M_VOICE,         UI_SLIDE_FMT_SETTING, "TREBLE DB"},
    {SET_PROFILE,        M_VOICE,         UI_SLIDE_FMT_SETTING, "PROFILE"},
    {SET_CW_MODE,        M_CW,            UI_SLIDE_FMT_SETTING, "KEYER"},
    {SET_CW_SWAP,        M_CW,            UI_SLIDE_FMT_SETTING, "PADL SWAP"},
    {SET_CW_PRACTICE,    M_CW,            UI_SLIDE_FMT_SETTING, "PRACTICE"},
    {SET_TRAIN_OPEN,     M_CW,            UI_SLIDE_FMT_ACTION,  "TRAINER"},
    {SET_REV1_MIC,       M_VOICE,         UI_SLIDE_FMT_SETTING, "TX INPUT"},
    {SET_MUTE,           M_ALL,           UI_SLIDE_FMT_SETTING, "TX MUTE"},
    {SET_UI_STEP,        M_ALL,           UI_SLIDE_FMT_SETTING, "STEP"},
    {SET_REV1_USB_IQ,    M_ALL,           UI_SLIDE_FMT_SETTING, "USB I/Q"},
    {SET_REV1_BACKLIGHT, M_ALL,           UI_SLIDE_FMT_PCT,     "BACKLIGHT"},
    {SET_SAVE,           M_ALL,           UI_SLIDE_FMT_ACTION,  "SETTINGS"},
};
#define SLIDE_COUNT (sizeof(slides) / sizeof(slides[0]))

static const slide_t *slide_find(uint8_t id)
{
    for (uint8_t i = 0U; i < SLIDE_COUNT; ++i)
        if (slides[i].id == id) return &slides[i];
    return NULL;
}

uint8_t UI_SlideItems(ui_umode_t mode, uint8_t usb_source, uint8_t out[UI_SLIDE_MAX])
{
    const uint8_t bit = (uint8_t)(1U << ((uint8_t)mode % (uint8_t)UI_UMODE_COUNT));
    const uint8_t source = usb_source ? M_USBIN : M_MIC;
    uint8_t n = 0U;
    for (uint8_t i = 0U; (i < SLIDE_COUNT) && (n < UI_SLIDE_MAX); ++i) {
        const uint8_t when = slides[i].when;
        if ((when & bit) == 0U) continue;
        if ((when & (M_MIC | M_USBIN)) && !(when & source)) continue;
        out[n++] = slides[i].id;
    }
    return n;
}

const char *UI_SlideCaption(uint8_t id)
{
    const slide_t *s = slide_find(id);
    return (s != NULL) ? s->caption : "";
}

ui_slide_fmt_t UI_SlideFormat(uint8_t id)
{
    const slide_t *s = slide_find(id);
    return (s != NULL) ? (ui_slide_fmt_t)s->fmt : UI_SLIDE_FMT_SETTING;
}

uint8_t UI_SlideFirst(uint8_t first, uint8_t focus, uint8_t count)
{
    if (count <= UI_SLIDE_VISIBLE) return 0U;
    if (focus < (uint8_t)(first + 1U)) first = focus ? (uint8_t)(focus - 1U) : 0U;
    if ((uint16_t)focus + 2U > (uint16_t)first + UI_SLIDE_VISIBLE) first = (uint8_t)(focus + 2U - UI_SLIDE_VISIBLE);
    if ((uint16_t)first + UI_SLIDE_VISIBLE > count) first = (uint8_t)(count - UI_SLIDE_VISIBLE);
    return first;
}

uint8_t UI_StepCycle(uint8_t digit)
{
    return ((digit >= 1U) && (digit < 5U)) ? (uint8_t)(digit + 1U) : 1U;
}
