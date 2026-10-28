/* A RAM card for the store's host suites (mod_sd_store.c): the first 1024
 * sectors of an SD card behind SD_StoreDev*, with the faults a real card and
 * a real power supply bring -- a write cut short part way through a sector
 * (the "power" stays off until the test turns it back on), a sector that
 * reads back damaged, a card that refuses writes. */
#include "sd_store_disk.h"
#include <string.h>

uint8_t disk[DISK_SECTORS][512];
disk_faults_t faults;
uint32_t disk_writes_total;
uint32_t disk_reads_total;
uint32_t disk_uid[3] = {0x11111111UL, 0x22222222UL, 0x33333333UL};
uint32_t disk_entropy = 0x1234567UL;
uint32_t disk_events;
uint32_t disk_last_event;
uint32_t disk_session;
static uint8_t buffer[512];
static int hold_depth;

uint8_t SD_StoreDevRead(uint32_t lba, uint8_t *buf)
{
    if (!faults.powered || lba >= DISK_SECTORS) return 0U;
    if (faults.read_fail_lba_set && lba == faults.read_fail_lba) return 0U;
    if (faults.not_ready) return 0U;
    if (faults.cut_read_lba_set && lba == faults.cut_read_lba) {
        faults.not_ready = 1U;
        return 0U;
    }
    ++disk_reads_total;
    memcpy(buf, disk[lba], 512U);
    if (faults.flip_lba_set && lba == faults.flip_lba) buf[faults.flip_byte] ^= 0x40U;
    return 1U;
}

uint8_t SD_StoreDevWrite(uint32_t lba, const uint8_t *buf)
{
    if (!faults.powered || lba >= DISK_SECTORS) return 0U;
    if (lba < 256U || lba >= 768U) faults.wrote_outside = 1U;
    if (faults.refuse_writes) return 0U;
    if (faults.cut_after_writes_set) {
        if (faults.cut_after_writes == 0U) {
            /* The power goes as this sector is being programmed: the first
             * cut_bytes reach the card, the rest is whatever the card leaves
             * (here: the old contents with the tail scrambled). */
            memcpy(disk[lba], buf, faults.cut_bytes);
            for (uint32_t i = faults.cut_bytes; i < 512U; ++i) disk[lba][i] ^= 0x5AU;
            faults.powered = 0U;
            return 0U;
        }
        --faults.cut_after_writes;
    }
    ++disk_writes_total;
    memcpy(disk[lba], buf, 512U);
    /* A card maps a sector it could not read elsewhere once it is written. */
    if (faults.read_fail_lba_set && lba == faults.read_fail_lba) faults.read_fail_lba_set = 0U;
    return 1U;
}

uint8_t *SD_StoreDevBuffer(void) { return buffer; }
uint8_t SD_StoreDevAvailable(void) { return faults.powered && !faults.no_card && !faults.not_ready; }
uint8_t SD_StoreDevIoAllowed(void) { return !faults.io_refused; }
uint32_t SD_StoreDevSession(void) { return disk_session; }
void SD_StoreDevHold(uint8_t on) { hold_depth += on ? 1 : -1; }
void SD_StoreDevUid(uint32_t uid[3]) { memcpy(uid, disk_uid, sizeof(disk_uid)); }
uint32_t SD_StoreDevEntropy(void) { return disk_entropy += 0x9E3779B9UL; }
uint32_t SD_StoreDevFirmware(void) { return 809U; }
uint8_t SD_StoreDevSupplyOk(void) { return !faults.low_supply; }
void SD_StoreDevEvent(uint8_t code, uint32_t v1, uint32_t v2)
{
    (void)v1; (void)v2;
    ++disk_events;
    disk_last_event = code;
}

int disk_hold_depth(void) { return hold_depth; }

void disk_reset(void)
{
    memset(disk, 0, sizeof(disk));
    memset(&faults, 0, sizeof(faults));
    faults.powered = 1U;
    disk_writes_total = 0U;
    disk_reads_total = 0U;
    hold_depth = 0;
}

/* An MBR with one FAT32 partition at `start` (the in-radio formatter: 2048). */
void disk_mbr(uint32_t start, uint32_t count, uint8_t type)
{
    memset(disk[0], 0, 512U);
    uint8_t *e = &disk[0][446];
    e[4] = type;
    for (uint8_t i = 0U; i < 4U; ++i) {
        e[8 + i] = (uint8_t)(start >> (8U * i));
        e[12 + i] = (uint8_t)(count >> (8U * i));
    }
    disk[0][510] = 0x55U;
    disk[0][511] = 0xAAU;
}
