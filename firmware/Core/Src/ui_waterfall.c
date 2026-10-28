#pragma GCC optimize ("Os") /* off the real-time paths, except the raster */

/* The WATERFALL page: layout, the colour raster behind the band, the
 * frequency scale and the settings list. The engine is radio_spectrum.c.
 *
 * Screen: title 0..15, band 16..207 (24 pages: the spectrum and/or the
 * waterfall, drawn by wf_raster in full colour), frequency scale 208..223,
 * footer 224..239. In CW with the decoder on, the band ends at 191 and the
 * decoded text takes 192..207. The band's own framebuffer bytes hold the engine's
 * history, trace and peak (GFX_BandClaim), so the page costs no RAM of its
 * own beyond this file's state. With the settings list open the list takes
 * rows 16..111 and a live band the rest, so a setting shows its effect at
 * once. History lines are as tall as the band's memory allows: 4 px on the
 * waterfall-only layout, 3 px beside the spectrum. */

#include "ui_waterfall.h"
#include "ui_state.h"
#include "ui_gfx.h"
#include "ui_format.h"
#include "radio_spectrum.h"
#include "radio.h"
#include "radio_audio.h"
#include "modulator.h"
#include <stddef.h>
#include <string.h>

#define BAND_PAGE     2U   /* row 16 */
#define BAND_PAGES    24U  /* to row 207 */
#define SETUP_ROWS    6U   /* list rows 16..111 */
#define SETUP_PAGE    (BAND_PAGE + SETUP_ROWS * 2U)
#define SCALE_Y       208U
#define CW_PAGES      2U   /* the decoded-CW line over the scale, rows 192..207 */
#define CW_Y          (SCALE_Y - CW_PAGES * 8U)
#define TRACE_BYTES   (SPEC_W * 3U) /* int16 trace + uint8 peak */
#define NODATA        INT16_MIN

/* Every colour here is kept byte-swapped: the panel takes big-endian RGB565,
 * so a swapped halfword stores as it is and two pixels go out in one word. */
#define BE(c)    ((uint16_t)((((c) & 0xFFU) << 8) | ((c) >> 8)))

/* 16 levels each, RGB565: CLASSIC (black, blue, cyan, green, yellow, red,
 * white), GRAY, HEAT (black, red, yellow, white), OCEAN. */
static const uint16_t palettes[SPEC_PALETTE_COUNT][16] = {
    {BE(0x0000), BE(0x0005), BE(0x000A), BE(0x0091), BE(0x0137), BE(0x01DE), BE(0x037E), BE(0x055C),
     BE(0x0673), BE(0x06C4), BE(0x5F40), BE(0xDFC0), BE(0xFE00), BE(0xFB80), BE(0xFD2E), BE(0xFFFF)},
    {BE(0x0000), BE(0x1082), BE(0x2104), BE(0x3186), BE(0x4228), BE(0x52AA), BE(0x632C), BE(0x73AE),
     BE(0x8C51), BE(0x9CD3), BE(0xAD55), BE(0xBDD7), BE(0xCE79), BE(0xDEFB), BE(0xEF7D), BE(0xFFFF)},
    {BE(0x0000), BE(0x1800), BE(0x3800), BE(0x5800), BE(0x7800), BE(0x9800), BE(0xB0C0), BE(0xC1E0),
     BE(0xDB00), BE(0xEC20), BE(0xFD40), BE(0xFE43), BE(0xFF26), BE(0xFFEA), BE(0xFFF5), BE(0xFFFF)},
    {BE(0x0002), BE(0x0824), BE(0x1047), BE(0x1869), BE(0x208C), BE(0x210E), BE(0x220F), BE(0x22F0),
     BE(0x1BD1), BE(0x2C90), BE(0x452E), BE(0x5DEC), BE(0x6E8B), BE(0x9EE9), BE(0xCF27), BE(0xFF85)},
};
#define C_BG     BE(0x0000U)
#define C_GRID   BE(0x2104U) /* 10 dB lines */
#define C_PASS   BE(0x0846U) /* the receive channel, behind the trace */
#define C_TRACE  BE(0xFFFFU)
#define C_PEAK   BE(0xFD20U)
#define C_DIAL   BE(0xF800U)
/* Trace row cache (wf.ycol): the row of each column's trace point plus one,
 * clamped to 0..rows+1 (above and below the area draw the same), or: */
#define Y_NONE   255U

static struct {
    uint8_t setup, cursor, first, editing, layout_key;
    uint8_t *mem;
    /* Latched at each frame build for the raster, which runs while the panel
     * is sent. Frames are only built between flushes (ui_lcd.c), so what the
     * raster reads cannot change under one. */
    uint16_t spec_rows, wf_rows;
    uint8_t line_h, markers, fill;
    uint8_t *ycol;               /* SPEC_W bytes of the band, with a spectrum */
    const uint8_t *peak;
    const uint16_t *pal;
    int16_t dial_x, pass_lo, pass_hi;
    int32_t top8, span8, k16;
    uint32_t look;
    char flash[20];
} wf; /* zeroed: layout_key is set by UI_WaterfallEnter before any layout() */

/* ---- Raster ---------------------------------------------------------------- */

/* Hot: every band row sent to the panel, ~200 of them a frame, so built for
 * speed with everything it calls inlined into wf_raster. The rest of the
 * file stays -Os. */
#pragma GCC push_options
#pragma GCC optimize ("O2")

static inline __attribute__((always_inline)) void put(uint8_t *o, unsigned x, uint16_t c)
{
    memcpy(&o[2U * x], &c, 2U);
}

/* Row of the spectrum area where dBFS x 8 `v` sits (may be off the area). */
static inline __attribute__((always_inline)) int32_t spec_y(int32_t v) { return ((wf.top8 - v) * wf.k16) >> 16; }

static inline __attribute__((always_inline)) void spectrum_row(uint16_t r, uint8_t *o)
{
    const uint8_t *yc = wf.ycol, *pk = wf.peak;
    const int32_t rows = wf.spec_rows, row = (int32_t)r;
    /* dB at this row and the next: a 10 dB grid line where they straddle one. */
    const int32_t db = wf.top8 - ((2 * row + 1) * wf.span8) / (2 * rows);
    const int32_t db_next = wf.top8 - ((2 * row + 3) * wf.span8) / (2 * rows);
    const uint8_t grid = (uint8_t)(((db + 1600) / 80) != ((db_next + 1600) / 80));
    const int32_t lv = ((db - (wf.top8 - wf.span8)) * 16) / wf.span8;
    const uint16_t fill = wf.fill ? wf.pal[lv < 0 ? 0 : (lv > 15 ? 15 : lv)] : C_BG;
    const uint16_t bg = grid ? C_GRID : C_BG;
    const uint16_t pass = (wf.markers >= SPEC_MARK_PASS && !grid) ? C_PASS : bg;
    /* One unsigned compare for the channel: x - lo <= hi - lo. */
    const int32_t plo = wf.pass_lo, phi = wf.pass_hi;
    const uint32_t pwidth = (phi >= plo) ? (uint32_t)(phi - plo) : 0U;
    const uint8_t has_pass = (uint8_t)(phi >= plo);
    /* The trace joins each point to the one before it: rows lo..hi of the
     * column are the line, those under the point the fill. Rows are +1. */
    int32_t prev = (yc[0] != Y_NONE) ? yc[0] : 0x7FFF;
    const int32_t rr = row + 1;
    for (unsigned x = 0U; x < SPEC_W; ++x) {
        uint16_t c = (has_pass && (uint32_t)((int32_t)x - plo) <= pwidth) ? pass : bg;
        const int32_t y = yc[x];
        if (y != (int32_t)Y_NONE) {
            const int32_t lo = y < prev ? y : prev, hi = y < prev ? prev : y;
            if (rr >= lo && rr <= hi) c = C_TRACE;
            else if (rr > y) c = fill;
            prev = y;
        }
        if (pk && pk[x] && row == spec_y(((int32_t)pk[x] - 160) * 8)) c = C_PEAK;
        put(o, x, c);
    }
    if (wf.markers && wf.dial_x >= 0 && wf.dial_x < (int16_t)SPEC_W) put(o, (unsigned)wf.dial_x, C_DIAL);
}

static inline __attribute__((always_inline)) void waterfall_row(uint16_t r, uint8_t *o)
{
    const uint8_t *line = SPEC_HistoryLine((uint8_t)(r / wf.line_h));
    if (line == NULL) {
        memset(o, 0, SPEC_W * 2U);
    } else {
        /* Two pixels a byte, one word a byte. */
        const uint16_t *pal = wf.pal;
        for (unsigned i = 0U; i < SPEC_W / 2U; ++i) {
            const unsigned b = line[i];
            const uint32_t two = (uint32_t)pal[b & 15U] | ((uint32_t)pal[b >> 4] << 16);
            memcpy(&o[4U * i], &two, 4U);
        }
    }
    /* The dial as a dotted line, so the signal under it stays readable. */
    if (wf.markers && wf.dial_x >= 0 && wf.dial_x < (int16_t)SPEC_W && (r & 3U) < 2U) put(o, (unsigned)wf.dial_x, C_DIAL);
}

static void wf_raster(uint16_t row, uint8_t *out)
{
    memset(out, 0, SPEC_X0 * 2U);
    memset(out + (SPEC_X0 + SPEC_W) * 2U, 0, (GFX_W - SPEC_X0 - SPEC_W) * 2U);
    uint8_t *o = out + SPEC_X0 * 2U;
    if (row < wf.spec_rows) spectrum_row(row, o);
    else if ((uint16_t)(row - wf.spec_rows) < (uint16_t)(wf.wf_rows / wf.line_h * wf.line_h)) waterfall_row((uint16_t)(row - wf.spec_rows), o);
    else memset(o, 0, SPEC_W * 2U);
}

#pragma GCC pop_options

/* Once a frame, before the raster: each column's trace row (see Y_NONE), so
 * the raster's inner loop has no multiply and no NODATA test on int16s. */
static void latch_trace(void)
{
    if (!wf.ycol) return;
    const int16_t *t = SPEC_Trace();
    const int32_t top = (int32_t)wf.spec_rows + 1;
    for (unsigned x = 0U; x < SPEC_W; ++x) {
        if (!t || t[x] == NODATA) { wf.ycol[x] = (uint8_t)Y_NONE; continue; }
        int32_t y = spec_y(t[x]) + 1;
        wf.ycol[x] = (uint8_t)(y < 0 ? 0 : (y > top ? top : y));
    }
}

/* ---- Layout ------------------------------------------------------------------ */

/* Claim the band for the layout and hand its memory to the engine: trace and
 * peak first when there is a spectrum (and the raster's trace rows after
 * them, the page's own), then the history at the shortest line height whose
 * ring still fits. */
static void layout(void)
{
    const spec_config_t *c = SPEC_Config();
    const uint8_t page = wf.setup ? SETUP_PAGE : BAND_PAGE;
    /* In CW with the decoder on, the band's last two pages are its line. */
    const uint8_t cw = UI_CwDecodeShown();
    const uint8_t pages = (uint8_t)(BAND_PAGE + BAND_PAGES - page - (cw ? CW_PAGES : 0U));
    const uint8_t key = (uint8_t)(c->layout | (wf.setup << 4) | (cw << 5));
    uint8_t fresh = 0U;
    uint8_t *mem = GFX_BandClaim(page, pages, wf_raster, &fresh);
    if (mem == NULL || (!fresh && key == wf.layout_key && mem == wf.mem)) return;
    wf.layout_key = key;
    wf.mem = mem;
    const uint16_t rows = (uint16_t)(pages * 8U);
    const uint32_t bytes = (uint32_t)pages * GFX_W;
    uint16_t spec = 0U;
    uint32_t extra = 0U;
    if (c->layout == SPEC_LAYOUT_SPECTRUM) { spec = rows; extra = TRACE_BYTES + SPEC_W; }
    else if (c->layout == SPEC_LAYOUT_BOTH) { spec = (uint16_t)((rows * 3U / 8U) & ~7U); extra = TRACE_BYTES + SPEC_W; }
    const uint16_t wf_rows = (uint16_t)(rows - spec);
    uint8_t h = 2U, slots = 0U;
    for (; wf_rows && h <= 8U; ++h) {
        slots = (uint8_t)(wf_rows / h + 1U);
        if ((uint32_t)slots * SPEC_LINE_BYTES + extra <= bytes) break;
    }
    if (h > 8U) slots = 0U;
    wf.spec_rows = spec;
    wf.wf_rows = slots ? wf_rows : 0U;
    wf.line_h = h;
    /* Band memory: trace (int16), peak, the raster's trace rows, history. */
    wf.ycol = extra ? mem + TRACE_BYTES : NULL;
    if (wf.ycol) memset(wf.ycol, Y_NONE, SPEC_W);
    /* With no history to show (SPECTRUM), two lines all the same, never
     * drawn: the newest is what lets S-CTRL's SPECTRUM frames go. */
    SPEC_Attach(mem + extra, slots ? slots : 2U, extra ? (int16_t *)(void *)mem : NULL,
                extra ? mem + SPEC_W * 2U : NULL);
}

/* ---- Settings ------------------------------------------------------------------ */

enum { F_NAMES = 0, F_SPAN, F_RATE, F_AVG, F_DB, F_DBFS, F_RANGE, F_LISTEN, F_SCANAVG, F_ACTION };
/* Which rows mean something now: always, VIEW, SCAN, raw captures, AUTO,
 * FIXED, with a spectrum. The rest are shown dimmed and still edit. */
enum { W_ALL = 0, W_VIEW, W_SCAN, W_RAW, W_AUTO, W_FIXED, W_TRACE };

typedef struct {
    const char *label;
    uint8_t off, fmt, when;
    int8_t min, max, step;
    const char *names;
} wf_row_t;

#define OFF(f) ((uint8_t)offsetof(spec_config_t, f))
static const wf_row_t rows_def[] = {
    {"MODE",       OFF(mode),         F_NAMES,   W_ALL,   0, 1, 1, "VIEW|SCAN (EXP)"},
    {"SPAN",       OFF(span),         F_SPAN,    W_VIEW,  0, 3, 1, NULL},
    {"LAYOUT",     OFF(layout),       F_NAMES,   W_ALL,   0, 2, 1, "WATERFALL|SPLIT|SPECTRUM"},
    {"SPEED",      OFF(rate),         F_RATE,    W_VIEW,  0, 5, 1, NULL},
    {"AVERAGE",    OFF(average),      F_AVG,     W_VIEW,  0, 4, 1, NULL},
    {"WINDOW",     OFF(window),       F_NAMES,   W_ALL,   0, 3, 1, "HANN|BLACKMAN-H|FLAT TOP|RECT"},
    {"DETECTOR",   OFF(detector),     F_NAMES,   W_ALL,   0, 1, 1, "AVERAGE|PEAK"},
    {"SCALE",      OFF(scale),        F_NAMES,   W_ALL,   0, 1, 1, "AUTO|FIXED"},
    {"FLOOR",      OFF(floor_offset), F_DB,      W_AUTO,  SPEC_FLOOR_MIN, SPEC_FLOOR_MAX, 1, NULL},
    {"REF LEVEL",  OFF(ref_dbfs),     F_DBFS,    W_FIXED, SPEC_REF_MIN, SPEC_REF_MAX, 5, NULL},
    {"RANGE",      OFF(range),        F_RANGE,   W_ALL,   0, 6, 1, NULL},
    {"PALETTE",    OFF(palette),      F_NAMES,   W_ALL,   0, 3, 1, "CLASSIC|GRAY|HEAT|OCEAN"},
    {"FILL",       OFF(fill),         F_NAMES,   W_TRACE, 0, 1, 1, "OFF|ON"},
    {"SMOOTH",     OFF(smooth),       F_NAMES,   W_TRACE, 0, 3, 1, "OFF|LOW|MID|HIGH"},
    {"PEAK HOLD",  OFF(peak_hold),    F_NAMES,   W_TRACE, 0, 3, 1, "OFF|FAST|SLOW|HOLD"},
    {"MARKERS",    OFF(markers),      F_NAMES,   W_ALL,   0, 2, 1, "OFF|DIAL|DIAL+PASS"},
    {"DC NOTCH",   OFF(dc_notch),     F_NAMES,   W_RAW,   0, 1, 1, "OFF|ON"},
    {"IQ CORRECT", OFF(iq_correct),   F_NAMES,   W_RAW,   0, 1, 1, "OFF|ON"},
    {"SCAN RANGE", OFF(scan_range),   F_NAMES,   W_SCAN,  0, 5, 1, "BAND|250 KHZ|500 KHZ|1 MHZ|2 MHZ|FILTER"},
    {"SCAN AUDIO", OFF(scan_audio),   F_NAMES,   W_SCAN,  0, 2, 1, "LISTEN|CONTINUOUS|SINGLE"},
    {"LISTEN",     OFF(listen),       F_LISTEN,  W_SCAN,  0, 5, 1, NULL},
    {"SCAN AVG",   OFF(scan_avg),     F_SCANAVG, W_SCAN,  0, 2, 1, NULL},
    {"DEFAULTS",   0U,                F_ACTION,  W_ALL,   0, 0, 0, NULL},
};
#define ROW_COUNT ((uint8_t)(sizeof(rows_def) / sizeof(rows_def[0])))

/* The row that edits config field `off` (by field, so reordering the list
 * cannot point the zoom or the title at the wrong one). */
static const wf_row_t *row_of(uint8_t off)
{
    for (uint8_t i = 0U; i < ROW_COUNT; ++i)
        if (rows_def[i].off == off && rows_def[i].fmt != F_ACTION) return &rows_def[i];
    return &rows_def[0];
}

static int32_t row_get(const wf_row_t *r)
{
    const uint8_t *p = (const uint8_t *)SPEC_Config() + r->off;
    return (r->min < 0) ? (int32_t)(int8_t)*p : (int32_t)*p;
}

static void row_set(const wf_row_t *r, int32_t v)
{
    uint8_t *p = (uint8_t *)SPEC_Config() + r->off;
    *p = (r->min < 0) ? (uint8_t)(int8_t)v : (uint8_t)v;
}

static uint8_t row_applies(const wf_row_t *r)
{
    const spec_config_t *c = SPEC_Config();
    switch (r->when) {
        case W_VIEW:  return (uint8_t)(c->mode == SPEC_MODE_VIEW);
        case W_SCAN:  return (uint8_t)(c->mode == SPEC_MODE_SCAN);
        case W_RAW:   return (uint8_t)(c->mode == SPEC_MODE_SCAN || c->span == SPEC_SPAN_WIDE);
        case W_AUTO:  return (uint8_t)(c->scale == SPEC_SCALE_AUTO);
        case W_FIXED: return (uint8_t)(c->scale == SPEC_SCALE_FIXED);
        case W_TRACE: return (uint8_t)(c->layout != SPEC_LAYOUT_WF);
        default:      return 1U;
    }
}

/* The n-th of '|'-separated names. */
static void pick_name(const char *names, int32_t n, char *dst)
{
    while (n-- > 0 && names) { names = strchr(names, '|'); if (names) ++names; }
    if (!names) { dst[0] = 0; return; }
    const char *end = strchr(names, '|');
    const size_t len = end ? (size_t)(end - names) : strlen(names);
    memcpy(dst, names, len);
    dst[len] = 0;
}

static void span_text(uint8_t span, char *dst)
{
    if (span == SPEC_SPAN_WIDE) {
        /* The flat part of the ADC's band: 304 bins of fS / 512. */
        strcpy(UI_AppendU(strcpy(dst, "WIDE ") + 5, (uint32_t)(RADIO_AudioRate() * 304.0f / 512.0f / 1000.0f + 0.5f), 1U), " KHZ");
        return;
    }
    static const char *const zoom[] = {"", "38 KHZ", "19 KHZ", "9.5 KHZ"};
    strcpy(dst, zoom[span < SPEC_SPAN_COUNT ? span : 1U]);
}

static void row_value(const wf_row_t *r, char *dst)
{
    const int32_t v = row_get(r);
    const unsigned u = (unsigned)(v < 0 ? 0 : v);
    dst[0] = 0;
    switch (r->fmt) {
        case F_SPAN:    span_text((uint8_t)u, dst); break;
        case F_RATE:    strcpy(UI_AppendU(dst, spec_rates[u % SPEC_RATE_COUNT], 1U), " LINES/S"); break;
        case F_AVG:     strcpy(UI_AppendU(dst, spec_averages[u % SPEC_AVG_COUNT], 1U), " FFT"); break;
        case F_DB:      strcpy(UI_AppendI(dst, v), " DB"); break; /* UI_AppendI signs it */
        case F_DBFS:    strcpy(UI_AppendI(dst, v), " DBFS"); break;
        case F_RANGE:   strcpy(UI_AppendU(dst, spec_ranges[u % SPEC_RANGE_COUNT], 1U), " DB"); break;
        case F_LISTEN:  strcpy(UI_AppendF(dst, (float)spec_listens[u % SPEC_LISTEN_COUNT] * 0.001f,
                                          spec_listens[u % SPEC_LISTEN_COUNT] < 1000U ? 1U : 0U), " S"); break;
        case F_SCANAVG: strcpy(UI_AppendU(dst, spec_scan_avgs[u % SPEC_SCAN_AVG_COUNT], 1U), " FFT"); break;
        case F_ACTION:  strcpy(dst, "RESET"); break;
        case F_NAMES:
        default:        pick_name(r->names, v, dst); break;
    }
}

uint8_t UI_WaterfallSetupOpen(void) { return wf.setup; }

void UI_WaterfallSetupShow(void)
{
    wf.setup = 1U;
    wf.editing = 0U;
}

static void setup_move(int32_t d)
{
    const wf_row_t *r = &rows_def[wf.cursor];
    if (!wf.editing) {
        int32_t c = (int32_t)wf.cursor + d;
        if (c < 0) c = 0;
        if (c >= (int32_t)ROW_COUNT) c = (int32_t)ROW_COUNT - 1;
        wf.cursor = (uint8_t)c;
        return;
    }
    int32_t v = row_get(r) + d * r->step;
    if (v < r->min) v = r->min;
    if (v > r->max) v = r->max;
    if (v == row_get(r)) return;
    row_set(r, v);
    SPEC_ConfigChanged();
}

void UI_WaterfallSetupEvents(void)
{
    const int32_t move = (int32_t)ui_ev.turn + (int32_t)ui_ev.turn_held + (int32_t)ui_ev.dn - (int32_t)ui_ev.up;
    if (ui_ev.hold || ui_ev.sel_long) {
        if (wf.editing) wf.editing = 0U;
        else wf.setup = 0U;
        return;
    }
    if (ui_ev.click || ui_ev.sel) {
        if (rows_def[wf.cursor].fmt == F_ACTION) {
            SPEC_ConfigDefaults();
            UI_NavFlash("DEFAULTS RESTORED");
        } else {
            wf.editing ^= 1U;
        }
        return;
    }
    if (move) setup_move(move);
}

void UI_WaterfallZoom(int8_t detents)
{
    spec_config_t *c = SPEC_Config();
    uint8_t *field = (c->mode == SPEC_MODE_SCAN) ? &c->scan_range : &c->span;
    const int32_t top = (c->mode == SPEC_MODE_SCAN) ? (int32_t)SPEC_SCAN_RANGE_COUNT - 1 : (int32_t)SPEC_SPAN_COUNT - 1;
    int32_t v = (int32_t)*field + detents;
    if (v < 0) v = 0;
    if (v > top) v = top;
    if ((uint8_t)v == *field) return;
    *field = (uint8_t)v;
    SPEC_ConfigChanged();
    const wf_row_t *r = row_of((c->mode == SPEC_MODE_SCAN) ? OFF(scan_range) : OFF(span));
    strcpy(wf.flash, r->label);
    strcat(wf.flash, " ");
    row_value(r, wf.flash + strlen(wf.flash));
    UI_NavFlash(wf.flash);
}

void UI_WaterfallSummary(char *dst)
{
    const spec_config_t *c = SPEC_Config();
    if (c->mode == SPEC_MODE_SCAN) { strcpy(dst, "SCAN"); return; }
    static const char *const s[SPEC_SPAN_COUNT] = {"WIDE", "38K", "19K", "9.5K"};
    strcpy(dst, s[c->span < SPEC_SPAN_COUNT ? c->span : 0U]);
}

/* ---- Page ------------------------------------------------------------------------ */

void UI_WaterfallEnter(void)
{
    wf.setup = 0U;
    wf.editing = 0U;
    wf.layout_key = 0xFFU;
    wf.mem = NULL;
    SPEC_Open();
}

void UI_WaterfallExit(void)
{
    SPEC_Close();
    GFX_BandRelease();
    wf.mem = NULL;
    wf.layout_key = 0xFFU;
}

uint8_t UI_WaterfallService(uint32_t now)
{
    /* Something else took the band (the fault screen): the engine must stop
     * writing into what is now plain framebuffer. */
    if (wf.mem != NULL && !GFX_BandHeld(wf_raster)) {
        SPEC_Detach();
        wf.mem = NULL;
        wf.layout_key = 0xFFU;
    }
    return SPEC_Service(now);
}

/* Ticks every `step` Hz across the band with their MHz.kHz labels, the step
 * the smallest nice one that keeps labels 48 px apart. */
static void draw_scale(const spec_view_t *v)
{
    static const uint32_t steps[] = {1000U, 2000U, 2500U, 5000U, 10000U, 20000U, 25000U, 50000U,
                                     100000U, 200000U, 250000U, 500000U, 1000000U};
    if (!v->valid || !(v->hz_per_px > 0.0f)) return;
    uint32_t step = steps[sizeof(steps) / sizeof(steps[0]) - 1U];
    for (unsigned i = 0U; i < sizeof(steps) / sizeof(steps[0]); ++i)
        if ((float)steps[i] / v->hz_per_px >= 48.0f) { step = steps[i]; break; }
    const float half = v->hz_per_px * (float)SPEC_CENTER;
    const uint32_t left = (uint32_t)((float)v->centre_hz - half), right = (uint32_t)((float)v->centre_hz + half);
    for (uint32_t f = (left / step + 1U) * step; f < right; f += step) {
        const int32_t x = (int32_t)SPEC_X0 + (int32_t)SPEC_CENTER + (int32_t)(((float)(int32_t)(f - v->centre_hz)) / v->hz_per_px);
        if (x < (int32_t)SPEC_X0 || x >= (int32_t)(SPEC_X0 + SPEC_W)) continue;
        GFX_VLine((uint16_t)x, SCALE_Y, 3U, GFX_DIM);
        char label[12];
        char *e = UI_AppendU(label, f / 1000000U, 1U);
        *e++ = '.';
        (void)UI_AppendU(e, (f % 1000000U) / 1000U, 3U);
        const uint16_t w = GFX_TextWidth(GFX_FONT_SMALL, label);
        int32_t lx = x - (int32_t)w / 2;
        if (lx < (int32_t)GFX_MARGIN) lx = GFX_MARGIN;
        if (lx + w > (int32_t)(GFX_W - GFX_MARGIN)) lx = (int32_t)(GFX_W - GFX_MARGIN - w);
        (void)GFX_Text((uint16_t)lx, (uint16_t)(SCALE_Y + 3U), GFX_FONT_SMALL, GFX_DIM, label, GFX_W - GFX_MARGIN);
    }
}

static void draw_setup(void)
{
    const uint8_t first = UI_ListFirst(&wf.first, wf.cursor, ROW_COUNT, SETUP_ROWS);
    for (uint8_t i = 0U; i < SETUP_ROWS && (uint8_t)(first + i) < ROW_COUNT; ++i) {
        const uint8_t n = (uint8_t)(first + i);
        const wf_row_t *r = &rows_def[n];
        char value[24];
        row_value(r, value);
        ui_row_state_t st = row_applies(r) ? UI_ROW_NORMAL : UI_ROW_DIM;
        if (n == wf.cursor) st = wf.editing ? UI_ROW_EDIT : UI_ROW_FOCUS;
        UI_DrawRow(0U, GFX_W, (uint16_t)(UI_LIST_Y + i * UI_ROW_H), st, r->label, value);
    }
    UI_DrawScrollbar((uint16_t)(GFX_W - 4U), UI_LIST_Y, (uint16_t)(SETUP_ROWS * UI_ROW_H), ROW_COUNT, first, SETUP_ROWS);
}

/* Title: the dial and mode, and what the band shows. */
static void draw_title(const spec_view_t *v)
{
    char left[24], right[28];
    (void)UI_FormatFreq(v->dial_hz, left, sizeof(left));
    strcat(left, " ");
    strcat(left, UI_UModeLabel(UI_ModeCurrent()));
    const spec_config_t *c = SPEC_Config();
    if (wf.setup) {
        UI_DrawTitle("WATERFALL SETUP", NULL);
        return;
    }
    if (c->mode == SPEC_MODE_SCAN) {
        strcpy(right, "SCAN ");
        if (v->scanning) strcpy(UI_AppendU(right + 5, v->sweep_pct, 1U), "%");
        else { char span[12]; pick_name(row_of(OFF(scan_range))->names, c->scan_range, span); strcat(right, span); }
    } else {
        span_text(c->span, right);
    }
    UI_DrawTitle(left, right);
}

static void draw_footer(const spec_view_t *v, uint32_t now)
{
    char right[20];
    right[0] = 0;
    const radio_state_t rs = RADIO_State();
    if (v->guard) strcpy(right, "TX HELD");
    else if ((rs == RADIO_TRANSMIT) || (rs == RADIO_TX_PREPARE)) strcpy(right, "TX"); /* the engine rests */
    else if (!v->receiving) strcpy(right, "NO RX");
    else strcpy(UI_AppendU(strcpy(right, "CPU ") + 4, (uint32_t)v->load_rx + v->load_wf, 1U), "%");
    const char *left;
    if (wf.setup) left = wf.editing ? GFX_SYM_TURN " CHANGE  " GFX_SYM_PRESS " DONE" : GFX_SYM_TURN " PICK  " GFX_SYM_PRESS " EDIT  HOLD CLOSE";
    else if (SPEC_Config()->mode == SPEC_MODE_SCAN) left = GFX_SYM_PRESS " SETUP  SEL SWEEP  HOLD BACK";
    else left = GFX_SYM_PRESS " SETUP  " GFX_SYM_PRESS GFX_SYM_TURN " ZOOM  HOLD BACK";
    UI_DrawFooter(left, right, now);
    if (v->guard) GFX_Ink((uint16_t)(GFX_W - GFX_MARGIN - GFX_TextWidth(GFX_FONT_SMALL, right)), UI_FOOTER_Y,
                          GFX_TextWidth(GFX_FONT_SMALL, right), 12U, GFX_ALERT);
}

/* In CW with the decoder on: its newest words over the scale, the tail of
 * the teleprinter's two lines from a word's start. */
static void draw_cw(void)
{
    const char *older, *newer;
    UI_CwTeletype(&older, &newer);
    char text[2U * UI_CW_LINE_CHARS + 2U];
    strcpy(text, older);
    if (older[0] && newer[0]) strcat(text, " ");
    strcat(text, newer);
    const char *t = text;
    const size_t n = strlen(text);
    if (n > UI_CW_LINE_CHARS) {
        t = text + n - UI_CW_LINE_CHARS;
        if (t[-1] != ' ') {
            const char *space = strchr(t, ' ');
            if (space) t = space + 1;
        }
    }
    (void)GFX_Text(GFX_MARGIN, CW_Y, GFX_FONT_BODY, GFX_WHITE, t, (uint16_t)(GFX_W - GFX_MARGIN));
}

void UI_WaterfallRender(uint32_t now)
{
    spec_view_t v;
    SPEC_GetView(&v);
    layout();
    const spec_config_t *c = SPEC_Config();
    /* What the raster needs, fixed for this frame. */
    wf.pal = palettes[c->palette < SPEC_PALETTE_COUNT ? c->palette : 0U];
    wf.markers = c->markers;
    wf.fill = c->fill;
    wf.dial_x = v.dial_x;
    wf.pass_lo = v.pass_lo_x;
    wf.pass_hi = v.pass_hi_x;
    wf.top8 = (int32_t)v.top_db * 8;
    wf.span8 = (int32_t)(v.top_db - v.bottom_db) * 8;
    if (wf.span8 < 80) wf.span8 = 80;
    wf.k16 = wf.spec_rows ? (int32_t)(((uint32_t)wf.spec_rows << 16) / (uint32_t)wf.span8) : 0;
    wf.peak = SPEC_Peak();
    SPEC_Latch();
    latch_trace();
    /* The band goes to the panel again only when what it shows changed. */
    const uint32_t look = SPEC_Version() ^ ((uint32_t)wf.dial_x << 8) ^ ((uint32_t)c->palette << 24) ^
                          ((uint32_t)wf.markers << 28) ^ ((uint32_t)wf.fill << 30) ^
                          ((uint32_t)(uint16_t)wf.pass_lo << 12) ^ ((uint32_t)(uint16_t)wf.pass_hi << 18) ^
                          (uint32_t)wf.top8 * 2654435761U;
    if (look != wf.look) { wf.look = look; GFX_BandTouch(); }
    draw_title(&v);
    if (wf.setup) draw_setup();
    if (UI_CwDecodeShown()) draw_cw();
    draw_scale(&v);
    draw_footer(&v, now);
}
