#ifndef MOD_MORSE_TABLES_H
#define MOD_MORSE_TABLES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* The Morse alphabet shared by the trainer, its player, the radio memories
 * and the receive decoder.
 *
 * A symbol is one byte: printable ASCII for letters (upper case), digits and
 * the punctuation . , : - / = ? @ +, and MORSE_AS..MORSE_BK for the
 * prosigns that have no character of their own. ' ' is a word space.
 *
 * A code is one byte too: a 1, then the elements first to last, 0 a dit and
 * 1 a dah -- E is 0b10, T 0b11, A 0b101 -- so up to seven elements (BK).
 * Symbols that sound the same are the same code: + is AR, = is BT. */
enum {
    MORSE_AS = 0x01U, /* .-...   wait */
    MORSE_KA,         /* -.-.-   attention */
    MORSE_KN,         /* -.--.   over, named station */
    MORSE_SK,         /* ...-.-  end of contact */
    MORSE_VE,         /* ...-.   understood */
    MORSE_BK,         /* -...-.- break */
    MORSE_PROSIGN_LAST = MORSE_BK
};

#define MORSE_CODE_EMPTY  1U
#define MORSE_ELEMENTS_MAX 7U

#define MORSE_SYMBOLS 51U

/* A symbol's place in the alphabet -- A..Z, 0..9, . , : - / = ? @ +, then
 * the prosigns -- 0xFF if it has none: bit i of a CW pack word mask
 * (tools/gen_cw_pack.py) is the symbol at i. */
uint8_t MORSE_Index(uint8_t sym);
uint8_t MORSE_SymbolAt(uint8_t index);
/* The code of a symbol (letters in either case), 0 if it has none. */
uint8_t MORSE_Code(uint8_t sym);
/* The symbol of a code, 0 if none. */
uint8_t MORSE_Symbol(uint8_t code);
/* Elements in a code (0 for MORSE_CODE_EMPTY or 0). */
uint8_t MORSE_Length(uint8_t code);
/* A symbol for the panel: "A", or "<SK>" for a prosign. buf: 5 bytes. */
const char *MORSE_Label(uint8_t sym, char buf[5]);
/* A symbol's code as dots and dashes, terminated. buf: 8 bytes. */
const char *MORSE_Pattern(uint8_t sym, char buf[8]);
/* The prosign "XY>" at s (just past its '<'), either case: its symbol --
 * <AR> and <BT> are the + and = they sound as -- or 0. Reads 3 bytes. */
uint8_t MORSE_Prosign(const char *s);
/* Text into codes (the radio memories): letters in either case, digits, the
 * punctuation above, <AS> <KA> <KN> <SK> <VE> <BK>, and a 0 for each run of
 * spaces between words. The count, or 0 for a character the alphabet lacks
 * or more than `cap` codes. */
uint8_t MORSE_ParseText(const char *text, uint8_t *code, uint8_t cap);

/* The receive decoder's view: bits shifted left as keyed (dit 0, dah 1),
 * len elements; '?' for anything without a printable character. */
char MTRAIN_DecodeSymbol(uint8_t bits, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif /* MOD_MORSE_TABLES_H */
