# CW pack sources

`tools/gen_cw_pack.py` builds `sd/CWPACK.BIN`, the trainer's resource pack on
the SD card, from these files. The radio needs the pack for every training
activity; `tools/flash_rev1.py` copies it to the card like `HELP.BIN`.

| File | What | Source |
|---|---|---|
| `courses.txt` | Koch orders: M32, LCWO, CW Academy, LICW (carousel), and where each course's lessons end | Morserino-32 `MorsePreferences.h`; the lesson groups from the CWops Beginner curriculum and LICW |
| `words.txt` | English words with frequency weights | Morserino-32 `english_words.h` (Oxford 5000; weights from hermitdave/FrequencyWords) |
| `abbrev.txt` | CW abbreviations | Morserino-32 `abbrev.h` |
| `prefixes.txt` | Call sign prefixes, region, CQ zone, frequency weight | Morserino-32 `callsign_prefixes.h` |
| `steps.txt` | The course START LEARNING runs (CW Academy, LICW and Koch practice, see the file) | this project |

The Morserino-32 data is taken from
<https://github.com/oe1wkl/Morserino-32> at commit `73a766f` (Software/src,
"Version 6 and newer"). It is (C) 2018-2025 Willi Kraml, OE1WKL, and licensed
GPL-3.0-or-later; the derived files and `sd/CWPACK.BIN` keep that licence
and attribution, as the firmware's own code is GPL-3.0-or-later too. The
word weights originate from <https://github.com/hermitdave/FrequencyWords>,
whose content is licensed CC BY-SA 4.0.
