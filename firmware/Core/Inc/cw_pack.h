#ifndef CW_PACK_H
#define CW_PACK_H

/* The Morse trainer's resource pack, CWPACK.BIN on the SD card
 * (tools/gen_cw_pack.py builds it; its docstring has the format): the Koch
 * courses and where their lessons end, words, abbreviations, call sign
 * prefixes and the course START LEARNING runs. Every training activity
 * needs it.
 *
 * Main loop only, and only while blocking I/O is allowed: each call reads
 * the card. CWPACK_Open checks the whole file once per mount against the id
 * this image was built with; every record is checked again as it is read. */

#include <stdint.h>

#define CWPACK_TEXT_MAX  12U    /* characters in a word, abbreviation or prefix record */
#define CWPACK_WORDS_MAX 2048U
#define CWPACK_ABBR_MAX  256U
#define CWPACK_STEPS_MAX 32U
#define CWPACK_COURSES   4U

typedef enum {
    CWPACK_WORDS = 0,
    CWPACK_ABBR
} cwpack_list_t;

/* The course's steps, as tools/cw_pack/steps.txt names them; a step is
 * op, a, b, text (gen_cw_pack.py's docstring). */
enum { CWPACK_STEP_PADDLE = 1U, CWPACK_STEP_EXPLORE, CWPACK_STEP_INTRO, CWPACK_STEP_LISTEN, CWPACK_STEP_ECHO,
       CWPACK_STEP_MASTER, CWPACK_STEP_SPEED, CWPACK_STEP_GOTO };
#define CWPACK_SRC_DRILL 5U  /* after mtrain_src_t: the lesson's newest characters, each repeated */

/* For the boot check and the console's `card`: 0 missing, 1 another build's
 * or damaged, 2 this image's; and the id this image wants. */
uint8_t CWPACK_CardState(void);
uint32_t CWPACK_CardId(void);

/* The card can be read now. Every call below fails while it cannot; a
 * failure while it can is the card's or the pack's. */
uint8_t CWPACK_Ready(void);
uint8_t CWPACK_Mounted(void);   /* an SD card is there */
/* The pack is there and whole (checked once per mount). */
uint8_t CWPACK_Open(void);
/* Forget the check: the card went away or a read failed. */
void CWPACK_Close(void);

/* A course's symbols in teaching order (mod_morse_tables.h symbols), into
 * out[MORSE_SYMBOLS], and where its lessons end into stops[7] (bit i of
 * the little-endian bytes: a lesson of i + 1 characters); returns how
 * many, 0 on failure. */
uint8_t CWPACK_Course(uint8_t course, uint8_t *out, uint8_t *stops);
/* The operator's own course, CW/CUSTOM.TXT: the characters in the order to
 * learn them, prosigns as <SK>, anything else skipped; into
 * out[MORSE_SYMBOLS], returns how many (0: no file or nothing usable). */
uint8_t CWPACK_Custom(uint8_t *out);
/* Which of a list's records use only symbols in `allowed` (bit i = symbol
 * at MORSE_Index i) and have at most `max_len` characters: a bit each in
 * `bitmap` (CWPACK_WORDS_MAX / 8 bytes for words, CWPACK_ABBR_MAX / 8 for
 * abbreviations). Returns how many, or -1 when the card could not be read. */
int32_t CWPACK_Scan(cwpack_list_t list, uint64_t allowed, uint8_t max_len, uint8_t *bitmap);
/* Record `index` of a list, terminated, into out[CWPACK_TEXT_MAX + 1];
 * returns its length, 0 on failure. */
uint8_t CWPACK_Text(cwpack_list_t list, uint16_t index, uint8_t *out);
/* A call sign prefix, picked by `pick` in proportion to how often it is
 * heard; out[5], returns its length, 0 on failure. */
uint8_t CWPACK_Prefix(uint32_t pick, uint8_t *out);
/* Step `i` of the course into out[4]; 0 past its end or on failure. */
uint8_t CWPACK_Step(uint8_t i, uint8_t *out);

#endif /* CW_PACK_H */
