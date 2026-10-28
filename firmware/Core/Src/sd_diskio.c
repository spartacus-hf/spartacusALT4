#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
/**
 * @file    sd_diskio.c
 * @brief   FatFs disk I/O bridge — translates disk_read/disk_write/disk_ioctl
 *          to mod_sdcard block operations over SPI1.
 */

#include "../../Middlewares/Third_Party/FatFs/src/ff.h"
#include "../../Middlewares/Third_Party/FatFs/src/diskio.h"
#include "mod_sdcard.h"
#include <string.h>

/* ═══════════════ Local state ═══════════════════════════ */

static volatile DSTATUS sd_status = STA_NOINIT;

/* ═══════════════ Implementation ═══════════════════════ */

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0U) return STA_NOINIT;
    /* A card initialised already (SD_ServiceRefresh did, just before its
     * f_mount) is not initialised again: that doubled the time a mount held
     * the main loop, and a new initialisation makes the settings store look
     * at the card again (SD_StoreNeedsReopen). One that went not ready -- an
     * error, a transfer cut short -- is. */
    if (SD_IsReady()) {
        sd_status = 0U;
        return sd_status;
    }
    if (SD_Init() != SD_OK) {
        sd_status = STA_NOINIT | STA_NODISK;
        return sd_status;
    }
    sd_status = 0U;
    return sd_status;
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0U) return STA_NOINIT;
    if (!SD_IsReady()) return STA_NOINIT | STA_NODISK;
    return 0U;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0U || buff == NULL || count == 0U) return RES_PARERR;
    if (!SD_IsReady()) return RES_NOTRDY;

    for (UINT i = 0U; i < count; i++) {
        if (SD_ReadBlock((uint32_t)(sector + i), &buff[i * 512U]) != SD_OK) {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0U || buff == NULL || count == 0U) return RES_PARERR;
    if (!SD_IsReady()) return RES_NOTRDY;

    for (UINT i = 0U; i < count; i++) {
        if (SD_WriteBlock((uint32_t)(sector + i), &buff[i * 512U]) != SD_OK) {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0U) return RES_PARERR;

    switch (cmd) {
        case CTRL_SYNC:
            /* SD writes are already synchronous — nothing to flush */
            return RES_OK;

        case GET_SECTOR_COUNT:
            if (buff == NULL) return RES_PARERR;
            if (!SD_IsReady()) return RES_NOTRDY;
            *(DWORD *)buff = (DWORD)SD_GetBlockCount();
            return RES_OK;

        case GET_SECTOR_SIZE:
            if (buff == NULL) return RES_PARERR;
            *(WORD *)buff = 512U;
            return RES_OK;

        case GET_BLOCK_SIZE:
            if (buff == NULL) return RES_PARERR;
            *(DWORD *)buff = 1U;  /* SD card erase block — report 1 for simplicity */
            return RES_OK;

        case CTRL_TRIM:
            /* Not supported */
            return RES_OK;

        default:
            return RES_PARERR;
    }
}
