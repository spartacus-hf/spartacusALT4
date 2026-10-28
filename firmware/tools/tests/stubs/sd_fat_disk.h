#ifndef SD_FAT_DISK_H
#define SD_FAT_DISK_H

#include "mod_storage_store_priv.h"
#include "mod_cal_storage_priv.h"
#include "mod_sd_store.h"
#include <stdint.h>

extern uint8_t fat_card_present;
extern uint32_t fat_uid[3];
extern uint8_t fat_muted;
extern uint32_t fat_events;
extern uint8_t fat_last_event_code;
extern uint8_t fat_wrote_outside;
extern settings_blob_t fat_applied;
extern uint32_t fat_applied_count;

void fat_disk_format(void);
uint8_t *fat_disk_sector(uint32_t lba);
uint8_t fat_put_file(const char *path, const void *data, uint32_t len);
uint8_t fat_mkdir(const char *path);
uint8_t fat_rm(const char *path);
/* The card goes away after this many more store-sector writes (a power cut
 * or a pulled card part way through an import or a restore). */
extern uint8_t fat_cut_writes_set;
extern uint32_t fat_cut_writes;
uint8_t fat_get_file(const char *path, void *data, uint32_t cap, uint32_t *len);
/* The firmware's I/O gate and transmitter as the calibration store sees them:
 * the gate shut (a retune, a USB audio stream), a retune under way, the
 * radio transmitting; the grants the store's dev layer gave, and the card's
 * initialisation count (a bump: initialised again). */
extern uint8_t fat_gate_closed;
extern uint8_t fat_retune_pending;
extern uint8_t fat_transmitting;
extern uint32_t fat_grants;
extern uint32_t fat_session;
/* RUN WITHOUT CARD chosen; the clock (HAL_GetTick adds the step each call). */
extern uint8_t fat_nocard;
extern uint32_t fat_tick;
extern uint32_t fat_tick_step;

#endif /* SD_FAT_DISK_H */
