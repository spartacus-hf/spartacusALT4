#ifndef MOD_SDCARD_POLICY_H
#define MOD_SDCARD_POLICY_H

/* What the SD block driver does after a block transfer failed: pure, so the
 * host suites can hold it to its word (tools/tests/sd_driver_test.c). */

#include <stdint.h>
#include "mod_sdcard.h"

typedef enum {
    SD_RETRY_NONE = 0,  /* report the error */
    SD_RETRY_AGAIN,     /* the same transfer once more */
    SD_RETRY_REINIT     /* re-initialise the card first, then once more */
} sd_retry_t;

/* Three attempts in all. A CRC error is a bit lost on the wire: try again at
 * once. A timeout or a card error gets one more plain try, then the card is
 * initialised again (SD_Init waits out a card still programming). Inside a
 * local-audio read (`quick`, a 4 ms budget) the card is never re-initialised.
 * A transfer the real-time gate refused, a card that is not ready and a bad
 * argument are not retried at all. */
static inline sd_retry_t sd_retry_policy(sd_error_t err, uint8_t attempt, uint8_t quick)
{
    if (err == SD_OK || err == SD_ERR_ABORTED || err == SD_ERR_NOT_READY ||
        err == SD_ERR_PARAM || err == SD_ERR_NO_CARD) return SD_RETRY_NONE;
    if (attempt >= 2U) return SD_RETRY_NONE;
    if (err == SD_ERR_CRC) return SD_RETRY_AGAIN;
    if (attempt == 0U) return SD_RETRY_AGAIN;
    return quick ? SD_RETRY_NONE : SD_RETRY_REINIT;
}

/* CRC-16/XMODEM, the SD data-block CRC, one byte at a time. */
static inline uint16_t sd_crc16_step(uint16_t crc, uint8_t byte)
{
    crc ^= (uint16_t)((uint16_t)byte << 8);
    for (uint8_t b = 0U; b < 8U; b++) {
        crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* CMD13's R2: the R1 byte and the status byte both all clear. */
static inline uint8_t sd_status_ok(uint8_t r1, uint8_t r2)
{
    return (uint8_t)(r1 == 0U && r2 == 0U);
}

#endif /* MOD_SDCARD_POLICY_H */
