#pragma GCC optimize ("Os")

/* Landscape frame builder: the shared widgets, the home screen and the
 * pipeline-fault screen. Layout lives on the 8-pixel tile grid of ui_gfx.c:
 * each 8x8 tile has one foreground and one background colour, so elements
 * with different colours never share a tile. */

#include "ui_state.h"
#include "ui_gfx.h"
#include "ui_sd.h"
#include "ui_format.h"
#include "main.h"
#include <string.h>

static modulator_status_t frame_status;

const modulator_status_t *UI_FrameStatus(void) { return &frame_status; }

/* ---- Widgets -------------------------------------------------------------- */

void UI_DrawTitle(const char *title, const char *right)
{
    uint16_t limit = (uint16_t)(GFX_W - GFX_MARGIN);
    GFX_Fill(0U, 0U, GFX_W, UI_TITLE_H, GFX_PANEL_HI);
    if ((right != NULL) && (right[0] != 0)) {
        const uint16_t w = GFX_TextWidth(GFX_FONT_SMALL, right);
        (void)GFX_TextRight(limit, 2U, GFX_FONT_SMALL, GFX_DIM, right);
        limit = (uint16_t)(((limit - w) & ~7U) - 8U);
    }
    (void)GFX_Text(GFX_MARGIN, 0U, GFX_FONT_BODY, GFX_WHITE, title, limit);
}

static void draw_footer(const char *left, const char *right, const char *flash)
{
    uint16_t limit = (uint16_t)(GFX_W - GFX_MARGIN);
    GFX_Fill(0U, UI_FOOTER_Y, GFX_W, (uint16_t)(GFX_H - UI_FOOTER_Y), GFX_PANEL);
    if ((flash == NULL) && (right != NULL) && (right[0] != 0)) {
        const uint16_t w = GFX_TextWidth(GFX_FONT_SMALL, right);
        (void)GFX_TextRight(limit, (uint16_t)(UI_FOOTER_Y + 2U), GFX_FONT_SMALL, GFX_MUTED, right);
        limit = (uint16_t)(((limit - w) & ~7U) - 8U);
    }
    (void)GFX_Text(GFX_MARGIN, (uint16_t)(UI_FOOTER_Y + 2U), GFX_FONT_SMALL,
                   flash ? GFX_ACCENT : GFX_DIM, flash ? flash : left, limit);
}

void UI_DrawFooter(const char *left, const char *right, uint32_t now)
{
    const uint8_t live = (uint8_t)((ui.flash_text != NULL) && ((int32_t)(now - ui.flash_until) < 0));
    draw_footer(left, right, live ? ui.flash_text : NULL);
}

void UI_DrawRow(uint16_t x, uint16_t w, uint16_t y, ui_row_state_t state,
                const char *label, const char *value)
{
    static const uint8_t bg[5] = {GFX_BG, GFX_SELECT, GFX_PANEL_HI, GFX_SELECT, GFX_BG};
    static const uint8_t fg[5] = {GFX_TEXT, GFX_WHITE, GFX_TEXT, GFX_WHITE, GFX_MUTED};
    static const uint8_t vc[5] = {GFX_DIM, GFX_ACCENT, GFX_DIM, GFX_WHITE, GFX_MUTED};
    const uint16_t right = (uint16_t)(x + w - 8U);
    uint16_t limit = right;
    if (bg[state] != GFX_BG) GFX_Fill(x, y, w, UI_ROW_H, bg[state]);
    if ((state == UI_ROW_FOCUS) || (state == UI_ROW_EDIT)) GFX_Rect(x, (uint16_t)(y + 2U), 3U, 12U, GFX_ACCENT);
    if ((value != NULL) && (value[0] != 0)) {
        uint16_t vx = (uint16_t)(right - GFX_TextWidth(GFX_FONT_BODY, value));
        if (state == UI_ROW_EDIT) {
            /* < value > with the arrows in their own tiles */
            vx = (uint16_t)(vx - 16U);
            (void)GFX_Text((uint16_t)(right - 8U), y, GFX_FONT_BODY, GFX_ACCENT, GFX_SYM_RIGHT, right);
            (void)GFX_Text((uint16_t)(vx - 16U), y, GFX_FONT_BODY, GFX_ACCENT, GFX_SYM_LEFT, (uint16_t)(vx - 8U));
            (void)GFX_Text(vx, y, GFX_FONT_BODY, vc[state], value, (uint16_t)(right - 16U));
            vx = (uint16_t)(vx - 16U);
        } else {
            (void)GFX_Text(vx, y, GFX_FONT_BODY, vc[state], value, right);
        }
        limit = (uint16_t)((vx & ~7U) - 8U);
    }
    (void)GFX_Text((uint16_t)(x + 8U), y, GFX_FONT_BODY, fg[state], label, limit);
}

void UI_DrawSection(uint16_t x, uint16_t w, uint16_t y, const char *title)
{
    const uint16_t end = GFX_Text((uint16_t)(x + 8U), (uint16_t)(y + 3U), GFX_FONT_SMALL, GFX_ACCENT, title,
                                  (uint16_t)(x + w - 8U));
    const uint16_t lx = (uint16_t)((end + 15U) & ~7U);
    if (lx + 8U < x + w) GFX_HLine(lx, (uint16_t)(y + 9U), (uint16_t)(x + w - 8U - lx), GFX_LINE);
}

uint8_t UI_ListFirst(uint8_t *first, uint8_t cursor, uint8_t total, uint8_t visible)
{
    uint8_t f = *first;
    if (total <= visible) {
        f = 0U;
    } else {
        /* Keep one row of context above and below the cursor. */
        if (cursor < (uint8_t)(f + 1U)) f = cursor ? (uint8_t)(cursor - 1U) : 0U;
        if ((uint16_t)cursor + 2U > (uint16_t)f + visible) f = (uint8_t)(cursor + 2U - visible);
        if ((uint16_t)f + visible > total) f = (uint8_t)(total - visible);
    }
    *first = f;
    return f;
}

void UI_DrawScrollbar(uint16_t x, uint16_t y, uint16_t h, uint8_t total, uint8_t first, uint8_t visible)
{
    if ((total <= visible) || (h < 8U)) return;
    for (uint16_t yy = y; yy < y + h; yy = (uint16_t)(yy + 2U)) GFX_Pixels((uint16_t)(x + 1U), yy, 1U, 1U);
    uint16_t th = (uint16_t)((uint32_t)h * visible / total);
    if (th < 8U) th = 8U;
    const uint16_t ty = (uint16_t)(y + (uint32_t)(h - th) * first / (uint32_t)(total - visible));
    GFX_Pixels(x, ty, 3U, th);
    GFX_Ink(x, y, 3U, h, GFX_DIM);
}

void UI_DrawList(const char *const *labels, uint8_t total, uint8_t cursor, uint8_t visible)
{
    const uint8_t first = UI_ListFirst(&ui.list_first, cursor, total, visible);
    for (uint8_t i = 0U; (i < visible) && ((uint8_t)(first + i) < total); ++i) {
        const uint8_t index = (uint8_t)(first + i);
        UI_DrawRow(0U, (uint16_t)(GFX_W - 8U), (uint16_t)(UI_LIST_Y + i * UI_ROW_H),
                   (index == cursor) ? UI_ROW_FOCUS : UI_ROW_NORMAL, labels[index], NULL);
    }
    UI_DrawScrollbar((uint16_t)(GFX_W - 6U), UI_LIST_Y, (uint16_t)(visible * UI_ROW_H), total, first, visible);
}

uint8_t UI_DrawWrapped(uint16_t x, uint16_t y, uint16_t w, uint8_t max_lines,
                       uint8_t color, const char *text)
{
    char line[64];
    const uint8_t cols = (uint8_t)((w / 6U < sizeof(line) - 1U) ? (w / 6U) : (sizeof(line) - 1U));
    uint8_t lines = 0U;
    while ((text != NULL) && (*text != 0) && (lines < max_lines)) {
        uint8_t n = 0U, brk = 0U;
        while ((text[n] != 0) && (text[n] != '|') && (n < cols)) {
            if (text[n] == ' ') brk = n;
            n++;
        }
        if ((text[n] != 0) && (text[n] != '|') && (text[n] != ' ') && (brk != 0U)) n = brk;
        memcpy(line, text, n);
        line[n] = 0;
        (void)GFX_Text(x, (uint16_t)(y + lines * 13U), GFX_FONT_SMALL, color, line, (uint16_t)(x + w));
        lines++;
        text += n;
        while ((*text == ' ') || (*text == '|')) text++;
    }
    return lines;
}

void UI_DrawPair(uint16_t x, uint16_t y, const char *key, const char *value, uint8_t color)
{
    const uint16_t end = GFX_Text(x, y, GFX_FONT_SMALL, GFX_MUTED, key, (uint16_t)(GFX_W - GFX_MARGIN));
    (void)GFX_Text((uint16_t)((end + 13U) & ~7U), y, GFX_FONT_SMALL, color, value, (uint16_t)(GFX_W - GFX_MARGIN));
}

void UI_DrawBar(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t part, uint32_t whole, uint8_t color)
{
    if ((w < 4U) || (h < 3U)) return;
    GFX_Frame(x, y, w, h, color);
    if (whole == 0U) return;
    if (part > whole) part = whole;
    const uint16_t fill = (uint16_t)(((uint32_t)(w - 4U) * part) / whole);
    if (fill > 0U) GFX_Rect((uint16_t)(x + 2U), (uint16_t)(y + 2U), fill, (uint16_t)(h - 4U), color);
}

/* ---- Home ------------------------------------------------------------------ */
/* Three zones on the tile grid, each in its own tile rows:
 *   0..15    status band
 *   16..79   frequency (digits 20..53, step mark 57..59) and meter (64..75)
 *   88..127  settings row: caption, value, level bar
 *   136..199 diagnostics: two rows of three cells
 *   208..223 message line, 224..239 footer
 * Dividers at y 83, 131 and 203 sit in tile rows of their own. */

#define HOME_FREQ_RIGHT  276U
#define HOME_FREQ_Y      20U
#define HOME_METER_Y     64U
#define HOME_SLIDE_Y     88U
#define HOME_DIAG_Y      136U
#define HOME_MSG_Y       208U
#define HOME_SEGMENTS    26U
#define SLIDE_X0         16U
#define SLIDE_PITCH      72U
#define SLIDE_W          64U

/* Small-font badge ending at `right`; returns where the next one ends. */
static uint16_t badge_left(uint16_t right, uint8_t color, const char *text)
{
    const uint16_t w = (uint16_t)((GFX_TextWidth(GFX_FONT_SMALL, text) + 10U + 7U) & ~7U);
    const uint16_t x = (uint16_t)((right - w) & ~7U);
    (void)GFX_Badge(x, 1U, GFX_FONT_SMALL, color, text);
    return (uint16_t)(x - 8U);
}

static void home_status(const ui_home_view_t *v)
{
    static const char *const state_label[4] = {"RX", "TX", "WAIT", "OFF"};
    static const uint8_t state_color[4] = {GFX_OK, GFX_ALERT, GFX_WARN, GFX_MUTED};
    const uint8_t txs = (v->txs < 4U) ? v->txs : (uint8_t)UI_TXS_OFF;
    GFX_Fill(0U, 0U, GFX_W, UI_TITLE_H, (txs == UI_TXS_TX) ? GFX_ALERT_BG : GFX_PANEL_HI);
    uint16_t x = GFX_Badge(GFX_MARGIN, 1U, GFX_FONT_SMALL, state_color[txs], state_label[txs]);
    x = GFX_Text((uint16_t)(x + 8U), 0U, GFX_FONT_BODY, GFX_WHITE, v->band, 160U);
    x = (uint16_t)((x + 15U) & ~7U);
    (void)GFX_Text(x, 0U, GFX_FONT_BODY, GFX_ACCENT, UI_UModeLabel((ui_umode_t)v->umode), 200U);

    /* Right side, right to left. */
    uint16_t r = (uint16_t)(GFX_W - GFX_MARGIN);
    if (v->tx_muted) r = badge_left(r, GFX_WARN, "TX MUTE");
    if (v->usb_audio) r = badge_left(r, GFX_ACCENT, "USB");
    if (v->pc_link) r = badge_left(r, (v->pc_link > 1U) ? GFX_ALERT : GFX_ACCENT, (v->pc_link > 1U) ? "PC TX" : "PC");
    if (v->rx_mismatch) {
        char text[12] = "RX ";
        strcat(text, UI_RxModeLabel(v->rx_mode));
        (void)badge_left(r, GFX_WARN, text);
    }
}

/* CW keying holds the display service off for as long as the keyer needs
 * the CPU (MOD_AudioRealtimeCritical), which left the screen saying RX
 * through every over. The badge alone is redrawn in place: "RX" and "TX"
 * make badges of one width, so the new one covers the old. */
void UI_RenderStateCue(uint8_t txs)
{
    const uint8_t tx = (uint8_t)(txs == (uint8_t)UI_TXS_TX);
    (void)GFX_Badge(GFX_MARGIN, 1U, GFX_FONT_SMALL, tx ? GFX_ALERT : GFX_OK, tx ? "TX" : "RX");
}

/* Left edge of digit `d` (0 = 1 Hz) of a frequency right-aligned at `right`. */
static uint16_t home_digit_x(uint16_t right, uint8_t d)
{
    const uint16_t dw = GFX_CharWidth(GFX_FONT_FREQ);
    uint16_t dots = 0U;
    if (d >= 3U) dots = (uint16_t)(dots + 12U);
    if (d >= 6U) dots = (uint16_t)(dots + 12U);
    return (uint16_t)(right - (uint16_t)(d + 1U) * dw - dots);
}

static void home_frequency(const ui_home_view_t *v)
{
    char f[16];
    (void)UI_FormatFreq(v->freq_hz, f, sizeof(f));
    const uint16_t w = GFX_TextWidth(GFX_FONT_FREQ, f);
    (void)GFX_Text((uint16_t)(HOME_FREQ_RIGHT - w), HOME_FREQ_Y, GFX_FONT_FREQ, GFX_FREQ, f, HOME_FREQ_RIGHT);
    (void)GFX_Text(284U, 40U, GFX_FONT_SMALL, GFX_DIM, "MHz", (uint16_t)(GFX_W - GFX_MARGIN));
    const uint8_t colour = v->tune_hold ? GFX_WARN : GFX_ACCENT;
    if (v->step_digit == UI_TUNE_STEP_500) {
        /* 500 Hz: under the separator left of the 100 Hz digit (12 px). */
        GFX_Rect((uint16_t)(home_digit_x(HOME_FREQ_RIGHT, 2U) - 10U), 57U, 8U, 3U, colour);
    } else {
        const uint8_t digit = (uint8_t)((v->step_digit > UI_TUNE_STEP_500) ? v->step_digit - 1U : v->step_digit);
        const uint16_t ux = home_digit_x(HOME_FREQ_RIGHT, digit);
        GFX_Rect((uint16_t)(ux + 3U), 57U, (uint16_t)(GFX_CharWidth(GFX_FONT_FREQ) - 6U), 3U, colour);
    }
}

/* ---- Live power and SWR ----------------------------------------------- */

/* Watts and SWR for the big digits (GFX_FONT_FREQ: digits and '.'). */
static void fmt_meter(char *t, float v)
{
    t[0] = 0;
    if (!(v > 0.0f)) v = 0.0f;
    if (v > 99.9f) v = 99.9f;
    (void)UI_AppendF(t, v, (v < 9.995f) ? 2U : 1U);
}

/* `n` segments of 6 x 6 pixels, one per tile column, the first `lit` lit. */
static void meter_segments(uint16_t x, uint16_t y, uint8_t n, uint8_t lit, uint8_t warn_from,
                           uint8_t alert_from)
{
    for (uint8_t i = 0U; i < n; ++i) {
        uint8_t color = GFX_PANEL_HI;
        if (i < lit) color = (i >= alert_from) ? GFX_ALERT : ((i >= warn_from) ? GFX_WARN : GFX_OK);
        GFX_Rect((uint16_t)(x + i * 8U), y, 6U, 6U, color);
    }
}

static uint8_t meter_lit(float v, float per_segment, uint8_t n)
{
    if (!(v > 0.0f)) return 0U;
    const float lit = v / per_segment + 0.5f;
    return (lit >= (float)n) ? n : (uint8_t)lit;
}

/* The TX panel: the diagnostics zone (y 136-199, tile columns 1-38) while
 * transmitting and for METER HANG MS after. Big forward PEP and SWR, a power
 * bar (0.5 W a segment, 0-9 W) and an SWR bar (0.2 a segment from 1.0,
 * QMX's scale), reflected power, the supply as it sags under the key-down,
 * and the PA drain. */
#define TXP_Y   136U
#define TXP_W   (GFX_W - 2U * GFX_MARGIN)
void UI_RenderTxPanel(const ui_tx_meter_t *m)
{
    char t[16];
    GFX_ClearPixels(GFX_MARGIN, TXP_Y, TXP_W, 64U);
    GFX_Fill(GFX_MARGIN, TXP_Y, TXP_W, 64U, GFX_BG);
    const uint16_t right = (uint16_t)(GFX_W - GFX_MARGIN);
    if (m->valid) {
        fmt_meter(t, m->pep_w);
        uint16_t x = GFX_Text(GFX_MARGIN, TXP_Y, GFX_FONT_FREQ, GFX_FREQ, t, 152U);
        x = (uint16_t)((x + 7U) & ~7U);
        (void)GFX_Text(x, (uint16_t)(TXP_Y + 24U), GFX_FONT_BODY, GFX_DIM, "W", 160U);
    } else {
        (void)GFX_Text(GFX_MARGIN, (uint16_t)(TXP_Y + 24U), GFX_FONT_BODY, GFX_MUTED, "-- W", 152U);
    }
    const uint8_t swr_color = !m->swr_valid ? GFX_MUTED : (m->swr_alert ? GFX_ALERT : (m->swr >= 2.0f ? GFX_WARN : GFX_OK));
    uint16_t label = 176U;
    if (m->valid && m->swr_valid) {
        fmt_meter(t, m->swr);
        const uint16_t w = GFX_TextWidth(GFX_FONT_FREQ, t);
        (void)GFX_TextRight(right, TXP_Y, GFX_FONT_FREQ, swr_color, t);
        label = (uint16_t)((((uint16_t)(right - w)) & ~7U) - 32U);
    } else {
        (void)GFX_TextRight(right, (uint16_t)(TXP_Y + 24U), GFX_FONT_BODY, GFX_MUTED, "--");
    }
    (void)GFX_Text(label, (uint16_t)(TXP_Y + 24U), GFX_FONT_BODY, GFX_DIM, "SWR", (uint16_t)(label + 24U));
    const uint16_t bars = (uint16_t)(TXP_Y + 41U);
    meter_segments(GFX_MARGIN, bars, 18U, m->valid ? meter_lit(m->pep_w, 0.5f, 18U) : 0U, 18U, 18U);
    const uint8_t swr_lit = (m->valid && m->swr_valid) ? meter_lit(m->swr - 1.0f, 0.2f, 18U) : 0U;
    meter_segments(168U, bars, 18U, swr_lit, 5U, m->swr_alert ? 0U : 18U);
    const uint16_t y = (uint16_t)(TXP_Y + 52U);
    strcpy(t, "REF ");
    if (m->valid && m->swr_valid) strcat(UI_AppendF(t + strlen(t), m->ref_w, 2U), " W");
    else strcat(t, "-");
    (void)GFX_Text(GFX_MARGIN, y, GFX_FONT_SMALL, GFX_DIM, t, 112U);
    strcpy(t, "VIN ");
    if (m->valid) strcat(UI_AppendF(t + strlen(t), m->vin_v, 2U), " V");
    else strcat(t, "-");
    (void)GFX_TextCenter(112U, 96U, y, GFX_FONT_SMALL, GFX_DIM, t);
    strcpy(t, "DRAIN ");
    if (m->valid) strcat(UI_AppendF(t + strlen(t), m->drain_v, 1U), " V");
    else strcat(t, "-");
    (void)GFX_TextRight(right, y, GFX_FONT_SMALL, GFX_DIM, t);
}

/* The home meter row in TX: forward power instead of the microphone level
 * (which stays on the DIAG transmit page), 0.35 W a segment. */
void UI_RenderTxMeterRow(const ui_tx_meter_t *m, uint8_t clip)
{
    char text[16];
    const uint16_t y = HOME_METER_Y;
    GFX_ClearPixels(GFX_MARGIN, y, (uint16_t)(GFX_W - 2U * GFX_MARGIN), 16U);
    GFX_Fill(GFX_MARGIN, y, (uint16_t)(GFX_W - 2U * GFX_MARGIN), 16U, GFX_BG);
    (void)GFX_Text(GFX_MARGIN, y, GFX_FONT_SMALL, GFX_DIM, "PWR", 32U);
    const uint8_t lit = m->valid ? meter_lit(m->pep_w, 0.35f, HOME_SEGMENTS) : 0U;
    for (uint8_t i = 0U; i < HOME_SEGMENTS; ++i)
        GFX_Rect((uint16_t)(32U + i * 8U), (uint16_t)(y + 1U), 6U, 10U, (i < lit) ? GFX_OK : GFX_PANEL_HI);
    const uint16_t tr = (uint16_t)(GFX_W - GFX_MARGIN);
    if (clip) {
        (void)GFX_Badge((uint16_t)(tr - 40U), y, GFX_FONT_SMALL, GFX_ALERT, "CLIP");
        return;
    }
    text[0] = 0;
    if (m->valid) strcat(UI_AppendF(text, m->pep_w, 2U), " W");
    else strcpy(text, "-- W");
    (void)GFX_TextRight(tr, y, GFX_FONT_SMALL, GFX_TEXT, text);
}

static void home_meter(const ui_home_view_t *v)
{
    char text[24];
    uint8_t lit, peak = 255U;
    const uint16_t y = HOME_METER_Y;
    if (v->meter_tx && v->tx_panel) {
        UI_RenderTxMeterRow(&v->txm, v->clip);
        return;
    }
    if (v->meter_tx) {
        lit = (uint8_t)((uint32_t)v->tx_rms * HOME_SEGMENTS / 100U);
        peak = (uint8_t)((uint32_t)v->tx_peak * HOME_SEGMENTS / 100U);
        text[0] = 0;
        strcat(UI_AppendU(text, v->tx_peak, 1U), "% PK");
    } else {
        int32_t level = (int32_t)v->rx_dbfs + 130;
        if (level < 0) level = 0;
        lit = (uint8_t)(((uint32_t)level * HOME_SEGMENTS) / 120U);
        text[0] = 0;
        strcat(UI_AppendI(text, v->rx_dbfs), " dBFS");
    }
    if (lit > HOME_SEGMENTS) lit = HOME_SEGMENTS;
    (void)GFX_Text(GFX_MARGIN, y, GFX_FONT_SMALL, GFX_DIM, v->meter_tx ? "TX" : "S", 24U);
    for (uint8_t i = 0U; i < HOME_SEGMENTS; ++i) {
        uint8_t color = GFX_PANEL_HI;
        if (i == peak) color = GFX_WHITE;
        else if (i < lit) color = (i < 16U) ? GFX_OK : ((i < 21U) ? GFX_WARN : GFX_ALERT);
        GFX_Rect((uint16_t)(32U + i * 8U), (uint16_t)(y + 1U), 6U, 10U, color);
    }
    const uint16_t tr = (uint16_t)(GFX_W - GFX_MARGIN);
    if (v->clip) {
        (void)GFX_Badge((uint16_t)(tr - 40U), y, GFX_FONT_SMALL, GFX_ALERT, "CLIP");
    } else {
        (void)GFX_TextRight(tr, y, GFX_FONT_SMALL, GFX_TEXT, text);
    }
}

/* One cell of the settings row: caption, value, and an eight-segment level
 * for numbers. Editing inverts the cell. */
static void home_slide_cell(const ui_home_view_t *v, uint8_t i)
{
    const uint16_t x = (uint16_t)(SLIDE_X0 + i * SLIDE_PITCH);
    const uint8_t index = (uint8_t)(v->slide_first + i);
    if (index >= v->slide_count) return;
    const uint8_t focus = (uint8_t)(index == v->slide_focus);
    const uint8_t edit = (uint8_t)(focus && v->editing);
    uint8_t cap = GFX_DIM, val = GFX_TEXT, lit = GFX_DIM, unlit = GFX_PANEL_HI;
    if (edit) {
        GFX_Fill(x, HOME_SLIDE_Y, SLIDE_W, 40U, GFX_ACCENT);
        cap = val = lit = GFX_BG;
        unlit = GFX_SELECT;
    } else if (focus) {
        GFX_Fill(x, HOME_SLIDE_Y, SLIDE_W, 40U, GFX_SELECT);
        cap = GFX_ACCENT;
        val = GFX_WHITE;
        lit = GFX_ACCENT;
        unlit = GFX_LINE;
    }
    (void)GFX_TextCenter(x, SLIDE_W, (uint16_t)(HOME_SLIDE_Y + 2U), GFX_FONT_SMALL, cap, UI_SlideCaption(v->slide_id[i]));
    (void)GFX_TextCenter(x, SLIDE_W, (uint16_t)(HOME_SLIDE_Y + 16U), GFX_FONT_BODY, val, v->slide_value[i]);
    if (v->slide_level[i] > 100U) return;
    for (uint8_t k = 0U; k < 8U; ++k) {
        const uint8_t on = (uint8_t)((v->slide_level[i] > 0U) && ((uint16_t)v->slide_level[i] * 8U > k * 100U));
        GFX_Rect((uint16_t)(x + k * 8U + 1U), (uint16_t)(HOME_SLIDE_Y + 34U), 6U, 3U, on ? lit : unlit);
    }
}

static void home_slide(const ui_home_view_t *v)
{
    for (uint8_t i = 0U; i < UI_SLIDE_VISIBLE; ++i) home_slide_cell(v, i);
    /* More to either side. */
    const uint8_t arrow = (v->slide_focus != 255U) ? GFX_ACCENT : GFX_MUTED;
    if (v->slide_first > 0U)
        (void)GFX_Text(GFX_MARGIN, (uint16_t)(HOME_SLIDE_Y + 16U), GFX_FONT_BODY, arrow, GFX_SYM_LEFT, 16U);
    if ((uint16_t)v->slide_first + UI_SLIDE_VISIBLE < v->slide_count)
        (void)GFX_Text(304U, (uint16_t)(HOME_SLIDE_Y + 16U), GFX_FONT_BODY, arrow, GFX_SYM_RIGHT, 312U);
}

/* Zero-beat, where the zone's name was: the station's tone against the
 * pitch, a pixel per Hz to +-40 either side of a centre tick -- right of it
 * the station is above the dial, so tune up -- and within +-10 Hz a ZERO
 * badge on the tick instead. */
static void home_cw_tune(int16_t hz, uint16_t y)
{
    const uint16_t cx = GFX_MARGIN + 44U;
    if (hz >= -10 && hz <= 10) {
        (void)GFX_Badge((uint16_t)(cx - 20U), (uint16_t)(y - 1U), GFX_FONT_SMALL, GFX_OK, "ZERO");
        return;
    }
    const int16_t d = (hz < -40) ? -40 : ((hz > 40) ? 40 : hz);
    GFX_Rect((uint16_t)(cx - 40U), (uint16_t)(y + 6U), 81U, 1U, GFX_LINE);
    GFX_Rect(cx, (uint16_t)(y + 1U), 1U, 11U, GFX_DIM);
    GFX_Rect((uint16_t)(cx + d - 2), (uint16_t)(y + 2U), 5U, 9U, GFX_ACCENT);
}

/* In CW with the decoder on the zone is the decoder's: a status line --
 * the tuning bar while locked, what it hears, any warning the hidden cells
 * had -- over the teleprinter's two lines, the newer one white with a
 * cursor. */
static void home_cw_decode(const ui_home_view_t *v)
{
    const uint16_t y = HOME_DIAG_Y, right = (uint16_t)(GFX_W - GFX_MARGIN);
    if (v->cw_state == UI_CW_LOCKED) home_cw_tune(v->cw_offset_hz, (uint16_t)(y + 2U));
    else (void)GFX_Text(GFX_MARGIN, (uint16_t)(y + 2U), GFX_FONT_SMALL, GFX_MUTED, "CW DECODE", right);
    char status[24];
    uint8_t color = GFX_MUTED;
    if (v->cw_state == UI_CW_LOCKED) {
        /* Speed, and how far off the pitch: tune it to 0 to zero-beat. */
        char *e = UI_AppendU(status, v->cw_wpm, 1U);
        strcpy(e, " WPM  ");
        strcpy(UI_AppendI(e + 6, v->cw_offset_hz), " HZ"); /* signed, +0 too */
        color = GFX_ACCENT;
    } else {
        strcpy(status, (v->cw_state == UI_CW_LISTENING) ? "LISTENING" : "PAUSED");
    }
    (void)GFX_TextRight(right, (uint16_t)(y + 2U), GFX_FONT_SMALL, color, status);
    if (v->cw_alert) {
        const ui_diag_cell_t *c = &v->diag[v->cw_alert - 1U];
        (void)GFX_TextCenter(0U, GFX_W, (uint16_t)(y + 2U), GFX_FONT_SMALL, UI_ToneColor(c->tone), c->value);
    }
    (void)GFX_Text(GFX_MARGIN, (uint16_t)(y + 20U), GFX_FONT_BODY, GFX_TEXT, v->cw_line[0], right);
    const uint16_t end = GFX_Text(GFX_MARGIN, (uint16_t)(y + 40U), GFX_FONT_BODY, GFX_WHITE, v->cw_line[1], right);
    /* Where the next character lands, while one can. */
    if ((v->cw_state != UI_CW_PAUSED) && (end + 7U <= right)) GFX_Rect(end, (uint16_t)(y + 55U), 7U, 2U, GFX_ACCENT);
}

static void home_diag(const ui_home_view_t *v)
{
    if (v->cw_decode) { home_cw_decode(v); return; }
    for (uint8_t i = 0U; i < UI_DIAG_SUMMARY; ++i) {
        const ui_diag_cell_t *c = &v->diag[i];
        const uint16_t x = (uint16_t)(GFX_MARGIN + (i % 3U) * 104U);
        const uint16_t y = (uint16_t)(HOME_DIAG_Y + (i / 3U) * 32U);
        (void)GFX_Text(x, (uint16_t)(y + 2U), GFX_FONT_SMALL, GFX_MUTED, c->label, (uint16_t)(x + 96U));
        (void)GFX_Text(x, (uint16_t)(y + 16U), GFX_FONT_BODY, UI_ToneColor(c->tone), c->value, (uint16_t)(x + 96U));
    }
}

/* Dividers, and the picker's bracket around the zone it would enter. */
static void home_frame(const ui_home_view_t *v)
{
    static const uint8_t top[UI_ZONE_COUNT] = {16U, 88U, 136U};
    static const uint8_t bottom[UI_ZONE_COUNT] = {80U, 128U, 200U};
    static const uint8_t line_y[3] = {83U, 131U, 203U};
    const uint8_t pick = (uint8_t)(v->home_level == UI_HOME_PICK);
    for (uint8_t i = 0U; i < 3U; ++i) {
        const uint8_t lit = (uint8_t)(pick && ((v->zone == i) || (v->zone == (uint8_t)(i + 1U))));
        if (lit) GFX_HLine(0U, line_y[i], GFX_W, GFX_ACCENT);
        else GFX_HLine(GFX_MARGIN, line_y[i], (uint16_t)(GFX_W - 2U * GFX_MARGIN), GFX_LINE);
    }
    if (!pick || (v->zone >= UI_ZONE_COUNT)) return;
    const uint16_t y0 = top[v->zone], h = (uint16_t)(bottom[v->zone] - top[v->zone]);
    GFX_Rect(0U, y0, 3U, h, GFX_ACCENT);
    GFX_Rect((uint16_t)(GFX_W - 3U), y0, 3U, h, GFX_ACCENT);
}

static void home_message(const ui_home_view_t *v)
{
    const char *text = v->message;
    uint8_t color = GFX_ACCENT;
    if ((text == NULL) || (text[0] == 0)) {
        text = v->edit_name; /* the full name of what the knob is changing */
        if (text == NULL) return;
    } else if (v->message_level == UI_MSG_ALERT) {
        GFX_Fill(0U, HOME_MSG_Y, GFX_W, 16U, GFX_ALERT_BG);
        color = GFX_WHITE;
    } else if (v->message_level == UI_MSG_WARN) {
        color = GFX_WARN;
    }
    (void)GFX_TextCenter(0U, GFX_W, HOME_MSG_Y, GFX_FONT_BODY, color, text);
}

static void home_footer(const ui_home_view_t *v)
{
    static const char *const steps[UI_TUNE_DIGITS] = {"1 Hz", "10 Hz", "100 Hz", "500 Hz", "1 kHz", "10 kHz", "100 kHz", "1 MHz"};
    char right[16];
    const char *left;
    right[0] = 0;
    switch ((ui_home_level_t)v->home_level) {
        case UI_HOME_PICK:
            left = GFX_SYM_TURN " ZONE  " GFX_SYM_PRESS " ENTER  HOLD MENU";
            if (v->has_fkey) strcpy(right, "FN BACK");
            break;
        case UI_HOME_SLIDE:
            if (v->editing) {
                left = GFX_SYM_TURN " ADJUST  " GFX_SYM_PRESS " SET  HOLD CANCEL";
            } else {
                left = GFX_SYM_TURN " SLIDE  " GFX_SYM_PRESS " EDIT  HOLD BACK";
                strcat(UI_AppendU(right, (uint32_t)v->slide_focus + 1U, 1U), "/");
                (void)UI_AppendU(right + strlen(right), v->slide_count, 1U);
            }
            break;
        case UI_HOME_TUNE:
        default:
            left = GFX_SYM_TURN " TUNE  " GFX_SYM_PRESS " STEP  HOLD ZONES";
            strcpy(right, "STEP ");
            strcat(right, steps[(v->step_digit < UI_TUNE_DIGITS) ? v->step_digit : UI_TUNE_DIGIT_DEF]);
            break;
    }
    draw_footer(v->show_hints ? left : "", right, NULL);
}

void UI_RenderHome(const ui_home_view_t *v)
{
    home_status(v);
    home_frequency(v);
    if (v->show_meter) home_meter(v);
    home_slide(v);
    if (v->tx_panel) UI_RenderTxPanel(&v->txm);
    else home_diag(v);
    home_frame(v);
    home_message(v);
    home_footer(v);
}

/* ---- Pipeline fault ------------------------------------------------------- */

void UI_RenderFault(const modulator_status_t *st)
{
    const uint8_t hard = (uint8_t)(st->pipeline.hard_lockout != 0U);
    char line[48];
    GFX_BandRelease(); /* the fault takes the whole screen, the WATERFALL band too */
    GFX_Fill(0U, 0U, GFX_W, 48U, hard ? GFX_ALERT_BG : GFX_PANEL_HI);
    (void)GFX_Badge(GFX_MARGIN, 4U, GFX_FONT_BODY, hard ? GFX_ALERT : GFX_WARN, hard ? "LOCKOUT" : "NOTICE");
    /* Only a hard event inhibits RF. The soft notice is always a concealed
     * USB audio gap (the only CONCEALED reasons), and TX carried on. */
    (void)GFX_Text(GFX_MARGIN, 26U, GFX_FONT_BODY, GFX_WHITE,
                   hard ? "RF PIPELINE FAULT - RF INHIBITED" : "USB AUDIO GAP - TX CONTINUED",
                   (uint16_t)(GFX_W - GFX_MARGIN));
    UI_DrawPair(GFX_MARGIN, 64U, "STAGE", MOD_PipelineStageName(st->pipeline.last_stage), GFX_TEXT);
    UI_DrawPair(GFX_MARGIN, 80U, "REASON", MOD_PipelineReasonName(st->pipeline.last_reason), GFX_TEXT);
    line[0] = 0;
    (void)UI_AppendU(line, st->pipeline.soft_fault_events, 1U);
    UI_DrawPair(GFX_MARGIN, 96U, "EVENTS", line, GFX_TEXT);
    line[0] = 0;
    (void)UI_AppendU(line, st->pipeline.last_value, 1U);
    UI_DrawPair(GFX_MARGIN, 112U, "VALUE", line, GFX_TEXT);
    GFX_Fill(0U, 144U, GFX_W, 48U, GFX_PANEL);
    if (st->pipeline.rearm_pending) {
        (void)GFX_TextCenter(0U, GFX_W, 160U, GFX_FONT_BODY, GFX_WARN, "WAITING FOR SOURCE / BUFFERS");
    } else {
        (void)GFX_TextCenter(0U, GFX_W, 152U, GFX_FONT_BODY, GFX_WHITE, "HOLD THE KNOB OR SELECT 1 S");
        (void)GFX_TextCenter(0U, GFX_W, 168U, GFX_FONT_BODY, GFX_ACCENT,
                             hard ? "WITH THE KEY UP TO CLEAR AND REARM" : "TO ACKNOWLEDGE");
    }
    (void)GFX_TextCenter(0U, GFX_W, 204U, GFX_FONT_SMALL, GFX_MUTED, "THE FAULT STAYS UNTIL ACKNOWLEDGED OR REBOOT");
}

/* ---- Boot log --------------------------------------------------------------- */

void UI_DrawBootHeader(void)
{
    GFX_Clear(GFX_BG);
    GFX_Fill(0U, 0U, GFX_W, UI_TITLE_H, GFX_PANEL_HI);
    (void)GFX_Text(GFX_MARGIN, 0U, GFX_FONT_BODY, GFX_WHITE, "SPARTACUS REV1", 200U);
    const uint16_t w = GFX_TextWidth(GFX_FONT_SMALL, FW_VERSION_STRING);
    (void)GFX_TextRight((uint16_t)(GFX_W - GFX_MARGIN), 2U, GFX_FONT_SMALL, GFX_DIM, FW_VERSION_STRING);
    (void)GFX_TextRight((uint16_t)(((GFX_W - GFX_MARGIN - w) & ~7U) - 8U), 2U, GFX_FONT_SMALL, GFX_MUTED, "BOOT");
}

/* Two columns of twelve: status square, stage, detail in the status colour. */
void UI_DrawBootRow(uint8_t row, const char *name, uint8_t status, const char *detail)
{
    static const uint8_t colors[3] = {GFX_ALERT, GFX_OK, GFX_WARN};
    static const uint8_t detail_colors[3] = {GFX_ALERT, GFX_DIM, GFX_WARN};
    if (status > 2U) status = 0U;
    const uint16_t x = (uint16_t)(GFX_MARGIN + (row / 12U) * 160U);
    const uint16_t y = (uint16_t)(24U + (row % 12U) * 16U);
    const uint16_t x_end = (uint16_t)(x + 152U);
    GFX_Rect(x, (uint16_t)(y + 4U), 6U, 6U, colors[status]);
    uint16_t nx = GFX_Text((uint16_t)(x + 16U), (uint16_t)(y + 1U), GFX_FONT_SMALL, GFX_TEXT, name, x_end);
    if ((detail != NULL) && (detail[0] != 0)) {
        nx = (uint16_t)((nx + 6U + 7U) & ~7U); /* own tiles, so it takes its own colour */
        (void)GFX_Text(nx, (uint16_t)(y + 1U), GFX_FONT_SMALL, detail_colors[status], detail, x_end);
    }
}

/* ---- Frame ------------------------------------------------------------------ */

void UI_AppBuildFrame(void *ctx, uint32_t now)
{
    MOD_CPU_DIAG_DECLARE(cpu_diag_start, MOD_CPU_DIAG_UI_BUILD);
    (void)ctx;
    MOD_GetStatus(&frame_status);
    GFX_Clear(GFX_BG);
    if (frame_status.pipeline.sticky_latched != 0U) {
        UI_RenderFault(&frame_status);
        MOD_CPU_DIAG_FINISH(MOD_CPU_DIAG_UI_BUILD, cpu_diag_start);
        return;
    }
    switch (ui.page) {
        case PAGE_HOME: {
            ui_home_view_t view;
            UI_HomeView(&view, &frame_status, now);
            UI_RenderHome(&view);
            break;
        }
        case PAGE_MENU:
            UI_RenderMenu(now);
            break;
        case PAGE_SD:
            UI_SDRender();
            break;
        default:
            UI_RenderPage(now);
            break;
    }
    MOD_CPU_DIAG_FINISH(MOD_CPU_DIAG_UI_BUILD, cpu_diag_start);
}
