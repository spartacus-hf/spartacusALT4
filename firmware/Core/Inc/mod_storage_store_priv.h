#ifndef MOD_STORAGE_STORE_PRIV_H
#define MOD_STORAGE_STORE_PRIV_H

#include "mod_storage_format_priv.h"

/* Between the settings store (mod_storage.c) and the import, backup and
 * restore of it (mod_storage_import.c). */

/* Run on these settings and write them to the store (with the band's
 * calibration, which settings_apply reloads from the store first). */
uint8_t storage_settings_apply_save(const settings_blob_t *b);
/* A raw settings blob as this image loads it: v26, v27 or v28, magic, length and
 * CRC right (the checks of settings_blob_load_current, nothing copied). */
uint8_t storage_blob_valid(const void *raw_blob);
/* settings_blob_load_current on a blob read into its own buffer (one record
 * on the stack instead of two). */
uint8_t storage_blob_load_in_place(settings_blob_t *b);
/* The blob length a version means (v26, v27, v28), 0 for any other. */
uint32_t storage_blob_length(uint32_t version);

/* The store's learner record changed under the trainer (a restore): read
 * it again at the next entry, and save nothing over it before. */
void storage_trainer_forget(void);

#endif /* MOD_STORAGE_STORE_PRIV_H */
