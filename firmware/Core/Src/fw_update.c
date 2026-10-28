#pragma GCC optimize ("Os")

#include "fw_update.h"
#include "fw_update_format.h"
#include "mod_sdcard_service.h"
#include "modulator.h"
#include "radio.h"
#include "sd_audio_player.h"
#include "main.h"
#include "../../Middlewares/Third_Party/FatFs/src/ff.h"
#include <stddef.h>
#include <string.h>

#define FW_PACKAGE_FILE "REV1FW.UPD"
#define FW_MAP_FILE     "FWMAP.BIN"

/* Both flash writers below arm the PVD with FW_PVD_LEVEL; fw_update_format.h
 * says why it must not be level 7. */
_Static_assert(FW_PVD_LEVEL != PWR_CR2_PLS,
               "PVD level 7 selects the external PVD_IN pin, not a VDD threshold");

static uint8_t confirm_finished __attribute__((section(".ccmram_bss")));

/* A one-byte FatFs read uses its sector cache and records the actual disk LBA
 * in FIL.sect. Whole-sector direct reads bypass that field. */
static uint8_t read_file_sector(FIL *file, uint8_t *bytes, uint32_t *lba)
{
    UINT done;
    if ((f_tell(file) & 511U) != 0U ||
        f_read(file, bytes, 1U, &done) != FR_OK || done != 1U) return 0U;
    *lba = file->sect;
    if (*lba == 0U || *lba >= SD_GetBlockCount()) return 0U;
    return (f_read(file, bytes + 1U, 511U, &done) == FR_OK && done == 511U) ? 1U : 0U;
}

static uint8_t write_map_sector(FIL *file, uint32_t *crc, uint8_t *map_sector)
{
    UINT done;
    if (f_write(file, map_sector, FW_SECTOR_BYTES, &done) != FR_OK ||
        done != FW_SECTOR_BYTES) return 0U;
    *crc = fw_crc32_continue(*crc, map_sector, FW_SECTOR_BYTES);
    memset(map_sector, 0, FW_SECTOR_BYTES);
    return 1U;
}

static uint8_t image_vectors_valid(const uint8_t *bytes, uint32_t size)
{
    uint32_t sp, pc;
    memcpy(&sp, bytes, sizeof(sp));
    memcpy(&pc, bytes + 4U, sizeof(pc));
    return (sp >= 0x20000000UL && sp <= 0x20018000UL && (sp & 7U) == 0U &&
            (pc & 1U) != 0U && (pc & ~1UL) >= FW_APP_BASE &&
            (pc & ~1UL) < FW_APP_BASE + size) ? 1U : 0U;
}

static fw_update_result_t check_package(uint8_t make_map, fw_journal_t *journal)
{
    FIL package, map;
    uint8_t update_sector[FW_SECTOR_BYTES];
    uint8_t *map_sector;
    uint32_t lba, crc = 0xFFFFFFFFUL, map_crc = 0xFFFFFFFFUL;
    uint8_t map_open = 0U;
    fw_update_result_t result = FW_UPDATE_IO_ERROR;

    if (SD_UsbMscActive() || !MOD_BlockingIoAllowed(HAL_GetTick()) ||
        SD_ServiceOperation() != SD_SERVICE_OP_IDLE || RADIO_CalActive()) return FW_UPDATE_BUSY;
    if (SD_UsbMscUnsafeExit()) return FW_UPDATE_UNSAFE_MSC;
    if (!SD_IsMounted()) return FW_UPDATE_NO_CARD;
    map_sector = SD_UpdateWorkSector();
    if (f_open(&package, FW_PACKAGE_FILE, FA_READ) != FR_OK) return FW_UPDATE_NO_FILE;
    if (f_size(&package) < 1024U || (f_size(&package) & 511U) != 0U ||
        f_size(&package) > (FW_MAP_ENTRIES * FW_SECTOR_BYTES)) {
        result = FW_UPDATE_BAD_HEADER;
        goto done_package;
    }
    if (!read_file_sector(&package, update_sector, &lba)) goto done_package;
    fw_package_header_t header;
    memcpy(&header, update_sector, sizeof(header));
    uint32_t saved_crc = header.header_crc32;
    header.header_crc32 = 0U;
    if (header.magic != FW_PACKAGE_MAGIC || header.format != FW_FORMAT_VERSION ||
        !fw_abi_fits_layout(header.min_loader_abi) ||
        header.image_base != FW_APP_BASE ||
        header.image_bytes < 512U || header.image_bytes > FW_FOOTER_ADDR - FW_APP_BASE ||
        f_size(&package) != 512U + ((header.image_bytes + 511U) & ~511U) ||
        fw_crc32(&header, sizeof(header)) != saved_crc) {
        result = FW_UPDATE_BAD_HEADER;
        goto done_package;
    }
    if (header.board_id != FW_BOARD_ID) {
        result = FW_UPDATE_WRONG_BOARD;
        goto done_package;
    }
    if (make_map) {
        if (f_open(&map, FW_MAP_FILE, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) goto done_package;
        map_open = 1U;
        memset(map_sector, 0, FW_SECTOR_BYTES);
        memcpy(map_sector, &lba, sizeof(lba));
    }
    const uint32_t count = 1U + (header.image_bytes + 511U) / 512U;
    uint32_t remaining = header.image_bytes;
    for (uint32_t n = 1U; n < count; n++) {
        if (!read_file_sector(&package, update_sector, &lba)) goto done_package;
        if (n == 1U && !image_vectors_valid(update_sector, header.image_bytes)) {
            result = FW_UPDATE_BAD_IMAGE;
            goto done_package;
        }
        uint32_t take = remaining > 512U ? 512U : remaining;
        crc = fw_crc32_continue(crc, update_sector, take);
        remaining -= take;
        if (make_map) {
            memcpy(map_sector + (n % 128U) * 4U, &lba, sizeof(lba));
            if ((n % 128U) == 127U && !write_map_sector(&map, &map_crc, map_sector)) goto done_package;
        }
        APP_WatchdogProgress();
    }
    if (remaining != 0U || ~crc != header.image_crc32) {
        result = FW_UPDATE_BAD_IMAGE;
        goto done_package;
    }
    if (make_map) {
        if ((count % 128U) != 0U && !write_map_sector(&map, &map_crc, map_sector)) goto done_package;
        for (uint32_t n = (count + 127U) / 128U; n < FW_MAP_SECTORS; n++) {
            if (!write_map_sector(&map, &map_crc, map_sector)) goto done_package;
        }
        if (f_sync(&map) != FR_OK || f_close(&map) != FR_OK) goto done_package;
        map_open = 0U;
        if (f_open(&map, FW_MAP_FILE, FA_READ) != FR_OK) goto done_package;
        map_open = 1U;
        uint32_t check_crc = 0xFFFFFFFFUL;
        for (uint32_t n = 0U; n < FW_MAP_SECTORS; n++) {
            if (!read_file_sector(&map, map_sector, &lba)) goto done_package;
            journal->map_lba[n] = lba;
            check_crc = fw_crc32_continue(check_crc, map_sector, FW_SECTOR_BYTES);
        }
        if (~map_crc != ~check_crc || f_size(&map) != FW_MAP_SECTORS * FW_SECTOR_BYTES) goto done_package;
        journal->magic = FW_JOURNAL_MAGIC;
        journal->format = FW_FORMAT_VERSION;
        journal->board_id = FW_BOARD_ID;
        journal->image_bytes = header.image_bytes;
        journal->image_crc32 = header.image_crc32;
        journal->firmware_version = header.firmware_version;
        journal->package_sectors = count;
        journal->map_crc32 = ~map_crc;
        journal->header_crc32 = saved_crc;
        journal->record_crc32 = fw_crc32(journal, offsetof(fw_journal_t, record_crc32));
        journal->commit = FW_JOURNAL_COMMIT;
    }
    result = FW_UPDATE_OK;
done_package:
    if (map_open && f_close(&map) != FR_OK) result = FW_UPDATE_IO_ERROR;
    if (f_close(&package) != FR_OK) result = FW_UPDATE_IO_ERROR;
    return result;
}

static uint8_t loader_ready(void)
{
    const fw_loader_info_t *info = (const fw_loader_info_t *)FW_LOADER_INFO_ADDR;
    if ((FLASH->OPTR & FLASH_OPTR_DBANK) == 0U ||
        (FLASH->OPTR & FLASH_OPTR_BFB2) != 0U ||
        info->magic != FW_LOADER_INFO_MAGIC || info->abi < FW_LOADER_ABI ||
        info->app_base != FW_APP_BASE || info->loader_bytes != FW_LOADER_BYTES ||
        fw_crc32(info, offsetof(fw_loader_info_t, crc32)) != info->crc32) return 0U;
    return 1U;
}

static uint8_t loader_protected(void)
{
    return ((FLASH->WRP1AR & FLASH_WRP1AR_WRP1A_STRT) == 0U &&
            ((FLASH->WRP1AR & FLASH_WRP1AR_WRP1A_END) >> FLASH_WRP1AR_WRP1A_END_Pos) == 1U &&
            !fw_wrp_overlaps(FLASH->WRP1BR, 2U, 127U) &&
            !fw_wrp_overlaps(FLASH->WRP2AR, 0U, FW_BANK2_LAST_PAGE) &&
            !fw_wrp_overlaps(FLASH->WRP2BR, 0U, FW_BANK2_LAST_PAGE)) ? 1U : 0U;
}

static uint8_t current_footer_valid(void)
{
    const fw_image_footer_t *footer = (const fw_image_footer_t *)FW_FOOTER_ADDR;
    return (footer->magic == FW_FOOTER_MAGIC && footer->board_id == FW_BOARD_ID &&
            footer->image_base == FW_APP_BASE &&
            footer->image_bytes >= 512U && footer->image_bytes <= FW_FOOTER_ADDR - FW_APP_BASE &&
            fw_crc32(footer, offsetof(fw_image_footer_t, footer_crc32)) == footer->footer_crc32 &&
            fw_crc32((const void *)FW_APP_BASE, footer->image_bytes) == footer->image_crc32) ? 1U : 0U;
}

const char *FW_UpdateBootFault(void)
{
    if (!loader_ready()) return "LOADER MISSING";
    if (!current_footer_valid()) return "APP/FOOTER BAD";
    return NULL;
}

static uint8_t journal_matches_current(void)
{
    const fw_journal_t *journal = (const fw_journal_t *)FW_JOURNAL_ADDR;
    const fw_image_footer_t *footer = (const fw_image_footer_t *)FW_FOOTER_ADDR;
    return (journal->commit == FW_JOURNAL_COMMIT && journal->magic == FW_JOURNAL_MAGIC &&
            journal->format == FW_FORMAT_VERSION && journal->board_id == FW_BOARD_ID &&
            fw_crc32(journal, offsetof(fw_journal_t, record_crc32)) == journal->record_crc32 &&
            footer->magic == FW_FOOTER_MAGIC && footer->image_crc32 == journal->image_crc32 &&
            footer->image_bytes == journal->image_bytes &&
            footer->firmware_version == journal->firmware_version) ? 1U : 0U;
}

fw_update_result_t FW_UpdateVerify(void)
{
    return check_package(0U, NULL);
}

fw_update_result_t FW_UpdateInstall(void)
{
    if (!loader_ready()) return FW_UPDATE_NO_LOADER;
    if (!loader_protected()) return FW_UPDATE_UNPROTECTED;
    if (!current_footer_valid()) return FW_UPDATE_BAD_IMAGE;
    if (RADIO_State() == RADIO_TRANSMIT || RADIO_Ptt() || RADIO_CalActive()) return FW_UPDATE_BUSY;

    fw_journal_t journal = {0};
    fw_update_result_t result = check_package(1U, &journal);
    if (result != FW_UPDATE_OK) return result;
    /* The map and package are now closed and verified. Stop RF before the
     * flash journal transaction; the loader rechecks both CRCs before erase. */
    SD_AudioPlayerClose();
    RADIO_BeforeRetune();
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    const uint32_t cr2_saved = PWR->CR2;
    PWR->CR2 = (cr2_saved & ~PWR_CR2_PLS) | PWR_CR2_PVDE | FW_PVD_LEVEL;
    /* The comparator is not ready in the instruction after PVDE rises; the
     * datasheet allows it a few microseconds. This runs once per install, so
     * spend forty. */
    for (volatile uint32_t settle = 0U; settle < 2000U; ++settle) { }
    if ((PWR->SR2 & PWR_SR2_PVDO) != 0U || HAL_FLASH_Unlock() != HAL_OK) {
        PWR->CR2 = cr2_saved;
        RADIO_AfterRetune();
        return FW_UPDATE_FLASH_ERROR;
    }
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t failed_page = 0U;
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.Banks = FLASH_BANK_2;
    erase.Page = (FW_JOURNAL_ADDR - 0x08040000UL) / FW_PAGE_BYTES;
    erase.NbPages = 1U;
    if (HAL_FLASHEx_Erase(&erase, &failed_page) != HAL_OK) goto flash_fail;
    for (uint32_t off = 0U; off < offsetof(fw_journal_t, commit); off += 8U) {
        uint64_t word;
        memcpy(&word, (const uint8_t *)&journal + off, sizeof(word));
        if ((PWR->SR2 & PWR_SR2_PVDO) != 0U ||
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, FW_JOURNAL_ADDR + off, word) != HAL_OK) goto flash_fail;
        APP_WatchdogProgress();
    }
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                          FW_JOURNAL_ADDR + offsetof(fw_journal_t, commit), journal.commit) != HAL_OK) goto flash_fail;
    HAL_FLASH_Lock();
    NVIC_SystemReset();
    for (;;) { }
flash_fail:
    HAL_FLASH_Lock();
    PWR->CR2 = cr2_saved;
    RADIO_AfterRetune();
    return FW_UPDATE_FLASH_ERROR;
}

void FW_UpdateConfirmService(uint32_t now)
{
    if (confirm_finished || now < 2000U || RADIO_State() == RADIO_FAULT ||
        RADIO_State() == RADIO_TRANSMIT || RADIO_CalActive() ||
        !MOD_BackgroundIoAllowed(now)) return;
    if (!journal_matches_current()) {
        confirm_finished = 1U;
        return;
    }
    if (*(const uint64_t *)FW_JOURNAL_CONFIRM_ADDR == FW_CONFIRM_MARK) {
        confirm_finished = 1U;
        return;
    }
    if (*(const uint64_t *)FW_JOURNAL_CONFIRM_ADDR != UINT64_MAX ||
        !loader_ready() || !loader_protected()) return;
    /* Same guard, same reason it must not be level 7 -- see FW_PVD_LEVEL. This
     * copy of the bug was the quieter half: an install that did reach flash
     * would then never be confirmed, and the loader would roll it back after
     * FW_BOOT_ATTEMPTS boots. Only reached in the window between a new image
     * booting and its mark being written, so the settle costs nothing. */
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    const uint32_t cr2_saved = PWR->CR2;
    PWR->CR2 = (cr2_saved & ~PWR_CR2_PLS) | PWR_CR2_PVDE | FW_PVD_LEVEL;
    for (volatile uint32_t settle = 0U; settle < 2000U; ++settle) { }
    if ((PWR->SR2 & PWR_SR2_PVDO) == 0U && HAL_FLASH_Unlock() == HAL_OK) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, FW_JOURNAL_CONFIRM_ADDR,
                              FW_CONFIRM_MARK) == HAL_OK) confirm_finished = 1U;
        HAL_FLASH_Lock();
    }
    PWR->CR2 = cr2_saved;
}

const char *FW_UpdateStateText(void)
{
    if (!loader_ready()) return "LOADER MISSING";
    if (!loader_protected()) return "LOADER UNPROTECTED";
    if (journal_matches_current()) {
        return (*(const uint64_t *)FW_JOURNAL_CONFIRM_ADDR == FW_CONFIRM_MARK) ?
               "CURRENT CONFIRMED" : "CURRENT UNCONFIRMED";
    }
    const fw_journal_t *journal = (const fw_journal_t *)FW_JOURNAL_ADDR;
    if (journal->commit == FW_JOURNAL_COMMIT && journal->magic == FW_JOURNAL_MAGIC &&
        fw_crc32(journal, offsetof(fw_journal_t, record_crc32)) == journal->record_crc32)
        return "STAGED UPDATE";
    return "READY";
}

const char *FW_UpdateResultText(fw_update_result_t result)
{
    static const char *const messages[] = {
        "OK", "RADIO OR STORAGE BUSY", "NO SD CARD", "UNSAFE USB EJECT",
        "REV1FW.UPD MISSING", "BAD PACKAGE HEADER", "WRONG BOARD",
        "BAD IMAGE CRC/VECTOR", "SD IO ERROR", "LOADER MISSING",
        "LOADER NOT PROTECTED", "FLASH ERROR"
    };
    return (result < sizeof(messages) / sizeof(messages[0])) ? messages[result] : "UNKNOWN";
}
