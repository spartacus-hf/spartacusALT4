#include "mod_priv.h"

/* CW keyer and TX-test paddle policy API.
 *
 * Keying settings publish new timing (cw_recompute_timing); the keyer takes
 * it between marks, a new mode once it is idle with the levers open. They
 * no longer reset the keyer, which cut a mark short on the air when the
 * speed was turned while sending. Practice, paddle protection and the
 * both-key policy still reset it at once: they stop keying. */

void MOD_SetCwKeyerMode(cw_keyer_mode_t mode)
{
    cfg_cw_keyer_mode = ((uint32_t)mode < (uint32_t)CW_KEYER_COUNT) ? mode : CW_DEFAULT_KEYER_MODE;
    cw_recompute_timing();
}

cw_keyer_mode_t MOD_GetCwKeyerMode(void)
{
    return ((uint32_t)cfg_cw_keyer_mode < (uint32_t)CW_KEYER_COUNT) ? cfg_cw_keyer_mode : CW_DEFAULT_KEYER_MODE;
}

void MOD_SetCwWpm(float wpm)
{
    cfg_cw_wpm = wpm;
    cw_recompute_timing();
}

float MOD_GetCwWpm(void) { return cfg_cw_wpm; }

void MOD_SetCwSwap(uint8_t swap)
{
    /* The sampler turns it once both levers are open. */
    cfg_cw_swap = swap ? 1U : 0U;
}

uint8_t MOD_GetCwSwap(void) { return cfg_cw_swap ? 1U : 0U; }

void MOD_SetCwWeight(float weight)
{
    cfg_cw_weight = weight;
    cw_recompute_timing();
}

float MOD_GetCwWeight(void) { return cfg_cw_weight; }

void MOD_SetCwAutoSpace(uint8_t enable)
{
    MOD_SetCwAutoSpaceDits(enable ? 3U : 0U);
}

uint8_t MOD_GetCwAutoSpace(void) { return (MOD_GetCwAutoSpaceDits() != 0U) ? 1U : 0U; }

void MOD_SetCwAutoSpaceDits(uint8_t dits)
{
    if (dits == 1U) dits = 3U;
    if ((dits != 0U) && (dits != 2U) && (dits != 3U) && (dits != 4U)) {
        dits = CW_DEFAULT_AUTOSPACE;
    }
    cfg_cw_autospace = dits;
    cw_recompute_timing();
}

uint8_t MOD_GetCwAutoSpaceDits(void)
{
    uint8_t dits = cfg_cw_autospace;
    if (dits == 1U) return 3U;
    return ((dits == 2U) || (dits == 3U) || (dits == 4U)) ? dits : 0U;
}

void MOD_SetCwCurtisBDitPct(float pct)
{
    cfg_cw_curtis_b_dit_pct = clampf(pct, CW_CURTIS_B_PCT_MIN, CW_CURTIS_B_PCT_MAX);
    cw_recompute_timing();
}

float MOD_GetCwCurtisBDitPct(void) { return cfg_cw_curtis_b_dit_pct; }

void MOD_SetCwCurtisBDahPct(float pct)
{
    cfg_cw_curtis_b_dah_pct = clampf(pct, CW_CURTIS_B_PCT_MIN, CW_CURTIS_B_PCT_MAX);
    cw_recompute_timing();
}

float MOD_GetCwCurtisBDahPct(void) { return cfg_cw_curtis_b_dah_pct; }

void MOD_SetCwLatencyEighths(float eighths)
{
    cfg_cw_latency_eighths = mod_control_round_u8(
        eighths, CW_DEFAULT_LATENCY_EIGHTHS, CW_LATENCY_EIGHTHS_MAX);
    cw_recompute_timing();
}

float MOD_GetCwLatencyEighths(void) { return (float)cfg_cw_latency_eighths; }

void MOD_SetCwMemoryRepeatS(float s) { cfg_cw_mem_repeat_s = (s >= 0.5f) ? (uint8_t)((s < 60.0f ? s : 60.0f) + 0.5f) : 0U; }
float MOD_GetCwMemoryRepeatS(void) { return (float)cfg_cw_mem_repeat_s; }

void MOD_SetCwBreakInMode(cw_breakin_mode_t mode)
{
    cfg_cw_breakin_mode = ((uint32_t)mode < (uint32_t)CW_BREAKIN_COUNT) ? mode : CW_DEFAULT_BREAKIN;
}

cw_breakin_mode_t MOD_GetCwBreakInMode(void)
{
    return ((uint32_t)cfg_cw_breakin_mode < (uint32_t)CW_BREAKIN_COUNT) ? cfg_cw_breakin_mode : CW_DEFAULT_BREAKIN;
}

void MOD_SetCwSemiQskTail(float dits)
{
    cfg_cw_semi_qsk_tail_dits = dits;
    cw_recompute_timing();
}

float MOD_GetCwSemiQskTail(void) { return cfg_cw_semi_qsk_tail_dits; }

void MOD_SetCwRampMs(float ramp_ms)
{
    cfg_cw_ramp_ms = ramp_ms;
    cw_recompute_timing();
}

float MOD_GetCwRampMs(void) { return cfg_cw_ramp_ms; }

void MOD_SetCwDrive(float drive)
{
    cfg_cw_drive = drive;
    cw_recompute_timing();
}

float MOD_GetCwDrive(void) { return cfg_cw_drive; }

void MOD_SetCwThetaTrim(float trim)
{
    cfg_cw_theta_trim = trim;
    cw_recompute_timing();
}

float MOD_GetCwThetaTrim(void) { return cfg_cw_theta_trim; }

void MOD_SetCwThetaModel(cw_theta_model_t model)
{
    cfg_cw_theta_model = ((uint32_t)model < (uint32_t)CW_THETA_MODEL_COUNT)
                       ? model : CW_THETA_MODEL_DEFAULT;
    cw_recompute_timing();
}

cw_theta_model_t MOD_GetCwThetaModel(void)
{
    return ((uint32_t)cfg_cw_theta_model < (uint32_t)CW_THETA_MODEL_COUNT)
         ? cfg_cw_theta_model : CW_THETA_MODEL_DEFAULT;
}

void MOD_SetCwPractice(uint8_t enable)
{
    cfg_cw_practice = enable ? 1U : 0U;
    cw_reset_state();
}

uint8_t MOD_GetCwPractice(void) { return cfg_cw_practice ? 1U : 0U; }

void MOD_SetCwDebounceMs(float debounce_ms)
{
    cfg_cw_debounce_ms = debounce_ms;
    cw_recompute_timing();
}

float MOD_GetCwDebounceMs(void) { return cfg_cw_debounce_ms; }

void MOD_SetCwPaddleProtectMode(cw_paddle_protect_mode_t mode)
{
    cfg_cw_paddle_protect_mode = ((uint32_t)mode < (uint32_t)CW_PADDLE_PROTECT_COUNT)
                              ? mode : CW_DEFAULT_PADDLE_PROTECT_MODE;
    cw_recompute_timing();
    cw_paddle_guard_reset();
    cw_reset_state();
}

cw_paddle_protect_mode_t MOD_GetCwPaddleProtectMode(void)
{
    return ((uint32_t)cfg_cw_paddle_protect_mode < (uint32_t)CW_PADDLE_PROTECT_COUNT)
         ? cfg_cw_paddle_protect_mode : CW_DEFAULT_PADDLE_PROTECT_MODE;
}

void MOD_SetCwPaddleArmOpenMs(float ms)
{
    cfg_cw_paddle_arm_open_ms = clampf(ms, CW_PADDLE_ARM_MS_MIN, CW_PADDLE_ARM_MS_MAX);
    cw_recompute_timing();
    cw_paddle_guard_reset();
}

float MOD_GetCwPaddleArmOpenMs(void) { return cfg_cw_paddle_arm_open_ms; }

void MOD_SetCwPaddleArmPressMs(float ms)
{
    cfg_cw_paddle_arm_press_ms = clampf(ms, CW_PADDLE_ARM_MS_MIN, CW_PADDLE_ARM_MS_MAX);
    cw_recompute_timing();
    cw_paddle_guard_reset();
}

float MOD_GetCwPaddleArmPressMs(void) { return cfg_cw_paddle_arm_press_ms; }

void MOD_SetCwPaddleArmReleaseMs(float ms)
{
    cfg_cw_paddle_arm_release_ms = clampf(ms, CW_PADDLE_ARM_MS_MIN, CW_PADDLE_ARM_MS_MAX);
    cw_recompute_timing();
    cw_paddle_guard_reset();
}

float MOD_GetCwPaddleArmReleaseMs(void) { return cfg_cw_paddle_arm_release_ms; }

void MOD_SetCwBothPolicy(cw_both_policy_t policy)
{
    cfg_cw_both_policy = ((uint32_t)policy < (uint32_t)CW_BOTH_COUNT) ? policy : CW_DEFAULT_BOTH_POLICY;
    cw_paddle_guard_reset();
    cw_reset_state();
}

cw_both_policy_t MOD_GetCwBothPolicy(void)
{
    return ((uint32_t)cfg_cw_both_policy < (uint32_t)CW_BOTH_COUNT) ? cfg_cw_both_policy : CW_DEFAULT_BOTH_POLICY;
}

void MOD_SetCwPaddleStuckMs(float ms)
{
    cfg_cw_paddle_stuck_ms = clampf(ms, CW_PADDLE_STUCK_MS_MIN, CW_PADDLE_STUCK_MS_MAX);
    cw_recompute_timing();
    cw_paddle_guard_reset();
}

float MOD_GetCwPaddleStuckMs(void) { return cfg_cw_paddle_stuck_ms; }

void MOD_SetTxTestBothPolicy(tx_test_both_policy_t policy)
{
    cfg_tx_test_both_policy = ((uint32_t)policy < (uint32_t)TXTEST_BOTH_COUNT) ? policy : TXTEST_DEFAULT_BOTH_POLICY;
    cw_paddle_guard_reset();
    MOD_TxCarrierTestStop();
}

tx_test_both_policy_t MOD_GetTxTestBothPolicy(void)
{
    return ((uint32_t)cfg_tx_test_both_policy < (uint32_t)TXTEST_BOTH_COUNT) ? cfg_tx_test_both_policy : TXTEST_DEFAULT_BOTH_POLICY;
}

void MOD_SetTxTestPaddleStuckMs(float ms)
{
    cfg_tx_test_paddle_stuck_ms = clampf(ms, TXTEST_PADDLE_STUCK_MS_MIN, TXTEST_PADDLE_STUCK_MS_MAX);
    cw_recompute_timing();
    cw_paddle_guard_reset();
    MOD_TxCarrierTestStop();
}

float MOD_GetTxTestPaddleStuckMs(void) { return cfg_tx_test_paddle_stuck_ms; }

/* The sidetone follows the keyer whether or not TX is muted, as in
 * practice mode: the operator hears what the paddle sends. */
uint8_t MOD_CwMonitorActive(void) { return active_mod_type == MOD_TYPE_CW; }
float MOD_CwMonitorEnvelope(void) { return MOD_CwMonitorActive() ? cw_env.env : 0.0f; }
