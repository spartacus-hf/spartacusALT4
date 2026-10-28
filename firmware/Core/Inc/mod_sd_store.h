#ifndef MOD_SD_STORE_H
#define MOD_SD_STORE_H

/* The radio's own area on the SD card: raw sectors between the MBR and the
 * first partition, outside every file system the radio or a computer
 * creates. USB mass storage refuses writes there and the in-radio formatter
 * starts its volume after it.
 *
 * Settings and calibration live here since v8.09 (mod_sd_store.c), in two
 * copies, A and B, of 256 sectors each. Each copy has a header sector, then
 * one slot per record: slot 0 the settings (up to 8 sectors), slots 1..192
 * one sector each (the calibration records, mod_cal_storage.c), slot 193
 * the Morse trainer's learner record (mod_storage.c). A record is
 * written to the copy that does not hold the newest valid version, read
 * back, and only then counts: a write cut by a power loss leaves the other
 * copy -- the last good version -- in place. Nothing here touches the FAT,
 * a directory or a file. */

#include <stdint.h>

#define SD_SYSTEM_FIRST_LBA   256UL
#define SD_SYSTEM_SECTORS     512UL
#define SD_SYSTEM_END_LBA     (SD_SYSTEM_FIRST_LBA + SD_SYSTEM_SECTORS)

#define SD_STORE_COPY_SECTORS     256UL
#define SD_STORE_SETTINGS_SECTORS 8U
#define SD_STORE_SLOTS            194U   /* 0 settings, 1..193 one sector each */
#define SD_STORE_SLOT_TRAINER     193U   /* the Morse trainer's learner record (since v8.10) */
#define SD_STORE_REC_HEADER       24U
#define SD_STORE_SLOT0_MAX        (SD_STORE_SETTINGS_SECTORS * 512U - SD_STORE_REC_HEADER)
#define SD_STORE_SLOT_MAX         (512U - SD_STORE_REC_HEADER)

/* 1 when [lba, lba + count) touches the system area. */
static inline uint8_t SD_SystemAreaOverlaps(uint32_t lba, uint32_t count)
{
    return (uint8_t)(count != 0UL && lba < SD_SYSTEM_END_LBA &&
                     (uint64_t)lba + count > (uint64_t)SD_SYSTEM_FIRST_LBA);
}

typedef enum {
    SD_STORE_NO_CARD = 0, /* no card, or not looked at yet */
    SD_STORE_READY,       /* this radio's store */
    SD_STORE_BLANK,       /* nothing stored on this card yet */
    SD_STORE_FOREIGN,     /* another radio's store (MCU UID differs) */
    SD_STORE_DAMAGED,     /* records without a valid header */
    SD_STORE_LAYOUT,      /* no MBR, GPT, or a partition over the area */
    SD_STORE_IO           /* the area could not be read */
} sd_store_state_t;

typedef enum {
    SD_STORE_REC_NONE = 0, /* no valid record in either copy */
    SD_STORE_REC_OK,
    SD_STORE_REC_ERROR     /* not available now: no store, I/O refused or failed */
} sd_store_rec_t;

/* Where a store's contents came from (header, `storage`). */
#define SD_STORE_ORIGIN_NEW       0x01UL
#define SD_STORE_ORIGIN_NVM       0x02UL /* imported from a v8.08 flash dump */
#define SD_STORE_ORIGIN_FILES     0x04UL /* imported from v8.08 card files */
#define SD_STORE_ORIGIN_RESTORED  0x08UL
#define SD_STORE_ORIGIN_ADOPTED   0x10UL
/* An import or a restore began writing and has not finished (a power cut,
 * a card pulled): the records are a mixture. Set before the first record,
 * cleared after the last; the boot holds while either is set. */
#define SD_STORE_ORIGIN_IMPORTING  0x40000000UL
#define SD_STORE_ORIGIN_RESTORING  0x80000000UL
#define SD_STORE_ORIGIN_INCOMPLETE (SD_STORE_ORIGIN_IMPORTING | SD_STORE_ORIGIN_RESTORING)

typedef struct {
    sd_store_state_t state;
    uint32_t instance;
    uint32_t origin;
    uint32_t created_fw;
    uint32_t reads, writes;      /* records */
    uint32_t verify_failures;    /* a written copy that did not read back */
    uint32_t bad_copies;         /* invalid copies met while the other was good */
    uint32_t repairs;            /* damaged copies the scrub rewrote */
    uint32_t mirrors;            /* copies the scrub made of a record that had one */
    uint32_t io_errors;
    uint16_t scrub_slot;         /* next slot the scrub looks at */
    uint8_t  headers;            /* bit 0: copy A's header valid, bit 1: copy B's */
    uint8_t  degraded;           /* a write failed after its retry */
} sd_store_info_t;

/* One copy of one slot, as `storage list` reports it. */
typedef enum {
    SD_STORE_COPY_EMPTY = 0, /* no record there */
    SD_STORE_COPY_OK,        /* a valid record of this store */
    SD_STORE_COPY_BAD,       /* a record of this store that does not check */
    SD_STORE_COPY_OTHER,     /* a record of an earlier store on this card */
    SD_STORE_COPY_IO         /* the card did not read */
} sd_store_copy_state_t;

typedef struct {
    sd_store_copy_state_t state;
    uint32_t seq;            /* SD_STORE_COPY_OK only */
} sd_store_copy_t;

/* After every mount, and after USB disk mode: look at the card. */
sd_store_state_t SD_StoreOpen(void);
/* The card is gone or handed to USB. */
void SD_StoreClose(void);
sd_store_state_t SD_StoreState(void);
/* The card was initialised again since the store was read (a driver error,
 * a card put back -- perhaps another one): nothing is read or written until
 * SD_StoreOpen has looked at it again. */
uint8_t SD_StoreNeedsReopen(void);
const char *SD_StoreStateName(sd_store_state_t state);
void SD_StoreGetInfo(sd_store_info_t *out);

/* A store of this radio's on the card: fresh (BLANK, or replacing a foreign
 * or damaged one), or adopting the records there (FOREIGN, DAMAGED). */
uint8_t SD_StoreCreate(uint32_t origin);
uint8_t SD_StoreAdopt(void);
/* Origin bits set and cleared in both headers (an import or restore). */
uint8_t SD_StoreSetOrigin(uint32_t set, uint32_t clear);

/* The newest valid record of a slot into `data` (at most `cap` bytes). */
sd_store_rec_t SD_StoreRead(uint16_t slot, void *data, uint32_t cap, uint32_t *len, uint32_t *seq);
/* The version before the newest: the other copy, when both are valid and
 * it is the older. NONE when there is no such copy. For a newest record the
 * caller cannot use (a later firmware's settings, a layout changed under
 * development), so the one before it is not overwritten with defaults. */
sd_store_rec_t SD_StoreReadOlder(uint16_t slot, void *data, uint32_t cap, uint32_t *len, uint32_t *seq);
/* A new version of a slot; 1 once it has been written and read back. */
uint8_t SD_StoreWrite(uint16_t slot, const void *data, uint32_t len);
/* Both copies of a slot invalidated (factory reset). */
uint8_t SD_StoreErase(uint16_t slot);
/* One slot of the background check: a copy that does not read back valid is
 * rewritten from the other. Returns 1 when it rewrote a copy. */
uint8_t SD_StoreScrubStep(void);
/* What each copy of a slot holds (A, B), read now. */
void SD_StoreSlotCopies(uint16_t slot, sd_store_copy_t out[2]);

/* ---- What the store needs from the card (mod_sdcard_service.c; the host
 * suite has its own). ---- */
uint8_t SD_StoreDevRead(uint32_t lba, uint8_t *buf);
uint8_t SD_StoreDevWrite(uint32_t lba, const uint8_t *buf);
uint8_t *SD_StoreDevBuffer(void);    /* 512-byte scratch, main loop only */
uint8_t SD_StoreDevAvailable(void);  /* card ready, not USB disk, not formatting */
uint8_t SD_StoreDevIoAllowed(void);  /* boot window, blocking I/O allowed now, or granted */
uint32_t SD_StoreDevSession(void);  /* changes with every initialisation of the card */
void SD_StoreDevHold(uint8_t on);    /* a write runs whole */
void SD_StoreDevUid(uint32_t uid[3]);
uint32_t SD_StoreDevEntropy(void);
uint32_t SD_StoreDevFirmware(void);
uint8_t SD_StoreDevSupplyOk(void);
void SD_StoreDevEvent(uint8_t code, uint32_t v1, uint32_t v2);
/* Not a hook: the calibration of a band just tuned is read past the I/O gate
 * while TX is locked for it (mod_cal_storage.c), the driver held so the read
 * is not cut short. On and off around the read. */
void SD_StoreDevGrantIo(uint8_t on);

/* Event codes (MOD_EVENT_STORAGE, after the v8.08 ones 1..5). */
enum {
    SD_STORE_EV_VERIFY = 6,   /* a written copy did not read back (v1 slot, v2 lba) */
    SD_STORE_EV_WRITE_FAILED, /* gave up after the retry */
    SD_STORE_EV_REPAIRED,     /* scrub rewrote a damaged copy (v1 slot, v2 copy) */
    SD_STORE_EV_BAD_COPY,     /* reserved: bad copies are counted, and logged once repaired */
    SD_STORE_EV_CREATED,
    SD_STORE_EV_STATE         /* v1: the state the card opened in */
};

#endif /* MOD_SD_STORE_H */
