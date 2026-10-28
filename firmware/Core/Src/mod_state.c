#include "mod_priv.h"
#include "clock_plan.h"

/* Carrier/profile state */
CarrierProfile custom_carrier = { 388U, 194U, 97U, HRTIM_PHASE_MIN_COMPARE, 384U, 2U, 0 };
float current_carrier_mhz = 14.02f;
uint32_t current_carrier_requested_hz = 14020000UL;
uint8_t active_band_idx = 13U; /* 20 m default */

volatile uint32_t mod_hrtim_input_clock_hz = MOD_HRTIM_INPUT_CLK_DEFAULT_HZ;
volatile uint32_t mod_tim6_input_clock_hz  = MOD_HRTIM_INPUT_CLK_DEFAULT_HZ;
volatile mod_clock_policy_t cfg_carrier_clock_policy = MOD_CLOCK_POLICY_AUTO;
volatile mod_retune_state_t carrier_retune_state = MOD_RETUNE_IDLE;
volatile uint8_t carrier_retune_last_status = 0U;
volatile uint8_t carrier_retune_last_reason = 0U;
volatile uint8_t carrier_clock_retune_in_progress = 0U;
volatile uint8_t carrier_retune_fault_latched = 0U;
volatile uint8_t carrier_frequency_apply_pending = 0U;
volatile uint8_t cfg_precision_clock_arm = 0U;
volatile uint32_t carrier_retune_requested_count = 0U;
volatile uint32_t carrier_retune_applied_count = 0U;
volatile uint32_t carrier_retune_deferred_count = 0U;
volatile uint32_t carrier_retune_rejected_count = 0U;
volatile uint32_t carrier_retune_failed_count = 0U;
volatile uint32_t carrier_retune_last_old_clock_hz = MOD_HRTIM_INPUT_CLK_DEFAULT_HZ;
volatile uint32_t carrier_retune_last_new_clock_hz = MOD_HRTIM_INPUT_CLK_DEFAULT_HZ;
volatile int32_t carrier_retune_last_error_before_hz = 0;
volatile int32_t carrier_retune_last_error_after_hz = 0;
volatile uint16_t carrier_retune_last_pll_m = CLOCK_BOOT_PLL_M;
volatile uint16_t carrier_retune_last_pll_n = CLOCK_BOOT_PLL_N;
volatile uint16_t carrier_retune_last_pll_r = CLOCK_BOOT_PLL_R;

/* DMA/transport state */
uint32_t dma_mcmp1[DMA_BUF_SIZE];
uint32_t dma_mcmp2[DMA_BUF_SIZE];
#if MOD_HRTIM_ATOMIC_EXPERIMENT
/* REV1 ordered burst stream: [MCMP1, MCMP2] per sample. */
/* Halfwords: every compare is below 6000 ticks (a 1 MHz carrier at 170 MHz
 * is 5440), and DMA1 channel 1 zero-extends each one into the 32-bit HRTIM
 * burst register (MSIZE 16, PSIZE 32). Half the RAM, which is what makes
 * room for TX PHASE 384K. */
uint16_t dma_mcmp_pair[RF_DMA_SIZE_MAX * 2U];
#endif
volatile uint32_t dma_write_half = 0;
volatile uint32_t g_last_audio_tick = 0;
volatile uint32_t g_last_usb_tick = 0;
volatile uint32_t g_last_usb_audio_tick = 0;
volatile uint32_t g_last_cw_tick = 0;

DMA_HandleTypeDef hdma_mcmp2;
volatile uint8_t dma_mcmp2_initialized = 0U;
volatile uint8_t dma_streams_running   = 0U;
volatile uint8_t dma_static_idle_parked = 0U;
volatile uint8_t transport_dma_park_request = 0U;
uint8_t carrier_apply_valid   = 0U;
uint16_t carrier_apply_period_ticks = 0U;
uint16_t carrier_apply_null_ticks   = 0U;
uint8_t carrier_apply_frac_cycles   = 2U;
int8_t carrier_apply_frac_extra     = 0;
uint16_t carrier_apply_deadtime_ticks = 0U;
uint16_t carrier_apply_deadtime_rise_ticks = 0U;
uint16_t carrier_apply_deadtime_fall_ticks = 0U;
int16_t  carrier_apply_duty_offset_ticks = 0;
volatile uint8_t last_carrier_apply_path = MOD_CARRIER_APPLY_PATH_NONE;
uint32_t tim6_servo_frame_counter = 0U;

HOT_BUFFER_BSS int16_t  pcm_ring[PCM_RING_SAMPLES];
volatile uint32_t pcm_wr_idx = 0U, pcm_rd_idx = 0U, pcm_count = 0U;
volatile uint8_t  pcm_push_in_progress = 0U;
volatile uint8_t  pcm_pop_in_progress = 0U;
volatile uint32_t pcm_reset_seq = 0U;
volatile int8_t tim6_servo_bias = TRANSPORT_LOCKED_BIAS;
int32_t tim6_servo_error_avg = 0;

HOT_BUFFER_BSS RenderFrame render_fifo[RENDER_QUEUE_FRAMES];
RenderFrame last_dma_frame;
volatile uint8_t  last_dma_valid = 0U;
volatile uint32_t render_wr_idx = 0U, render_rd_idx = 0U, render_count = 0U;
volatile uint32_t render_reset_seq = 0U;
volatile uint32_t render_push_seq = 0U;
volatile uint8_t  render_push_in_progress = 0U, render_pop_in_progress = 0U;
volatile uint32_t render_next_seq = 0U;
volatile uint32_t render_dma_expected_seq = 0U;
volatile uint8_t  render_dma_seq_valid = 0U;

volatile uint32_t transport_started = 0U;
volatile uint32_t pcm_peak = 0U, pcm_drop_samples = 0U;
volatile uint32_t pcm_drop_during_pop_samples = 0U;
volatile uint32_t render_peak = 0U, render_low_water = RENDER_QUEUE_FRAMES, render_underruns = 0U, render_overflows = 0U;
volatile uint32_t transport_resyncs = 0U;
volatile uint32_t transport_underrun_streak = 0U;
volatile uint32_t usb_packets = 0U, usb_last_submit_count = 0U;
volatile uint32_t usb_nonzero_packets = 0U, usb_gap_resets = 0U;
volatile uint32_t usb_sof_count = 0U, usb_missing_frames = 0U;
volatile uint32_t usb_packet_size_anomalies = 0U, usb_rx_samples = 0U;
volatile uint32_t usb_packet_size_min = 0U, usb_packet_size_max = 0U;
volatile uint32_t usb_feedback_q10_14 = USB_FEEDBACK_NOMINAL_Q10_14;
int32_t usb_feedback_integrator = 0;
volatile uint8_t usb_stream_active = 0U;
volatile uint16_t usb_missing_frame_streak = 0U;
volatile uint8_t usb_alt_active = 0U;
volatile uint8_t usb_frame_tracking_armed = 0U;
volatile uint16_t usb_expected_frame_number = 0U;
volatile uint8_t usb_rearm_good_frames = 0U;
volatile uint8_t audio_seen = 0U;
volatile uint8_t usb_audio_seen = 0U;
volatile uint8_t audio_critical_reason = 0U;
volatile uint32_t tim6_arr_last = TIM6_DEFAULT_ARR_170M;
volatile uint32_t dma_ch1_error_count = 0U;
volatile uint32_t dma_ch2_error_count = 0U;
volatile uint32_t dma_start_fail_count = 0U;
volatile uint32_t transport_dma_faults = 0U;
volatile uint8_t dma_expected_half = 0U;
volatile uint8_t dma_expected_half_valid = 0U;
volatile mod_pipeline_health_t pipeline_health;
volatile uint8_t pipeline_hard_lockout = 0U;
volatile uint8_t pipeline_ack_requested = 0U;
volatile uint8_t pipeline_rearm_pending = 0U;
volatile uint32_t pipeline_dma_progress_epoch = 0U;

/* Active mode/configuration state */
const CarrierProfile *active_carrier = &custom_carrier;
volatile uint32_t requested_config = (uint32_t)MOD_CFG_PROD_SSB_14MHZ;
volatile uint32_t active_config    = (uint32_t)MOD_CFG_PROD_SSB_14MHZ;
uint8_t   active_internal_source = 0U;
volatile uint8_t active_audio_source = MOD_AUDIO_SOURCE_NONE;
volatile uint8_t local_audio_armed = 0U;
volatile uint8_t local_audio_eos = 0U;
volatile uint8_t local_audio_io_scope = 0U;
volatile uint8_t local_audio_clean = 1U;
float     active_sample_drive = SAMPLE_DRIVE;
BranchMode active_branch_mode = BRANCH_MODE_BOTH;
mod_type_t active_mod_type = MOD_TYPE_SSB;
render_fn_t active_render_fn = NULL;

volatile sideband_t active_sideband_enum = SIDEBAND_USB;
float active_sideband_sign = -1.0f;

phase_cal_t  phase_cal_active = {0};
phase_cal_t *active_cal = &phase_cal_active;
float theta_linearity_pd[PHASE_CAL_BINS] = {0};
uint16_t active_cal_flags = 0U;

volatile uint32_t diag_branch_a = 0U, diag_branch_c = 0U;
volatile float    diag_envelope = 0.0f, diag_theta_deg = 0.0f, diag_phi_deg = 0.0f;
volatile uint8_t requested_reset = 0U;

volatile audio_profile_t active_audio_profile = AUDIO_PROFILE_CLEAN;
volatile uint8_t cfg_usb_clean_mode = 1U;
volatile usb_audio_channel_mode_t cfg_usb_channel_mode = USB_AUDIO_CHAN_AVERAGE;
#if MOD_DEBUG_SWITCHES
volatile dsp_math_mode_t cfg_dsp_math_mode = DSP_MATH_SAFE;
volatile gain_telemetry_mode_t cfg_gain_telemetry_mode = GAIN_TELEMETRY_OFF;
volatile comp_math_mode_t cfg_comp_math_mode = COMP_MATH_LUT;
volatile rf_metric_mode_t cfg_rf_metric_mode = RF_METRIC_EXACT;
#endif
volatile uint8_t cfg_runtime_diag_enable = 0U;
volatile uint8_t cfg_cpu_diag_enable = 0U;
volatile mod_cpu_scope_mode_t cfg_cpu_scope_mode = MOD_CPU_SCOPE_OFF;
volatile ssb_pipeline_mode_t cfg_ssb_pipeline_mode = SSB_PIPELINE_LEGACY;
volatile uint8_t rate_plan_fault_latched = 0U;
volatile mod_storage_backend_t cfg_storage_backend = MOD_STORAGE_SD; /* default since v8.08 */
volatile uint8_t storage_sd_missing = 0U;
volatile ui_home_layout_t cfg_ui_home_layout = UI_HOME_LAYOUT_DETAILED;
volatile uint8_t cfg_ui_home_flags = UI_HOME_FLAGS_DEFAULT;
volatile ui_shortcut_action_t cfg_ui_shortcut_up = UI_SHORTCUT_AB_SWAP;
volatile ui_shortcut_action_t cfg_ui_shortcut_down = UI_SHORTCUT_BAND;

float cfg_gate       = SAMPLE_GATE;
float cfg_knee       = SOFT_KNEE;
float cfg_drive      = SAMPLE_DRIVE;
float cfg_ssb_phase_bias = 0.0f;
uint8_t cfg_ssb_ce_enable = 0U;
outphase_law_t cfg_outphase_law = OUTPHASE_LAW_CLASSIC;
float   cfg_ssb_ce_mix    = 0.35f;
float   cfg_ssb_ce_floor  = 0.06f;
float   cfg_ssb_ce_knee   = 0.50f;
float   cfg_ssb_low_cut   = 0.004f;
ssb_analytic_backend_t cfg_ssb_analytic_backend = SSB_ANALYTIC_WEAVER;
tx_bw_preset_t cfg_tx_bw_preset = TX_BW_300_2700;
#if MOD_DEBUG_SWITCHES
debug_branch_mode_t cfg_debug_branch_mode = DEBUG_BRANCH_BOTH;
uint8_t cfg_debug_cal_bypass = 0U;
#endif
float cfg_rf_en_assert_metric = RF_EN_GPIO_ASSERT_METRIC_DEFAULT;
float cfg_rf_en_release_metric = RF_EN_GPIO_RELEASE_METRIC_DEFAULT;
uint8_t cfg_rf_en_hold_frames = RF_EN_GPIO_HOLD_FRAMES_DEFAULT;
float rf_en_follow_metric = 0.0f;
float cfg_fm_dev     = FM_DEVIATION_HZ;
float cfg_fm_input_gain = 1.0f;
fm_deviation_preset_t cfg_fm_deviation_preset = FM_DEVIATION_PRESET_NFM_2K5;
uint8_t cfg_fm_test_tone = 0U;
float cfg_am_carrier = AM_CARRIER_LEVEL;
float cfg_am_depth   = AM_MOD_DEPTH;
am_profile_t cfg_am_profile = AM_PROFILE_EFFICIENT;
float cfg_usb_input_gain = 1.0f;
float cfg_outphase_ssb_max_theta = OUTPHASE_SSB_MAX_THETA_DEFAULT;
float cfg_outphase_am_max_theta = OUTPHASE_AM_MAX_THETA_DEFAULT;
float cfg_outphase_fm_theta = OUTPHASE_FM_THETA_DEFAULT;
float cfg_outphase_cw_theta = OUTPHASE_CW_THETA_DEFAULT;
float cfg_outphase_test_theta = OUTPHASE_TEST_THETA_DEFAULT;
mod_outphase_fallback_t cfg_outphase_fallback_policy = MOD_OUTPHASE_FALLBACK_COMPRESS;
volatile uint8_t outphase_limit_latched = 0U;
volatile uint32_t outphase_limit_count = 0U;
volatile uint32_t low_env_inhibit_count = 0U;
volatile uint32_t fm_silent_frame_count = 0U;
volatile uint32_t fm_limiter_count = 0U;
volatile float fm_deviation_peak_hz = 0.0f;
volatile uint32_t am_clamp_count = 0U;
volatile float am_pos_peak = 0.0f;
volatile float am_neg_peak = 0.0f;
volatile uint32_t usb_input_clip_count = 0U;
volatile uint16_t usb_input_peak_gain_raw = 0U;
volatile float usb_input_rms_gain = 0.0f;
volatile uint8_t usb_input_activity = 0U;
volatile uint32_t cessb_clip_count = 0U;
volatile float cessb_peak_pre = 0.0f;
volatile float cessb_peak_post_clip = 0.0f;
volatile float cessb_peak_post_filter = 0.0f;
volatile float cessb_overshoot = 0.0f;

cw_keyer_mode_t   cfg_cw_keyer_mode = CW_DEFAULT_KEYER_MODE;
cw_breakin_mode_t cfg_cw_breakin_mode = CW_DEFAULT_BREAKIN;
float   cfg_cw_wpm = CW_DEFAULT_WPM;
uint8_t cfg_cw_swap = CW_DEFAULT_SWAP;
float   cfg_cw_weight = CW_DEFAULT_WEIGHT;
uint8_t cfg_cw_autospace = CW_DEFAULT_AUTOSPACE;
float   cfg_cw_curtis_b_dit_pct = CW_DEFAULT_CURTIS_B_DIT_PCT;
float   cfg_cw_curtis_b_dah_pct = CW_DEFAULT_CURTIS_B_DAH_PCT;
uint8_t cfg_cw_latency_eighths = CW_DEFAULT_LATENCY_EIGHTHS;
uint8_t cfg_cw_mem_repeat_s;
float   cfg_cw_semi_qsk_tail_dits = CW_DEFAULT_SEMI_QSK_TAIL_DITS;
float   cfg_cw_ramp_ms = CW_DEFAULT_RAMP_MS;
float   cfg_cw_drive = CW_DEFAULT_DRIVE;
float   cfg_cw_theta_trim = CW_DEFAULT_THETA_TRIM;
cw_theta_model_t cfg_cw_theta_model = CW_THETA_MODEL_DEFAULT;
uint8_t cfg_cw_practice = CW_DEFAULT_PRACTICE;
float   cfg_cw_debounce_ms = CW_DEFAULT_DEBOUNCE_MS;
cw_paddle_protect_mode_t cfg_cw_paddle_protect_mode = CW_DEFAULT_PADDLE_PROTECT_MODE;
float   cfg_cw_paddle_arm_open_ms = CW_DEFAULT_PADDLE_ARM_OPEN_MS;
float   cfg_cw_paddle_arm_press_ms = CW_DEFAULT_PADDLE_ARM_PRESS_MS;
float   cfg_cw_paddle_arm_release_ms = CW_DEFAULT_PADDLE_ARM_RELEASE_MS;
cw_both_policy_t cfg_cw_both_policy = CW_DEFAULT_BOTH_POLICY;
float   cfg_cw_paddle_stuck_ms = CW_DEFAULT_PADDLE_STUCK_MS;
tx_test_both_policy_t cfg_tx_test_both_policy = TXTEST_DEFAULT_BOTH_POLICY;
float   cfg_tx_test_paddle_stuck_ms = TXTEST_DEFAULT_PADDLE_STUCK_MS;
HOT_BUFFER_BSS uint16_t cw_ramp_lut[CW_RAMP_LUT_SIZE + 1U];
CwEnvelopeState cw_env = {0};
uint32_t cw_rf_hold_samples = 0U;
uint8_t  cw_key_sample_active = 0U;
uint8_t  cw_paddle_dit = 0U;
uint8_t  cw_paddle_dah = 0U;
uint8_t  cw_ui_key_latch = 0U;
uint8_t  cw_tx_live = 0U;
uint8_t  cw_rf_enabled = 0U;
uint8_t  cw_render_frame_rf_active = 0U;
uint8_t  cw_render_frame_tx_live = 0U;

uint8_t cfg_cessb_enable = 0U;
float   cfg_cessb_clip   = 0.94f;
uint8_t cfg_comp_enable = 0U;
float   cfg_comp_target = 0.50f;
float   cfg_comp_ratio  = 4.0f;
uint8_t cfg_eq_enable = 0U;
float   cfg_eq_bass   = 0.0f;
float   cfg_eq_treble = 0.0f;
volatile uint8_t cfg_muted = 0U;
/* The USB audio transmit gate, as QMX has it: with USB VOX off (the default)
 * USB audio keys the transmitter only while a CAT client has asked for
 * transmit (TX;/TQ1;). usb_tx_gate_closed is the one byte the USB interrupt
 * reads; MOD_SetUsbVox and MOD_SetUsbCatPtt keep it current. */
uint8_t          cfg_usb_vox = 0U;
uint8_t          cfg_digi_tone = 1U; /* DIGI TONE, QMX's transmit (mod_tx_pipeline.c) */
volatile uint8_t usb_cat_ptt = 0U;
volatile uint8_t usb_tx_gate_closed = 1U;
float            mute_gain = 1.0f;
float            mute_target = 1.0f;
volatile uint8_t carrier_values_capped = 0U;
volatile uint32_t last_mode = (uint32_t)MOD_CFG_PROD_SSB_14MHZ;
uint8_t cfg_mic_agc_enable = 0U;
float   cfg_mic_agc_target = MIC_AGC_TARGET_DEFAULT;
float   cfg_mic_agc_max_gain = MIC_AGC_MAX_GAIN_DEFAULT;
uint8_t cfg_ng_enable = 0U;
float   cfg_ng_thresh = NG_THRESH_DEFAULT;
uint8_t cfg_alc_enable = 0U;
float   cfg_alc_ceiling = ALC_DEFAULT_CEILING;
uint8_t cfg_cessb_iter = CESSB_ITER_DEFAULT;
uint16_t cfg_deadtime_ticks = PA_DEADTIME_DEFAULT_FINE_TICKS;
uint16_t cfg_deadtime_rise_ticks = PA_DEADTIME_DEFAULT_FINE_TICKS;
uint16_t cfg_deadtime_fall_ticks = PA_DEADTIME_DEFAULT_FINE_TICKS;
int16_t  cfg_duty_offset_ticks = 0;
int16_t  cfg_branch_ac_offset_ticks = 0;
pa_geometry_t cfg_pa_geometry = PA_GEOMETRY_DEFAULT;
#if MOD_DEBUG_SWITCHES
volatile uint8_t cfg_cordic_enable = 1U;
#endif
volatile uint32_t dma_ht_count = 0U;
volatile uint32_t dma_tc_count = 0U;
volatile uint32_t apply_config_count = 0U;
volatile uint32_t fm_hold_mcmp1 = 0U;
volatile uint32_t fm_hold_mcmp2 = 0U;
volatile uint8_t  fm_hold_valid = 0U;
float fm_phase_accum = 0.0f;
