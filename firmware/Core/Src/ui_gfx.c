#pragma GCC optimize ("Os")

#include "ui_gfx.h"
#include <string.h>

#include "ui_font_data.inc"

#define RGB(r, g, b) ((uint16_t)((((uint16_t)(r) & 0xF8U) << 8) | \
                                 (((uint16_t)(g) & 0xFCU) << 3) | \
                                 (((uint16_t)(b) & 0xF8U) >> 3)))

/* Phosphor green on true black: no grey anywhere. Structure (bands, lines,
 * captions) is drawn in dark saturated greens, text in near-white green, the
 * frequency in bright green and focus in cyan; amber and red are kept for
 * warnings and transmit only, so they stand out. */
static const uint16_t gfx_palette[GFX_COLORS] = {
    [GFX_BG]       = RGB(0, 0, 0),
    [GFX_PANEL]    = RGB(0, 28, 16),
    [GFX_PANEL_HI] = RGB(0, 52, 32),
    [GFX_LINE]     = RGB(0, 104, 56),
    [GFX_TEXT]     = RGB(224, 255, 232),
    [GFX_DIM]      = RGB(96, 224, 144),
    [GFX_MUTED]    = RGB(40, 160, 96),
    [GFX_ACCENT]   = RGB(0, 232, 224),
    [GFX_SELECT]   = RGB(0, 88, 88),
    [GFX_FREQ]     = RGB(72, 255, 120),
    [GFX_OK]       = RGB(48, 240, 96),
    [GFX_WARN]     = RGB(255, 196, 0),
    [GFX_ALERT]    = RGB(255, 64, 48),
    [GFX_ALERT_BG] = RGB(128, 0, 0),
    [GFX_OK_BG]    = RGB(0, 80, 32),
    [GFX_WHITE]    = RGB(255, 255, 255),
};

static uint8_t gfx_fb[GFX_W * GFX_PAGES] __attribute__((aligned(4)));
static uint8_t gfx_cut; /* glyphs clear pixels instead of setting them (badges) */
static uint8_t gfx_attr[GFX_TCOLS * GFX_PAGES] __attribute__((aligned(4)));
/* The colour band: pages [band_page, band_page + band_pages) belong to
 * band_fn while band_pages is nonzero. */
static uint8_t band_page, band_pages;
static gfx_band_fn_t band_fn;
static uint32_t band_version;

static inline uint8_t in_band(uint32_t page) { return (uint8_t)((page - band_page) < band_pages); }

const uint8_t *GFX_FrameBits(void) { return gfx_fb; }
const uint8_t *GFX_TileAttrs(void) { return gfx_attr; }
uint16_t GFX_Color565(uint8_t color) { return gfx_palette[color & 15U]; }
uint8_t GFX_Madctl(uint8_t rot270) { return rot270 ? 0xA0U : 0x60U; }

void GFX_Clear(uint8_t bg)
{
    const uint32_t first = (uint32_t)band_page * GFX_W, end = first + (uint32_t)band_pages * GFX_W;
    memset(gfx_fb, 0, band_pages ? first : sizeof(gfx_fb));
    if (band_pages) memset(&gfx_fb[end], 0, sizeof(gfx_fb) - end);
    memset(gfx_attr, (int)(((unsigned)GFX_TEXT << 4) | (bg & 15U)), sizeof(gfx_attr));
}

uint8_t *GFX_BandClaim(uint8_t page, uint8_t pages, gfx_band_fn_t fn, uint8_t *fresh)
{
    if (fresh) *fresh = 0U;
    if ((pages == 0U) || (fn == NULL) || ((uint32_t)page + pages > GFX_PAGES)) return NULL;
    if ((band_pages != pages) || (band_page != page) || (band_fn != fn)) {
        GFX_BandRelease();
        band_page = page;
        band_pages = pages;
        band_fn = fn;
        ++band_version;
        if (fresh) *fresh = 1U;
    }
    return &gfx_fb[(uint32_t)page * GFX_W];
}

void GFX_BandRelease(void)
{
    if (band_pages) memset(&gfx_fb[(uint32_t)band_page * GFX_W], 0, (uint32_t)band_pages * GFX_W);
    band_pages = 0U;
    band_fn = NULL;
    ++band_version;
}

void GFX_BandTouch(void) { ++band_version; }
uint8_t GFX_BandHeld(gfx_band_fn_t fn) { return (uint8_t)((band_pages != 0U) && (band_fn == fn)); }
void GFX_BandSpan(uint8_t *page, uint8_t *pages) { *page = band_page; *pages = band_pages; }

/* Tiles touched by a clipped rectangle; 0 when it is empty or off screen. */
static uint8_t tile_span(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         uint16_t *c0, uint16_t *c1, uint16_t *r0, uint16_t *r1)
{
    if ((w == 0U) || (h == 0U) || (x >= GFX_W) || (y >= GFX_H)) return 0U;
    uint32_t x1 = (uint32_t)x + w - 1U, y1 = (uint32_t)y + h - 1U;
    if (x1 >= GFX_W) x1 = GFX_W - 1U;
    if (y1 >= GFX_H) y1 = GFX_H - 1U;
    *c0 = (uint16_t)(x >> 3); *c1 = (uint16_t)(x1 >> 3);
    *r0 = (uint16_t)(y >> 3); *r1 = (uint16_t)(y1 >> 3);
    return 1U;
}

static void paint(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color, uint8_t fg)
{
    uint16_t c0, c1, r0, r1;
    if (!tile_span(x, y, w, h, &c0, &c1, &r0, &r1)) return;
    const uint8_t keep = fg ? 0x0FU : 0xF0U;
    const uint8_t set = fg ? (uint8_t)((color & 15U) << 4) : (uint8_t)(color & 15U);
    for (uint16_t r = r0; r <= r1; ++r) {
        uint8_t *a = &gfx_attr[r * GFX_TCOLS];
        for (uint16_t c = c0; c <= c1; ++c) a[c] = (uint8_t)((a[c] & keep) | set);
    }
}

void GFX_Fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t bg) { paint(x, y, w, h, bg, 0U); }
void GFX_Ink(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t fg) { paint(x, y, w, h, fg, 1U); }

static void pixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t on)
{
    if ((w == 0U) || (h == 0U) || (x >= GFX_W) || (y >= GFX_H)) return;
    uint32_t x1 = (uint32_t)x + w, y1 = (uint32_t)y + h;
    if (x1 > GFX_W) x1 = GFX_W;
    if (y1 > GFX_H) y1 = GFX_H;
    const uint16_t p0 = (uint16_t)(y >> 3), p1 = (uint16_t)((y1 - 1U) >> 3);
    for (uint16_t p = p0; p <= p1; ++p) {
        if (in_band(p)) continue;
        const unsigned first = (p == p0) ? (y & 7U) : 0U;
        const unsigned last = (p == p1) ? ((y1 - 1U) & 7U) : 7U;
        const uint8_t mask = (uint8_t)((0xFFU << first) & (0xFFU >> (7U - last)));
        uint8_t *dst = &gfx_fb[(uint32_t)p * GFX_W + x];
        for (uint32_t px = x; px < x1; ++px, ++dst) {
            if (on) *dst |= mask;
            else *dst &= (uint8_t)~mask;
        }
    }
}

void GFX_Pixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h) { pixels(x, y, w, h, 1U); }
void GFX_ClearPixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h) { pixels(x, y, w, h, 0U); }

void GFX_Rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t fg)
{
    pixels(x, y, w, h, 1U);
    paint(x, y, w, h, fg, 1U);
}

void GFX_HLine(uint16_t x, uint16_t y, uint16_t w, uint8_t fg) { GFX_Rect(x, y, w, 1U, fg); }
void GFX_VLine(uint16_t x, uint16_t y, uint16_t h, uint8_t fg) { GFX_Rect(x, y, 1U, h, fg); }

void GFX_Frame(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t fg)
{
    if ((w < 2U) || (h < 2U)) { GFX_Rect(x, y, w, h, fg); return; }
    GFX_HLine(x, y, w, fg);
    GFX_HLine(x, (uint16_t)(y + h - 1U), w, fg);
    GFX_VLine(x, y, h, fg);
    GFX_VLine((uint16_t)(x + w - 1U), y, h, fg);
}

/* ---- Text ------------------------------------------------------------- */

/* Next glyph code from UTF-8: ASCII and the 0x01..0x08 symbols pass
 * through, Latin-1 letters fold to their base letter, anything else is '?'. */
static uint8_t next_code(const char **text)
{
    const uint8_t *p = (const uint8_t *)*text;
    uint32_t cp;
    uint8_t count;
    if (p[0] < 0x80U) { (*text)++; return p[0]; }
    if (((p[0] & 0xE0U) == 0xC0U) && ((p[1] & 0xC0U) == 0x80U)) {
        cp = ((uint32_t)(p[0] & 0x1FU) << 6) | (uint32_t)(p[1] & 0x3FU);
        count = 2U;
    } else if (((p[0] & 0xF0U) == 0xE0U) && ((p[1] & 0xC0U) == 0x80U) && ((p[2] & 0xC0U) == 0x80U)) {
        cp = 0x800U;
        count = 3U;
    } else if (((p[0] & 0xF8U) == 0xF0U) && ((p[1] & 0xC0U) == 0x80U) &&
               ((p[2] & 0xC0U) == 0x80U) && ((p[3] & 0xC0U) == 0x80U)) {
        cp = 0x10000U;
        count = 4U;
    } else {
        (*text)++;
        return '?';
    }
    *text += count;
    if (cp == 0x00B0U) return 0x05U; /* degree sign */
    static const char fold[65] = "AAAAAAACEEEEIIIIDNOOOOO?OUUUUYPSaaaaaaaceeeeiiiidnooooo?ouuuuypy";
    if ((cp >= 0x00C0U) && (cp <= 0x00FFU)) return (uint8_t)fold[cp - 0x00C0U];
    return '?';
}

static const uint8_t *text_glyph(gfx_font_t font, uint8_t code)
{
    uint32_t index;
    if ((code >= 0x20U) && (code <= 0x7EU)) index = (uint32_t)code - 0x20U;
    else if ((code >= 1U) && (code <= GFX_FONT_SYMBOLS)) index = 0x5FU + (uint32_t)code - 1U;
    else index = (uint32_t)'?' - 0x20U;
    return (font == GFX_FONT_SMALL) ? &gfx_font_small_bits[index * 12U]
                                    : &gfx_font_body_bits[index * 16U];
}

static int8_t freq_index(uint8_t code)
{
    for (uint8_t i = 0U; i < GFX_FREQ_GLYPHS; ++i)
        if ((uint8_t)gfx_freq_chars[i] == code) return (int8_t)i;
    return -1;
}

static uint8_t glyph_width(gfx_font_t font, uint8_t code)
{
    if (font == GFX_FONT_SMALL) return 6U;
    if (font == GFX_FONT_BODY) return 8U;
    int8_t i = freq_index(code);
    return (i < 0) ? (uint8_t)GFX_FREQ_DIGIT_W : gfx_freq_width[i];
}

uint8_t GFX_FontHeight(gfx_font_t font)
{
    return (font == GFX_FONT_SMALL) ? 12U : (font == GFX_FONT_BODY) ? 16U : (uint8_t)GFX_FREQ_HEIGHT;
}

uint8_t GFX_CharWidth(gfx_font_t font) { return glyph_width(font, '0'); }

uint16_t GFX_TextWidth(gfx_font_t font, const char *s)
{
    uint32_t w = 0U;
    while ((s != NULL) && (*s != 0)) w += glyph_width(font, next_code(&s));
    return (uint16_t)((w > 0xFFFFU) ? 0xFFFFU : w);
}

/* OR one column-major glyph into the framebuffer at any y. */
/* Hot: every glyph of every frame. */
__attribute__((optimize("O2")))
static void blit(uint16_t x, uint16_t y, const uint8_t *g, uint8_t w, uint8_t pages)
{
    const unsigned shift = y & 7U;
    const uint16_t page0 = (uint16_t)(y >> 3);
    for (uint8_t c = 0U; (c < w) && ((uint32_t)x + c < GFX_W); ++c) {
        for (uint8_t p = 0U; p < pages; ++p) {
            const uint8_t b = g[(uint32_t)p * w + c];
            if (b == 0U) continue;
            const uint32_t pg = (uint32_t)page0 + p;
            const uint8_t lo = (uint8_t)(b << shift), hi = (uint8_t)(b >> (8U - shift));
            if ((pg < GFX_PAGES) && !in_band(pg)) {
                uint8_t *d = &gfx_fb[pg * GFX_W + x + c];
                *d = gfx_cut ? (uint8_t)(*d & (uint8_t)~lo) : (uint8_t)(*d | lo);
            }
            if ((shift != 0U) && (pg + 1U < GFX_PAGES) && !in_band(pg + 1U)) {
                uint8_t *d = &gfx_fb[(pg + 1U) * GFX_W + x + c];
                *d = gfx_cut ? (uint8_t)(*d & (uint8_t)~hi) : (uint8_t)(*d | hi);
            }
        }
    }
}

static uint16_t draw_code(uint16_t x, uint16_t y, gfx_font_t font, uint8_t code)
{
    const uint8_t w = glyph_width(font, code);
    if (font == GFX_FONT_FREQ) {
        const int8_t i = freq_index(code);
        if (i >= 0) blit(x, y, &gfx_freq_bits[gfx_freq_offset[i]], w, (uint8_t)GFX_FREQ_PAGES);
    } else if (code != ' ') {
        blit(x, y, text_glyph(font, code), w, 2U);
    }
    return (uint16_t)(x + w);
}

uint16_t GFX_Text(uint16_t x, uint16_t y, gfx_font_t font, uint8_t fg,
                  const char *s, uint16_t x_max)
{
    if ((s == NULL) || (x >= x_max)) return x;
    if (x_max > GFX_W) x_max = GFX_W;
    const uint16_t x0 = x;
    const uint16_t full = GFX_TextWidth(font, s);
    if ((uint32_t)x + full <= x_max) {
        while (*s != 0) x = draw_code(x, y, font, next_code(&s));
    } else {
        /* Leave room for the ellipsis (text fonts) and stop before x_max. */
        const uint8_t tail = (font == GFX_FONT_FREQ) ? 0U : glyph_width(font, 0x06U);
        while (*s != 0) {
            const char *peek = s;
            const uint8_t code = next_code(&peek);
            if ((uint32_t)x + glyph_width(font, code) + tail > x_max) break;
            x = draw_code(x, y, font, code);
            s = peek;
        }
        if ((tail != 0U) && ((uint32_t)x + tail <= x_max)) x = draw_code(x, y, font, 0x06U);
    }
    if (x > x0) GFX_Ink(x0, y, (uint16_t)(x - x0), GFX_FontHeight(font), fg);
    return x;
}

uint16_t GFX_TextBig(uint16_t x, uint16_t y, uint8_t scale, uint8_t fg, const char *s)
{
    const uint16_t x0 = x;
    while ((s != NULL) && (*s != 0) && ((uint32_t)x + 8U * scale <= GFX_W)) {
        const uint8_t code = next_code(&s);
        if (code != ' ') {
            const uint8_t *g = text_glyph(GFX_FONT_BODY, code);
            for (uint16_t c = 0U; c < 8U; ++c)
                for (uint16_t r = 0U; r < 16U; ++r)
                    if ((g[(r >> 3) * 8U + c] >> (r & 7U)) & 1U)
                        pixels((uint16_t)(x + c * scale), (uint16_t)(y + r * scale), scale, scale, 1U);
        }
        x = (uint16_t)(x + 8U * scale);
    }
    if (x > x0) GFX_Ink(x0, y, (uint16_t)(x - x0), (uint16_t)(16U * scale), fg);
    return x;
}

uint16_t GFX_Badge(uint16_t x, uint16_t y, gfx_font_t font, uint8_t color, const char *s)
{
    const uint16_t tw = GFX_TextWidth(font, s);
    const uint16_t w = (uint16_t)((tw + 10U + 7U) & ~7U);
    const uint16_t h = (uint16_t)(GFX_FontHeight(font) + 2U);
    if ((s == NULL) || (x >= GFX_W)) return x;
    GFX_Rect(x, y, w, h, color);
    gfx_cut = 1U;
    uint16_t cx = (uint16_t)(x + (w - tw) / 2U);
    while (*s != 0) cx = draw_code(cx, (uint16_t)(y + 1U), font, next_code(&s));
    gfx_cut = 0U;
    return (uint16_t)(x + w);
}

uint16_t GFX_TextRight(uint16_t x_right, uint16_t y, gfx_font_t font, uint8_t fg, const char *s)
{
    const uint16_t w = GFX_TextWidth(font, s);
    const uint16_t x = (w < x_right) ? (uint16_t)(x_right - w) : 0U;
    return GFX_Text(x, y, font, fg, s, x_right);
}

uint16_t GFX_TextCenter(uint16_t x, uint16_t w, uint16_t y, gfx_font_t font, uint8_t fg, const char *s)
{
    const uint16_t tw = GFX_TextWidth(font, s);
    const uint16_t x0 = (tw < w) ? (uint16_t)(x + (w - tw) / 2U) : x;
    return GFX_Text(x0, y, font, fg, s, (uint16_t)(x + w));
}

/* ---- Panel side ------------------------------------------------------- */

__attribute__((optimize("O2")))
uint32_t GFX_PageHash(uint8_t page)
{
    if (page >= GFX_PAGES) return 0U;
    uint32_t h = 2166136261U;
    /* A band page is what its raster draws: its bytes are the raster's data,
     * not pixels, and the version says when that changed. */
    if (in_band(page)) return ((h ^ band_version) * 16777619U ^ page) * 16777619U;
    const uint8_t *fb = &gfx_fb[(uint32_t)page * GFX_W];
    for (uint32_t i = 0U; i < GFX_W; i += 4U) {
        uint32_t word;
        memcpy(&word, &fb[i], 4U);
        h = (h ^ word) * 16777619U;
    }
    const uint8_t *at = &gfx_attr[(uint32_t)page * GFX_TCOLS];
    for (uint32_t i = 0U; i < GFX_TCOLS; i += 4U) {
        uint32_t word;
        memcpy(&word, &at[i], 4U);
        h = (h ^ word) * 16777619U;
    }
    return h;
}

/* A colour as the panel takes it, high byte first, in a little-endian half. */
static inline uint32_t wire565(uint8_t color)
{
    const uint32_t c = gfx_palette[color & 15U];
    return ((c >> 8) | (c << 8)) & 0xFFFFU;
}

/* Hot: runs for every row sent to the panel, in the main loop, which gets
 * about a tenth of the CPU while receiving. Two pixels per store, from a
 * four-entry table that changes only with the tile's colours: about 8 cycles
 * a pixel, and half the main-loop time of the byte-wise loop it replaces
 * (bench, 2026-10-01). */
__attribute__((optimize("O2")))
void GFX_PrepareLines(uint16_t y0, uint16_t lines, uint8_t *out)
{
    for (uint16_t n = 0U; (n < lines) && ((uint32_t)y0 + n < GFX_H); ++n) {
        const uint16_t y = (uint16_t)(y0 + n);
        if (in_band((uint32_t)y >> 3)) {
            band_fn((uint16_t)(y - (uint16_t)band_page * 8U), out);
            out += GFX_W * 2U;
            continue;
        }
        const uint32_t sh = y & 7U;
        const uint8_t *row = &gfx_fb[(uint32_t)(y >> 3) * GFX_W];
        const uint8_t *at = &gfx_attr[(uint32_t)(y >> 3) * GFX_TCOLS];
        uint32_t pair[4] = {0U, 0U, 0U, 0U}, attr = 0x100U;
        for (uint32_t c = 0U; c < GFX_TCOLS; ++c) {
            if (at[c] != attr) {
                attr = at[c];
                const uint32_t fg = wire565((uint8_t)(attr >> 4)), bg = wire565((uint8_t)attr);
                pair[0] = bg | (bg << 16);
                pair[1] = fg | (bg << 16);
                pair[2] = bg | (fg << 16);
                pair[3] = fg | (fg << 16);
            }
            /* The tile's eight columns, this row's bit of each at bits 0, 8,
             * 16 and 24 of two words: pixels 0..3 and 4..7. */
            for (uint32_t h = 0U; h < 2U; ++h) {
                uint32_t b;
                memcpy(&b, &row[c * 8U + h * 4U], 4U);
                b = (b >> sh) & 0x01010101U;
                const uint32_t p01 = pair[(b | (b >> 7)) & 3U];
                const uint32_t p23 = pair[((b >> 16) | (b >> 23)) & 3U];
                memcpy(out, &p01, 4U);
                memcpy(out + 4U, &p23, 4U);
                out += 8U;
            }
        }
    }
}
