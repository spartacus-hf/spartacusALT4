/**
 * @file    mod_sdcard.h
 * @brief   Low-level SD card block driver over SPI1 — public API.
 *
 * This driver handles SD card initialization in SPI mode and provides
 * 512-byte block read/write operations. It is designed to be called from
 * background (non-ISR, non-RF) context only.
 *
 * Pin mapping:
 *   PA4  — SD_CS   (software GPIO)
 *   PA5  — SD_SCK  (SPI1_SCK)
 *   PA6  — SD_MISO (SPI1_MISO)
 *   PA7  — SD_MOSI (SPI1_MOSI)
 */
#ifndef MOD_SDCARD_H
#define MOD_SDCARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ── Public types ─────────────────────────────────────────── */

typedef enum {
    SD_OK = 0,
    SD_ERR_NO_CARD,
    SD_ERR_TIMEOUT,
    SD_ERR_CRC,
    SD_ERR_WP,
    SD_ERR_PARAM,
    SD_ERR_STATE,
    SD_ERR_IO,
    SD_ERR_ABORTED,
    SD_ERR_NOT_READY
} sd_error_t;

typedef enum {
    SD_TYPE_UNKNOWN = 0,
    SD_TYPE_SDV1,
    SD_TYPE_SDV2,
    SD_TYPE_SDV2HC
} sd_type_t;

/* What the driver has met since boot (console `sd status`). */
typedef struct {
    uint32_t inits;        /* successful SD_Init */
    uint32_t crc_errors;   /* block or CSD CRC mismatches, CRC-rejected writes */
    uint32_t timeouts;
    uint32_t io_errors;    /* error tokens, rejected commands, CMD13 status */
    uint32_t retries;      /* transfers tried again */
    uint32_t reinits;      /* re-initialisations inside a retry */
    uint32_t failures;     /* transfers that failed after their retries */
    uint32_t crc_refused;  /* inits where the card refused CMD59 */
    uint32_t aborts;       /* transfers the I/O gate cut short (the card initialised again after) */
    uint32_t reject;       /* the card's last refusal: stage << 24 | byte << 8 | byte (1 command R1,
                            2 data response token, 3 CMD13 R1 and status after a write) */
    uint32_t reject_lba;
    uint8_t  write_fail_run; /* block writes failed in a row (after their retries) */
    uint8_t  crc_on;       /* the card checks CRCs now */
} sd_stats_t;

/* ── Public API ───────────────────────────────────────────── */

/**
 * Initialize the SD card in SPI mode. Must be called once after
 * SPI1 and GPIO have been configured.
 *
 * Returns SD_OK on success, or an error code.
 */
sd_error_t SD_Init(void);

/**
 * Read a single 512-byte block.
 *
 * @param block  Logical block address (LBA).
 * @param dst    Destination buffer (must hold 512 bytes).
 * @return       SD_OK or error code.
 */
sd_error_t SD_ReadBlock(uint32_t block, uint8_t *dst);

/**
 * Write a single 512-byte block.
 *
 * @param block  Logical block address (LBA).
 * @param src    Source buffer (must hold 512 bytes).
 * @return       SD_OK or error code.
 */
sd_error_t SD_WriteBlock(uint32_t block, const uint8_t *src);
/* Through the boot: transfers are not cut short for the I/O gate. */
void SD_DriverBootIo(uint8_t on);
/* A file operation that writes holds the driver for its whole length:
 * nothing in it is cut short for the I/O gate. Nests. */
void SD_DriverHold(uint8_t on);

/**
 * Return the most recent low-level SD error.
 */
sd_error_t SD_GetLastError(void);

void SD_GetStats(sd_stats_t *out);
/* Writes keep failing (SD_WRITE_FAIL_RUN in a row, each after its retries)
 * while the card still reads: a card worn out or gone read-only. Cleared by
 * the next write that lands. */
#define SD_WRITE_FAIL_RUN 3U
uint8_t SD_WritesFailing(void);

/**
 * Return the most recent low-level wait/timeout location.
 */
const char *SD_GetLastWaitString(void);

/**
 * Return the detected card type after a successful SD_Init().
 */
sd_type_t SD_GetType(void);

/**
 * Return the capacity in 512-byte blocks, or 0 if unknown.
 */
uint32_t SD_GetBlockCount(void);

/**
 * Return 1 if a card was successfully initialized, 0 otherwise.
 */
uint8_t SD_IsReady(void);

/**
 * Return a short human-readable status string for UI/displays.
 */
const char *SD_GetStatusString(void);

/**
 * Return a short string for any SD error code.
 */
const char *SD_ErrorString(sd_error_t err);

/**
 * De-initialize / release the card (deselect, re-init on next use).
 */
void SD_Deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* MOD_SDCARD_H */
