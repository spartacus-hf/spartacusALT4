#ifndef SD_STORE_DISK_H
#define SD_STORE_DISK_H

#include "mod_sd_store.h"
#include <stdint.h>

#define DISK_SECTORS 1024U

typedef struct {
    uint8_t powered;            /* 0: every access fails (power cut) */
    uint8_t no_card;
    uint8_t io_refused;         /* the real-time gate says no */
    uint8_t low_supply;
    uint8_t refuse_writes;
    uint8_t wrote_outside;      /* a write landed outside LBA 256..767 */
    uint8_t cut_after_writes_set;
    uint32_t cut_after_writes;  /* writes that still complete before the cut */
    uint32_t cut_bytes;         /* bytes of the cut sector that land */
    uint8_t flip_lba_set;
    uint32_t flip_lba;          /* this sector reads back with a bit flipped */
    uint32_t flip_byte;
    uint8_t read_fail_lba_set;
    uint32_t read_fail_lba;     /* will not read (the card stays ready) until written */
    uint8_t cut_read_lba_set;
    uint32_t cut_read_lba;      /* the read of this sector is cut short (the I/O
                                 * gate, a timeout): the driver goes not ready */
    uint8_t not_ready;
} disk_faults_t;

extern uint8_t disk[DISK_SECTORS][512];
extern disk_faults_t faults;
extern uint32_t disk_writes_total;
extern uint32_t disk_reads_total;
extern uint32_t disk_uid[3];
extern uint32_t disk_events;
extern uint32_t disk_last_event;
/* The driver's initialisation count: a bump is the card initialised again. */
extern uint32_t disk_session;

void disk_reset(void);
void disk_mbr(uint32_t start, uint32_t count, uint8_t type);
int disk_hold_depth(void);

#endif /* SD_STORE_DISK_H */
