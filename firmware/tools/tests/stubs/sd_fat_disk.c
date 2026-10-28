/* A whole SD card for the import suite: a RAM image with an MBR, the radio's
 * system area at LBA 256..767 and a FAT16 volume at LBA 2048 (where the
 * in-radio formatter puts its own), the real FatFs on it through diskio, and
 * the store's SD_StoreDev* on the same image -- so the import, the store and
 * the calibration store run as they do on the radio. The firmware globals
 * and the few services mod_cal_storage.c and mod_storage_import.c reach for
 * are stood in for here. */
#include "sd_fat_disk.h"
#include "radio.h"
#include "board_rev1.h"
#include "../../../Middlewares/Third_Party/FatFs/src/ff.h"
#include "../../../Middlewares/Third_Party/FatFs/src/diskio.h"
#include <stdio.h>
#include <string.h>

#define VOLBASE   2048U
#define VOLSECT   32768U   /* 16 MiB */
#define SPC       4U
#define RESERVED  4U
#define FATSZ     32U
#define ROOTENT   512U

static uint8_t img[(VOLBASE + VOLSECT) * 512U];
static uint8_t buffer[512];
static FATFS fs;
static uint8_t mounted;
uint8_t fat_card_present = 1U;
uint32_t fat_uid[3] = {0xA1A1A1A1UL, 0xB2B2B2B2UL, 0xC3C3C3C3UL};
uint8_t fat_muted;
uint32_t fat_events;
uint8_t fat_last_event_code;
uint8_t fat_wrote_outside;
settings_blob_t fat_applied;
uint32_t fat_applied_count;

/* ---- FatFs on the image ---- */
DSTATUS disk_initialize(BYTE pdrv) { return pdrv ? STA_NOINIT : 0; }
DSTATUS disk_status(BYTE pdrv) { return pdrv ? STA_NOINIT : 0; }
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || (uint64_t)sector + count > VOLBASE + VOLSECT) return RES_PARERR;
    memcpy(buff, img + (size_t)sector * 512U, (size_t)count * 512U);
    return RES_OK;
}
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || (uint64_t)sector + count > VOLBASE + VOLSECT) return RES_PARERR;
    if (sector < VOLBASE) fat_wrote_outside = 1U; /* FatFs never below its volume */
    memcpy(img + (size_t)sector * 512U, buff, (size_t)count * 512U);
    return RES_OK;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv) return RES_PARERR;
    if (cmd == GET_SECTOR_COUNT) *(LBA_t *)buff = VOLBASE + VOLSECT;
    if (cmd == GET_SECTOR_SIZE) *(WORD *)buff = 512U;
    if (cmd == GET_BLOCK_SIZE) *(DWORD *)buff = 1U;
    return RES_OK;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

void fat_disk_format(void)
{
    memset(img, 0, sizeof(img));
    uint8_t *mbr = img;
    uint8_t *e = mbr + 446;
    e[4] = 0x06U;
    put32(e + 8, VOLBASE);
    put32(e + 12, VOLSECT);
    mbr[510] = 0x55U; mbr[511] = 0xAAU;
    uint8_t *b = img + (size_t)VOLBASE * 512U;
    b[0] = 0xEBU; b[1] = 0x3CU; b[2] = 0x90U;
    memcpy(b + 3, "MSWIN4.1", 8);
    put16(b + 11, 512U);
    b[13] = SPC;
    put16(b + 14, RESERVED);
    b[16] = 2U;
    put16(b + 17, ROOTENT);
    put16(b + 19, (uint16_t)VOLSECT);
    b[21] = 0xF8U;
    put16(b + 22, FATSZ);
    put16(b + 24, 63U);
    put16(b + 26, 255U);
    put32(b + 28, VOLBASE);
    b[36] = 0x80U;
    b[38] = 0x29U;
    put32(b + 39, 0x12345678UL);
    memcpy(b + 43, "NO NAME    ", 11);
    memcpy(b + 54, "FAT16   ", 8);
    b[510] = 0x55U; b[511] = 0xAAU;
    for (unsigned f = 0U; f < 2U; ++f) {
        uint8_t *fat = img + (size_t)(VOLBASE + RESERVED + f * FATSZ) * 512U;
        fat[0] = 0xF8U; fat[1] = 0xFFU; fat[2] = 0xFFU; fat[3] = 0xFFU;
    }
    mounted = (uint8_t)(f_mount(&fs, "", 1U) == FR_OK);
}

uint8_t *fat_disk_sector(uint32_t lba) { return img + (size_t)lba * 512U; }

uint8_t fat_put_file(const char *path, const void *data, uint32_t len)
{
    FIL f;
    UINT bw = 0U;
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return 0U;
    const uint8_t ok = (uint8_t)(f_write(&f, data, len, &bw) == FR_OK && bw == len);
    return (uint8_t)((f_close(&f) == FR_OK) && ok);
}

uint8_t fat_cut_writes_set;
uint32_t fat_cut_writes;

uint8_t fat_rm(const char *path)
{
    return (uint8_t)(f_unlink(path) == FR_OK);
}

uint8_t fat_mkdir(const char *path)
{
    const FRESULT r = f_mkdir(path);
    return (uint8_t)(r == FR_OK || r == FR_EXIST);
}

uint8_t fat_get_file(const char *path, void *data, uint32_t cap, uint32_t *len)
{
    FIL f;
    UINT br = 0U;
    if (f_open(&f, path, FA_READ) != FR_OK) return 0U;
    const uint8_t ok = (uint8_t)(f_read(&f, data, cap, &br) == FR_OK);
    (void)f_close(&f);
    if (len) *len = br;
    return ok;
}

/* ---- What mod_sdcard_service.c gives the import and the store ---- */
uint8_t SD_IsMounted(void) { return (uint8_t)(mounted && fat_card_present); }
void SD_DriverHold(uint8_t on) { (void)on; }

uint8_t SD_DiagReadFile(const char *path, uint32_t offset, void *data, uint32_t len, uint32_t *out_len)
{
    FIL f;
    UINT br = 0U;
    if (out_len) *out_len = 0U;
    if (!SD_IsMounted() || f_open(&f, path, FA_READ) != FR_OK) return 0U;
    FRESULT r = f_lseek(&f, offset);
    if (r == FR_OK) r = f_read(&f, data, len, &br);
    (void)f_close(&f);
    if (out_len) *out_len = br;
    return (uint8_t)(r == FR_OK);
}

uint8_t SD_DiagWriteFile(const char *path, uint32_t offset, const void *data, uint32_t len, uint8_t truncate)
{
    FIL f;
    UINT bw = 0U;
    if (!SD_IsMounted() || f_open(&f, path, truncate ? (FA_CREATE_ALWAYS | FA_WRITE) : (FA_OPEN_ALWAYS | FA_WRITE)) != FR_OK)
        return 0U;
    FRESULT r = f_lseek(&f, offset);
    if (r == FR_OK) r = f_write(&f, data, len, &bw);
    if (f_close(&f) != FR_OK) r = FR_DISK_ERR;
    return (uint8_t)(r == FR_OK && bw == len);
}

uint8_t SD_StoreDevRead(uint32_t lba, uint8_t *buf)
{
    if (!fat_card_present || lba >= VOLBASE + VOLSECT) return 0U;
    memcpy(buf, img + (size_t)lba * 512U, 512U);
    return 1U;
}

uint8_t SD_StoreDevWrite(uint32_t lba, const uint8_t *buf)
{
    if (fat_cut_writes_set) {
        if (fat_cut_writes == 0U) fat_card_present = 0U;
        else --fat_cut_writes;
    }
    if (!fat_card_present) return 0U;
    if (lba < SD_SYSTEM_FIRST_LBA || lba >= SD_SYSTEM_END_LBA) {
        fat_wrote_outside = 1U;
        return 0U;
    }
    memcpy(img + (size_t)lba * 512U, buf, 512U);
    return 1U;
}

uint8_t *SD_StoreDevBuffer(void) { return buffer; }
uint8_t SD_StoreDevAvailable(void) { return fat_card_present; }
uint8_t fat_gate_closed;
uint8_t fat_retune_pending;
uint8_t fat_transmitting;
uint32_t fat_grants;
uint32_t fat_session;
static uint8_t fat_granted;
uint8_t SD_StoreDevIoAllowed(void) { return (uint8_t)(!fat_gate_closed || fat_granted); }
uint32_t SD_StoreDevSession(void) { return fat_session; }
void SD_StoreDevGrantIo(uint8_t on)
{
    if (on && !fat_granted) ++fat_grants;
    fat_granted = on ? 1U : 0U;
}
void SD_StoreDevHold(uint8_t on) { (void)on; }
void SD_StoreDevUid(uint32_t uid[3]) { memcpy(uid, fat_uid, sizeof(fat_uid)); }
uint32_t SD_StoreDevEntropy(void) { static uint32_t x = 0x51ED270BUL; return x = x * 1664525UL + 1013904223UL; }
uint32_t SD_StoreDevFirmware(void) { return 809U; }
uint8_t SD_StoreDevSupplyOk(void) { return 1U; }
void SD_StoreDevEvent(uint8_t code, uint32_t v1, uint32_t v2) { (void)v1; (void)v2; ++fat_events; fat_last_event_code = code; }

/* ---- The firmware around mod_cal_storage.c and mod_storage_import.c ---- */
uint8_t active_band_idx;
volatile uint8_t carrier_values_capped;
volatile sideband_t active_sideband_enum;
phase_cal_t phase_cal_active;
phase_cal_t *active_cal = &phase_cal_active;
float theta_linearity_pd[PHASE_CAL_BINS];
uint16_t active_cal_flags;
ssb_analytic_backend_t cfg_ssb_analytic_backend;
int16_t cfg_branch_ac_offset_ticks;

void refresh_active_cal_ptr(void) { active_cal = &phase_cal_active; }
int16_t mod_branch_ac_limit_ticks(void) { return 1000; }
uint8_t cal_backend_storage_key(uint8_t backend)
{
    if (backend == (uint8_t)SSB_ANALYTIC_WEAVER) return (uint8_t)SSB_ANALYTIC_WEAVER;
    if (backend == (uint8_t)SSB_ANALYTIC_H12P) return (uint8_t)SSB_ANALYTIC_H12P;
    return (uint8_t)SSB_ANALYTIC_HILBERT;
}
uint8_t MOD_GetMuted(void) { return fat_muted; }
void MOD_SetMuted(uint8_t muted) { fat_muted = muted ? 1U : 0U; }
void MOD_EventLog(uint8_t kind, uint8_t code, uint16_t aux, uint32_t v1, uint32_t v2)
{
    (void)kind; (void)aux; (void)v1; (void)v2;
    ++fat_events;
    fat_last_event_code = code;
}
uint8_t MOD_BlockingIoAllowed(uint32_t now) { (void)now; return (uint8_t)!fat_gate_closed; }
uint8_t MOD_CarrierFrequencyApplyPending(void) { return fat_retune_pending; }
volatile uint8_t carrier_clock_retune_in_progress;
radio_state_t RADIO_State(void) { return fat_transmitting ? RADIO_TRANSMIT : RADIO_RECEIVE; }
uint8_t BOARD_RfActive(void) { return fat_transmitting; }
/* mod_storage.c's: the store looks at a card initialised again. */
uint8_t MOD_StorageReconnect(void)
{
    if (SD_StoreState() != SD_STORE_READY || !fat_card_present) return 0U;
    if (!SD_StoreNeedsReopen()) return 1U;
    return (uint8_t)(SD_StoreOpen() == SD_STORE_READY);
}
uint8_t fat_nocard;
uint8_t MOD_StorageRunsWithoutCard(void) { return fat_nocard; }
void APP_WatchdogProgress(void) {}
uint32_t fat_tick;
uint32_t fat_tick_step = 1000U;
uint32_t HAL_GetTick(void) { return fat_tick += fat_tick_step; }

/* mod_storage.c's side: the settings land in slot 0 as the firmware's
 * MOD_SaveSettings puts them there. */
uint8_t storage_settings_apply_save(const settings_blob_t *b)
{
    settings_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.rec_magic = SETTINGS_REC_MAGIC;
    rec.blob = *b;
    fat_applied = *b;
    ++fat_applied_count;
    return SD_StoreWrite(0U, &rec, sizeof(rec));
}
uint8_t MOD_LoadSettings(void) { return 1U; }
/* The Morse trainer's learner record (mod_storage.c): a backup entry for its
 * slot is taken when it carries the record's magic ("MTRP"). */
uint8_t MOD_TrainerRecordAccept(const void *data, uint32_t len)
{
    const uint8_t *b = (const uint8_t *)data;
    return (uint8_t)(len >= 4U && b[0] == 'M' && b[1] == 'T' && b[2] == 'R' && b[3] == 'P');
}
void storage_trainer_forget(void) {}
