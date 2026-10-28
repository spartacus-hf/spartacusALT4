#ifndef UI_FORMAT_H
#define UI_FORMAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

char *UI_AppendU(char *dst, uint32_t value, uint8_t min_digits);
char *UI_AppendI(char *dst, int32_t value);
char *UI_AppendF(char *dst, float value, uint8_t decimals);
char *UI_AppendHex(char *dst, uint32_t value, uint8_t digits);

#ifdef __cplusplus
}
#endif

#endif
