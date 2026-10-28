#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
/**
 * @file    usbd_msc_if.c
 * @brief   USB MSC storage callbacks backed by the raw SD block driver.
 */

#include "usbd_msc_if.h"
#include "modulator.h"
#include "mod_sdcard.h"
#include "mod_sdcard_service.h"
#include "mod_sd_store.h"
#include <stddef.h>

#define STORAGE_LUN_NBR  1U
#define STORAGE_BLK_SIZ  512U

static int8_t STORAGE_Init_FS(uint8_t lun);
static int8_t STORAGE_GetCapacity_FS(uint8_t lun, uint32_t *block_num, uint16_t *block_size);
static int8_t STORAGE_IsReady_FS(uint8_t lun);
static int8_t STORAGE_IsWriteProtected_FS(uint8_t lun);
static int8_t STORAGE_Read_FS(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len);
static int8_t STORAGE_Write_FS(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len);
static int8_t STORAGE_GetMaxLun_FS(void);

static const int8_t STORAGE_Inquirydata_FS[] =
{
    0x00,
    (int8_t)0x80,
    0x02,
    0x02,
    (STANDARD_INQUIRY_DATA_LEN - 5),
    0x00,
    0x00,
    0x00,
    'C', 'H', 'I', 'R', 'E', 'I', 'X', ' ',
    'T', 'X', ' ', 'S', 'D', ' ', 'D', 'I',
    'S', 'K', ' ', ' ', ' ', ' ', ' ', ' ',
    FW_VERSION_INQUIRY_MAJOR, '.',
    FW_VERSION_INQUIRY_MINOR_TENS,
    FW_VERSION_INQUIRY_MINOR_ONES
};

const USBD_StorageTypeDef USBD_MSC_fops_FS =
{
    STORAGE_Init_FS,
    STORAGE_GetCapacity_FS,
    STORAGE_IsReady_FS,
    STORAGE_IsWriteProtected_FS,
    STORAGE_Read_FS,
    STORAGE_Write_FS,
    STORAGE_GetMaxLun_FS,
    (int8_t *)STORAGE_Inquirydata_FS
};

static int8_t storage_fail(void)
{
    return -1;
}

static int8_t storage_ok(void)
{
    return 0;
}

static uint8_t storage_msc_available(void)
{
    return (SD_UsbMscActive() != 0U && SD_IsReady() != 0U) ? 1U : 0U;
}

static uint8_t storage_range_valid(uint32_t blk_addr, uint16_t blk_len)
{
    uint32_t block_count = SD_GetBlockCount();

    if ((block_count == 0UL) || (blk_len == 0U)) return 0U;
    if (blk_addr >= block_count) return 0U;
    if ((uint32_t)blk_len > (block_count - blk_addr)) return 0U;
    return 1U;
}

static int8_t STORAGE_Init_FS(uint8_t lun)
{
    if (lun >= STORAGE_LUN_NBR) return storage_fail();
    return storage_msc_available() ? storage_ok() : storage_fail();
}

static int8_t STORAGE_GetCapacity_FS(uint8_t lun, uint32_t *block_num, uint16_t *block_size)
{
    if ((lun >= STORAGE_LUN_NBR) || (block_num == NULL) || (block_size == NULL)) {
        return storage_fail();
    }
    if (storage_msc_available() == 0U) return storage_fail();

    *block_num = SD_GetBlockCount();
    *block_size = STORAGE_BLK_SIZ;

    return (*block_num != 0UL) ? storage_ok() : storage_fail();
}

static int8_t STORAGE_IsReady_FS(uint8_t lun)
{
    if (lun >= STORAGE_LUN_NBR) return storage_fail();
    return storage_msc_available() ? storage_ok() : storage_fail();
}

static int8_t STORAGE_IsWriteProtected_FS(uint8_t lun)
{
    if (lun >= STORAGE_LUN_NBR) return storage_fail();
    return storage_ok();
}

static int8_t STORAGE_Read_FS(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len)
{
    if ((lun >= STORAGE_LUN_NBR) || (buf == NULL) || (storage_msc_available() == 0U)) {
        SD_UsbMscRecordRead(blk_addr, blk_len, 0U);
        return storage_fail();
    }
    if (storage_range_valid(blk_addr, blk_len) == 0U) {
        SD_UsbMscRecordRead(blk_addr, blk_len, 0U);
        return storage_fail();
    }

    for (uint16_t i = 0U; i < blk_len; i++) {
        if (SD_ReadBlock(blk_addr + (uint32_t)i, &buf[(uint32_t)i * STORAGE_BLK_SIZ]) != SD_OK) {
            SD_UsbMscRecordRead(blk_addr + (uint32_t)i, 1UL, 0U);
            return storage_fail();
        }
    }
    SD_UsbMscRecordRead(blk_addr, blk_len, 1U);
    return storage_ok();
}

static int8_t STORAGE_Write_FS(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len)
{
    if ((lun >= STORAGE_LUN_NBR) || (buf == NULL) || (storage_msc_available() == 0U)) {
        SD_UsbMscRecordWrite(blk_addr, blk_len, 0U);
        return storage_fail();
    }
    if (storage_range_valid(blk_addr, blk_len) == 0U) {
        SD_UsbMscRecordWrite(blk_addr, blk_len, 0U);
        return storage_fail();
    }
    /* The radio's settings and calibration live in raw sectors before the
     * first partition (mod_sd_store.h): a computer may read them, never
     * write them -- not a partition tool, not a stray dd. */
    if (SD_SystemAreaOverlaps(blk_addr, blk_len)) {
        SD_UsbMscRecordWrite(blk_addr, blk_len, 0U);
        return storage_fail();
    }

    for (uint16_t i = 0U; i < blk_len; i++) {
        if (SD_WriteBlock(blk_addr + (uint32_t)i, &buf[(uint32_t)i * STORAGE_BLK_SIZ]) != SD_OK) {
            SD_UsbMscRecordWrite(blk_addr + (uint32_t)i, 1UL, 0U);
            return storage_fail();
        }
    }
    SD_UsbMscRecordWrite(blk_addr, blk_len, 1U);
    return storage_ok();
}

static int8_t STORAGE_GetMaxLun_FS(void)
{
    return (int8_t)(STORAGE_LUN_NBR - 1U);
}
