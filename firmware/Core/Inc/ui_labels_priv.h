#ifndef UI_LABELS_PRIV_H
#define UI_LABELS_PRIV_H

#include "ui_priv.h"

const char *profile_label(audio_profile_t p);
const char *analytic_label(ssb_analytic_backend_t v);
const char *txbw_label(tx_bw_preset_t v);
const char *debug_branch_label(debug_branch_mode_t v);
const char *branch_law_label(uint8_t v);
const char *outphase_law_label(outphase_law_t v);
const char *pa_geometry_label(uint8_t v);
const char *cw_theta_model_label(cw_theta_model_t v);
const char *usb_chan_label(usb_audio_channel_mode_t v);
const char *usb_policy_label(usb_audio_policy_t v);
const char *fm_preset_label(fm_deviation_preset_t v);
const char *am_profile_label(am_profile_t v);
const char *dsp_math_label(dsp_math_mode_t v);
const char *dsp_math_short_label(uint8_t v);
const char *gain_telem_label(gain_telemetry_mode_t v);
const char *comp_math_label(comp_math_mode_t v);
const char *rf_metric_label(rf_metric_mode_t v);
const char *cpu_scope_label(mod_cpu_scope_mode_t v);
const char *ssb_pipeline_label(ssb_pipeline_mode_t v);
const char *home_layout_label(ui_home_layout_t v);
const char *shortcut_label(ui_shortcut_action_t v);
const char *cw_keyer_label(cw_keyer_mode_t v);
const char *mtrain_act_label(uint8_t v);
const char *mtrain_src_label(uint8_t v);
const char *mtrain_show_label(uint8_t v);
const char *mtrain_course_label(uint8_t v);
const char *cw_breakin_label(cw_breakin_mode_t v);
const char *cw_paddle_protect_label(cw_paddle_protect_mode_t v);
const char *cw_both_policy_label(cw_both_policy_t v);
const char *tx_test_both_policy_label(tx_test_both_policy_t v);
void append_ms_timeout_label(char *dst, float v);
const char *stream_state(const modulator_status_t *st, uint32_t now);
const char *queue_state(const modulator_status_t *st);
const char *carrier_apply_path_label(mod_carrier_apply_path_t path);
const char *tx_state_label(tx_state_t s);
const char *cw_paddle_guard_label(uint8_t state);

#endif /* UI_LABELS_PRIV_H */
