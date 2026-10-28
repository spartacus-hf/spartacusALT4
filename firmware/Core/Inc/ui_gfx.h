#ifndef UI_GFX_H
#define UI_GFX_H

/* Landscape frame for the ST7789: a 320x240 one-bit framebuffer plus one
 * colour attribute per 8x8 tile (foreground nibble, background nibble) from a
 * fixed 16-colour palette. Colour is therefore free in two dimensions at an
 * 8-pixel granularity, for 1200 bytes, where a full RGB565 frame would need
 * 150 KB. Layout rule that follows: two colours never share a tile, so panels,
 * rows and text are placed on the 8-pixel grid.
 *
 * Pure C with no HAL: the LCD driver streams GFX_PrepareLines() to the panel,
 * and the host suites and tools/ui_preview.py use the same code. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_W      320U
#define GFX_H      240U
#define GFX_PAGES  (GFX_H / 8U)  /* 8-row framebuffer pages */
#define GFX_TCOLS  (GFX_W / 8U)  /* tile columns */
/* Text keeps this far from every edge: the bezel hides the outermost pixels. */
#define GFX_MARGIN 8U

enum {
    GFX_BG = 0,     /* screen background */
    GFX_PANEL,      /* raised panel */
    GFX_PANEL_HI,   /* panel under the cursor / header band */
    GFX_LINE,       /* separators, unlit meter segments */
    GFX_TEXT,       /* primary text */
    GFX_DIM,        /* secondary text */
    GFX_MUTED,      /* captions, disabled */
    GFX_ACCENT,     /* focus and selection */
    GFX_SELECT,     /* selected row background */
    GFX_FREQ,       /* the frequency */
    GFX_OK,
    GFX_WARN,
    GFX_ALERT,
    GFX_ALERT_BG,   /* transmit / fault band */
    GFX_OK_BG,
    GFX_WHITE,
    GFX_COLORS
};

typedef enum {
    GFX_FONT_SMALL = 0, /* 6 x 12 */
    GFX_FONT_BODY,      /* 8 x 16 */
    GFX_FONT_FREQ       /* 28 x 34 digits and '.' only */
} gfx_font_t;

/* Symbols inside strings (text fonts only). */
#define GFX_SYM_LEFT     "\x01"
#define GFX_SYM_RIGHT    "\x02"
#define GFX_SYM_UP       "\x03"
#define GFX_SYM_DOWN     "\x04"
#define GFX_SYM_DEG      "\x05"
#define GFX_SYM_ELLIPSIS "\x06"
#define GFX_SYM_PRESS    "\x07"
#define GFX_SYM_TURN     "\x08"

/* ---- Drawing ---------------------------------------------------------- */
/* Blank every pixel; every tile becomes TEXT on `bg`. */
void GFX_Clear(uint8_t bg);
/* Background of every tile the rectangle touches. No pixels change. */
void GFX_Fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t bg);
/* Foreground of every tile the rectangle touches. No pixels change. */
void GFX_Ink(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t fg);
/* Solid pixels, drawn in `fg` (which colours the tiles they touch). */
void GFX_Rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t fg);
void GFX_Frame(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t fg);
void GFX_HLine(uint16_t x, uint16_t y, uint16_t w, uint8_t fg);
void GFX_VLine(uint16_t x, uint16_t y, uint16_t h, uint8_t fg);
/* Pixels only, leaving tile colours alone. */
void GFX_Pixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void GFX_ClearPixels(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/* Draws UTF-8 text (Latin-1 letters fold to ASCII) and returns the x after
 * it. Nothing is drawn at or beyond x_max: text that would cross it ends in
 * an ellipsis instead. */
uint16_t GFX_Text(uint16_t x, uint16_t y, gfx_font_t font, uint8_t fg,
                  const char *s, uint16_t x_max);
/* A square block in `color` with the text cut out of it, so the text shows the tile
 * background. Width rounds up to whole tiles, so x on the 8-pixel grid keeps
 * neighbours in their own colour. Returns the x after it. */
uint16_t GFX_Badge(uint16_t x, uint16_t y, gfx_font_t font, uint8_t color, const char *s);
/* Body-font text `scale` times as large, each glyph pixel a block (the
 * Morse trainer's characters). Stops at the screen edge. */
uint16_t GFX_TextBig(uint16_t x, uint16_t y, uint8_t scale, uint8_t fg, const char *s);
uint16_t GFX_TextRight(uint16_t x_right, uint16_t y, gfx_font_t font, uint8_t fg, const char *s);
uint16_t GFX_TextCenter(uint16_t x, uint16_t w, uint16_t y, gfx_font_t font, uint8_t fg, const char *s);
uint16_t GFX_TextWidth(gfx_font_t font, const char *s);
uint8_t GFX_FontHeight(gfx_font_t font);
uint8_t GFX_CharWidth(gfx_font_t font); /* digit advance for GFX_FONT_FREQ */

/* ---- Colour band -------------------------------------------------------- */
/* A band of full-width 8-row pages drawn by a raster instead of the one-bit
 * frame: the WATERFALL page's spectrum and history. While a band is held,
 * the framebuffer bytes of its pages are the holder's memory (pages x GFX_W
 * bytes, returned by the claim), drawing and GFX_Clear leave them alone,
 * and the panel shows the rows `fn` writes: row counts from the band's top,
 * `out` takes GFX_W big-endian RGB565 pixels. Claiming the band already held
 * keeps its memory; any other claim clears it and sets *fresh. A claim the
 * frame cannot hold (off screen, zero pages) returns NULL. */
typedef void (*gfx_band_fn_t)(uint16_t row, uint8_t *out);
uint8_t *GFX_BandClaim(uint8_t page, uint8_t pages, gfx_band_fn_t fn, uint8_t *fresh);
/* Give the pages back, cleared. */
void GFX_BandRelease(void);
/* 1 while `fn` holds the band. */
uint8_t GFX_BandHeld(gfx_band_fn_t fn);
/* Where the band is now: first page and page count (0: none). */
void GFX_BandSpan(uint8_t *page, uint8_t *pages);
/* What the raster draws changed: the band's pages are sent again. */
void GFX_BandTouch(void);

/* ---- Panel side --------------------------------------------------------- */
/* Change detector for one 8-row page: pixels and tile colours. */
uint32_t GFX_PageHash(uint8_t page);
/* `lines` full-width rows from y0 as big-endian RGB565, 640 bytes a row. */
void GFX_PrepareLines(uint16_t y0, uint16_t lines, uint8_t *out);
uint16_t GFX_Color565(uint8_t color);
/* ST7789 MADCTL for landscape: 90 degrees = MV|MX, 270 = MV|MY, RGB order
 * (this panel shows red and blue swapped with the BGR bit set). */
uint8_t GFX_Madctl(uint8_t rot270);

/* Read-only views for the host suites and the preview tool. */
const uint8_t *GFX_FrameBits(void);
const uint8_t *GFX_TileAttrs(void);

#ifdef __cplusplus
}
#endif

#endif
