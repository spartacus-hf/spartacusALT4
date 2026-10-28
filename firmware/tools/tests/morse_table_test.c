#include "mod_morse_tables.h"

#include <stdio.h>

typedef struct { char value; unsigned bits; unsigned length; } case_t;

int main(void)
{
    static const case_t cases[] = {
        {'A', 0x01U, 2U}, {'B', 0x08U, 4U}, {'C', 0x0AU, 4U}, {'D', 0x04U, 3U},
        {'E', 0x00U, 1U}, {'F', 0x02U, 4U}, {'G', 0x06U, 3U}, {'H', 0x00U, 4U},
        {'I', 0x00U, 2U}, {'J', 0x07U, 4U}, {'K', 0x05U, 3U}, {'L', 0x04U, 4U},
        {'M', 0x03U, 2U}, {'N', 0x02U, 2U}, {'O', 0x07U, 3U}, {'P', 0x06U, 4U},
        {'Q', 0x0DU, 4U}, {'R', 0x02U, 3U}, {'S', 0x00U, 3U}, {'T', 0x01U, 1U},
        {'U', 0x01U, 3U}, {'V', 0x01U, 4U}, {'W', 0x03U, 3U}, {'X', 0x09U, 4U},
        {'Y', 0x0BU, 4U}, {'Z', 0x0CU, 4U},
        {'0', 0x1FU, 5U}, {'1', 0x0FU, 5U}, {'2', 0x07U, 5U}, {'3', 0x03U, 5U},
        {'4', 0x01U, 5U}, {'5', 0x00U, 5U}, {'6', 0x10U, 5U}, {'7', 0x18U, 5U},
        {'8', 0x1CU, 5U}, {'9', 0x1EU, 5U},
        {'.', 0x15U, 6U}, {',', 0x33U, 6U}, {'?', 0x0CU, 6U}, {'/', 0x12U, 5U},
        {'=', 0x11U, 5U}, {'+', 0x0AU, 5U}, {'-', 0x21U, 6U}, {'@', 0x1AU, 6U}
    };
    for (unsigned i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char actual = MTRAIN_DecodeSymbol((uint8_t)cases[i].bits,
                                          (uint8_t)cases[i].length);
        if (actual != cases[i].value) {
            fprintf(stderr, "Morse case %u decoded %c, expected %c\n",
                    i, actual, cases[i].value);
            return 1;
        }
    }
    if (MTRAIN_DecodeSymbol(0U, 0U) != '?' ||
        MTRAIN_DecodeSymbol(0U, 7U) != '?' ||
        MTRAIN_DecodeSymbol(0x3FU, 6U) != '?') return 1;
    /* Memory text: prosigns in either case, runs of spaces one word space,
     * none at either end; anything else unknown refuses the whole text. */
    {
        uint8_t c[16];
        const uint8_t n = MORSE_ParseText("  cq  de <sk><Ar> 5nn ", c, sizeof(c));
        if (n != 12U) { fprintf(stderr, "parse: %u codes\n", n); return 1; }
        if (c[0] != MORSE_Code('C') || c[2] != 0U || c[5] != 0U || c[6] != MORSE_Code(MORSE_SK) ||
            c[7] != MORSE_Code('+') || c[8] != 0U || c[9] != MORSE_Code('5') || c[11] != MORSE_Code('N')) {
            fprintf(stderr, "parse: wrong codes\n");
            return 1;
        }
        if (MORSE_ParseText("<bt>", c, sizeof(c)) != 1U || c[0] != MORSE_Code('=')) {
            fprintf(stderr, "parse: <BT>\n");
            return 1;
        }
        if (MORSE_ParseText("CQ #", c, sizeof(c)) || MORSE_ParseText("<XX>", c, sizeof(c)) ||
            MORSE_ParseText("<SK", c, sizeof(c)) || MORSE_ParseText("ABCDE", c, 4U) ||
            MORSE_ParseText("   ", c, sizeof(c))) {
            fprintf(stderr, "parse: took a text it should refuse\n");
            return 1;
        }
    }
    puts("Morse production table: PASS");
    return 0;
}
