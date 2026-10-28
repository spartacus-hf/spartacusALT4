/* The SD block driver's decisions: what is retried, what re-initialises the
 * card, what is reported at once (mod_sdcard_policy.h), and the CRCs it
 * checks and sends. */
#include "mod_sdcard_policy.h"
#include "mod_sd_store.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t crc16_block(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0U;
    for (uint32_t i = 0U; i < len; i++) crc = sd_crc16_step(crc, data[i]);
    return crc;
}

/* Attempts a failing transfer gets under the policy, and how many card
 * re-initialisations among them. */
static void attempts(sd_error_t err, uint8_t quick, unsigned *tries, unsigned *reinits)
{
    *tries = 0U;
    *reinits = 0U;
    for (uint8_t attempt = 0U;; ++attempt) {
        ++*tries;
        const sd_retry_t next = sd_retry_policy(err, attempt, quick);
        if (next == SD_RETRY_NONE) break;
        if (next == SD_RETRY_REINIT) ++*reinits;
        assert(*tries < 10U);
    }
}

int main(void)
{
    /* SD Physical Layer spec 4.5: 512 bytes of 0xFF have the CRC16 0x7FA1. */
    uint8_t block[512];
    memset(block, 0xFF, sizeof(block));
    assert(crc16_block(block, sizeof(block)) == 0x7FA1U);
    /* CRC-16/XMODEM check value. */
    assert(crc16_block((const uint8_t *)"123456789", 9U) == 0x31C3U);

    unsigned tries, reinits;
    /* Success and the errors that a retry cannot help end at once. */
    const sd_error_t final_errs[] = {SD_OK, SD_ERR_ABORTED, SD_ERR_NOT_READY, SD_ERR_PARAM, SD_ERR_NO_CARD};
    for (unsigned i = 0U; i < sizeof(final_errs) / sizeof(final_errs[0]); ++i) {
        for (uint8_t quick = 0U; quick < 2U; ++quick) {
            attempts(final_errs[i], quick, &tries, &reinits);
            assert(tries == 1U && reinits == 0U);
        }
    }
    /* A CRC error: three plain attempts, never a re-init, local audio or not. */
    for (uint8_t quick = 0U; quick < 2U; ++quick) {
        attempts(SD_ERR_CRC, quick, &tries, &reinits);
        assert(tries == 3U && reinits == 0U);
    }
    /* A timeout or card error: again, then re-init and once more -- but
     * never a re-init inside a local-audio read. */
    const sd_error_t card_errs[] = {SD_ERR_TIMEOUT, SD_ERR_IO, SD_ERR_STATE, SD_ERR_WP};
    for (unsigned i = 0U; i < sizeof(card_errs) / sizeof(card_errs[0]); ++i) {
        attempts(card_errs[i], 0U, &tries, &reinits);
        assert(tries == 3U && reinits == 1U);
        attempts(card_errs[i], 1U, &tries, &reinits);
        assert(tries == 2U && reinits == 0U);
    }

    assert(sd_status_ok(0U, 0U));
    assert(!sd_status_ok(0U, 0x04U)); /* card ECC failed */
    assert(!sd_status_ok(0x01U, 0U));

    /* The system area: 256..767, and nothing else. */
    assert(!SD_SystemAreaOverlaps(0UL, 256UL));
    assert(SD_SystemAreaOverlaps(0UL, 257UL));
    assert(SD_SystemAreaOverlaps(255UL, 2UL));
    assert(SD_SystemAreaOverlaps(767UL, 1UL));
    assert(!SD_SystemAreaOverlaps(768UL, 64UL));
    assert(!SD_SystemAreaOverlaps(300UL, 0UL));
    assert(SD_SystemAreaOverlaps(0UL, 0xFFFFFFFFUL));
    assert(!SD_SystemAreaOverlaps(0xFFFFFFF0UL, 0x10UL));

    printf("sd_driver: PASS\n");
    return 0;
}
