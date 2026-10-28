#include "mod_crc32.h"

/* Persisted-storage CRC. This stays independent of flash/HAL access so the
 * storage format can be exercised by an ordinary host compiler. */
uint32_t crc32_simple(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0U; i < len; i++) {
        crc ^= data[i];
        for (uint32_t bit = 0U; bit < 8U; bit++) {
            uint32_t mask = -(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320UL & mask);
        }
    }
    return ~crc;
}
