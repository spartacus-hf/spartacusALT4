#ifndef MOD_CAL_STORAGE_PRIV_H
#define MOD_CAL_STORAGE_PRIV_H

#include "mod_storage_format_priv.h"

/* Per-band, sideband and analytic-backend calibration in the SD store. */
#ifndef MOD_ENABLE_CAL_STORAGE
#define MOD_ENABLE_CAL_STORAGE 1
#endif


void load_current_calibration_context(void);
uint8_t save_current_band_calibrations(void);
/* The store changed (card replaced, records imported or restored): the next
 * load reads the band's records again. */
void cal_store_forget(void);
/* Factory reset: every calibration record in the store invalidated. */
uint8_t cal_store_erase_all(void);
/* For the import and restore (mod_storage_import.c): a raw record checked
 * (CAL4, CRC) and normalised, the store slot of a key (0: none), and a
 * record written to its slot. */
uint8_t cal_record_accept(const void *raw, ssb_cal_record_t *out);
uint16_t cal_slot_for(uint16_t band_idx, uint8_t sideband, uint8_t backend);
uint8_t cal_store_put(const ssb_cal_record_t *rec);

#endif /* MOD_CAL_STORAGE_PRIV_H */
