#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
/**
 * @file    ui_format.c
 * @brief   Small numeric text format helpers for the UI.
 */

#include "ui_format.h"

#include <math.h>

char *UI_AppendU(char *dst, uint32_t value, uint8_t min_digits)
{
    char tmp[11];
    uint8_t n = 0U;
    if (min_digits > 10U) min_digits = 10U;
    do {
        tmp[n++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);
    while (n < min_digits) tmp[n++] = '0';
    while (n > 0U) *dst++ = tmp[--n];
    *dst = 0;
    return dst;
}

char *UI_AppendI(char *dst, int32_t value)
{
    if (value < 0) {
        *dst++ = '-';
        return UI_AppendU(dst, 0U - (uint32_t)value, 1U);
    }
    *dst++ = '+';
    return UI_AppendU(dst, (uint32_t)value, 1U);
}

char *UI_AppendF(char *dst, float value, uint8_t decimals)
{
    if (!isfinite(value)) {
        *dst++ = 'E';
        *dst++ = 'R';
        *dst++ = 'R';
        *dst = 0;
        return dst;
    }
    if (value < 0.0f) {
        *dst++ = '-';
        value = -value;
    }
    if (value >= 0x1p32f) {
        *dst++ = 'O';
        *dst++ = 'V';
        *dst++ = 'F';
        *dst = 0;
        return dst;
    }
    if (decimals > 9U) decimals = 9U;

    uint32_t ip = (uint32_t)value;
    float frac = value - (float)ip;
    uint32_t mul = 1U;
    for (uint8_t i = 0U; i < decimals; i++) mul *= 10U;
    uint32_t fp = (uint32_t)(frac * (float)mul + 0.5f);
    if (fp >= mul) {
        fp -= mul;
        ip++;
    }

    dst = UI_AppendU(dst, ip, 1U);
    if (decimals != 0U) {
        *dst++ = '.';
        dst = UI_AppendU(dst, fp, decimals);
    }
    return dst;
}

char *UI_AppendHex(char *dst, uint32_t value, uint8_t digits)
{
    static const char hex[] = "0123456789ABCDEF";
    if (digits == 0U) digits = 1U;
    if (digits > 8U) digits = 8U;
    for (uint8_t i = 0U; i < digits; i++) {
        uint8_t shift = (uint8_t)((digits - 1U - i) * 4U);
        *dst++ = hex[(value >> shift) & 0xFU];
    }
    *dst = 0;
    return dst;
}
