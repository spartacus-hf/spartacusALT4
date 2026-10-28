/* Hardware/scheduler hooks the render path references, for the host harness. */
#include "mod_priv.h"
uint32_t HAL_GetTick(void) { return 1000U; }
void BOARD_WriteRf(uint8_t on) { (void)on; }
ssb_pipeline_mode_t MOD_GetSsbPipelineMode(void)
{
    return ((uint32_t)cfg_ssb_pipeline_mode < (uint32_t)SSB_PIPELINE_COUNT) ? cfg_ssb_pipeline_mode : SSB_PIPELINE_LEGACY;
}
void mod_rate_plan_validate_active(void) {}
uint8_t MOD_CarrierFrequencyReadyForTx(void) { return 1U; }
uint32_t MOD_GetTim6InputClockHz(void) { return 170000000U; }
uint8_t cw_get_paddles_for_tx_test(uint8_t *d, uint8_t *h) { if (d) *d = 0U; if (h) *h = 0U; return 0U; }
void cw_reset_state(void) {}
volatile uint8_t cw_train_inhibit;
void render_cw(const int16_t *mono, uint32_t count, render_cmp_t *o1, render_cmp_t *o2) { (void)mono; (void)count; (void)o1; (void)o2; }
void MOD_FillAllNullBuffers(void) {}
uint32_t mod_tim6_next_base_arr(void) { return 0U; }
void mod_tim6_scheduler_init(uint32_t a, uint32_t b) { (void)a; (void)b; }
uint32_t pipeline_notes;
void mod_pipeline_note(mod_pipeline_stage_t stage, mod_pipeline_reason_t reason, uint32_t value)
{ (void)stage; (void)reason; (void)value; pipeline_notes++; }
void MOD_SetSideband(sideband_t sb)
{
    active_sideband_enum = sb;
    active_sideband_sign = (sb == SIDEBAND_LSB) ? 1.0f : -1.0f;
    requested_reset = 1U;
}
