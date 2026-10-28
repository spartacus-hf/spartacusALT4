#ifndef MOD_PRIV_H
#define MOD_PRIV_H

#include "modulator.h"
#include "mod_control_logic.h"
#include "mod_transport_logic.h"

#define MOD_HRTIM_INPUT_CLK_DEFAULT_HZ   170000000UL
#define MOD_HRTIM_COMPARE_CLK_DEFAULT_HZ (170000000ULL * 32ULL)
#define MOD_HRTIM_COMPARE_CLK_DEFAULT_MHZ ((float)MOD_HRTIM_COMPARE_CLK_DEFAULT_HZ * 0.000001f)
#define MOD_HRTIM_HIGHRES_MUL            32ULL
#define MOD_HRTIM_DEADTIME_MUL           MOD_DEADTIME_FINE_MULTIPLIER
#define MOD_CARRIER_MIN_HZ               1000000UL
#define MOD_CARRIER_MAX_HZ               28333333UL
#define MOD_BOOT_DYNAMIC_RETUNE_DELAY_MS  1200U
#include "hrtim_phase.h"
#include "main.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#define HOT_RENDER __attribute__((optimize("O3"), hot))
#define HOT_CODE   __attribute__((section(".ccmram_text"), optimize("O3"), hot))
#define CCMRAM_BSS __attribute__((section(".ccmram_bss")))
#define HOT_INLINE static inline __attribute__((always_inline, hot))
#else
#define HOT_RENDER
#define HOT_CODE
#define CCMRAM_BSS
#define HOT_INLINE static inline
#endif

/* CPU-hot transport buffers default to CCM SRAM.
 *
 * Keep DMA-owned buffers in regular SRAM when using the current linker mapping,
 * because .ccmram_bss is linked at 0x10000000. On STM32G4, ST documents DMA
 * access for CCM SRAM only via the SRAM alias window, not the native
 * 0x10000000 mapping. */
#ifndef USE_CCMRAM_HOT_BUFFERS
#define USE_CCMRAM_HOT_BUFFERS 1
#endif

#if defined(__GNUC__) && (USE_CCMRAM_HOT_BUFFERS)
#define HOT_BUFFER_BSS __attribute__((section(".ccmram_bss"), aligned(4)))
#else
#define HOT_BUFFER_BSS
#endif

/* Single-precision pi for the DSP. <math.h> (gnu11) already defines M_PI
 * as a double, so the float fallback this used to offer never applied and
 * every `x * M_PI`, INV_PI and TWO_PI in the renderers was a software
 * double multiply on a single-precision FPU. */
#define MOD_PI_F 3.14159265358979323846f

#define CPU_SCOPE_FULL_STAGE(mode_) \
    (((mode_) == MOD_CPU_SCOPE_USB_SUBMIT) || \
     ((mode_) == MOD_CPU_SCOPE_RENDER_EXT) || \
     ((mode_) == MOD_CPU_SCOPE_DMA_POP) || \
     ((mode_) == MOD_CPU_SCOPE_SERVICE_BG))
#define CPU_SCOPE_MATCH(mode_) \
    ((cfg_cpu_scope_mode == (mode_)) || \
     ((cfg_cpu_scope_mode == MOD_CPU_SCOPE_FULL_AUDIO_RF) && CPU_SCOPE_FULL_STAGE((mode_))))

#if MOD_DEEP_DIAG_ENABLE
#define CPU_DIAG_DECLARE(name_, id_) uint32_t name_ = CPU_DIAG_BEGIN((id_))
void mod_diag_capture(uint32_t branch_a, uint32_t branch_c,
                      float envelope, float theta, float phi);
#define DIAG_CAPTURE(ta_, tc_, env_, theta_, phi_) \
    mod_diag_capture((ta_), (tc_), (env_), (theta_), (phi_))
#define CPU_DIAG_BEGIN(id_) (cfg_cpu_diag_enable ? MOD_CpuDiagBegin((id_)) : 0U)
/* Keep the conditional in one helper so each real-time caller does not gain
 * a separate branch for every diagnostic exit point. */
#if defined(__GNUC__)
__attribute__((noinline, unused))
#endif
static void cpu_diag_end(mod_cpu_diag_id_t id, uint32_t start_cycles)
{
    if (start_cycles != 0U) MOD_CpuDiagEnd(id, start_cycles);
}
#define CPU_DIAG_END(id_, start_) cpu_diag_end((id_), (start_))
#define CPU_SCOPE_BEGIN(mode_) do { if (CPU_SCOPE_MATCH((mode_))) MOD_CpuScopeBegin((mode_)); } while (0)
#define CPU_SCOPE_END(mode_)   do { if (CPU_SCOPE_MATCH((mode_))) MOD_CpuScopeEnd((mode_)); } while (0)
#else
/* Production hot paths contain no diagnostic load, call, branch, or argument evaluation. */
#define CPU_DIAG_DECLARE(name_, id_)
#define DIAG_CAPTURE(ta_, tc_, env_, theta_, phi_) ((void)0)
#define CPU_DIAG_BEGIN(id_)                         (0U)
#define CPU_DIAG_END(id_, start_)                   ((void)0)
#define CPU_SCOPE_BEGIN(mode_)                      ((void)0)
#define CPU_SCOPE_END(mode_)                        ((void)0)
#endif

#ifndef MOD_CRITICAL_DIAG_ENABLE
#define MOD_CRITICAL_DIAG_ENABLE 0
#endif

#if MOD_CRITICAL_DIAG_ENABLE
void MOD_CriticalDiagEnter(uint32_t primask);
void MOD_CriticalDiagExit(uint32_t primask);
#endif

#define TIM6_DEFAULT_ARR_170M             3541U
#define PCM_RING_SAMPLES                   (AUDIO_FRAME_SIZE * 128U)
#define RENDER_QUEUE_FRAMES                40U
#define PCM_HIGH_WATER_SAMPLES             (PCM_RING_SAMPLES - (AUDIO_FRAME_SIZE * 8U))
#define PCM_LOW_WATER_SAMPLES              (AUDIO_FRAME_SIZE * 8U)
#define TRANSPORT_START_THRESHOLD          (AUDIO_FRAME_SIZE * 18U)
#define TRANSPORT_START_THRESHOLD_FM       (AUDIO_FRAME_SIZE * 32U)
#define TRANSPORT_TARGET_TOTAL_SAMPLES     (AUDIO_FRAME_SIZE * 28U)
#define TRANSPORT_TARGET_TOTAL_SAMPLES_FM  (AUDIO_FRAME_SIZE * 40U)
#define TRANSPORT_SERVO_INTERVAL           16U
#define TRANSPORT_SERVO_BIAS_MAX           3
#define TRANSPORT_SERVO_EMERGENCY_LOW_FRAMES       8U
#define TRANSPORT_SERVO_EMERGENCY_HIGH_EXTRA_FRAMES 24U
#define TRANSPORT_SERVO_ENABLE             0U
#define TRANSPORT_LOCKED_BIAS              0
#define TRANSPORT_RENDER_TARGET_FRAMES_EXTERNAL 20U
#define TRANSPORT_RENDER_TARGET_FRAMES_CRITICAL 28U
#define TRANSPORT_RENDER_TARGET_FRAMES_INTERNAL 20U
#define TRANSPORT_RENDER_TARGET_FRAMES_FM  32U
#define TRANSPORT_RENDER_TARGET_FRAMES_CW  3U
#define TRANSPORT_RENDER_BUDGET_EXTERNAL   4U
#define TRANSPORT_RENDER_BUDGET_CRITICAL   4U
#define TRANSPORT_RENDER_BUDGET_INTERNAL   4U
#define TRANSPORT_RENDER_BUDGET_FM         4U
#define TRANSPORT_RENDER_BUDGET_CW         3U
#define TRANSPORT_RENDER_LOW_WATER_CW      1U
#define TRANSPORT_RENDER_LOW_WATER_EXTERNAL 2U
#define TRANSPORT_RENDER_IO_RESERVE_EXTERNAL (TRANSPORT_RENDER_TARGET_FRAMES_EXTERNAL + 4U)
#define TRANSPORT_RENDER_IO_RESERVE_FM       (TRANSPORT_RENDER_TARGET_FRAMES_FM + 2U)
#define TRANSPORT_RENDER_IO_RESERVE_INTERNAL (TRANSPORT_RENDER_TARGET_FRAMES_INTERNAL + 2U)
#define TRANSPORT_RENDER_IO_RESERVE_CW       (TRANSPORT_RENDER_TARGET_FRAMES_CW)
#define TRANSPORT_UNDERRUN_RESYNC_STREAK    3U
#define USB_GAP_RESET_MS                   1200U
#define USB_PACKET_MISS_LOCKOUT_FRAMES     3U
#define USB_CONCEAL_SAMPLES                AUDIO_FRAME_SIZE
#define USB_FRAME_NUMBER_MASK              0x07FFU
_Static_assert(USB_FRAME_NUMBER_MASK == MOD_USB_FRAME_MASK,
               "USB frame masks must agree");
#define DMA_REFILL_MIN_SLACK_SAMPLES       24U
#define USB_FEEDBACK_NOMINAL_Q10_14        ((uint32_t)AUDIO_FRAME_SIZE << 14)
#define USB_FEEDBACK_TRIM_MAX_Q10_14       16384
#define USB_FEEDBACK_I_CLAMP               (AUDIO_FRAME_SIZE * 512)
#define AUDIO_CRITICAL_HOLDOFF_MS          2000U
#define AUDIO_RECENT_ACTIVITY_MS           2000U
#define USB_AUDIO_CRITICAL_HOLDOFF_MS      180U
#define USB_AUDIO_SILENCE_RELEASE_MS       120U
#define USB_AUDIO_ACTIVITY_THRESHOLD       4
#define FM_USB_SILENCE_TAIL_MS             180U

#define AUDIO_CRIT_REASON_USB_AUDIO        0x01U
#define AUDIO_CRIT_REASON_RENDER_LOW       0x02U
#define AUDIO_CRIT_REASON_RENDER_AUDIO     0x04U
#define AUDIO_CRIT_REASON_CW               0x08U
#define AUDIO_CRIT_REASON_USB_STREAM       0x10U
#define AUDIO_CRIT_REASON_PCM_HIGH         0x20U

#define RF_EN_GPIO_ASSERT_METRIC_DEFAULT    0.010f
#define RF_EN_GPIO_RELEASE_METRIC_DEFAULT   0.000f
#define RF_EN_GPIO_HOLD_FRAMES_DEFAULT      2U
#define RF_EN_GPIO_ASSERT_METRIC_LEGACY     0.18f
#define RF_EN_GPIO_RELEASE_METRIC_LEGACY    0.14f
#define RF_EN_SSB_ATTACK_COEFF              0.12f
#define RF_EN_SSB_RELEASE_COEFF             0.0025f
#define RF_EN_TRACK_FLOOR                   0.0001f
#define RF_EN_GPIO_ASSERT_METRIC_MIN        0.00f
#define RF_EN_GPIO_ASSERT_METRIC_MAX        1.00f
#define RF_EN_GPIO_RELEASE_METRIC_MIN       0.00f
#define RF_EN_GPIO_RELEASE_METRIC_MAX       1.00f
#define RF_EN_GPIO_HOLD_FRAMES_MAX          50U
#define RF_METRIC_LUT_SIZE                  256U

#define AUDIO_RATE_HZ                        48000.0f
#define AUDIO_RATE_HZ_U32                    48000U
#define FM_DEVIATION_HZ                      2500.0f
#define FM_DEVIATION_MIN_HZ                  500.0f
#define FM_DEVIATION_MAX_HZ                  7500.0f
#define OUTPHASE_THETA_HARD_MAX              0.499f
#define OUTPHASE_SSB_MAX_THETA_DEFAULT       0.499f
#define OUTPHASE_AM_MAX_THETA_DEFAULT        0.320f
#define OUTPHASE_FM_THETA_DEFAULT            0.420f
#define OUTPHASE_CW_THETA_DEFAULT            0.050f
#define OUTPHASE_TEST_THETA_DEFAULT          0.030f
#define OUTPHASE_CW_DRIVE_MAX_THETA          0.200f
#define OUTPHASE_CW_EDGE_THETA_MAX           0.460f
#define FM_FIXED_THETA_PHASE                 OUTPHASE_FM_THETA_DEFAULT
#define AM_CARRIER_LEVEL                     0.75f
#define AM_MOD_DEPTH                         0.55f
#define PHASE_CAL_DIFFERENTIAL_LIMIT         0.45f
#define PHASE_CAL_COMMON_LIMIT               0.45f
#define PHASE_CAL_THETA_LIMIT                0.45f
#define PHASE_CAL_LEGACY_STATIC_LIMIT        0.45f
#define PHASE_CAL_LEGACY_ENVELOPE_LIMIT      0.45f
#define PHASE_CAL_LEGACY_AMP_LIMIT           0.10f

#define CW_DEFAULT_KEYER_MODE                CW_KEYER_IAMBIC_B
/* An everyday speed: comfortable for most operators, slow enough for a newer
 * one, and set in one turn on the home row (CW SPEED). v8.05 shipped 20. */
#define CW_DEFAULT_WPM                       15.0f
#define CW_DEFAULT_WEIGHT                    50.0f
#define CW_DEFAULT_AUTOSPACE                 0U
#define CW_DEFAULT_CURTIS_B_DIT_PCT          75.0f
#define CW_DEFAULT_CURTIS_B_DAH_PCT          45.0f
#define CW_DEFAULT_LATENCY_EIGHTHS           0U
#define CW_DEFAULT_SWAP                      0U
/* SEMI, receiver back 8 dits after the last key-up (the 1-dit keyer gap plus
 * this tail): QMX's "Auto", just longer than a word space, so the radio stays
 * in transmit through a whole over. REV1 cannot do true full break-in: every
 * return to transmit waits ~55 ms for the PIN bias (TX_PREPARE), and FULL
 * pays that at every character. v8.05 shipped FULL with a 4-dit tail. */
#define CW_DEFAULT_BREAKIN                   CW_BREAKIN_SEMI
#define CW_DEFAULT_SEMI_QSK_TAIL_DITS        7.0f
#define CW_DEFAULT_RAMP_MS                   5.0f
#define CW_DEFAULT_DRIVE                     1.00f
#define CW_DEFAULT_THETA_TRIM                0.0f
#define CW_DEFAULT_PRACTICE                  0U
/* Paddle and key contacts bounce for 1-5 ms. 0.3 ms (v8.05) let that through:
 * a straight key keyed the carrier on every bounce. 3 ms is inaudible
 * latency even at 60 WPM (20 ms dits). */
#define CW_DEFAULT_DEBOUNCE_MS               3.0f
#define CW_DEFAULT_PADDLE_PROTECT_MODE       CW_PADDLE_PROTECT_OFF
#define CW_DEFAULT_PADDLE_ARM_OPEN_MS        40.0f
#define CW_DEFAULT_PADDLE_ARM_PRESS_MS       40.0f
#define CW_DEFAULT_PADDLE_ARM_RELEASE_MS     40.0f
#define CW_DEFAULT_BOTH_POLICY               CW_BOTH_ALLOW
#define CW_DEFAULT_PADDLE_STUCK_MS           0.0f
#define TXTEST_DEFAULT_BOTH_POLICY           TXTEST_BOTH_ALLOW
#define TXTEST_DEFAULT_PADDLE_STUCK_MS       0.0f
#define CW_PADDLE_ARM_MS_MIN                 10.0f
#define CW_PADDLE_ARM_MS_MAX                 500.0f
#define CW_PADDLE_STUCK_MS_MIN               0.0f
#define CW_PADDLE_STUCK_MS_MAX               30000.0f
#define TXTEST_PADDLE_STUCK_MS_MIN           0.0f
#define TXTEST_PADDLE_STUCK_MS_MAX           10000.0f
#define CW_WPM_MIN                           5.0f
#define CW_WPM_MAX                           60.0f
#define CW_WEIGHT_MIN                        35.0f
#define CW_WEIGHT_MAX                        65.0f
#define CW_SEMI_QSK_TAIL_MIN                 1.0f
#define CW_SEMI_QSK_TAIL_MAX                 12.0f
#define CW_RAMP_MS_MIN                       0.1f
#define CW_RAMP_MS_MAX                       10.0f
#define CW_DRIVE_MIN                         0.05f
#define CW_DRIVE_MAX                         1.00f
#define CW_THETA_TRIM_MIN                    -0.10f
#define CW_THETA_TRIM_MAX                    0.07f
#define CW_DEBOUNCE_MS_MIN                   0.0f
#define CW_DEBOUNCE_MS_MAX                   10.0f
#define CW_RAMP_LUT_SIZE                     512U
#define CW_IDLE_HOLD_SAMPLE_MIN              48U
#define CW_UI_KEY_DIT                        0x01U
#define CW_UI_KEY_DAH                        0x02U

#define SAMPLE_DRIVE                         0.95f
#define SAMPLE_GATE                          0.004f
#define SOFT_KNEE                            0.7f
#define ALC_DEFAULT_CEILING                  0.92f
#define MIC_AGC_TARGET_DEFAULT               0.30f
#define MIC_AGC_MAX_GAIN_DEFAULT             12.0f
#define NG_THRESH_DEFAULT                    0.010f
#define CESSB_ITER_MAX                       3U
#define CESSB_ITER_DEFAULT                   1U

#define PA_DEADTIME_HALF_CYCLE_LIMIT_PCT      40U
#define PA_DUTY_PERMILLE_SCALE                1000
#define PA_DEADTIME_STORAGE_UNIT_PS            1UL
#define PA_DEADTIME_DEFAULT_PS                0UL
#define PA_DEADTIME_DEFAULT_FINE_TICKS       ((uint16_t)((((uint64_t)PA_DEADTIME_DEFAULT_PS * 170000000ULL * 8ULL) + 500000000000ULL) / 1000000000000ULL))

enum { MOD_BAND_TABLE_COUNT = 23U };
#define BAND_COUNT MOD_BAND_TABLE_COUNT

typedef struct { float b0, b1, b2, a1, a2; } BiquadCoeffs;
typedef struct { float z1, z2; } BiquadState;
typedef struct { float re, im, step_re, step_im; } ComplexOsc;
typedef uint16_t render_cmp_t;

typedef struct {
    float    hist[97U];
    uint32_t wr_idx;
    uint8_t  decim_phase;
    uint8_t  interp_phase;
    uint8_t  primed;
    float    last_i;
    float    last_q;
    float    seg_i0;
    float    seg_q0;
    float    seg_i1;
    float    seg_q1;
} HilbertExperimentState;

typedef struct {
    uint16_t period_ticks, half_period_ticks, null_ticks;
    uint16_t min_compare_ticks, max_compare_ticks;
    /* The carrier fraction (clock_plan.h clock_carrier_t): the master runs
     * frac_cycles periods plus frac_extra ticks; 2 and 0 for a whole period. */
    uint8_t  frac_cycles;
    int8_t   frac_extra;
} CarrierProfile;

typedef struct {
    uint32_t deadtime_rise_ps;
    uint32_t deadtime_fall_ps;
    int16_t  duty_offset_half_permille;
} pa_band_profile_t;

enum {
    PA_PROFILE_SOURCE_TEMPLATE = 0U,
    PA_PROFILE_SOURCE_PER_BAND = 1U
};

typedef struct {
    uint8_t  band_idx;
    uint8_t  source_flags;
    uint8_t  carrier_values_capped;
    uint8_t  reserved0;
    uint32_t requested_deadtime_rise_ps;
    uint32_t requested_deadtime_fall_ps;
    int16_t  requested_duty_offset_half_permille;
    uint16_t resolved_deadtime_rise_ticks;
    uint16_t resolved_deadtime_fall_ticks;
    uint32_t resolved_deadtime_rise_ps;
    uint32_t resolved_deadtime_fall_ps;
    int16_t  resolved_duty_offset_ticks;
    uint16_t applied_deadtime_rise_ticks;
    uint16_t applied_deadtime_fall_ticks;
    uint32_t applied_deadtime_rise_ps;
    uint32_t applied_deadtime_fall_ps;
    int16_t  applied_duty_offset_ticks;
} pa_resolved_runtime_t;

typedef struct {
    render_cmp_t mcmp1[AUDIO_FRAME_SIZE];
    render_cmp_t mcmp2[AUDIO_FRAME_SIZE];
    uint32_t frame_seq;
    uint8_t  frame_kind;
    uint8_t  rf_active;
    uint8_t  tx_live;
    uint8_t  rf_gate_valid;
} RenderFrame;

_Static_assert(sizeof(render_cmp_t) == sizeof(uint16_t), "render compare storage must stay 16-bit");

enum {
    RENDER_FRAME_KIND_NONE = 0U,
    RENDER_FRAME_KIND_CW = 1U,
    RENDER_FRAME_KIND_TX_TEST = 2U,
    RENDER_FRAME_KIND_MODULATED = 3U
};

typedef enum { BRANCH_MODE_BOTH = 0, BRANCH_MODE_A_ONLY, BRANCH_MODE_C_ONLY } BranchMode;
typedef struct {
    float env;
    uint32_t ramp_pos;
    uint8_t target_on;
    uint8_t prev_target_on;
} CwEnvelopeState;

typedef void (*render_fn_t)(const int16_t *mono, uint32_t count, render_cmp_t *out1, render_cmp_t *out2);

extern HRTIM_HandleTypeDef hhrtim1;
extern TIM_HandleTypeDef   htim6;
extern DMA_HandleTypeDef   hdma_tim6_up;
extern DMA_HandleTypeDef   hdma_mcmp2;

HOT_INLINE uint32_t enter_critical(void)
{
#if defined(MOD_HOST_TEST_CRITICAL_STUBS)
    return 0U;
#else
    uint32_t pm = __get_PRIMASK();
#if MOD_CRITICAL_DIAG_ENABLE
    MOD_CriticalDiagEnter(pm);
#endif
    __disable_irq();
    return pm;
#endif
}
HOT_INLINE void exit_critical(uint32_t pm)
{
#if defined(MOD_HOST_TEST_CRITICAL_STUBS)
    (void)pm;
#else
#if MOD_CRITICAL_DIAG_ENABLE
    MOD_CriticalDiagExit(pm);
#endif
    __set_PRIMASK(pm);
#endif
}
HOT_INLINE float clampf(float x, float lo, float hi) { if (x < lo) return lo; if (x > hi) return hi; return x; }
HOT_INLINE uint32_t clampu32(uint32_t x, uint32_t lo, uint32_t hi) { if (x < lo) return lo; if (x > hi) return hi; return x; }
HOT_INLINE float sanitize_nan_default(float x, float def) { return (x == x) ? x : def; }

float outphase_theta(float r);
float mod_outphase_theta_for_env(mod_type_t mode, float env);
float mod_outphase_limit_theta_for_mode(mod_type_t mode, float theta);
uint8_t mod_tx_voice_processing(void);
void mod_tx_handover_mark(void);
float mod_fixed_theta_for_mode(mod_type_t mode);
extern cw_theta_model_t cfg_cw_theta_model;

HOT_INLINE float mod_keyed_carrier_body_theta(float drive)
{
    drive = clampf(sanitize_nan_default(drive, CW_DEFAULT_DRIVE), 0.0f, 1.0f);
    if (cfg_cw_theta_model == CW_THETA_MODEL_TRUE_OUTPHASE) {
        return mod_outphase_theta_for_env(MOD_TYPE_CW, drive);
    }
    float body_theta = mod_fixed_theta_for_mode(MOD_TYPE_CW);
    float low_drive_theta = OUTPHASE_CW_DRIVE_MAX_THETA;
    if (low_drive_theta < body_theta) low_drive_theta = body_theta;
    body_theta += (1.0f - drive) * (low_drive_theta - body_theta);
    return mod_outphase_limit_theta_for_mode(MOD_TYPE_CW, body_theta);
}

HOT_INLINE float mod_keyed_carrier_effective_theta(float body_drive, float key_env, float theta_trim)
{
    float env = clampf(sanitize_nan_default(body_drive, CW_DEFAULT_DRIVE), 0.0f, 1.0f);
    key_env = clampf(sanitize_nan_default(key_env, 0.0f), 0.0f, 1.0f);
    if (cfg_cw_theta_model == CW_THETA_MODEL_TRUE_OUTPHASE) {
        return mod_outphase_limit_theta_for_mode(MOD_TYPE_CW,
                                                 outphase_theta(env * key_env) + theta_trim);
    }
    float body_theta = mod_keyed_carrier_body_theta(env) + theta_trim;
    body_theta = mod_outphase_limit_theta_for_mode(MOD_TYPE_CW, body_theta);
    float edge_theta = OUTPHASE_CW_EDGE_THETA_MAX;
    if (edge_theta < body_theta) edge_theta = body_theta;
    return mod_outphase_limit_theta_for_mode(MOD_TYPE_CW,
                                             body_theta + (1.0f - key_env) * (edge_theta - body_theta));
}

extern CarrierProfile custom_carrier;
extern float current_carrier_mhz;
extern uint32_t current_carrier_requested_hz;
extern uint8_t active_band_idx;
extern const CarrierProfile *active_carrier;

HOT_INLINE render_cmp_t mod_null_compare_tick(void)
{
#if MOD_HRTIM_ATOMIC_EXPERIMENT
    uint32_t period = active_carrier->period_ticks;
    uint32_t raw = active_carrier->null_ticks;
    if (period == 0U) return (render_cmp_t)MOD_HRTIM_COMPARE_FLOOR_MUL32;
    while (raw >= period) raw -= period;
    return (render_cmp_t)(MOD_HRTIM_COMPARE_FLOOR_MUL32 + raw);
#else
    return (render_cmp_t)active_carrier->null_ticks;
#endif
}
extern volatile uint8_t carrier_values_capped;
extern volatile uint32_t mod_hrtim_input_clock_hz;
extern volatile uint32_t mod_tim6_input_clock_hz;
extern volatile mod_clock_policy_t cfg_carrier_clock_policy;
extern volatile mod_retune_state_t carrier_retune_state;
extern volatile uint8_t carrier_retune_last_status;
extern volatile uint8_t carrier_retune_last_reason;
extern volatile uint8_t carrier_clock_retune_in_progress;
extern volatile uint8_t carrier_retune_fault_latched;
extern volatile uint8_t carrier_frequency_apply_pending;
extern volatile uint8_t cfg_precision_clock_arm;
extern volatile uint32_t carrier_retune_requested_count;
extern volatile uint32_t carrier_retune_applied_count;
extern volatile uint32_t carrier_retune_deferred_count;
extern volatile uint32_t carrier_retune_rejected_count;
extern volatile uint32_t carrier_retune_failed_count;
extern volatile uint32_t carrier_retune_last_old_clock_hz;
extern volatile uint32_t carrier_retune_last_new_clock_hz;
extern volatile int32_t carrier_retune_last_error_before_hz;
extern volatile int32_t carrier_retune_last_error_after_hz;
extern volatile uint16_t carrier_retune_last_pll_m;
extern volatile uint16_t carrier_retune_last_pll_n;
extern volatile uint16_t carrier_retune_last_pll_r;

extern volatile uint32_t requested_config;
extern volatile uint32_t active_config;
extern volatile uint8_t requested_reset;
extern uint8_t active_internal_source;
extern volatile uint8_t active_audio_source;
extern volatile uint8_t local_audio_armed;
extern volatile uint8_t local_audio_eos;
extern volatile uint8_t local_audio_io_scope;
extern volatile uint8_t local_audio_clean;
extern float active_sample_drive;
extern BranchMode active_branch_mode;
extern mod_type_t active_mod_type;
extern render_fn_t active_render_fn;
extern volatile sideband_t active_sideband_enum;
extern float active_sideband_sign;
extern volatile audio_profile_t active_audio_profile;
extern volatile uint8_t cfg_usb_clean_mode;
extern volatile usb_audio_channel_mode_t cfg_usb_channel_mode;
/* The DSP ENGINE and DEBUG A/B switches (DSP MATH, GAIN TELEMETRY, COMP
 * MATH, RF METRIC MATH, CORDIC, DEBUG BRANCH, CAL BYPASS) belong to the
 * deep-diagnostic image, whose debug UI offers them. Every other image runs
 * their factory values, as constants the renderers fold away: their setters
 * keep nothing, their getters and the settings record read these. */
#if MOD_DEEP_DIAG_ENABLE
#define MOD_DEBUG_SWITCHES 1
#else
#define MOD_DEBUG_SWITCHES 0
#define cfg_dsp_math_mode       ((dsp_math_mode_t)DSP_MATH_SAFE)
#define cfg_gain_telemetry_mode ((gain_telemetry_mode_t)GAIN_TELEMETRY_OFF)
#define cfg_comp_math_mode      ((comp_math_mode_t)COMP_MATH_LUT)
#define cfg_rf_metric_mode      ((rf_metric_mode_t)RF_METRIC_EXACT)
#define cfg_cordic_enable       ((uint8_t)1U)
#define cfg_debug_branch_mode   ((debug_branch_mode_t)DEBUG_BRANCH_BOTH)
#define cfg_debug_cal_bypass    ((uint8_t)0U)
#endif
#if MOD_DEBUG_SWITCHES
extern volatile dsp_math_mode_t cfg_dsp_math_mode;
extern volatile gain_telemetry_mode_t cfg_gain_telemetry_mode;
extern volatile comp_math_mode_t cfg_comp_math_mode;
extern volatile rf_metric_mode_t cfg_rf_metric_mode;
#endif
extern volatile uint8_t cfg_runtime_diag_enable;
extern volatile uint8_t cfg_cpu_diag_enable;
extern volatile mod_cpu_scope_mode_t cfg_cpu_scope_mode;
extern volatile ssb_pipeline_mode_t cfg_ssb_pipeline_mode;
extern volatile uint8_t rate_plan_fault_latched;
extern volatile mod_storage_backend_t cfg_storage_backend;
extern volatile uint8_t storage_sd_missing;
extern volatile ui_home_layout_t cfg_ui_home_layout;
extern volatile uint8_t cfg_ui_home_flags;
extern volatile ui_shortcut_action_t cfg_ui_shortcut_up;
extern volatile ui_shortcut_action_t cfg_ui_shortcut_down;
extern volatile uint32_t last_mode;

extern phase_cal_t phase_cal_active;
extern phase_cal_t *active_cal;
extern float theta_linearity_pd[PHASE_CAL_BINS];
extern uint16_t active_cal_flags;

extern float cfg_gate;
extern float cfg_knee;
extern float cfg_drive;
extern float cfg_ssb_phase_bias;
extern uint8_t cfg_ssb_ce_enable;
extern outphase_law_t cfg_outphase_law;
extern float cfg_ssb_ce_mix;
extern float cfg_ssb_ce_floor;
extern float cfg_ssb_ce_knee;
extern float cfg_ssb_low_cut;
extern ssb_analytic_backend_t cfg_ssb_analytic_backend;
extern tx_bw_preset_t cfg_tx_bw_preset;
#if MOD_DEBUG_SWITCHES
extern debug_branch_mode_t cfg_debug_branch_mode;
extern uint8_t cfg_debug_cal_bypass;
#endif
extern float cfg_rf_en_assert_metric;
extern float cfg_rf_en_release_metric;
extern uint8_t cfg_rf_en_hold_frames;
extern float rf_en_follow_metric;
#if MOD_DEBUG_SWITCHES /* RF METRIC FAST's table: 1 KB of RAM */
extern float rf_metric_lut[RF_METRIC_LUT_SIZE];
#endif
extern float cfg_fm_dev;
extern float cfg_fm_input_gain;
extern fm_deviation_preset_t cfg_fm_deviation_preset;
extern uint8_t cfg_fm_test_tone;
extern float cfg_am_carrier;
extern float cfg_am_depth;
extern am_profile_t cfg_am_profile;
extern float cfg_usb_input_gain;
extern float cfg_outphase_ssb_max_theta;
extern float cfg_outphase_am_max_theta;
extern float cfg_outphase_fm_theta;
extern float cfg_outphase_cw_theta;
extern float cfg_outphase_test_theta;
extern mod_outphase_fallback_t cfg_outphase_fallback_policy;
extern volatile uint8_t outphase_limit_latched;
extern volatile uint32_t outphase_limit_count;

/* The keyed-carrier helpers (mod_keyed_carrier_*) with the mode's effective theta limit
 * given (MOD_GetOutphaseEffectiveThetaLimit, fixed while a frame renders)
 * instead of recomputed -- a division and a walk of the configuration -- on
 * each of their three or four limit calls a sample. Same clamps, same order,
 * same limit counting. */
HOT_INLINE float mod_outphase_limit_theta_cap(float theta, float cap)
{
    if (!isfinite(theta)) theta = 0.0f;
    theta = clampf(theta, 0.0f, OUTPHASE_THETA_HARD_MAX);
    if (theta > cap) {
        theta = cap;
        outphase_limit_latched = 1U;
        outphase_limit_count++;
    }
    return theta;
}

/* mod_outphase_theta_for_env with the mode's cap given. */
HOT_INLINE float mod_outphase_theta_for_env_cap(float env, float cap)
{
    env = clampf(sanitize_nan_default(env, 0.0f), 0.0f, 0.999f);
    return mod_outphase_limit_theta_cap(outphase_theta(env), cap);
}

HOT_INLINE float mod_keyed_carrier_body_theta_cap(float drive, float cap)
{
    drive = clampf(sanitize_nan_default(drive, CW_DEFAULT_DRIVE), 0.0f, 1.0f);
    if (cfg_cw_theta_model == CW_THETA_MODEL_TRUE_OUTPHASE) {
        return mod_outphase_theta_for_env_cap(drive, cap);
    }
    float body_theta = mod_outphase_limit_theta_cap(cfg_outphase_cw_theta, cap); /* mod_fixed_theta_for_mode */
    float low_drive_theta = OUTPHASE_CW_DRIVE_MAX_THETA;
    if (low_drive_theta < body_theta) low_drive_theta = body_theta;
    body_theta += (1.0f - drive) * (low_drive_theta - body_theta);
    return mod_outphase_limit_theta_cap(body_theta, cap);
}

HOT_INLINE float mod_keyed_carrier_effective_theta_cap(float body_drive, float key_env, float theta_trim, float cap)
{
    float env = clampf(sanitize_nan_default(body_drive, CW_DEFAULT_DRIVE), 0.0f, 1.0f);
    key_env = clampf(sanitize_nan_default(key_env, 0.0f), 0.0f, 1.0f);
    if (cfg_cw_theta_model == CW_THETA_MODEL_TRUE_OUTPHASE) {
        return mod_outphase_limit_theta_cap(outphase_theta(env * key_env) + theta_trim, cap);
    }
    float body_theta = mod_keyed_carrier_body_theta_cap(env, cap) + theta_trim;
    body_theta = mod_outphase_limit_theta_cap(body_theta, cap);
    float edge_theta = OUTPHASE_CW_EDGE_THETA_MAX;
    if (edge_theta < body_theta) edge_theta = body_theta;
    return mod_outphase_limit_theta_cap(body_theta + (1.0f - key_env) * (edge_theta - body_theta), cap);
}

extern volatile uint32_t low_env_inhibit_count;
extern volatile uint32_t fm_silent_frame_count;
extern volatile uint32_t fm_limiter_count;
extern volatile float fm_deviation_peak_hz;
extern volatile uint32_t am_clamp_count;
extern volatile float am_pos_peak;
extern volatile float am_neg_peak;
extern volatile uint32_t usb_input_clip_count;
extern volatile uint16_t usb_input_peak_gain_raw;
extern volatile float usb_input_rms_gain;
extern volatile uint8_t usb_input_activity;
extern volatile uint32_t cessb_clip_count;
extern volatile float cessb_peak_pre;
extern volatile float cessb_peak_post_clip;
extern volatile float cessb_peak_post_filter;
extern volatile float cessb_overshoot;
extern uint8_t cfg_cessb_enable;
extern float cfg_cessb_clip;
extern uint8_t cfg_comp_enable;
extern float cfg_comp_target;
extern float cfg_comp_ratio;
extern uint8_t cfg_eq_enable;
extern float cfg_eq_bass;
extern float cfg_eq_treble;
extern volatile uint8_t cfg_muted;
extern uint8_t cfg_usb_vox;
extern uint8_t cfg_digi_tone;
extern volatile uint8_t usb_cat_ptt;
extern volatile uint8_t usb_tx_gate_closed;
/* USB audio may not transmit: TX MUTE, or the gate (USB VOX off and no CAT
 * transmit). USB packets are then held exactly as muted ones are. */
static inline uint8_t mod_usb_tx_held(void) { return (uint8_t)(cfg_muted | usb_tx_gate_closed); }
extern uint8_t cfg_mic_agc_enable;
extern float cfg_mic_agc_target;
extern float cfg_mic_agc_max_gain;
extern uint8_t cfg_ng_enable;
extern float cfg_ng_thresh;
extern uint8_t cfg_alc_enable;
extern float cfg_alc_ceiling;
extern uint8_t cfg_cessb_iter;
extern uint16_t cfg_deadtime_ticks;
extern uint16_t cfg_deadtime_rise_ticks;
extern uint16_t cfg_deadtime_fall_ticks;
extern int16_t cfg_duty_offset_ticks;
extern int16_t cfg_branch_ac_offset_ticks;
extern pa_geometry_t cfg_pa_geometry;
#if MOD_DEBUG_SWITCHES
extern volatile uint8_t cfg_cordic_enable;
#endif
extern pa_band_profile_t pa_legacy_profile;
extern pa_band_profile_t pa_band_profiles[BAND_COUNT];
extern pa_resolved_runtime_t pa_runtime;

extern cw_keyer_mode_t cfg_cw_keyer_mode;
extern cw_breakin_mode_t cfg_cw_breakin_mode;
extern float cfg_cw_wpm;
extern uint8_t cfg_cw_swap;
extern float cfg_cw_weight;
extern uint8_t cfg_cw_autospace;
extern float cfg_cw_curtis_b_dit_pct;
extern float cfg_cw_curtis_b_dah_pct;
extern uint8_t cfg_cw_latency_eighths;
extern float cfg_cw_semi_qsk_tail_dits;
extern float cfg_cw_ramp_ms;
extern float cfg_cw_drive;
extern float cfg_cw_theta_trim;
extern cw_theta_model_t cfg_cw_theta_model;
extern uint8_t cfg_cw_practice;
/* Radio memories: seconds between repeats of a memory, 0 sends it once. */
extern uint8_t cfg_cw_mem_repeat_s;
/* The trainer owns the keyer (mod_cw.c, MOD_CwSetTrainInhibit). */
extern volatile uint8_t cw_train_inhibit;
/* The keyer drives the sidetone only: practice, or the trainer. */
static inline uint8_t cw_sidetone_only(void) { return (uint8_t)((cfg_cw_practice != 0U) || (cw_train_inhibit != 0U)); }
extern float cfg_cw_debounce_ms;
extern cw_paddle_protect_mode_t cfg_cw_paddle_protect_mode;
extern float cfg_cw_paddle_arm_open_ms;
extern float cfg_cw_paddle_arm_press_ms;
extern float cfg_cw_paddle_arm_release_ms;
extern cw_both_policy_t cfg_cw_both_policy;
extern float cfg_cw_paddle_stuck_ms;
extern tx_test_both_policy_t cfg_tx_test_both_policy;
extern float cfg_tx_test_paddle_stuck_ms;
/* The CW key-edge ramp, 0..65535 for 0..1 (cw_build_ramp_lut). */
extern uint16_t cw_ramp_lut[];
extern CwEnvelopeState cw_env;
extern uint32_t cw_rf_hold_samples;
extern uint8_t cw_key_sample_active;
extern uint8_t cw_paddle_dit;
extern uint8_t cw_paddle_dah;
extern uint8_t cw_ui_key_latch;
extern uint8_t cw_tx_live;
extern uint8_t cw_rf_enabled;
extern uint8_t cw_render_frame_rf_active;
extern uint8_t cw_render_frame_tx_live;

extern volatile uint32_t usb_feedback_q10_14;
extern uint32_t dma_mcmp1[];
extern uint32_t dma_mcmp2[];
#if MOD_HRTIM_ATOMIC_EXPERIMENT
extern uint16_t dma_mcmp_pair[];
#endif
extern volatile uint32_t dma_write_half;
extern volatile uint32_t g_last_audio_tick;
extern volatile uint32_t g_last_usb_tick;
extern volatile uint32_t g_last_usb_audio_tick;
extern volatile uint32_t g_last_cw_tick;
extern volatile uint8_t dma_mcmp2_initialized;
extern volatile uint8_t dma_streams_running;
extern volatile uint8_t dma_static_idle_parked;
extern volatile uint8_t transport_dma_park_request;
extern uint8_t carrier_apply_valid;
extern uint16_t carrier_apply_period_ticks;
extern uint16_t carrier_apply_null_ticks;
extern uint8_t carrier_apply_frac_cycles;
extern int8_t carrier_apply_frac_extra;
extern uint16_t carrier_apply_deadtime_ticks;
extern uint16_t carrier_apply_deadtime_rise_ticks;
extern uint16_t carrier_apply_deadtime_fall_ticks;
extern int16_t carrier_apply_duty_offset_ticks;
extern volatile uint8_t last_carrier_apply_path;
extern uint32_t tim6_servo_frame_counter;
extern int16_t pcm_ring[];
extern volatile uint32_t pcm_wr_idx;
extern volatile uint32_t pcm_rd_idx;
extern volatile uint32_t pcm_count;
extern volatile uint8_t  pcm_push_in_progress;
extern volatile uint8_t  pcm_pop_in_progress;
extern volatile uint32_t pcm_reset_seq;
extern RenderFrame render_fifo[];
extern RenderFrame last_dma_frame;
extern volatile uint8_t last_dma_valid;
extern volatile uint32_t render_wr_idx;
extern volatile uint32_t render_rd_idx;
extern volatile uint32_t render_count;
extern volatile uint32_t render_reset_seq;
extern volatile uint32_t render_push_seq;
extern volatile uint8_t  render_push_in_progress;
extern volatile uint8_t  render_pop_in_progress;
extern volatile uint32_t render_next_seq;
extern volatile uint32_t render_dma_expected_seq;
extern volatile uint8_t  render_dma_seq_valid;
extern volatile uint32_t transport_started;
extern volatile int8_t tim6_servo_bias;
extern int32_t tim6_servo_error_avg;
extern volatile uint32_t pcm_peak;
extern volatile uint32_t pcm_drop_samples;
extern volatile uint32_t pcm_drop_during_pop_samples;
extern volatile uint32_t render_peak;
extern volatile uint32_t render_low_water;
extern volatile uint32_t render_underruns;
extern volatile uint32_t render_overflows;
extern volatile uint32_t transport_resyncs;
extern volatile uint32_t transport_underrun_streak;
extern volatile uint32_t usb_packets;
extern volatile uint32_t usb_last_submit_count;
extern volatile uint32_t usb_nonzero_packets;
extern volatile uint32_t usb_gap_resets;
extern volatile uint32_t usb_sof_count;
extern volatile uint32_t usb_missing_frames;
extern volatile uint32_t usb_packet_size_anomalies;
extern volatile uint32_t usb_rx_samples;
extern volatile uint32_t usb_packet_size_min;
extern volatile uint32_t usb_packet_size_max;
extern int32_t usb_feedback_integrator;
extern volatile uint8_t usb_stream_active;
/* An open USB stream that is no transmission: held (mod_usb_tx_held), so its
 * packets are dropped on arrival, with no over started or still draining. A
 * sound server streams silence into the card for as long as an application
 * has it open -- WSJT-X all the time it runs -- and such a stream holds
 * nothing back: the clock retune, mode changes and the card's I/O go as in
 * receive (MOD_AudioRealtimeCritical, mod_blocking_io_allowed_snapshot,
 * carrier_timing_apply_can_run_now). */
static inline uint8_t mod_usb_stream_idle_held(void)
{
    return (uint8_t)((mod_usb_tx_held() != 0U) && (transport_started == 0U) &&
                     (dma_streams_running == 0U) && !MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source));
}
extern volatile uint16_t usb_missing_frame_streak;
extern volatile uint8_t usb_alt_active;
extern volatile uint8_t usb_frame_tracking_armed;
extern volatile uint16_t usb_expected_frame_number;
extern volatile uint8_t usb_rearm_good_frames;
extern volatile uint8_t audio_seen;
extern volatile uint8_t usb_audio_seen;
extern volatile uint8_t audio_critical_reason;
extern volatile uint32_t tim6_arr_last;

extern volatile uint32_t dma_ht_count;
extern volatile uint32_t dma_tc_count;
extern volatile uint32_t dma_ch1_error_count;
extern volatile uint32_t dma_ch2_error_count;
extern volatile uint32_t dma_start_fail_count;
extern volatile uint32_t transport_dma_faults;
extern volatile uint8_t dma_expected_half;
extern volatile uint8_t dma_expected_half_valid;
extern volatile mod_pipeline_health_t pipeline_health;
extern volatile uint8_t pipeline_hard_lockout;
extern volatile uint8_t pipeline_ack_requested;
extern volatile uint8_t pipeline_rearm_pending;
extern volatile uint32_t pipeline_dma_progress_epoch;
extern volatile uint32_t apply_config_count;
extern volatile uint32_t fm_hold_mcmp1;
extern volatile uint32_t fm_hold_mcmp2;
extern volatile uint8_t  fm_hold_valid;
extern volatile uint32_t diag_branch_a;
extern volatile uint32_t diag_branch_c;
extern volatile float diag_envelope;
extern volatile float diag_theta_deg;
extern volatile float diag_phi_deg;
extern float mute_gain;
extern float mute_target;
extern float fm_phase_accum;
extern uint8_t cordic_ready;

void refresh_active_cal_ptr(void);
void sanitize_rf_en_settings(void);
void apply_audio_profile(audio_profile_t profile);
void mod_tx_pipeline_comp_lut_sync(void);
void mod_tx_pipeline_init(void);
void mod_tx_pipeline_reset(void);
void mod_tx_pipeline_select_profile(mod_config_t cfg);
void mod_tx_pipeline_process_frame(RenderFrame *s, const int16_t *mono, uint32_t count);
void mod_tx_pipeline_render_ext(void);
void mod_tx_pipeline_render_int(void);
void mod_tx_test_gate_reset(void);
void MOD_FillAllNullBuffers(void);
void MOD_UsbStreamReset(void);
void MOD_UpdateCarrierTxReadyCache(void);
uint8_t MOD_CarrierFrequencyReadyForTxFast(void);
void mod_pipeline_note(mod_pipeline_stage_t stage, mod_pipeline_reason_t reason, uint32_t value);
void mod_pipeline_note_severity(mod_pipeline_stage_t stage, mod_pipeline_reason_t reason,
                                mod_pipeline_severity_t severity, uint32_t value);
void mod_pipeline_note_controlled_reset(void);
void mod_pipeline_emergency_stop(void);
void mod_pipeline_service_rearm(uint32_t now);
void mod_dma_irq_channel1(void);
void mod_dma_irq_channel2(void);
void pcm_push(const int16_t *m, uint32_t n);
uint8_t pcm_pop_frame(int16_t *m);
void mod_tim6_scheduler_init(uint32_t timer_clk_hz, uint32_t sample_rate_hz);
uint32_t mod_tim6_next_base_arr(void);
RenderFrame *rfifo_begin(void);
void rfifo_commit(void);
void rfifo_cancel(void);
void mod_dma_stop_streams(void);
void mod_dma_park_static_idle(void);
uint8_t mod_transport_stream_needed(uint32_t now);
uint8_t mod_transport_cw_keyer_source(void);
uint8_t mod_transport_cw_keyer_busy(uint32_t now);
/* Unmuted CW with an idle keyer: allowed to retune and change mode. */
uint8_t mod_transport_cw_idle(uint32_t now);
void mod_transport_service_dma_state(uint32_t now);
void rf_en_force_idle(void);
void rf_en_force_idle_preserve_cw(void);
uint8_t MOD_RfEnIsActiveOrHolding(void);
uint8_t MOD_CarrierFrequencyApplyPending(void);
uint8_t MOD_CarrierFrequencyReadyForTx(void);
void MOD_ValidateCarrierClockForCurrentMode(void);
void MOD_ValidateCarrierClockForCurrentModeEx(uint8_t already_in_apply_config);
uint16_t mod_deadtime_ticks_limit(void);
uint32_t mod_deadtime_ps_limit(void);
uint16_t mod_deadtime_ticks_from_ps(uint32_t ps);
uint32_t mod_deadtime_ps_from_ticks(uint16_t ticks);
uint16_t mod_deadtime_ticks_from_ns(uint16_t ns);
uint16_t mod_deadtime_ticks_to_ns(uint16_t ticks);
uint16_t mod_deadtime_ns_limit(void);
void mod_pa_runtime_sync_resolved_state(void);
void mod_pa_runtime_sync_applied_state_from_shadow(void);
int16_t mod_branch_ac_limit_ticks(void);
int16_t mod_duty_ticks_from_permille_for_carrier(const CarrierProfile *carrier, int16_t permille);
int16_t mod_duty_permille_from_ticks_for_carrier(const CarrierProfile *carrier, int16_t ticks);
void pa_profiles_fill_from_legacy(void);
void pa_resolve_active_band_profile(uint8_t load_cal_context, uint8_t apply_hw, uint8_t queue_reset);
void mod_dma_start_streams(void);
/* Re-read the geometry the TX DMA fill's RM0440 step guard works from (CMP1
 * with the duty offset, the carrier fraction): at every stream start and
 * carrier apply. */
void mod_transport_refresh_step_guard(void);
uint8_t mod_usb_transport_recent_at(uint32_t now, uint32_t window_ms);
uint8_t mod_usb_audio_recent_at(uint32_t now, uint32_t window_ms);
uint8_t mod_external_render_reserve_low(void);
void mod_runtime_reconfigure_active_carrier(void);
void mod_runtime_apply_active_timing_update(void);
uint8_t mod_runtime_timing_only_update_possible(void);
void rfifo_pop_to_dma(uint32_t half_idx);
void tim6_schedule(void);

void cw_build_ramp_lut(void);
void cw_recompute_timing(void);
void cw_reset_state(void);
uint8_t cw_read_paddles_logical_raw(uint8_t *dit_out, uint8_t *dah_out);
void cw_paddle_guard_service(uint32_t now);
uint8_t cw_get_paddles_for_cw(uint8_t *dit_out, uint8_t *dah_out);
uint8_t cw_get_paddles_for_tx_test(uint8_t *dit_out, uint8_t *dah_out);
uint8_t cw_paddle_guard_state(void);
uint8_t cw_paddle_guard_is_armed(void);
uint8_t cw_paddle_guard_is_faulted(void);
void cw_paddle_guard_reset(void);
HOT_RENDER void render_cw(const int16_t *mono, uint32_t count, render_cmp_t *out1, render_cmp_t *out2);
uint8_t cw_keyer_monitor_only(void);
void update_custom_carrier_geometry(float mhz);
void update_custom_carrier_geometry_hz(uint32_t hz);
void mod_recompute_carrier_for_current_clock(void);
uint8_t band_index_for_freq(float mhz);
void load_current_calibration_context(void);
void sanitize_ssb_runtime_settings(void);
void sanitize_audio_runtime_settings(void);
void sanitize_outphase_policy_settings(void);
void mod_rate_plan_validate_active(void);
void build_outphase_lut(void);
void recompute_eq_coeffs(void);
void mark_profile_custom(void);
void sanitize_cw_settings(void);
uint8_t ssb_backend_is_hilbert(ssb_analytic_backend_t backend);
uint8_t cal_backend_storage_key(uint8_t backend);
float mute_gain_step(void);
void mute_gain_step_n(uint32_t samples);
void build_idle_branch_pair(uint32_t *ta, uint32_t *tc);
void outphase_branch_deltas(float theta, float cal, float *a_delta, float *c_delta);
void route_branch_pair(render_cmp_t *out_a, render_cmp_t *out_c, uint32_t ta, uint32_t tc);
BranchMode mod_effective_branch_mode(void);
void route_branch_pair_dma(uint32_t *out_a, uint32_t *out_c, uint32_t ta, uint32_t tc);
void apply_branch_ac_offset(uint32_t *ta, uint32_t *tc);
void rf_en_commit_frame(float peak_metric, uint8_t any_active);
void rf_en_reset_tracking(void);

/* Inline for the renderers' two calls a sample; see mod_hrtim_phase_raw_tick. */
#if MOD_HRTIM_ATOMIC_EXPERIMENT
/* On a period the caller holds: the block renderer reads the carrier once a
 * frame, where a compare store (uint16_t) could alias period_ticks and made
 * every sample reload it. */
HOT_INLINE uint32_t phase_compare_encode_raw_tick_period(uint32_t period, uint32_t raw_tick)
{
    if (period == 0U) return MOD_HRTIM_COMPARE_FLOOR_MUL32;
    while (raw_tick >= period) raw_tick -= period;
    return MOD_HRTIM_COMPARE_FLOOR_MUL32 + raw_tick;
}

HOT_INLINE uint32_t phase_compare_encode_raw_tick(uint32_t raw_tick)
{
    return phase_compare_encode_raw_tick_period(active_carrier->period_ticks, raw_tick);
}

HOT_INLINE uint32_t phase_to_ticks_period(uint16_t period, float phase)
{
    return phase_compare_encode_raw_tick_period(period,
                                                mod_hrtim_phase_raw_tick(period, phase));
}
#endif

HOT_INLINE uint32_t phase_to_ticks(float phase)
{
#if MOD_HRTIM_ATOMIC_EXPERIMENT
    return phase_compare_encode_raw_tick(
        mod_hrtim_phase_raw_tick(active_carrier->period_ticks, phase));
#else
    /* Keep the live path branch-cheap, but make the cold corrupted-calibration
     * path robust enough to absorb arbitrary finite phase values. */
    phase = mod_phase_wrap_pi(phase);
    uint16_t origin = active_carrier->half_period_ticks;
    if ((origin < active_carrier->min_compare_ticks) ||
        (origin > active_carrier->max_compare_ticks)) {
        origin = (uint16_t)(((uint32_t)active_carrier->min_compare_ticks +
                             (uint32_t)active_carrier->max_compare_ticks + 1U) / 2U);
    }
    float tf = phase * (float)active_carrier->half_period_ticks + (float)origin;
    int32_t t = (int32_t)(tf + 0.5f);
    if (t < 0)                                      t += (int32_t)active_carrier->period_ticks;
    if (t >= (int32_t)active_carrier->period_ticks) t -= (int32_t)active_carrier->period_ticks;
    if (t < (int32_t)active_carrier->min_compare_ticks) {
        t = (int32_t)active_carrier->min_compare_ticks;
        outphase_limit_latched = 1U;
    }
    if (t > (int32_t)active_carrier->max_compare_ticks) {
        t = (int32_t)active_carrier->max_compare_ticks;
        outphase_limit_latched = 1U;
    }
    return (uint32_t)t;
#endif
}

/* One step of the RF enable follower on explicit state: the block renderer
 * keeps the follower in a local for the frame. */
HOT_INLINE void rf_en_track_step(float *follow, float source_metric, float *frame_peak, uint8_t *frame_active)
{
    float target = isfinite(source_metric) ? source_metric : 0.0f;
    if (!isfinite(*follow)) *follow = 0.0f;
    if (target <= 0.0f) target = 0.0f;
    else if (target >= 1.0f) target = 1.0f;

    float coeff = (target > *follow) ? RF_EN_SSB_ATTACK_COEFF : RF_EN_SSB_RELEASE_COEFF;
    *follow += coeff * (target - *follow);
    if (*follow < RF_EN_TRACK_FLOOR) *follow = 0.0f;
    if (*follow > *frame_peak) *frame_peak = *follow;
    if (*follow > 0.0f) *frame_active = 1U;
}

/* Inline: every SSB renderer calls it once a sample. */
HOT_INLINE void rf_en_track_ssb_sample(float source_metric, float *frame_peak, uint8_t *frame_active)
{
    float target = isfinite(source_metric) ? source_metric : 0.0f;
    if (!isfinite(rf_en_follow_metric)) rf_en_follow_metric = 0.0f;
    if (target <= 0.0f) target = 0.0f;
    else if (target >= 1.0f) target = 1.0f;

    float coeff = (target > rf_en_follow_metric) ? RF_EN_SSB_ATTACK_COEFF : RF_EN_SSB_RELEASE_COEFF;
    rf_en_follow_metric += coeff * (target - rf_en_follow_metric);
    if (rf_en_follow_metric < RF_EN_TRACK_FLOOR) rf_en_follow_metric = 0.0f;
    if (rf_en_follow_metric > *frame_peak) *frame_peak = rf_en_follow_metric;
    if (rf_en_follow_metric > 0.0f) *frame_active = 1U;
}

/* theta(r) is in [0, 0.5] of a cycle; stored as Q17 fixed point (step
 * 7.6e-6 of a cycle, a six-hundredth of one HRTIM tick even at 28 MHz), in
 * half the RAM of floats. Built by build_outphase_lut (mod_outphase.c). */
#define OUTPHASE_LUT_SIZE  2048U
#define OUTPHASE_LUT_SCALE 131070.0f
extern uint16_t outphase_lut[OUTPHASE_LUT_SIZE];

/* Inline for the SSB block renderer's one lookup a sample; outphase_theta
 * is this as a call for everyone else. */
HOT_INLINE float outphase_theta_lookup(float r)
{
    r = mod_control_sanitize(r, 0.0f, 0.0f, 1.0f);
    const float unit = 1.0f / OUTPHASE_LUT_SCALE;
    if (r <= 0.0f) return (float)outphase_lut[0] * unit;
    if (r >= 1.0f) return (float)outphase_lut[OUTPHASE_LUT_SIZE - 1U] * unit;
    float fidx = r * (float)(OUTPHASE_LUT_SIZE - 1U);
    uint32_t lo = (uint32_t)fidx;
    float frac = fidx - (float)lo;
    if (lo >= OUTPHASE_LUT_SIZE - 1U) return (float)outphase_lut[OUTPHASE_LUT_SIZE - 1U] * unit;
    const float a = (float)outphase_lut[lo], b = (float)outphase_lut[lo + 1U];
    return (a + frac * (b - a)) * unit;
}

HOT_INLINE float rf_output_metric_from_theta(float theta)
{
#if MOD_DEBUG_SWITCHES
    if (cfg_rf_metric_mode == RF_METRIC_FAST) {
        if (theta <= 0.0f) return 1.0f;
        if (theta >= 0.5f) return 0.0f;
        float pos = theta * (float)(RF_METRIC_LUT_SIZE - 1U) * 2.0f;
        uint32_t lo = (uint32_t)pos;
        if (lo >= RF_METRIC_LUT_SIZE - 1U) return rf_metric_lut[RF_METRIC_LUT_SIZE - 1U];
        float frac = pos - (float)lo;
        return rf_metric_lut[lo] + frac * (rf_metric_lut[lo + 1U] - rf_metric_lut[lo]);
    }
#endif
    float m = cosf(theta * MOD_PI_F);
    return (m < 0.0f) ? -m : m;
}

HOT_INLINE float rf_en_metric_from_env_theta(float env, float theta)
{
    if ((cfg_outphase_law == OUTPHASE_LAW_CLASSIC) ||
        (cfg_outphase_law == OUTPHASE_LAW_CLASSIC_SWAP)) {
        return clampf(env, 0.0f, 1.0f);
    }
    return rf_output_metric_from_theta(theta);
}

HOT_INLINE void build_branch_pair_from_relative(float common_phase,
                                                float a_relative,
                                                float c_relative,
                                                uint32_t *ta,
                                                uint32_t *tc)
{
    *ta = phase_to_ticks(common_phase + a_relative);
    *tc = phase_to_ticks(common_phase + c_relative);
    apply_branch_ac_offset(ta, tc);
}

void rf_en_gpio_set(uint8_t on);

#endif
