/**
 * @file    integer.h
 * @brief   FatFs integer type definitions.
 *
 * Adapted for STM32G4 (ARM Cortex-M4, 32-bit).
 */
#ifndef _INTEGER_H
#define _INTEGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef int8_t    CHAR;
typedef uint8_t   UCHAR;
typedef uint8_t   BYTE;
typedef int16_t   SHORT;
typedef uint16_t  USHORT;
typedef uint16_t  WORD;
typedef int32_t   INT;
typedef uint32_t  UINT;
typedef int32_t   LONG;
typedef uint32_t  ULONG;
typedef uint32_t  DWORD;
typedef uint64_t  QWORD;

/* Boolean type */
typedef enum { FALSE = 0, TRUE } BOOL;

#ifdef __cplusplus
}
#endif

#endif /* _INTEGER_H */
