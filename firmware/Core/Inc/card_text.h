#ifndef CARD_TEXT_H
#define CARD_TEXT_H

/* Text kept on the SD card's text pack (HELP.BIN) instead of in flash
 * (Core/Inc/card_text.def; ui_help.c reads it). */

#include <stdint.h>

typedef enum {
    TXT_NONE = 0, /* no card text: the caller has its own */
#define TEXT(id, text) id,
#include "card_text.def"
#undef TEXT
    TXT_COUNT
} card_text_t;

/* The text into `buf` (at most cap - 1 characters, always terminated);
 * returns its length, 0 when the card, its HELP.BIN or blocking I/O is not
 * there now. Main loop only; reads the card each call. */
uint16_t UI_CardText(card_text_t id, char *buf, uint16_t cap);

/* The same for a text drawn every frame (the menu's category descriptions):
 * read once the cursor has rested and kept until another is asked for, ""
 * until then -- the settings help's cache. */
const char *UI_CardTextCached(card_text_t id);

#endif /* CARD_TEXT_H */
