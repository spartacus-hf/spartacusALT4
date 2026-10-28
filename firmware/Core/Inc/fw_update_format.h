#ifndef FW_UPDATE_FORMAT_H
#define FW_UPDATE_FORMAT_H

#include <stdint.h>

/* REV1 dual-bank, 2 KiB/page layout. These constants are shared by the
 * packer, application and write-protected first-stage loader.
 *
 * Layout 2 (loader ABI 2, v8.09): the application runs to the image footer
 * in the last 32 bytes of bank-2 page 126, and the update journal is page
 * 127. Layout 1 (v7.xx-v8.08) ended the application at 0x080747E0 and kept
 * settings (0x08075000) and calibration (0x08076000) in the last 44 KiB;
 * they live on the SD card now (mod_sd_store.c). A layout-1 loader cannot
 * start a layout-2 image and refuses its packages (min_loader_abi 2); a
 * layout-2 loader refuses layout-1 packages (FW_LAYOUT_ABI). The change is
 * made once per radio over SWD or ROM DFU (tools/flash_rev1.py --factory). */
#define FW_LOADER_BASE       0x08000000UL
#define FW_LOADER_BYTES      0x00001000UL
#define FW_APP_BASE          0x08001000UL
#define FW_FOOTER_ADDR       0x0807F7E0UL
#define FW_JOURNAL_ADDR      0x0807F800UL
#define FW_FLASH_END         0x08080000UL
#define FW_PAGE_BYTES        2048UL
#define FW_SECTOR_BYTES      512UL
#define FW_MAP_SECTORS       8U
#define FW_MAP_ENTRIES       1024U
#define FW_BOARD_ID         0x31564552UL /* "REV1" */
#define FW_PACKAGE_MAGIC    0x31445055UL /* "UPD1" */
#define FW_FOOTER_MAGIC     0x31474D49UL /* "IMG1" */
#define FW_JOURNAL_MAGIC    0x314C4E4AUL /* "JNL1" */
#define FW_JOURNAL_COMMIT   0xC04D174D6A35B82FULL
#define FW_CONFIRM_MARK     0xC04F19B69A50717DULL
#define FW_FORMAT_VERSION   1UL
#define FW_LOADER_ABI       2UL
/* The lowest min_loader_abi of a package built for this layout. */
#define FW_LAYOUT_ABI       2UL
/* Bank-2 pages 0..FW_BANK2_LAST_PAGE hold the application and the journal:
 * no write protection may cover any of them. Bank 1: pages 0-1 are the
 * loader (protected), 2..127 application. */
#define FW_BANK2_LAST_PAGE  127U

/* PVD level for the brownout guard around a flash transaction: the highest
 * threshold that actually watches VDD, about 2.9 V against this board's 3.3 V
 * rail. Shared so the loader and the application cannot drift apart.
 *
 * It must never be level 7. On STM32G4 that coding does not raise the
 * threshold; it disconnects the comparator from VDD and points it at the
 * external PVD_IN pin, measured against VREFINT (stm32g4xx_hal_pwr.h calls
 * PWR_PVDLEVEL_7 "External input analog voltage (compared internally to
 * VREFINT)"). PVD_IN is PB7, and on REV1 PB7 is STATUS_LED -- which the
 * loader's own led_init() drives LOW three instructions before arming the PVD,
 * and which BOARD_EarlyInit drives low for the rest of the application's life.
 * With level 7 the guard therefore measured an unlit LED against 1.2 V, called
 * it a brownout, and refused: the application would not write the update
 * journal, and the loader would not erase or program a staged one. Neither
 * half of the SD firmware update path could work. */
#define FW_PVD_LEVEL        PWR_CR2_PLS_LEV6
#define FW_BOOT_ATTEMPTS    8U
#define FW_LOADER_INFO_ADDR (FW_APP_BASE - 32UL)
#define FW_LOADER_INFO_MAGIC 0x31444F4CUL /* "LOD1" */

typedef struct {
    uint32_t magic;
    uint32_t abi;
    uint32_t app_base;
    uint32_t loader_bytes;
    uint32_t crc32;
    uint32_t reserved[3];
} fw_loader_info_t;

typedef struct {
    uint32_t magic;
    uint32_t format;
    uint32_t board_id;
    uint32_t min_loader_abi;
    uint32_t image_base;
    uint32_t image_bytes;
    uint32_t firmware_version;
    uint32_t image_crc32;
    uint32_t header_crc32;
    uint8_t reserved[FW_SECTOR_BYTES - 36U];
} fw_package_header_t;

typedef struct {
    uint32_t magic;
    uint32_t board_id;
    uint32_t image_base;
    uint32_t image_bytes;
    uint32_t firmware_version;
    uint32_t image_crc32;
    uint32_t loader_abi;
    uint32_t footer_crc32;
} fw_image_footer_t;

/* The map file contains one little-endian absolute SD LBA per package sector.
 * Its own eight physical LBAs fit in the committed flash journal. Mutable
 * attempt/confirmation doublewords follow the immutable journal record. */
typedef struct {
    uint32_t magic;
    uint32_t format;
    uint32_t board_id;
    uint32_t image_bytes;
    uint32_t image_crc32;
    uint32_t firmware_version;
    uint32_t package_sectors;
    uint32_t map_crc32;
    uint32_t map_lba[FW_MAP_SECTORS];
    uint32_t header_crc32;
    uint32_t record_crc32;
    uint64_t commit;
} fw_journal_t;

#define FW_JOURNAL_ATTEMPTS_ADDR (FW_JOURNAL_ADDR + sizeof(fw_journal_t))
#define FW_JOURNAL_CONFIRM_ADDR  (FW_JOURNAL_ATTEMPTS_ADDR + FW_BOOT_ATTEMPTS * 8UL)

_Static_assert(sizeof(fw_package_header_t) == 512U, "update header must occupy one sector");
_Static_assert(sizeof(fw_image_footer_t) == 32U, "image footer size changed");
_Static_assert(sizeof(fw_loader_info_t) == 32U, "loader info size changed");
_Static_assert(sizeof(fw_journal_t) < 256U, "journal record grew unexpectedly");
_Static_assert((FW_JOURNAL_ADDR % FW_PAGE_BYTES) == 0U, "journal page alignment");
_Static_assert(FW_FOOTER_ADDR + sizeof(fw_image_footer_t) == FW_JOURNAL_ADDR,
               "footer must end at journal page");
_Static_assert(FW_JOURNAL_CONFIRM_ADDR + 8UL <= FW_JOURNAL_ADDR + FW_PAGE_BYTES,
               "attempts and confirmation must fit the journal page");
_Static_assert(FW_JOURNAL_ADDR + FW_PAGE_BYTES == FW_FLASH_END,
               "the journal is the last page of flash");
_Static_assert((FW_JOURNAL_ADDR - 0x08040000UL) / FW_PAGE_BYTES == FW_BANK2_LAST_PAGE,
               "the journal is bank-2 page FW_BANK2_LAST_PAGE");

/* CRC-32/ISO-HDLC: initial all ones, reflected polynomial, final xor. */
static inline uint32_t fw_crc32_continue(uint32_t crc, const void *bytes, uint32_t count)
{
    /* Two table lookups per byte keep the loader below its 4 KiB limit while
     * avoiding the eight conditional polynomial steps of the original loop. */
    static const uint32_t nibble[16] = {
        0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
        0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
        0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
        0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL
    };
    const uint8_t *p = (const uint8_t *)bytes;
    for (uint32_t i = 0U; i < count; i++) {
        crc ^= p[i];
        crc = (crc >> 4U) ^ nibble[crc & 15U];
        crc = (crc >> 4U) ^ nibble[crc & 15U];
    }
    return crc;
}

static inline uint32_t fw_crc32(const void *bytes, uint32_t count)
{
    return ~fw_crc32_continue(0xFFFFFFFFUL, bytes, count);
}

/* G4 WRP range fields have the same 7-bit positions in each bank/window. */
static inline uint8_t fw_wrp_overlaps(uint32_t reg, uint32_t first, uint32_t last)
{
    uint32_t start = reg & 0x7FUL;
    uint32_t end = (reg >> 16U) & 0x7FUL;
    return (start <= end && start <= last && end >= first) ? 1U : 0U;
}

/* A package or image footer whose minimum loader ABI this loader serves and
 * whose layout is this one: older packages (layout 1, ABI 1) put the image
 * footer where layout 2 has application code, so they are refused -- no
 * downgrade through the SD card -- and newer ones need a newer loader. */
static inline uint8_t fw_abi_fits_layout(uint32_t min_loader_abi)
{
    return (min_loader_abi >= FW_LAYOUT_ABI && min_loader_abi <= FW_LOADER_ABI) ? 1U : 0U;
}

#endif /* FW_UPDATE_FORMAT_H */
