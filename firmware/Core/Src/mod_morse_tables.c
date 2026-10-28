#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "mod_morse_tables.h"

/* Symbol and code (mod_morse_tables.h): a 1, then the elements, dah = 1. */
typedef struct {
    uint8_t sym;
    uint8_t code;
} morse_entry_t;

static const morse_entry_t morse_table[] = {
    {'A', 0x05U}, {'B', 0x18U}, {'C', 0x1AU}, {'D', 0x0CU}, {'E', 0x02U}, {'F', 0x12U},
    {'G', 0x0EU}, {'H', 0x10U}, {'I', 0x04U}, {'J', 0x17U}, {'K', 0x0DU}, {'L', 0x14U},
    {'M', 0x07U}, {'N', 0x06U}, {'O', 0x0FU}, {'P', 0x16U}, {'Q', 0x1DU}, {'R', 0x0AU},
    {'S', 0x08U}, {'T', 0x03U}, {'U', 0x09U}, {'V', 0x11U}, {'W', 0x0BU}, {'X', 0x19U},
    {'Y', 0x1BU}, {'Z', 0x1CU},
    {'0', 0x3FU}, {'1', 0x2FU}, {'2', 0x27U}, {'3', 0x23U}, {'4', 0x21U},
    {'5', 0x20U}, {'6', 0x30U}, {'7', 0x38U}, {'8', 0x3CU}, {'9', 0x3EU},
    {'.', 0x55U}, {',', 0x73U}, {':', 0x78U}, {'-', 0x61U}, {'/', 0x32U},
    {'=', 0x31U}, {'?', 0x4CU}, {'@', 0x5AU}, {'+', 0x2AU},
    {MORSE_AS, 0x28U}, {MORSE_KA, 0x35U}, {MORSE_KN, 0x36U}, {MORSE_SK, 0x45U},
    {MORSE_VE, 0x22U}, {MORSE_BK, 0xC5U},
};
#define MORSE_ENTRIES (sizeof(morse_table) / sizeof(morse_table[0]))

static const char prosign_names[] = "ASKAKNSKVEBK";

_Static_assert(MORSE_ENTRIES == MORSE_SYMBOLS, "the CW pack's masks index this table");

uint8_t MORSE_Index(uint8_t sym)
{
    if (sym >= 'a' && sym <= 'z') sym = (uint8_t)(sym - ('a' - 'A'));
    for (uint32_t i = 0U; i < MORSE_ENTRIES; ++i)
        if (morse_table[i].sym == sym) return (uint8_t)i;
    return 0xFFU;
}

uint8_t MORSE_SymbolAt(uint8_t index)
{
    return (index < MORSE_ENTRIES) ? morse_table[index].sym : 0U;
}

uint8_t MORSE_Code(uint8_t sym)
{
    const uint8_t i = MORSE_Index(sym);
    return (i < MORSE_ENTRIES) ? morse_table[i].code : 0U;
}

uint8_t MORSE_Symbol(uint8_t code)
{
    for (uint32_t i = 0U; i < MORSE_ENTRIES; ++i)
        if (morse_table[i].code == code) return morse_table[i].sym;
    return 0U;
}

uint8_t MORSE_Length(uint8_t code)
{
    uint8_t n = 0U;
    while (code > 1U) { code >>= 1; ++n; }
    return n;
}

const char *MORSE_Label(uint8_t sym, char buf[5])
{
    if (sym >= MORSE_AS && sym <= MORSE_PROSIGN_LAST) {
        const char *n = &prosign_names[(sym - MORSE_AS) * 2U];
        buf[0] = '<'; buf[1] = n[0]; buf[2] = n[1]; buf[3] = '>'; buf[4] = '\0';
    } else {
        buf[0] = (char)sym; buf[1] = '\0';
    }
    return buf;
}

uint8_t MORSE_Prosign(const char *s)
{
    static const char alias[] = "AR+BT=";
    if (s[2] != '>') return 0U;
    for (uint8_t i = 0U; i <= MORSE_PROSIGN_LAST - MORSE_AS; ++i)
        if ((s[0] & 0xDF) == prosign_names[2U * i] && (s[1] & 0xDF) == prosign_names[2U * i + 1U])
            return (uint8_t)(MORSE_AS + i);
    for (uint8_t i = 0U; i < 6U; i += 3U)
        if ((s[0] & 0xDF) == alias[i] && (s[1] & 0xDF) == alias[i + 1U]) return (uint8_t)alias[i + 2U];
    return 0U;
}

uint8_t MORSE_ParseText(const char *text, uint8_t *code, uint8_t cap)
{
    uint8_t n = 0U;
    for (const char *s = text; *s; ++s) {
        uint8_t c = 0U;
        if (*s == ' ') {
            if (n == 0U || code[n - 1U] == 0U) continue;
        } else {
            uint8_t sym = (uint8_t)*s;
            if (sym == '<' && (sym = MORSE_Prosign(s + 1)) != 0U) s += 3;
            c = MORSE_Code(sym);
            if (c < 2U) return 0U;
        }
        if (n >= cap) return 0U;
        code[n++] = c;
    }
    while (n && code[n - 1U] == 0U) --n;
    return n;
}

const char *MORSE_Pattern(uint8_t sym, char buf[8])
{
    const uint8_t code = MORSE_Code(sym);
    uint8_t n = MORSE_Length(code);
    buf[n] = '\0';
    for (uint8_t i = n; i > 0U; --i) buf[i - 1U] = ((code >> (n - i)) & 1U) ? '-' : '.';
    return buf;
}

char MTRAIN_DecodeSymbol(uint8_t bits, uint8_t len)
{
    if ((len == 0U) || (len > MORSE_ELEMENTS_MAX)) return '?';
    const uint8_t sym = MORSE_Symbol((uint8_t)((1U << len) | (bits & ((1U << len) - 1U))));
    return (sym > MORSE_PROSIGN_LAST) ? (char)sym : '?';
}
