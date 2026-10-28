#ifndef MOD_STORAGE_FORMAT_PRIV_H
#define MOD_STORAGE_FORMAT_PRIV_H

#include "mod_storage_layout_priv.h"

/* Format/migration helper interface. Persisted layout definitions live in
 * mod_storage_layout_priv.h.
 */

uint8_t settings_pack_aux_modes(gain_telemetry_mode_t gain_mode, comp_math_mode_t comp_mode);
gain_telemetry_mode_t settings_unpack_gain_telem(uint8_t packed);
comp_math_mode_t settings_unpack_comp_math(uint8_t packed);
uint8_t settings_blob_equal(const settings_blob_t *a, const settings_blob_t *b);
void phase_cal_sanitize(phase_cal_t *cal);
void phase_cal_legacy_sanitize(phase_cal_legacy_t *cal);
int16_t settings_clamp_permille(int32_t v);
uint8_t settings_blob_load_current(const void *raw_blob, settings_blob_t *out);
/* The geometry a blob's reserved6 means: a pa_geometry_t, or the v8.05 PA
 * WIRING MAP value with the A/C sign and RF180 flags (see pa_geometry_t). */
pa_geometry_t settings_pa_geometry(const settings_blob_t *b);

#endif /* MOD_STORAGE_FORMAT_PRIV_H */
