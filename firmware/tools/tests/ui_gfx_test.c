/* Host suite for Core/Src/ui_gfx.c: text stays inside its box, clipped text
 * ends in an ellipsis, Latin-1 folds, tiles take the colours they are given,
 * the page hash sees every change, stripes convert to RGB565 exactly as a
 * pixel-by-pixel reference does, and the font tables are complete. */
#include "ui_gfx.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pixel(uint16_t x, uint16_t y)
{
    return (GFX_FrameBits()[(uint32_t)(y >> 3) * GFX_W + x] >> (y & 7U)) & 1U;
}

static uint8_t attr(uint16_t x, uint16_t y)
{
    return GFX_TileAttrs()[(uint32_t)(y >> 3) * GFX_TCOLS + (x >> 3)];
}

/* Every lit pixel lies inside [x0,x1) x [y0,y1). Returns the lit count. */
static unsigned lit_inside(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    unsigned lit = 0U;
    for (uint16_t y = 0U; y < GFX_H; ++y)
        for (uint16_t x = 0U; x < GFX_W; ++x)
            if (pixel(x, y)) {
                assert(x >= x0 && x < x1 && y >= y0 && y < y1);
                lit++;
            }
    return lit;
}

static void test_text_box(void)
{
    static const gfx_font_t fonts[3] = {GFX_FONT_SMALL, GFX_FONT_BODY, GFX_FONT_FREQ};
    for (unsigned f = 0U; f < 3U; ++f) {
        const char *s = (fonts[f] == GFX_FONT_FREQ) ? "14.074.000" : "Wg|jQ_ 0123 \x01\x02\x07\x08";
        for (uint16_t y = 0U; y < 12U; ++y) {
            GFX_Clear(0U);
            const uint16_t w = GFX_TextWidth(fonts[f], s);
            const uint16_t end = GFX_Text(13U, (uint16_t)(20U + y), fonts[f], GFX_TEXT, s, GFX_W);
            assert(end == 13U + w);
            assert(lit_inside(13U, (uint16_t)(20U + y), end,
                              (uint16_t)(20U + y + GFX_FontHeight(fonts[f]))) > 0U);
        }
    }
    assert(GFX_TextWidth(GFX_FONT_SMALL, "abc") == 18U);
    assert(GFX_TextWidth(GFX_FONT_BODY, "abc") == 24U);
    /* Frequency digits are one width and the separators narrow. */
    assert(GFX_TextWidth(GFX_FONT_FREQ, "14.074.000") == 8U * GFX_CharWidth(GFX_FONT_FREQ) + 2U * 12U);
    assert(GFX_CharWidth(GFX_FONT_FREQ) >= 24U && GFX_FontHeight(GFX_FONT_FREQ) <= 40U);
}

static void test_clip(void)
{
    GFX_Clear(0U);
    const char *s = "HEADPHONE VOLUME PERCENT";
    const uint16_t end = GFX_Text(40U, 8U, GFX_FONT_BODY, GFX_TEXT, s, 120U);
    assert(end <= 120U);
    lit_inside(40U, 8U, 120U, 24U);
    /* The last cell drawn is the ellipsis. */
    GFX_Clear(0U);
    (void)GFX_Text(0U, 0U, GFX_FONT_BODY, GFX_TEXT, GFX_SYM_ELLIPSIS, GFX_W);
    uint8_t ell[8 * 2];
    for (unsigned c = 0U; c < 8U; ++c) { ell[c] = GFX_FrameBits()[c]; ell[8U + c] = GFX_FrameBits()[GFX_W + c]; }
    GFX_Clear(0U);
    const uint16_t e2 = GFX_Text(40U, 8U, GFX_FONT_BODY, GFX_TEXT, s, 120U);
    for (unsigned c = 0U; c < 8U; ++c) {
        uint16_t x = (uint16_t)(e2 - 8U + c);
        uint8_t col_lo = 0U, col_hi = 0U;
        for (unsigned r = 0U; r < 8U; ++r) {
            col_lo |= (uint8_t)(pixel(x, (uint16_t)(8U + r)) << r);
            col_hi |= (uint8_t)(pixel(x, (uint16_t)(16U + r)) << r);
        }
        assert(col_lo == ell[c] && col_hi == ell[8U + c]);
    }
    /* A box too small for anything draws nothing past it. */
    GFX_Clear(0U);
    assert(GFX_Text(40U, 8U, GFX_FONT_BODY, GFX_TEXT, s, 44U) <= 44U);
    lit_inside(40U, 8U, 44U, 24U);
    /* Screen edge. */
    GFX_Clear(0U);
    (void)GFX_Text(300U, 232U, GFX_FONT_BODY, GFX_TEXT, "XYZW", 400U);
    lit_inside(300U, 232U, GFX_W, GFX_H);
}

static void test_fold(void)
{
    uint8_t a[GFX_W * 2];
    GFX_Clear(0U);
    (void)GFX_Text(0U, 0U, GFX_FONT_BODY, GFX_TEXT, "E e A n", GFX_W);
    memcpy(a, GFX_FrameBits(), sizeof(a));
    GFX_Clear(0U);
    (void)GFX_Text(0U, 0U, GFX_FONT_BODY, GFX_TEXT, "\xC3\x89 \xC3\xA9 \xC3\x84 \xC3\xB1", GFX_W);
    assert(!memcmp(a, GFX_FrameBits(), sizeof(a)));
    assert(GFX_TextWidth(GFX_FONT_BODY, "\xC3\x89\xE2\x82\xAC") == 16U); /* two glyphs */
}

static void test_tiles(void)
{
    GFX_Clear(GFX_BG);
    assert(attr(0U, 0U) == ((GFX_TEXT << 4) | GFX_BG));
    GFX_Fill(16U, 8U, 16U, 8U, GFX_PANEL);
    assert((attr(16U, 8U) & 15U) == GFX_PANEL && (attr(24U, 8U) & 15U) == GFX_PANEL);
    assert((attr(8U, 8U) & 15U) == GFX_BG && (attr(32U, 8U) & 15U) == GFX_BG);
    assert((attr(16U, 16U) & 15U) == GFX_BG);
    /* Text keeps the background and sets the foreground. */
    (void)GFX_Text(16U, 8U, GFX_FONT_SMALL, GFX_ACCENT, "Hi", GFX_W);
    assert(attr(16U, 8U) == ((GFX_ACCENT << 4) | GFX_PANEL));
    GFX_Rect(100U, 100U, 3U, 3U, GFX_OK);
    assert(pixel(100U, 100U) && pixel(102U, 102U) && !pixel(103U, 100U));
    assert((attr(100U, 100U) >> 4) == GFX_OK);
    GFX_ClearPixels(100U, 100U, 1U, 1U);
    assert(!pixel(100U, 100U) && pixel(101U, 100U));
    /* Badges are square: every corner pixel is lit, the text is cut out. */
    GFX_Clear(GFX_BG);
    const uint16_t bx = GFX_Badge(40U, 40U, GFX_FONT_SMALL, GFX_OK, "RX");
    assert(bx == 64U);
    assert(pixel(40U, 40U) && pixel(63U, 40U) && pixel(40U, 53U) && pixel(63U, 53U) && !pixel(64U, 40U));
    assert((attr(40U, 40U) >> 4) == GFX_OK);
    unsigned cut = 0U;
    for (uint16_t y = 41U; y < 53U; ++y)
        for (uint16_t x = 41U; x < 63U; ++x) cut += (unsigned)!pixel(x, y);
    assert(cut > 20U);
}

static void test_hash_and_stripes(void)
{
    GFX_Clear(GFX_BG);
    uint32_t before[GFX_PAGES];
    for (uint8_t p = 0U; p < GFX_PAGES; ++p) before[p] = GFX_PageHash(p);
    GFX_Pixels(123U, 77U, 1U, 1U);
    for (uint8_t p = 0U; p < GFX_PAGES; ++p)
        assert((GFX_PageHash(p) != before[p]) == (p == 77U / 8U));
    GFX_ClearPixels(123U, 77U, 1U, 1U);
    assert(GFX_PageHash(77U / 8U) == before[77U / 8U]);
    GFX_Fill(200U, 200U, 1U, 1U, GFX_SELECT);
    assert(GFX_PageHash(25U) != before[25U]);

    /* Stripe conversion: lit pixel in fg, others in bg, big-endian. */
    uint8_t line[GFX_W * 2U * 2U];
    GFX_Clear(GFX_BG);
    GFX_Fill(0U, 8U, 8U, 8U, GFX_PANEL);
    GFX_Rect(3U, 9U, 1U, 1U, GFX_FREQ);
    GFX_PrepareLines(9U, 2U, line);
    const uint16_t fg = GFX_Color565(GFX_FREQ), bg = GFX_Color565(GFX_PANEL), sb = GFX_Color565(GFX_BG);
    assert(line[6] == (uint8_t)(fg >> 8) && line[7] == (uint8_t)fg);
    assert(line[0] == (uint8_t)(bg >> 8) && line[1] == (uint8_t)bg);
    assert(line[16] == (uint8_t)(sb >> 8) && line[17] == (uint8_t)sb);
    assert(line[GFX_W * 2U + 6U] == (uint8_t)(bg >> 8)); /* next row: pixel off */
}

/* The rows as the panel takes them, one pixel at a time. */
static void reference_lines(uint16_t y0, uint16_t lines, uint8_t *out)
{
    for (uint16_t n = 0U; (n < lines) && ((uint32_t)y0 + n < GFX_H); ++n) {
        const uint16_t y = (uint16_t)(y0 + n);
        for (uint16_t x = 0U; x < GFX_W; ++x) {
            const uint8_t a = attr(x, y);
            const uint16_t c = GFX_Color565(pixel(x, y) ? (uint8_t)(a >> 4) : (uint8_t)(a & 15U));
            *out++ = (uint8_t)(c >> 8);
            *out++ = (uint8_t)c;
        }
    }
}

static void test_prepare_matches_reference(void)
{
    static uint8_t want[GFX_W * 2U * 8U], got[GFX_W * 2U * 8U];
    srand(7U);
    for (unsigned frame = 0U; frame < 24U; ++frame) {
        GFX_Clear((uint8_t)(rand() & 15));
        for (unsigned k = 0U; k < 40U; ++k) {
            GFX_Fill((uint16_t)(rand() % (int)GFX_W), (uint16_t)(rand() % (int)GFX_H), (uint16_t)(1 + rand() % 40),
                     (uint16_t)(1 + rand() % 30), (uint8_t)(rand() & 15));
            GFX_Rect((uint16_t)(rand() % (int)GFX_W), (uint16_t)(rand() % (int)GFX_H), (uint16_t)(1 + rand() % 20),
                     (uint16_t)(1 + rand() % 20), (uint8_t)(rand() & 15));
            (void)GFX_Text((uint16_t)(rand() % 300), (uint16_t)(rand() % 230), (rand() & 1) ? GFX_FONT_BODY : GFX_FONT_SMALL,
                           (uint8_t)(rand() & 15), "14.074.000 CW", GFX_W);
        }
        for (uint16_t y = 0U; y < GFX_H; ++y) {
            const uint16_t lines = (uint16_t)(1 + rand() % 8);
            memset(want, 0x5A, sizeof(want));
            memset(got, 0x5A, sizeof(got));
            reference_lines(y, lines, want);
            GFX_PrepareLines(y, lines, got);
            assert(memcmp(want, got, sizeof(want)) == 0);
        }
    }
}

static void test_fonts_and_panel(void)
{
    /* Every printable glyph and symbol has ink, except the space. */
    for (unsigned f = 0U; f < 2U; ++f) {
        for (unsigned c = 0x21U; c <= 0x7EU + 8U; ++c) {
            char s[2] = {(char)((c <= 0x7EU) ? c : (c - 0x7EU)), 0};
            GFX_Clear(0U);
            (void)GFX_Text(8U, 8U, f ? GFX_FONT_BODY : GFX_FONT_SMALL, GFX_TEXT, s, GFX_W);
            assert(lit_inside(8U, 8U, 16U, 24U) > 0U);
        }
    }
    for (const char *d = "0123456789."; *d; ++d) {
        char s[2] = {*d, 0};
        GFX_Clear(0U);
        (void)GFX_Text(8U, 8U, GFX_FONT_FREQ, GFX_FREQ, s, GFX_W);
        assert(lit_inside(8U, 8U, (uint16_t)(8U + GFX_TextWidth(GFX_FONT_FREQ, s)), (uint16_t)(8U + 40U)) > 0U);
    }
    assert(GFX_Madctl(0U) == 0x60U && GFX_Madctl(1U) == 0xA0U);
    /* The palette is distinct where it matters for legibility. */
    assert(GFX_Color565(GFX_TEXT) != GFX_Color565(GFX_BG));
    assert(GFX_Color565(GFX_ACCENT) != GFX_Color565(GFX_SELECT));
}

int main(void)
{
    test_text_box();
    test_clip();
    test_fold();
    test_tiles();
    test_hash_and_stripes();
    test_prepare_matches_reference();
    test_fonts_and_panel();
    puts("UI graphics: text boxes, clipping with ellipsis, Latin-1 folding, tile colours, "
         "square badges, page hash, RGB565 stripes, MADCTL and font tables: PASS");
    return 0;
}
