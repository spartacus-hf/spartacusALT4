#!/usr/bin/env python3
"""Generate Core/Src/ui_font_data.inc, the LCD fonts of the landscape UI.

Three fonts, all 1-bit and stored column-major in 8-pixel pages -- the byte
layout of the LCD framebuffer itself, so a glyph at a page-aligned y is one
OR per byte:

  small  Terminus Bold 12 (6 x 12)   dense bench pages, help text, captions
  body   Terminus Bold 16 (8 x 16)   everything else
  freq   DejaVu Sans Condensed Bold  the frequency digits and '.'

Both text fonts carry printable ASCII 0x20..0x7E plus eight symbols reached
through control codes (see SYMBOLS). Latin-1 letters are folded to ASCII by
the renderer, as before.

Glyph bytes: for each page p (8 rows, top row in bit 0) the glyph's columns
left to right, so byte [p * width + x] holds rows 8p..8p+7 of column x.

The fonts come from font files that are not part of this repository.
Terminus (http://terminus-font.sourceforge.net/) is under the SIL Open Font
License 1.1; DejaVu fonts are under the Bitstream Vera license with DejaVu
changes in the public domain. Both allow embedding the rendered bitmaps; the
notices are repeated in the generated file and in docs/THIRD_PARTY_FONTS.md.

Usage:
  gen_ui_fonts.py            render and write Core/Src/ui_font_data.inc
  gen_ui_fonts.py --check    fail if the checked-in file differs from a fresh
                             render; skip (exit 0) with a notice when the font
                             files are not installed on this machine
"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / "Core" / "Src" / "ui_font_data.inc"

TERMINUS_DIRS = [
    os.environ.get("UI_FONT_DIR", ""),
    "~/.local/share/fonts/terminus",
    "/usr/share/fonts/terminus",
    "/usr/share/fonts/misc",
]
DEJAVU_PATHS = [
    os.environ.get("UI_FREQ_FONT", ""),
    "/usr/share/fonts/TTF/DejaVuSansCondensed-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf",
    "/usr/share/fonts/dejavu/DejaVuSansCondensed-Bold.ttf",
]

# Control code -> Unicode symbol drawn from the Terminus font.
SYMBOLS = [
    (0x01, "◄"),  # left pointer: choice can go down
    (0x02, "►"),  # right pointer: choice can go up / opens a page
    (0x03, "▲"),  # up triangle
    (0x04, "▼"),  # down triangle
    (0x05, "°"),  # degree
    (0x06, "…"),  # ellipsis for clipped text
    (0x07, "●"),  # filled circle: knob press
    (0x08, "↻"),  # clockwise arrow: knob turn
]
ASCII = [chr(c) for c in range(0x20, 0x7F)]

FREQ_SIZE = 44
FREQ_CHARS = "0123456789."  # the frequency needs nothing else
FREQ_PAGES = 5            # 40 rows; the digits are 34 tall
FREQ_DOT_WIDTH = 12       # separators are narrow so the groups read as groups
FREQ_SPACE_WIDTH = 14


def find_terminus(name: str) -> Path | None:
    for d in TERMINUS_DIRS:
        if not d:
            continue
        p = Path(os.path.expanduser(d)) / name
        if p.exists():
            return p
    return None


def find_dejavu() -> Path | None:
    for p in DEJAVU_PATHS:
        if p and Path(p).exists():
            return Path(p)
    return None


def pack_columns(img, width: int, pages: int) -> list[int]:
    out = []
    for p in range(pages):
        for x in range(width):
            b = 0
            for bit in range(8):
                y = p * 8 + bit
                if y < img.height and img.getpixel((x, y)):
                    b |= 1 << bit
            out.append(b)
    return out


def render_text_font(path: Path, size: int, width: int, height: int):
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype(str(path), size)
    pages = (height + 7) // 8
    glyphs = []
    for ch in ASCII + [s for _code, s in SYMBOLS]:
        img = Image.new("1", (width, pages * 8), 0)
        ImageDraw.Draw(img).text((0, 0), ch, font=font, fill=1)
        data = pack_columns(img, width, pages)
        if ch not in " " and not any(data):
            raise SystemExit(f"{path.name}: no glyph for U+{ord(ch):04X}")
        glyphs.append(data)
    return glyphs


def render_freq_font(path: Path):
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype(str(path), FREQ_SIZE)
    top = min(font.getbbox(c)[1] for c in "0123456789")
    bottom = max(font.getbbox(c)[3] for c in "0123456789")
    if bottom - top > FREQ_PAGES * 8:
        raise SystemExit("frequency digits taller than their pages")
    digit_w = int(round(font.getlength("0")))
    widths, glyphs = [], []
    for ch in FREQ_CHARS:
        if ch == ".":
            w = FREQ_DOT_WIDTH
        elif ch == " ":
            w = FREQ_SPACE_WIDTH
        else:
            w = digit_w
        img = Image.new("1", (w, FREQ_PAGES * 8), 0)
        if ch != " ":
            ink = font.getbbox(ch)
            ink_w = ink[2] - ink[0]
            x = (w - ink_w) // 2 - ink[0]
            ImageDraw.Draw(img).text((x, -top), ch, font=font, fill=1)
        widths.append(w)
        glyphs.append(pack_columns(img, w, FREQ_PAGES))
    return widths, glyphs, bottom - top


def c_bytes(values: list[int], indent: str = "    ") -> str:
    lines = []
    for i in range(0, len(values), 16):
        lines.append(indent + ",".join(f"0x{v:02X}" for v in values[i:i + 16]) + ",")
    return "\n".join(lines)


def generate() -> str | None:
    small_path = find_terminus("ter-u12b.otb")
    body_path = find_terminus("ter-u16b.otb")
    freq_path = find_dejavu()
    if not (small_path and body_path and freq_path):
        return None
    small = render_text_font(small_path, 12, 6, 12)
    body = render_text_font(body_path, 16, 8, 16)
    freq_w, freq, freq_h = render_freq_font(freq_path)

    out = []
    out.append("/* Generated by tools/gen_ui_fonts.py -- do not edit by hand.")
    out.append(" *")
    out.append(" * Terminus Font: Copyright (c) 2019 Dimitar Toshkov Zhekov,")
    out.append(" * licensed under the SIL Open Font License, Version 1.1.")
    out.append(" * DejaVu fonts: Copyright (c) 2003 Bitstream, Inc. (Bitstream Vera")
    out.append(" * license); DejaVu changes are in the public domain.")
    out.append(" * The bitmaps below are renderings of those fonts; see")
    out.append(" * docs/THIRD_PARTY_FONTS.md for the full notices. */")
    out.append("")
    out.append(f"#define GFX_FONT_GLYPHS      {len(small)}U /* 0x20..0x7E, then symbols 0x01..0x{len(SYMBOLS):02X} */")
    out.append(f"#define GFX_FONT_SYMBOLS     {len(SYMBOLS)}U")
    out.append(f"#define GFX_FREQ_GLYPHS      {len(freq)}U")
    out.append(f"#define GFX_FREQ_PAGES       {FREQ_PAGES}U")
    out.append(f"#define GFX_FREQ_HEIGHT      {freq_h}U")
    out.append(f"#define GFX_FREQ_DIGIT_W     {freq_w[0]}U")
    out.append("")
    out.append("static const uint8_t gfx_font_small_bits[GFX_FONT_GLYPHS * 12U] = {")
    out.append(c_bytes([b for g in small for b in g]))
    out.append("};")
    out.append("")
    out.append("static const uint8_t gfx_font_body_bits[GFX_FONT_GLYPHS * 16U] = {")
    out.append(c_bytes([b for g in body for b in g]))
    out.append("};")
    out.append("")
    out.append(f'static const char gfx_freq_chars[GFX_FREQ_GLYPHS + 1U] = "{FREQ_CHARS}";')
    out.append("static const uint8_t gfx_freq_width[GFX_FREQ_GLYPHS] = {"
               + ",".join(str(w) for w in freq_w) + "};")
    offsets, acc = [], 0
    for g in freq:
        offsets.append(acc)
        acc += len(g)
    out.append("static const uint16_t gfx_freq_offset[GFX_FREQ_GLYPHS] = {"
               + ",".join(str(o) for o in offsets) + "};")
    out.append(f"static const uint8_t gfx_freq_bits[{acc}U] = {{")
    out.append(c_bytes([b for g in freq for b in g]))
    out.append("};")
    return "\n".join(out) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        text = generate()
    except ImportError:
        text = None
    if text is None:
        if args.check:
            print("SKIP  ui fonts: Terminus/DejaVu font files or Pillow not installed; "
                  "the checked-in table is used as is")
            return 0
        print("error: need Pillow, ter-u12b.otb/ter-u16b.otb and DejaVuSansCondensed-Bold.ttf",
              file=sys.stderr)
        return 1
    if args.check:
        current = TARGET.read_text() if TARGET.exists() else ""
        if current != text:
            print(f"FAIL  {TARGET.relative_to(ROOT)} is not the output of gen_ui_fonts.py", file=sys.stderr)
            return 1
        print(f"PASS  {TARGET.relative_to(ROOT)} matches gen_ui_fonts.py")
        return 0
    TARGET.write_text(text)
    print(f"wrote {TARGET.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
