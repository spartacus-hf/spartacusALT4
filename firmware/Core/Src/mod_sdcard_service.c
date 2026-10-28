/* Size over speed: nothing here runs in the RF or audio path. */
#pragma GCC optimize ("Os")

/**
 * @file    mod_sdcard_service.c
 * @brief   SD card subsystem lifecycle, FatFs integration, and background jobs.
 */

#include "mod_sdcard_service.h"
#include "mod_sdcard.h"
#include "mod_sd_store.h"
#include "fw_update_format.h"
#include "modulator.h"
#include "main.h"
#include "../../Middlewares/Third_Party/FatFs/src/ff.h"
#include <string.h>
#include <stdio.h>

#define SD_README_FILE        "README.TXT"

#define SD_FORMAT_VOLBASE     2048UL /* the volume starts past the system area */
#define SD_FORMAT_RSV_SECT    32UL
#define SD_FORMAT_NFATS       2U
#define SD_FORMAT_ROOT_CLST   2UL
#define SD_FORMAT_FSINFO_SEC  1UL
#define SD_FORMAT_BACKUP_SEC  6UL

_Static_assert(SD_FORMAT_VOLBASE >= SD_SYSTEM_END_LBA,
               "the formatter's volume would overwrite the radio's system area");

FATFS g_sd_fs;

typedef enum {
    FMT_IDLE = 0,
    FMT_MBR,
    FMT_BOOT,
    FMT_RESERVED,
    FMT_FAT,
    FMT_ROOT,
    FMT_FINISH,
    FMT_FAILED
} format_phase_t;

typedef struct {
    format_phase_t phase;
    uint32_t card_sectors;
    uint32_t volbase;
    uint32_t vol_sectors;
    uint32_t fat_sectors;
    uint32_t data_start;
    uint32_t clusters;
    uint8_t  sectors_per_cluster;
    uint32_t cursor;
    uint32_t work_done;
    uint32_t work_total;
} sd_format_job_t;

static uint8_t sd_mounted = 0U;
static uint8_t sd_usb_msc_active = 0U;
static FRESULT sd_last_fresult = FR_NOT_READY;
static char sd_last_fatfs_op[12] = "INIT"; /* longest operation is MSCCHK_READ */
static char sd_last_fatfs_path[80] = "";
static uint8_t sd_last_settings_step = 0U;
static uint8_t sd_post_msc_check_pending = 0U;
static uint8_t sd_usb_msc_dirty = 0U;
static uint8_t sd_usb_msc_synced = 1U;
static uint8_t sd_usb_msc_ejected = 0U;
static uint8_t sd_usb_msc_unsafe_exit = 0U;
static uint8_t sd_usb_msc_last_opcode = 0U;
static uint8_t sd_usb_msc_last_sense_key = 0U;
static uint8_t sd_usb_msc_last_sense_asc = 0U;
static uint32_t sd_usb_msc_last_lba = 0UL;
static uint32_t sd_usb_msc_last_blocks = 0UL;
static sd_format_job_t fmt_job;
static uint8_t sector_buf[512];

uint8_t *SD_UpdateWorkSector(void) { return sector_buf; }

extern void SD_SPI1_Init(void);

static uint8_t write_all_file(const char *name, const void *data, uint32_t len);
static uint8_t SD_ProvisionFactoryFiles(void);

/* Set by main.c until the boot hands over to the main loop: nothing that
 * card I/O could disturb runs yet (RF DMA, audio), and the gate below
 * refuses then for the first carrier apply -- which made the boot's card
 * check read HELP.BIN as missing on every boot (bench, 2026-10-01). */
static uint8_t sd_boot_io;

/* Every operation that writes the card runs whole once it has started
 * (SD_DriverHold): reads inside it are not cut short either, so a rename or
 * an append never stops between its directory and FAT updates. Whether it
 * may start is still sd_service_io_allowed's call. */
void SD_ServiceBootIo(uint8_t on)
{
    sd_boot_io = on ? 1U : 0U;
    SD_DriverBootIo(sd_boot_io); /* the driver's own abort test, the same window */
}

static uint8_t store_io_granted; /* SD_StoreDevGrantIo */

static uint8_t sd_service_io_allowed(void)
{
    return (uint8_t)(sd_boot_io || store_io_granted || MOD_BlockingIoAllowed(HAL_GetTick()));
}

static void st_word(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void st_dword(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint8_t write_sector(uint32_t lba, const uint8_t *src)
{
    return (SD_WriteBlock(lba, src) == SD_OK) ? 1U : 0U;
}

static void copy_file_name(char *dst, const char *src, uint32_t cap)
{
    uint32_t i = 0UL;
    if (dst == NULL || cap == 0UL) return;
    if (src != NULL) {
        while ((i + 1UL) < cap && src[i] != 0) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = 0;
}

static void sd_record_fresult(const char *op, const char *path, FRESULT res)
{
    copy_file_name(sd_last_fatfs_op, op, sizeof(sd_last_fatfs_op));
    copy_file_name(sd_last_fatfs_path, path, sizeof(sd_last_fatfs_path));
    sd_last_fresult = res;
}

static uint8_t sd_post_msc_sanity_check(void)
{
    DIR dir;
    FILINFO info;
    FRESULT res;

    if (sd_post_msc_check_pending == 0U) return 1U;
    if (sd_mounted == 0U) return 0U;
    if (sd_service_io_allowed() == 0U) return 0U;

    memset(&info, 0, sizeof(info));
    res = f_opendir(&dir, "");
    sd_record_fresult("MSCCHK_OPEN", "", res);
    if (res != FR_OK) return 0U;

    res = f_readdir(&dir, &info);
    (void)f_closedir(&dir);
    sd_record_fresult("MSCCHK_READ", "", res);
    if (res != FR_OK) return 0U;

    sd_post_msc_check_pending = 0U;
    return 1U;
}

static uint8_t sd_write_zero_sector(uint32_t lba)
{
    memset(sector_buf, 0, sizeof(sector_buf));
    return write_sector(lba, sector_buf);
}

static uint8_t choose_spc(uint32_t vol_sectors)
{
    uint32_t mib = vol_sectors / 2048UL;
    if (mib <= 260UL) return 1U;
    if (mib <= 8192UL) return 8U;
    if (mib <= 16384UL) return 16U;
    if (mib <= 32768UL) return 32U;
    return 64U;
}

static uint8_t format_geometry(sd_format_job_t *j)
{
    uint32_t fat_sectors = 1UL;

    memset(j, 0, sizeof(*j));
    j->card_sectors = SD_GetBlockCount();
    if (j->card_sectors < (SD_FORMAT_VOLBASE + 131072UL)) return 0U;

    j->volbase = SD_FORMAT_VOLBASE;
    j->vol_sectors = j->card_sectors - j->volbase;
    j->sectors_per_cluster = choose_spc(j->vol_sectors);

    for (uint8_t i = 0U; i < 12U; i++) {
        uint32_t overhead = SD_FORMAT_RSV_SECT + (uint32_t)SD_FORMAT_NFATS * fat_sectors;
        if (j->vol_sectors <= overhead) return 0U;
        uint32_t data_sectors = j->vol_sectors - overhead;
        uint32_t clusters = data_sectors / (uint32_t)j->sectors_per_cluster;
        uint32_t next_fat = (((clusters + 2UL) * 4UL) + 511UL) / 512UL;
        if (next_fat == fat_sectors) {
            j->clusters = clusters;
            break;
        }
        fat_sectors = next_fat;
        j->clusters = clusters;
    }

    if (j->clusters < 65525UL) return 0U;
    j->fat_sectors = fat_sectors;
    j->data_start = j->volbase + SD_FORMAT_RSV_SECT +
                    (uint32_t)SD_FORMAT_NFATS * j->fat_sectors;

    j->work_total = 1UL + 4UL + 28UL +
                    (uint32_t)SD_FORMAT_NFATS * j->fat_sectors +
                    (uint32_t)j->sectors_per_cluster;
    return 1U;
}

static void build_mbr(const sd_format_job_t *j)
{
    memset(sector_buf, 0, sizeof(sector_buf));
    uint8_t *p = &sector_buf[446];
    p[0] = 0x00U;
    p[1] = 0xFFU; p[2] = 0xFFU; p[3] = 0xFFU;
    p[4] = 0x0CU;
    p[5] = 0xFFU; p[6] = 0xFFU; p[7] = 0xFFU;
    st_dword(&p[8], j->volbase);
    st_dword(&p[12], j->vol_sectors);
    sector_buf[510] = 0x55U;
    sector_buf[511] = 0xAAU;
}

static void build_boot_sector(const sd_format_job_t *j)
{
    memset(sector_buf, 0, sizeof(sector_buf));
    sector_buf[0] = 0xEBU;
    sector_buf[1] = 0x58U;
    sector_buf[2] = 0x90U;
    memcpy(&sector_buf[3], "CHIREIX ", 8);
    st_word(&sector_buf[11], 512U);
    sector_buf[13] = j->sectors_per_cluster;
    st_word(&sector_buf[14], (uint16_t)SD_FORMAT_RSV_SECT);
    sector_buf[16] = SD_FORMAT_NFATS;
    st_word(&sector_buf[17], 0U);
    st_word(&sector_buf[19], 0U);
    sector_buf[21] = 0xF8U;
    st_word(&sector_buf[22], 0U);
    st_word(&sector_buf[24], 63U);
    st_word(&sector_buf[26], 255U);
    st_dword(&sector_buf[28], j->volbase);
    st_dword(&sector_buf[32], j->vol_sectors);
    st_dword(&sector_buf[36], j->fat_sectors);
    st_word(&sector_buf[40], 0U);
    st_word(&sector_buf[42], 0U);
    st_dword(&sector_buf[44], SD_FORMAT_ROOT_CLST);
    st_word(&sector_buf[48], (uint16_t)SD_FORMAT_FSINFO_SEC);
    st_word(&sector_buf[50], (uint16_t)SD_FORMAT_BACKUP_SEC);
    sector_buf[64] = 0x80U;
    sector_buf[66] = 0x29U;
    st_dword(&sector_buf[67], 0x65550001UL);
    memcpy(&sector_buf[71], "CHIREIXSD  ", 11);
    memcpy(&sector_buf[82], "FAT32   ", 8);
    sector_buf[510] = 0x55U;
    sector_buf[511] = 0xAAU;
}

static void build_fsinfo(const sd_format_job_t *j)
{
    memset(sector_buf, 0, sizeof(sector_buf));
    st_dword(&sector_buf[0], 0x41615252UL);
    st_dword(&sector_buf[484], 0x61417272UL);
    st_dword(&sector_buf[488], (j->clusters > 0UL) ? (j->clusters - 1UL) : 0xFFFFFFFFUL);
    st_dword(&sector_buf[492], 3UL);
    st_dword(&sector_buf[508], 0xAA550000UL);
}

static void build_first_fat_sector(void)
{
    memset(sector_buf, 0, sizeof(sector_buf));
    st_dword(&sector_buf[0], 0x0FFFFFF8UL);
    st_dword(&sector_buf[4], 0x0FFFFFFFUL);
    st_dword(&sector_buf[8], 0x0FFFFFFFUL);
}

static uint8_t format_step_boot(sd_format_job_t *j)
{
    switch (j->cursor) {
        case 0UL:
            build_boot_sector(j);
            if (!write_sector(j->volbase, sector_buf)) return 0U;
            break;
        case 1UL:
            build_fsinfo(j);
            if (!write_sector(j->volbase + SD_FORMAT_FSINFO_SEC, sector_buf)) return 0U;
            break;
        case 2UL:
            build_boot_sector(j);
            if (!write_sector(j->volbase + SD_FORMAT_BACKUP_SEC, sector_buf)) return 0U;
            break;
        case 3UL:
            build_fsinfo(j);
            if (!write_sector(j->volbase + SD_FORMAT_BACKUP_SEC + 1UL, sector_buf)) return 0U;
            break;
        default:
            j->phase = FMT_RESERVED;
            j->cursor = 0UL;
            return 1U;
    }
    j->cursor++;
    j->work_done++;
    return 1U;
}

static uint8_t format_step_reserved(sd_format_job_t *j)
{
    static const uint8_t skip[] = {0U, 1U, 6U, 7U};
    while (j->cursor < SD_FORMAT_RSV_SECT) {
        uint8_t do_skip = 0U;
        for (uint8_t i = 0U; i < (uint8_t)(sizeof(skip) / sizeof(skip[0])); i++) {
            if (j->cursor == skip[i]) do_skip = 1U;
        }
        if (do_skip) {
            j->cursor++;
            continue;
        }
        if (!sd_write_zero_sector(j->volbase + j->cursor)) return 0U;
        j->cursor++;
        j->work_done++;
        return 1U;
    }
    j->phase = FMT_FAT;
    j->cursor = 0UL;
    return 1U;
}

static uint8_t format_step_fat(sd_format_job_t *j)
{
    uint32_t total = (uint32_t)SD_FORMAT_NFATS * j->fat_sectors;
    if (j->cursor >= total) {
        j->phase = FMT_ROOT;
        j->cursor = 0UL;
        return 1U;
    }

    uint32_t sector = j->volbase + SD_FORMAT_RSV_SECT + j->cursor;
    if ((j->cursor % j->fat_sectors) == 0UL) {
        build_first_fat_sector();
        if (!write_sector(sector, sector_buf)) return 0U;
    } else {
        if (!sd_write_zero_sector(sector)) return 0U;
    }
    j->cursor++;
    j->work_done++;
    return 1U;
}

static uint8_t format_step_root(sd_format_job_t *j)
{
    if (j->cursor >= (uint32_t)j->sectors_per_cluster) {
        j->phase = FMT_FINISH;
        j->cursor = 0UL;
        return 1U;
    }
    if (!sd_write_zero_sector(j->data_start + j->cursor)) return 0U;
    j->cursor++;
    j->work_done++;
    return 1U;
}

void SD_ServiceInit(void)
{
    SD_SPI1_Init();
    SD_BrowserInit();
    SD_ServiceRefresh();
}

void SD_ServiceRefresh(void)
{
    if (sd_usb_msc_active != 0U) return;
    if (fmt_job.phase != FMT_IDLE && fmt_job.phase != FMT_FAILED) return;
    if (fmt_job.phase == FMT_FAILED) fmt_job.phase = FMT_IDLE;
    if (sd_service_io_allowed() == 0U) return;

    SD_BrowserReset();
    (void)f_mount(NULL, "", 0U);
    sd_mounted = 0U;
    memset(&g_sd_fs, 0, sizeof(g_sd_fs));
    SD_Deinit();

    const sd_error_t init = SD_Init();
    if (init != SD_OK) {
        /* An initialisation the real-time gate cut short says nothing about
         * the card: the store keeps its state (it does no I/O while the
         * driver is not ready) and the next refresh finishes the job. Closed,
         * it showed the card lost for 2-9 s after every such cut (bench,
         * 2026-10-02). */
        if (init != SD_ERR_ABORTED) SD_StoreClose();
        sd_last_fresult = FR_NOT_READY;
        sd_record_fresult("MOUNT", "", FR_NOT_READY);
        return;
    }
    /* The settings store is raw sectors before the first partition: it is
     * read whether or not the file system mounts. The store's headers and
     * the volume's boot sectors are a handful of reads, held to the end: cut
     * short, the store read as unreadable and the volume as gone. */
    SD_DriverHold(1U);
    (void)SD_StoreOpen();

    sd_last_fresult = f_mount(&g_sd_fs, "", 1U);
    SD_DriverHold(0U);
    sd_record_fresult("MOUNT", "", sd_last_fresult);
    if (sd_last_fresult == FR_OK) {
        sd_mounted = 1U;
        if (SD_BrowserOpen("/") != 0U && sd_post_msc_check_pending != 0U) {
            (void)sd_post_msc_sanity_check();
        }
    }
}

uint8_t SD_UsbMscEnter(void)
{
    if (sd_usb_msc_active != 0U) return 1U;
    if (fmt_job.phase != FMT_IDLE && fmt_job.phase != FMT_FAILED) return 0U;
    if (fmt_job.phase == FMT_FAILED) fmt_job.phase = FMT_IDLE;
    if (sd_service_io_allowed() == 0U) return 0U;

    if ((SD_IsReady() == 0U) && (SD_Init() != SD_OK)) return 0U;

    SD_BrowserReset();
    (void)f_mount(NULL, "", 0U);
    sd_mounted = 0U;
    sd_last_fresult = FR_NOT_READY;
    SD_StoreClose(); /* the computer has the card; back on exit (SD_ServiceRefresh) */

    sd_usb_msc_active = 1U;
    sd_usb_msc_dirty = 0U;
    sd_usb_msc_synced = 1U;
    sd_usb_msc_ejected = 0U;
    return 1U;
}

void SD_UsbMscExit(void)
{
    if (sd_usb_msc_active == 0U) return;
    if (sd_usb_msc_dirty != 0U && sd_usb_msc_synced == 0U && sd_usb_msc_ejected == 0U) {
        sd_usb_msc_unsafe_exit = 1U;
    }
    sd_post_msc_check_pending = 1U;
    sd_usb_msc_active = 0U;
    sd_mounted = 0U;
    SD_BrowserReset();
    if (sd_service_io_allowed() != 0U) SD_ServiceRefresh();
}

uint8_t SD_UsbMscActive(void)
{
    return sd_usb_msc_active;
}

const char *SD_ServiceLastFatFsOp(void) { return sd_last_fatfs_op; }
const char *SD_ServiceLastFatFsPath(void) { return sd_last_fatfs_path; }
uint8_t SD_ServiceLastFResult(void) { return (uint8_t)sd_last_fresult; }
uint8_t SD_ServiceLastSettingsStep(void) { return sd_last_settings_step; }
uint8_t SD_UsbMscDirty(void) { return sd_usb_msc_dirty; }
uint8_t SD_UsbMscUnsafeExit(void) { return sd_usb_msc_unsafe_exit; }
uint8_t SD_UsbMscLastOpcode(void) { return sd_usb_msc_last_opcode; }
uint8_t SD_UsbMscLastSenseKey(void) { return sd_usb_msc_last_sense_key; }
uint8_t SD_UsbMscLastSenseAsc(void) { return sd_usb_msc_last_sense_asc; }
uint32_t SD_UsbMscLastLba(void) { return sd_usb_msc_last_lba; }
uint32_t SD_UsbMscLastBlocks(void) { return sd_usb_msc_last_blocks; }

void SD_UsbMscRecordCommand(uint8_t opcode, uint32_t lba, uint32_t blocks)
{
    sd_usb_msc_last_opcode = opcode;
    sd_usb_msc_last_lba = lba;
    sd_usb_msc_last_blocks = blocks;
}

void SD_UsbMscRecordSense(uint8_t key, uint8_t asc)
{
    sd_usb_msc_last_sense_key = key;
    sd_usb_msc_last_sense_asc = asc;
}

void SD_UsbMscRecordRead(uint32_t lba, uint32_t blocks, uint8_t ok)
{
    sd_usb_msc_last_lba = lba;
    sd_usb_msc_last_blocks = blocks;
    if (ok == 0U) sd_usb_msc_synced = 0U;
}

void SD_UsbMscRecordWrite(uint32_t lba, uint32_t blocks, uint8_t ok)
{
    sd_usb_msc_last_lba = lba;
    sd_usb_msc_last_blocks = blocks;
    sd_usb_msc_dirty = 1U;
    sd_usb_msc_synced = 0U;
    sd_usb_msc_ejected = 0U;
    if (ok == 0U) sd_usb_msc_unsafe_exit = 1U;
}

uint8_t SD_UsbMscSynchronizeCache(void)
{
    if (sd_usb_msc_active == 0U) return 0U;
    if (SD_IsReady() == 0U || SD_GetLastError() != SD_OK) return 0U;
    sd_usb_msc_dirty = 0U;
    sd_usb_msc_synced = 1U;
    return 1U;
}

uint8_t SD_UsbMscHostEject(void)
{
    if (SD_UsbMscSynchronizeCache() == 0U) return 0U;
    sd_usb_msc_ejected = 1U;
    sd_usb_msc_unsafe_exit = 0U;
    return 1U;
}

void SD_ServiceBackground(void)
{
    if (sd_usb_msc_active != 0U) return;
    if (fmt_job.phase == FMT_IDLE || fmt_job.phase == FMT_FAILED) {
        SD_FileJobService();
        return;
    }
    if (sd_service_io_allowed() == 0U) return;

    uint8_t ok = 1U;
    switch (fmt_job.phase) {
        case FMT_MBR:
            build_mbr(&fmt_job);
            ok = write_sector(0UL, sector_buf);
            fmt_job.phase = ok ? FMT_BOOT : FMT_FAILED;
            fmt_job.cursor = 0UL;
            fmt_job.work_done++;
            break;
        case FMT_BOOT:
            ok = format_step_boot(&fmt_job);
            break;
        case FMT_RESERVED:
            ok = format_step_reserved(&fmt_job);
            break;
        case FMT_FAT:
            ok = format_step_fat(&fmt_job);
            break;
        case FMT_ROOT:
            ok = format_step_root(&fmt_job);
            break;
        case FMT_FINISH:
            if (sd_service_io_allowed() == 0U) return;
            fmt_job.phase = FMT_IDLE;
            SD_ServiceRefresh();
            if (sd_mounted != 0U) {
                (void)SD_ProvisionFactoryFiles();
            }
            break;
        default:
            break;
    }

    if (!ok) {
        fmt_job.phase = FMT_FAILED;
        sd_mounted = 0U;
        memset(&g_sd_fs, 0, sizeof(g_sd_fs));
    }
}

uint8_t SD_IsMounted(void)
{
    if (sd_usb_msc_active != 0U) return 0U;
    return sd_mounted;
}

sd_service_op_t SD_ServiceOperation(void)
{
    if (fmt_job.phase != FMT_IDLE && fmt_job.phase != FMT_FAILED) return SD_SERVICE_OP_FORMAT;
    if (SD_FileJobActive() != 0U) return SD_SERVICE_OP_FILE_JOB;
    return SD_SERVICE_OP_IDLE;
}

uint8_t SD_ServiceProgressPct(void)
{
    if (fmt_job.phase == FMT_IDLE) return 100U;
    if (fmt_job.work_total == 0UL) return 0U;
    uint32_t pct = (fmt_job.work_done * 100UL) / fmt_job.work_total;
    return (pct > 100UL) ? 100U : (uint8_t)pct;
}

uint8_t SD_FormatFactoryStart(void)
{
    if (sd_usb_msc_active != 0U) return 0U;
    if (fmt_job.phase != FMT_IDLE && fmt_job.phase != FMT_FAILED) return 0U;
    if (sd_service_io_allowed() == 0U) return 0U;
    sd_mounted = 0U;
    memset(&g_sd_fs, 0, sizeof(g_sd_fs));
    SD_StoreClose(); /* the formatter never writes the store's sectors; reopened after */

    if (SD_Init() != SD_OK) return 0U;
    if (!format_geometry(&fmt_job)) {
        fmt_job.phase = FMT_FAILED;
        return 0U;
    }

    fmt_job.phase = FMT_MBR;
    fmt_job.cursor = 0UL;
    fmt_job.work_done = 0UL;
    return 1U;
}

uint32_t SD_ServiceCapacityMiB(void)
{
    return SD_GetBlockCount() / 2048UL;
}

const char *SD_ServiceStatusString(void)
{
    if (sd_usb_msc_active != 0U) return "USB DISK";
    if (fmt_job.phase == FMT_FAILED) return "FORMAT FAIL";
    if (SD_ServiceOperation() == SD_SERVICE_OP_FORMAT) return "FORMATTING";
    if (sd_post_msc_check_pending != 0U) return "CHECKING SD";
    if (sd_usb_msc_unsafe_exit != 0U) return "MSC UNSAFE";
    if (sd_mounted != 0U) return "MOUNTED";
    if (SD_IsReady() != 0U) {
        return (sd_last_fresult == FR_NO_FILESYSTEM) ? "RAW CARD" : "MOUNT FAIL";
    }
    return SD_GetStatusString();
}

static uint8_t write_all_file_body(const char *name, const void *data, uint32_t len)
{
    FIL fp;
    UINT bw = 0U;
    if (sd_service_io_allowed() == 0U) return 0U;
    FRESULT res = f_open(&fp, name, FA_CREATE_ALWAYS | FA_WRITE);
    sd_record_fresult("OPENW", name, res);
    if (res != FR_OK) return 0U;
    res = f_write(&fp, data, (UINT)len, &bw);
    sd_record_fresult("WRITE", name, res);
    if (res == FR_OK) res = f_sync(&fp);
    sd_record_fresult("SYNC", name, res);
    FRESULT close_res = f_close(&fp);
    if (close_res != FR_OK) res = FR_DISK_ERR;
    sd_record_fresult("CLOSE", name, (close_res == FR_OK) ? res : close_res);
    return (res == FR_OK && bw == (UINT)len) ? 1U : 0U;
}

static uint8_t write_all_file(const char *name, const void *data, uint32_t len)
{
    SD_DriverHold(1U);
    const uint8_t ok = write_all_file_body(name, data, len);
    SD_DriverHold(0U);
    return ok;
}

static uint8_t mkdir_root_83(const char *name)
{
    if (sd_service_io_allowed() == 0U) return 0U;
    FRESULT res = f_mkdir(name);
    sd_record_fresult("MKDIR", name, res);
    return (res == FR_OK || res == FR_EXIST) ? 1U : 0U;
}

__attribute__((cold)) uint8_t SD_ExportLines(const char *stem, char *name, uint16_t lines, sd_export_line_fn line)
{
    char path[24];
    char text[200]; /* an export header line runs to 171 characters */
    FIL fp;
    UINT bw = 0U;
    if (sd_mounted == 0U || sd_usb_msc_active != 0U || SD_ServiceOperation() != SD_SERVICE_OP_IDLE) return 0U;
    if (!mkdir_root_83("EXPORTS")) return 0U;
    /* Keep earlier exports (before/after re-tuning runs); the last number is reused. */
    uint16_t n = 0U;
    for (; n < 999U; ++n) {
        (void)snprintf(path, sizeof(path), "EXPORTS/%.3s%03u.CSV", stem, (unsigned)n);
        if (f_stat(path, NULL) == FR_NO_FILE) break;
    }
    (void)snprintf(path, sizeof(path), "EXPORTS/%.3s%03u.CSV", stem, (unsigned)n);
    if (sd_service_io_allowed() == 0U) return 0U;
    FRESULT res = f_open(&fp, path, FA_CREATE_ALWAYS | FA_WRITE);
    sd_record_fresult("OPENW", path, res);
    if (res != FR_OK) return 0U;
    for (uint16_t i = 0U; i < lines && res == FR_OK; ++i) {
        uint16_t length = line(i, text, (uint16_t)sizeof(text));
        if (length >= sizeof(text)) length = (uint16_t)(sizeof(text) - 1U);
        res = f_write(&fp, text, length, &bw);
        if (res == FR_OK && bw != length) res = FR_DENIED;
    }
    sd_record_fresult("WRITE", path, res);
    if (res == FR_OK) res = f_sync(&fp);
    FRESULT close_res = f_close(&fp);
    if (close_res != FR_OK) res = FR_DISK_ERR;
    sd_record_fresult("CLOSE", path, res);
    if (res != FR_OK) return 0U;
    if (name) (void)snprintf(name, 24U, "%s", path);
    return 1U;
}

/* ---- Generic bounded file access for the self test and the USB console ----
 *
 * The browser in sd_file_browser.c walks directories and views files for the
 * on-screen file manager; these are the plain byte-level operations a host
 * needs to pull a log off the card or push a settings file back. Every one of
 * them is main-loop only and refuses while the card is handed to USB mass
 * storage or a background job owns it, which is the same gate SD_ExportLines
 * uses. Paths are FatFs paths from the card root, with no leading slash. */

static uint8_t diag_path_ok(const char *path)
{
    if (!path || !path[0] || path[0] == '/') return 0U;
    unsigned n = 0U;
    for (const char *p = path; *p; ++p, ++n) {
        if (n >= SD_BROWSER_PATH_MAX) return 0U;
        /* No parent traversal and no control characters: a host may not walk
         * out of the card, and FatFs has no business with raw control bytes. */
        if ((unsigned char)*p < 0x20U) return 0U;
        if (*p == '.' && p[1] == '.') return 0U;
    }
    return 1U;
}

uint8_t SD_DiagIoAllowed(void)
{
    return (uint8_t)(sd_mounted != 0U && sd_usb_msc_active == 0U && sd_service_io_allowed() != 0U);
}

uint8_t SD_DiagStat(const char *path, uint32_t *size, uint8_t *attr)
{
    FILINFO info;
    if (sd_mounted == 0U || sd_usb_msc_active != 0U || !diag_path_ok(path)) return 0U;
    if (sd_service_io_allowed() == 0U) return 0U;
    FRESULT res = f_stat(path, &info);
    sd_record_fresult("STAT", path, res);
    if (res != FR_OK) return 0U;
    if (size) *size = (uint32_t)info.fsize;
    if (attr) *attr = (uint8_t)info.fattrib;
    return 1U;
}

uint8_t SD_DiagReadFile(const char *path, uint32_t offset, void *data, uint32_t len, uint32_t *out_len)
{
    FIL fp;
    UINT br = 0U;
    if (out_len) *out_len = 0U;
    if (sd_mounted == 0U || sd_usb_msc_active != 0U || !diag_path_ok(path) || !data || !len) return 0U;
    if (sd_service_io_allowed() == 0U) return 0U;
    FRESULT res = f_open(&fp, path, FA_READ);
    sd_record_fresult("OPENR", path, res);
    if (res != FR_OK) return 0U;
    res = f_lseek(&fp, offset);
    if (res == FR_OK) res = f_read(&fp, data, len, &br);
    sd_record_fresult("READ", path, res);
    (void)f_close(&fp);
    if (res != FR_OK) return 0U;
    if (out_len) *out_len = (uint32_t)br;
    return 1U;
}

/* Two spans of one file in one open: a FatFs open is the costly part on a
 * card (its directory search, 4-55 ms on the bench card), and the settings
 * help needs its pair table and its text for every line it shows. */
/* FNV-1a of a file from `offset` to its end, in one open (HELP.BIN's check). */
uint8_t SD_DiagFileFnv1a(const char *path, uint32_t offset, uint32_t *hash)
{
    FIL fp;
    uint8_t chunk[64];
    UINT br = 0U;
    uint32_t h = 0x811C9DC5UL;
    if (sd_mounted == 0U || sd_usb_msc_active != 0U || !diag_path_ok(path) || !hash) return 0U;
    if (sd_service_io_allowed() == 0U) return 0U;
    FRESULT res = f_open(&fp, path, FA_READ);
    if (res != FR_OK) return 0U;
    res = f_lseek(&fp, offset);
    while (res == FR_OK) {
        res = f_read(&fp, chunk, sizeof(chunk), &br);
        if (res != FR_OK || br == 0U) break;
        for (UINT i = 0U; i < br; ++i) h = (h ^ chunk[i]) * 0x01000193UL;
    }
    sd_record_fresult("HASH", path, res);
    (void)f_close(&fp);
    *hash = h;
    return (uint8_t)(res == FR_OK);
}

/* `len` bytes from `offset` handed to `each` a chunk at a time, in one open:
 * the CW pack's word scan reads 16 KB that an open per block would make
 * dozens of opens (cw_pack.c). */
uint8_t SD_DiagReadEach(const char *path, uint32_t offset, uint32_t len, sd_chunk_fn each, void *ctx)
{
    FIL fp;
    uint8_t chunk[64];
    UINT br = 0U;
    if (sd_mounted == 0U || sd_usb_msc_active != 0U || !diag_path_ok(path) || !each) return 0U;
    if (sd_service_io_allowed() == 0U) return 0U;
    FRESULT res = f_open(&fp, path, FA_READ);
    if (res != FR_OK) return 0U;
    res = f_lseek(&fp, offset);
    while (res == FR_OK && len) {
        res = f_read(&fp, chunk, len < sizeof(chunk) ? len : sizeof(chunk), &br);
        if (res == FR_OK && br == 0U) res = FR_INT_ERR; /* the file ends early */
        if (res != FR_OK) break;
        each(chunk, (uint32_t)br, ctx);
        len -= (uint32_t)br;
    }
    sd_record_fresult("READ", path, res);
    (void)f_close(&fp);
    return (uint8_t)(res == FR_OK);
}

uint8_t SD_DiagReadSpans(const char *path, uint32_t off_a, void *a, uint32_t len_a,
                         uint32_t off_b, void *b, uint32_t len_b, uint32_t *got_b)
{
    FIL fp;
    UINT br = 0U;
    if (got_b) *got_b = 0U;
    if (sd_mounted == 0U || sd_usb_msc_active != 0U || !diag_path_ok(path) || !a || !b) return 0U;
    if (sd_service_io_allowed() == 0U) return 0U;
    FRESULT res = f_open(&fp, path, FA_READ);
    sd_record_fresult("OPENR", path, res);
    if (res != FR_OK) return 0U;
    res = f_lseek(&fp, off_a);
    if (res == FR_OK) res = f_read(&fp, a, len_a, &br);
    if (res == FR_OK && br != len_a) res = FR_INT_ERR;
    if (res == FR_OK) res = f_lseek(&fp, off_b);
    if (res == FR_OK) res = f_read(&fp, b, len_b, &br);
    sd_record_fresult("READ", path, res);
    (void)f_close(&fp);
    if (res != FR_OK) return 0U;
    if (got_b) *got_b = br;
    return 1U;
}

/* `truncate` starts the file again from nothing; otherwise the file is opened
 * or created and the write lands at `offset`, so a host can stream a large
 * file in blocks without holding it open across commands. */
static uint8_t diag_write_file(const char *path, uint32_t offset, const void *data, uint32_t len, uint8_t truncate)
{
    FIL fp;
    UINT bw = 0U;
    if (sd_mounted == 0U || sd_usb_msc_active != 0U || !diag_path_ok(path)) return 0U;
    if (len && !data) return 0U;
    if (sd_service_io_allowed() == 0U) return 0U;
    FRESULT res = f_open(&fp, path, truncate ? (FA_CREATE_ALWAYS | FA_WRITE) : (FA_OPEN_ALWAYS | FA_WRITE));
    sd_record_fresult("OPENW", path, res);
    if (res != FR_OK) return 0U;
    /* Seeking past the end leaves a hole FatFs fills with whatever the
     * allocation held, so refuse rather than write an undefined gap. */
    if (res == FR_OK && offset > (uint32_t)f_size(&fp)) res = FR_INVALID_PARAMETER;
    if (res == FR_OK) res = f_lseek(&fp, offset);
    if (res == FR_OK && len) {
        res = f_write(&fp, data, len, &bw);
        if (res == FR_OK && bw != len) res = FR_DENIED;
    }
    if (res == FR_OK) res = f_sync(&fp);
    sd_record_fresult("WRITE", path, res);
    FRESULT close_res = f_close(&fp);
    if (close_res != FR_OK) res = FR_DISK_ERR;
    return res == FR_OK ? 1U : 0U;
}

uint8_t SD_DiagWriteFile(const char *path, uint32_t offset, const void *data, uint32_t len, uint8_t truncate)
{
    SD_DriverHold(1U);
    const uint8_t ok = diag_write_file(path, offset, data, len, truncate);
    SD_DriverHold(0U);
    return ok;
}

static uint8_t diag_delete(const char *path)
{
    if (sd_mounted == 0U || sd_usb_msc_active != 0U || !diag_path_ok(path)) return 0U;
    if (sd_service_io_allowed() == 0U) return 0U;
    FRESULT res = f_unlink(path);
    sd_record_fresult("UNLINK", path, res);
    return res == FR_OK ? 1U : 0U;
}

uint8_t SD_DiagDelete(const char *path)
{
    SD_DriverHold(1U);
    const uint8_t ok = diag_delete(path);
    SD_DriverHold(0U);
    return ok;
}

static uint8_t diag_mkdir(const char *path)
{
    if (sd_mounted == 0U || sd_usb_msc_active != 0U || !diag_path_ok(path)) return 0U;
    if (sd_service_io_allowed() == 0U) return 0U;
    FRESULT res = f_mkdir(path);
    sd_record_fresult("MKDIR", path, res);
    return (res == FR_OK || res == FR_EXIST) ? 1U : 0U;
}

uint8_t SD_DiagMkdir(const char *path)
{
    SD_DriverHold(1U);
    const uint8_t ok = diag_mkdir(path);
    SD_DriverHold(0U);
    return ok;
}

static uint8_t diag_rename(const char *path, const char *to)
{
    if (sd_mounted == 0U || sd_usb_msc_active != 0U || !diag_path_ok(path) || !diag_path_ok(to)) return 0U;
    if (sd_service_io_allowed() == 0U) return 0U;
    FRESULT res = f_rename(path, to);
    sd_record_fresult("RENAME", path, res);
    return res == FR_OK ? 1U : 0U;
}

uint8_t SD_DiagRename(const char *path, const char *to)
{
    SD_DriverHold(1U);
    const uint8_t ok = diag_rename(path, to);
    SD_DriverHold(0U);
    return ok;
}

/* One directory entry by position. Re-opening the directory per entry is
 * O(n^2) over a listing, but it keeps the console stateless -- no handle is
 * held between commands, so a host that stops half way leaves nothing open,
 * and the 12 KiB browser name pool is not involved at all. Directories on
 * this card are small; a 200-entry listing is well under a second. */
uint8_t SD_DiagListEntry(const char *path, uint16_t index, char *name, uint16_t cap,
                         uint32_t *size, uint8_t *attr)
{
    DIR dir;
    FILINFO info;
    if (sd_mounted == 0U || sd_usb_msc_active != 0U || !name || !cap) return 0U;
    if (path && path[0] && !diag_path_ok(path)) return 0U;
    if (sd_service_io_allowed() == 0U) return 0U;
    FRESULT res = f_opendir(&dir, (path && path[0]) ? path : "");
    if (res != FR_OK) { sd_record_fresult("OPENDIR", path ? path : "", res); return 0U; }
    uint8_t found = 0U;
    for (uint16_t seen = 0U; ; ++seen) {
        info.fname[0] = '\0';
        res = f_readdir(&dir, &info);
        if (res != FR_OK || !info.fname[0]) break;
        if (seen != index) continue;
        (void)snprintf(name, cap, "%s", info.fname);
        if (size) *size = (uint32_t)info.fsize;
        if (attr) *attr = (uint8_t)info.fattrib;
        found = 1U;
        break;
    }
    (void)f_closedir(&dir);
    return found;
}

uint8_t SD_DiagFreeMiB(uint32_t *free_mib, uint32_t *total_mib)
{
    FATFS *fs = NULL;
    DWORD clusters = 0UL;
    if (sd_mounted == 0U || sd_usb_msc_active != 0U) return 0U;
    if (sd_service_io_allowed() == 0U) return 0U;
    FRESULT res = f_getfree("", &clusters, &fs);
    sd_record_fresult("GETFREE", "", res);
    if (res != FR_OK || !fs) return 0U;
    const uint32_t sectors_per_mib = 2048UL; /* 512-byte sectors */
    if (free_mib) *free_mib = (uint32_t)((uint64_t)clusters * fs->csize / sectors_per_mib);
    if (total_mib) *total_mib = (uint32_t)((uint64_t)(fs->n_fatent - 2U) * fs->csize / sectors_per_mib);
    return 1U;
}

static uint8_t SD_ProvisionFactoryFiles(void)
{
    static const char readme[] =
        "CHIREIX SD CARD\r\n"
        "FAT16/FAT32 with UTF-8 long file names. exFAT is not supported.\r\n"
        "AUDIO: WAV and MP3 played into the RF modulator: mono or stereo,\r\n"
        "8-48 kHz, up to 24-bit.\r\n"
        "SETTINGS: PC-side config backup area.\r\n"
        "EXPORTS: calibration and measurement exports.\r\n"
        "LOGS: diagnostics captured by future firmware.\r\n"
        "The radio's settings and calibration are not files: they live in\r\n"
        "sectors 256-767, before the first partition. Keep them there.\r\n";

    if (sd_mounted == 0U) return 0U;
    if (sd_service_io_allowed() == 0U) return 0U;

    uint8_t ok = 1U;
    if (!mkdir_root_83("AUDIO"))    ok = 0U;
    if (!mkdir_root_83("SETTINGS")) ok = 0U;
    if (!mkdir_root_83("EXPORTS"))  ok = 0U;
    if (!mkdir_root_83("LOGS"))     ok = 0U;
    if (!write_all_file(SD_README_FILE, readme, (uint32_t)(sizeof(readme) - 1U))) ok = 0U;
    (void)SD_FileManagerRefresh();
    return ok;
}

/* ---- What the settings store needs from the card (mod_sd_store.c) ---------- */

uint8_t SD_StoreDevRead(uint32_t lba, uint8_t *buf)
{
    return (uint8_t)(SD_ReadBlock(lba, buf) == SD_OK);
}

/* The store writes its own sectors and nothing else. */
uint8_t SD_StoreDevWrite(uint32_t lba, const uint8_t *buf)
{
    if (!SD_SystemAreaOverlaps(lba, 1UL) || lba < SD_SYSTEM_FIRST_LBA) return 0U;
    return (uint8_t)(SD_WriteBlock(lba, buf) == SD_OK);
}

uint8_t *SD_StoreDevBuffer(void) { return sector_buf; }

uint8_t SD_StoreDevAvailable(void)
{
    return (uint8_t)(SD_IsReady() && sd_usb_msc_active == 0U &&
                     (fmt_job.phase == FMT_IDLE || fmt_job.phase == FMT_FAILED));
}

/* Set only around the calibration read mod_cal_storage.c makes while TX is
 * locked for it (a remount included, when the card needs one); the driver is
 * held with it, so the gate's closing does not cut the read short. */
void SD_StoreDevGrantIo(uint8_t on)
{
    if ((on ? 1U : 0U) == store_io_granted) return;
    store_io_granted = on ? 1U : 0U;
    SD_DriverHold(store_io_granted);
}

uint8_t SD_StoreDevIoAllowed(void) { return sd_service_io_allowed(); }

uint32_t SD_StoreDevSession(void)
{
    sd_stats_t st;
    SD_GetStats(&st);
    return st.inits;
}

void SD_StoreDevHold(uint8_t on) { SD_DriverHold(on); }

void SD_StoreDevUid(uint32_t uid[3])
{
    uid[0] = HAL_GetUIDw0();
    uid[1] = HAL_GetUIDw1();
    uid[2] = HAL_GetUIDw2();
}

uint32_t SD_StoreDevEntropy(void)
{
    return DWT->CYCCNT ^ (HAL_GetTick() << 12) ^ SysTick->VAL;
}

uint32_t SD_StoreDevFirmware(void)
{
    return (uint32_t)FW_VERSION_MAJOR * 100UL + (uint32_t)FW_VERSION_MINOR;
}

/* VDD above the brownout guard the flash writers use (FW_PVD_LEVEL, ~2.9 V):
 * a store write is not started on a collapsing supply. */
uint8_t SD_StoreDevSupplyOk(void)
{
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    const uint32_t cr2_saved = PWR->CR2;
    PWR->CR2 = (cr2_saved & ~PWR_CR2_PLS) | PWR_CR2_PVDE | FW_PVD_LEVEL;
    for (volatile uint32_t settle = 0U; settle < 2000U; ++settle) { }
    const uint8_t ok = (uint8_t)((PWR->SR2 & PWR_SR2_PVDO) == 0U);
    PWR->CR2 = cr2_saved;
    return ok;
}

void SD_StoreDevEvent(uint8_t code, uint32_t v1, uint32_t v2)
{
    MOD_EventLog(MOD_EVENT_STORAGE, code, 0U, v1, v2);
}

uint8_t SD_FileManagerRefresh(void)
{
    return SD_BrowserRefresh();
}

uint8_t SD_FileManagerCount(void)
{
    return SD_BrowserCount();
}

const sd_file_entry_t *SD_FileManagerEntry(uint8_t idx)
{
    return SD_BrowserEntry(idx);
}

uint8_t SD_FileManagerDelete(uint8_t idx)
{
    return SD_BrowserDeleteStart(idx);
}
