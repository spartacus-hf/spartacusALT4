#include "ui_format.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;

static void check_text(const char *actual, const char *expected)
{
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "UI format: got \"%s\", expected \"%s\"\n",
                actual, expected);
        failures++;
    }
}

int main(void)
{
    char text[32];

    text[0] = '\0';
    UI_AppendU(text, 1U, UINT8_MAX);
    check_text(text, "0000000001");

    text[0] = '\0';
    UI_AppendI(text, INT32_MIN);
    check_text(text, "-2147483648");

    text[0] = '\0';
    UI_AppendF(text, 12.345f, 2U);
    check_text(text, "12.35");

    text[0] = '\0';
    UI_AppendF(text, NAN, 2U);
    check_text(text, "ERR");

    text[0] = '\0';
    UI_AppendF(text, INFINITY, 2U);
    check_text(text, "ERR");

    text[0] = '\0';
    UI_AppendF(text, -INFINITY, 2U);
    check_text(text, "ERR");

    text[0] = '\0';
    UI_AppendF(text, 0x1p32f, 0U);
    check_text(text, "OVF");

    text[0] = '\0';
    UI_AppendF(text, 1.25f, UINT8_MAX);
    check_text(text, "1.250000000");

    text[0] = '\0';
    UI_AppendHex(text, UINT32_C(0x12345678), UINT8_MAX);
    check_text(text, "12345678");

    if (failures != 0U) return 1;
    puts("UI formatting: finite rounding and invalid-value diagnostics: PASS");
    return 0;
}
