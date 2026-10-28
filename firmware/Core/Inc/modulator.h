/**
 * @file    modulator.h
 * @brief   Chireix outphasing multi-mode modulator — public API.
 *
 * Signal chains:
 *   SSB: USB Audio (48 kHz mono) -> optional speech conditioning -> Weaver or
 *        Hilbert analytic generation -> selectable exact/CORDIC/FAST phase math
 *        -> outphasing LUT -> render FIFO -> TIM6-triggered HRTIM MCMP1/2 DMA
 *   FM:  USB Audio -> pre-emphasis -> phase integration -> constant-envelope outphasing FM
 *   AM:  USB Audio -> envelope scaling -> symmetric outphasing angle, fixed carrier phase
 *   CW/TX TEST: keyed-carrier body theta -> RF-safe render FIFO -> HRTIM
 */
#ifndef MODULATOR_H
#define MODULATOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ────────────── Firmware version ────────────── */
/* This is the firmware-facing version source. tools/update_version.py keeps
 * the numeric components synchronized with VERSION; version_tool.py verifies
 * the expanded C string through a compiler instead of literal text searches. */
#define FW_VERSION_MAJOR 9
#define FW_VERSION_MINOR 0
#define FW_VERSION_STRINGIFY_INNER(value) #value
#define FW_VERSION_STRINGIFY(value) FW_VERSION_STRINGIFY_INNER(value)
#if FW_VERSION_MINOR < 10
#define FW_VERSION_MINOR_STRING "0" FW_VERSION_STRINGIFY(FW_VERSION_MINOR)
#else
#define FW_VERSION_MINOR_STRING FW_VERSION_STRINGIFY(FW_VERSION_MINOR)
#endif
#define FW_VERSION_STRING "v" FW_VERSION_STRINGIFY(FW_VERSION_MAJOR) "." FW_VERSION_MINOR_STRING
#define FW_VERSION_BCD ((uint16_t)((((FW_VERSION_MAJOR / 10) % 10) << 12) | \
                                   ((FW_VERSION_MAJOR % 10) << 8) | \
                                   (((FW_VERSION_MINOR / 10) % 10) << 4) | \
                                   (FW_VERSION_MINOR % 10)))
#define FW_VERSION_INQUIRY_MAJOR ((int8_t)('0' + (FW_VERSION_MAJOR % 10)))
#define FW_VERSION_INQUIRY_MINOR_TENS ((int8_t)('0' + ((FW_VERSION_MINOR / 10) % 10)))
#define FW_VERSION_INQUIRY_MINOR_ONES ((int8_t)('0' + (FW_VERSION_MINOR % 10)))
#ifndef MOD_DEEP_DIAG_ENABLE
#define MOD_DEEP_DIAG_ENABLE 0
#endif
/* The qualified H12P analytic candidate is intentionally independent of deep
 * diagnostics so it can be evaluated in the normal O2 Release-Speed image.
 * The build includes the candidate, while the persisted runtime default
 * remains WEAVER until RF measurements justify promotion. */
#ifndef MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE
#define MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE 1
#endif
#if (MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE != 0) && (MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE != 1)
#error "MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE must be 0 or 1"
#endif
/* Alternate per-branch envelope control remains a host-tested mathematical
 * contract. REV1's production envelope path is implemented by radio_analog.c;
 * this optional model stays disabled so it cannot add target buffers or pins. */
#ifndef MOD_DUAL_ENVELOPE_EXPERIMENT_ENABLE
#define MOD_DUAL_ENVELOPE_EXPERIMENT_ENABLE 0
#endif
#if (MOD_DUAL_ENVELOPE_EXPERIMENT_ENABLE != 0) && (MOD_DUAL_ENVELOPE_EXPERIMENT_ENABLE != 1)
#error "MOD_DUAL_ENVELOPE_EXPERIMENT_ENABLE must be 0 or 1"
#endif
#ifndef MOD_HRTIM_ATOMIC_EXPERIMENT
#define MOD_HRTIM_ATOMIC_EXPERIMENT 1
#endif
#if !MOD_HRTIM_ATOMIC_EXPERIMENT
#error "REV1 requires one atomic HRTIM DMA request; duplicate TIM6 requests are unsupported"
#endif
#ifndef MOD_CESSB_EXPERIMENT_ENABLE
#define MOD_CESSB_EXPERIMENT_ENABLE MOD_DEEP_DIAG_ENABLE
#endif
#if MOD_CESSB_EXPERIMENT_ENABLE && (!MOD_DEEP_DIAG_ENABLE || !MOD_HRTIM_ATOMIC_EXPERIMENT)
#error "The unvalidated CESSB experiment is restricted to RF-inhibited diagnostic builds"
#endif
#ifndef MOD_SSB_ENVELOPE_EXPERIMENT_ENABLE
#define MOD_SSB_ENVELOPE_EXPERIMENT_ENABLE MOD_DEEP_DIAG_ENABLE
#endif
#if MOD_SSB_ENVELOPE_EXPERIMENT_ENABLE && (!MOD_DEEP_DIAG_ENABLE || !MOD_HRTIM_ATOMIC_EXPERIMENT)
#error "Unvalidated nonlinear SSB envelope shaping is restricted to RF-inhibited diagnostic builds"
#endif
#define MOD_HRTIM_DLL_CAL_TIMEOUT_MS 50U
#define MOD_HRTIM_DLL_CAL_RETRIES    3U

#define CW_CURTIS_B_PCT_MIN        0.0f
#define CW_CURTIS_B_PCT_MAX        100.0f
#define CW_LATENCY_EIGHTHS_MAX     8U

/* ────────────── Buffer / Frame Sizes ────────────── */
#define AUDIO_FRAME_SIZE   48U
#define DMA_BUF_SIZE       96U
#define RF_INTERPOLATION   4U
#define RF_FRAME_SIZE      (AUDIO_FRAME_SIZE * RF_INTERPOLATION)
#define RF_DMA_SIZE        (DMA_BUF_SIZE * RF_INTERPOLATION)
/* TX PHASE 384K doubles the phase/NCO updates per audio sample. The buffers
 * are sized for it; the factor in use is latched at each TX stream start. */
#define RF_INTERPOLATION_MAX 8U
#define RF_FRAME_SIZE_MAX  (AUDIO_FRAME_SIZE * RF_INTERPOLATION_MAX)
#define RF_DMA_SIZE_MAX    (DMA_BUF_SIZE * RF_INTERPOLATION_MAX)
#define MOD_USB_AUDIO_LEGAL_MIN_SAMPLES (AUDIO_FRAME_SIZE - 1U)
#define MOD_USB_AUDIO_LEGAL_MAX_SAMPLES (AUDIO_FRAME_SIZE + 1U)

/* ────────────── Phase Calibration ────────────── */
/* Envelope-indexed phase predistortion bins. */
#define PHASE_CAL_BINS     32U

/* ────────────── Extern DMA state ────────────── */
extern volatile uint32_t dma_write_half;
extern volatile uint32_t g_last_audio_tick;
extern volatile uint32_t g_last_usb_tick;
extern volatile uint32_t g_last_usb_audio_tick;
extern uint32_t dma_mcmp1[DMA_BUF_SIZE];
extern uint32_t dma_mcmp2[DMA_BUF_SIZE];
#if MOD_HRTIM_ATOMIC_EXPERIMENT
extern uint16_t dma_mcmp_pair[RF_DMA_SIZE_MAX * 2U];
#endif

/* ────────────── Modulation Type ────────────── */
typedef enum {
    MOD_TYPE_SSB = 0,
    MOD_TYPE_FM,
    MOD_TYPE_AM,
    MOD_TYPE_CW,
    MOD_TYPE_TEST,
    MOD_TYPE_COUNT
} mod_type_t;

typedef enum {
    MOD_AUDIO_SOURCE_NONE = 0,
    MOD_AUDIO_SOURCE_USB,
    MOD_AUDIO_SOURCE_SD,
    MOD_AUDIO_SOURCE_MIC
} mod_audio_source_t;
#define MOD_AUDIO_SOURCE_IS_LOCAL(s) ((s) == MOD_AUDIO_SOURCE_SD || (s) == MOD_AUDIO_SOURCE_MIC)

/* ────────────── Sideband ────────────── */
typedef enum {
    SIDEBAND_USB = 0,
    SIDEBAND_LSB = 1
} sideband_t;

typedef enum {
    CW_KEYER_STRAIGHT = 0,
    CW_KEYER_IAMBIC_A,
    CW_KEYER_IAMBIC_B,
    CW_KEYER_ULTIMATIC,
    CW_KEYER_NONSQUEEZE,
    CW_KEYER_COUNT
} cw_keyer_mode_t;

typedef enum {
    CW_BREAKIN_FULL = 0,
    CW_BREAKIN_SEMI,
    CW_BREAKIN_COUNT
} cw_breakin_mode_t;

typedef enum {
    CW_PADDLE_PROTECT_OFF = 0,
    CW_PADDLE_PROTECT_ARM,
    CW_PADDLE_PROTECT_ARM_STUCK,
    /* USER keeps the same numeric value/storage slot as the previous CUSTOM
     * mode. It uses the configurable arming and both-paddle policy settings
     * rather than a distinct state machine. Do not reorder. */
    CW_PADDLE_PROTECT_CUSTOM,
    CW_PADDLE_PROTECT_COUNT
} cw_paddle_protect_mode_t;

typedef enum {
    CW_BOTH_ALLOW = 0,
    CW_BOTH_TIMER,
    CW_BOTH_IGNORE,
    CW_BOTH_FAULT,
    CW_BOTH_COUNT
} cw_both_policy_t;

typedef enum {
    TXTEST_BOTH_ALLOW = 0,
    TXTEST_BOTH_IGNORE,
    TXTEST_BOTH_FAULT,
    TXTEST_BOTH_COUNT
} tx_test_both_policy_t;

/* ────────────── SSB Analytic Backend ────────────── */
typedef enum {
    SSB_ANALYTIC_WEAVER = 0,
    SSB_ANALYTIC_HILBERT,          /* legacy 48 kHz single-Hilbert path */
    SSB_ANALYTIC_HILBERT_PAIR_24,  /* experimental matched-pair Hilbert at 24 ksps */
    SSB_ANALYTIC_HILBERT_PAIR_12,  /* experimental matched-pair Hilbert at 12 ksps */
    SSB_ANALYTIC_H12P,             /* v6.70 12 ksps production-candidate path */
    SSB_ANALYTIC_COUNT
} ssb_analytic_backend_t;

static inline uint8_t MOD_SsbAnalyticBackendBuildAvailable(ssb_analytic_backend_t backend)
{
    if (backend == SSB_ANALYTIC_WEAVER) return 1U;
    if (backend == SSB_ANALYTIC_H12P) {
        return MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE ? 1U : 0U;
    }
    if ((backend == SSB_ANALYTIC_HILBERT) ||
        (backend == SSB_ANALYTIC_HILBERT_PAIR_24) ||
        (backend == SSB_ANALYTIC_HILBERT_PAIR_12)) {
        return MOD_DEEP_DIAG_ENABLE ? 1U : 0U;
    }
    return 0U;
}

typedef enum {
    USB_AUDIO_POLICY_VOICE_PROC = 0,
    USB_AUDIO_POLICY_DIGI_CLEAN,
    USB_AUDIO_POLICY_COUNT
} usb_audio_policy_t;

typedef enum {
    FM_DEVIATION_PRESET_NFM_2K5 = 0,
    FM_DEVIATION_PRESET_FM_5K,
    FM_DEVIATION_PRESET_TEST_7K5,
    FM_DEVIATION_PRESET_USER,
    FM_DEVIATION_PRESET_COUNT
} fm_deviation_preset_t;

typedef enum {
    AM_PROFILE_EFFICIENT = 0,
    AM_PROFILE_LINEAR_TEST,
    AM_PROFILE_COUNT
} am_profile_t;

/* ────────────── TX Bandwidth Preset ────────────── */
typedef enum {
    TX_BW_100_3000 = 0,
    TX_BW_200_2900,
    TX_BW_300_2700,
    TX_BW_300_2400,
    TX_BW_COUNT
} tx_bw_preset_t;

/* ────────────── Debug branch routing ────────────── */
typedef enum {
    DEBUG_BRANCH_BOTH = 0,
    DEBUG_BRANCH_A_ONLY,
    DEBUG_BRANCH_C_ONLY,
    DEBUG_BRANCH_COUNT
} debug_branch_mode_t;

/* ────────────── Outphasing law selection ──────────────
 *
 * Normal RF operation uses CLASSIC or CLASSIC_SWAP as a branch direction law:
 * both use the live classic amplitude map where envelope 1 -> theta 0 and
 * envelope 0 -> theta 0.5. LEGACY is retained as an experimental inverted-
 * envelope diagnostic and should not be used for ordinary SSB/CW/TX testing.
 */
typedef enum {
    OUTPHASE_LAW_LEGACY = 0,
    OUTPHASE_LAW_CLASSIC,
    OUTPHASE_LAW_CLASSIC_SWAP,
    OUTPHASE_LAW_COUNT
} outphase_law_t;

typedef enum {
    CW_THETA_MODEL_V633_SAFE = 0,
    CW_THETA_MODEL_TRUE_OUTPHASE,
    CW_THETA_MODEL_COUNT
} cw_theta_model_t;

#define CW_THETA_MODEL_DEFAULT CW_THETA_MODEL_V633_SAFE

/* PA geometry: the polarity of branch A's drive against branch C's, for
 * output transformers that are not wound alike. A INVERTED swaps A's
 * complementary HRTIM outputs (PA8/PA9), turning A by 180 degrees in every
 * drain-modulation mode; the compare values, and so the envelope, are
 * untouched. Inverting C instead gives the same combined output with only
 * its absolute phase turned, so it is not offered, and neither is inverting
 * both (NORMAL, turned). Which branch takes +theta is THETA DIRECTION
 * (outphase_law_t); it matters only when the branches outphase.
 *
 * Through v8.05 this was a nine-entry PA WIRING MAP plus per-branch sign/RF180
 * flags and a sign scope. The mirrored maps negated one branch's theta,
 * which leaves no amplitude control with either transformer polarity (the
 * default, A MIR, gave the wrong output on the first TX bench run); the
 * RF180 flags turned the compare values, which envelope mode discards, and
 * the sign flags conjugated one branch, which no wiring does. The stored byte
 * keeps the two old values for these states (0 A SWAP, 1 NORMAL), so older
 * firmware reads it the same way; the storage load converts the rest. */
typedef enum {
    PA_GEOMETRY_A_INVERTED = 0,
    PA_GEOMETRY_NORMAL,
    PA_GEOMETRY_COUNT
} pa_geometry_t;

#define PA_GEOMETRY_DEFAULT PA_GEOMETRY_NORMAL

/* ────────────── Audio Processing Profile ────────────── */
typedef enum {
    AUDIO_PROFILE_CLEAN = 0,
    AUDIO_PROFILE_ENHANCED,
    AUDIO_PROFILE_DX,
    AUDIO_PROFILE_QRP,
    AUDIO_PROFILE_CUSTOM,
    AUDIO_PROFILE_USB_HIFI,
    AUDIO_PROFILE_COUNT
} audio_profile_t;

/* ────────────── USB Audio Packet Handling ────────────── */
typedef enum {
    USB_AUDIO_CHAN_LEFT = 0,
    USB_AUDIO_CHAN_RIGHT,
    USB_AUDIO_CHAN_AVERAGE,
    USB_AUDIO_CHAN_STRONGER,
    USB_AUDIO_CHAN_COUNT
} usb_audio_channel_mode_t;

/* ────────────── Runtime math / acceleration policy ────────────── */
typedef enum {
    DSP_MATH_LEGACY = 0,
    DSP_MATH_SAFE,
    DSP_MATH_FAST,
    DSP_MATH_COUNT
} dsp_math_mode_t;

/* ────────────── Gain telemetry policy ────────────── */
typedef enum {
    GAIN_TELEMETRY_OFF = 0,
    GAIN_TELEMETRY_UI,
    GAIN_TELEMETRY_LIVE,
    GAIN_TELEMETRY_COUNT
} gain_telemetry_mode_t;

/* ────────────── Compressor math backend ────────────── */
typedef enum {
    COMP_MATH_EXACT = 0,
    COMP_MATH_LUT,
    COMP_MATH_COUNT
} comp_math_mode_t;

/* ────────────── RF helper metric implementation ────────────── */
typedef enum {
    RF_METRIC_EXACT = 0,
    RF_METRIC_FAST,
    RF_METRIC_COUNT
} rf_metric_mode_t;

/* ────────────── GPIO CPU load scope ────────────── */
typedef enum {
    MOD_CPU_SCOPE_OFF = 0,
    MOD_CPU_SCOPE_USB_SUBMIT,
    MOD_CPU_SCOPE_RENDER_EXT,
    MOD_CPU_SCOPE_DMA_POP,
    MOD_CPU_SCOPE_SERVICE_BG,
    MOD_CPU_SCOPE_LCD_SERVICE,
    MOD_CPU_SCOPE_FULL_AUDIO_RF,
    MOD_CPU_SCOPE_COUNT
} mod_cpu_scope_mode_t;

/* ────────────── SSB pipeline scheduling policy ────────────── */
typedef enum {
    SSB_PIPELINE_LEGACY = 0,
    SSB_PIPELINE_STRICT_QMX12_EXP,
    SSB_PIPELINE_COUNT
} ssb_pipeline_mode_t;

typedef struct {
    uint32_t input_rate_hz;
    uint32_t dsp_rate_hz;
    uint32_t rf_update_rate_hz;
    uint16_t hilbert_taps;
    uint8_t  decimation;
    uint8_t  interpolation;
    uint8_t  low_rate_samples_per_frame;
    uint8_t  rf_samples_per_frame;
    uint8_t  strict_frame;
    uint8_t  backend;
    const char *label;
} mod_rate_plan_t;

/* ────────────── UI layout / shortcuts ────────────── */
typedef enum {
    UI_HOME_LAYOUT_DETAILED = 0,
    UI_HOME_LAYOUT_STANDARD,
    UI_HOME_LAYOUT_COMPACT,
    UI_HOME_LAYOUT_COUNT
} ui_home_layout_t;

typedef enum {
    UI_SHORTCUT_AB_SWAP = 0,
    UI_SHORTCUT_QUICK,
    UI_SHORTCUT_BAND,
    UI_SHORTCUT_MODES,
    UI_SHORTCUT_SYSTEM,
    UI_SHORTCUT_MENU,
    UI_SHORTCUT_NONE,
    UI_SHORTCUT_COUNT
} ui_shortcut_action_t;

#define UI_HOME_FLAG_METER   0x01U
#define UI_HOME_FLAG_GAINS   0x02U
#define UI_HOME_FLAG_HEALTH  0x04U
#define UI_HOME_FLAG_RFEN    0x08U
#define UI_HOME_FLAG_HINTS   0x10U
#define UI_HOME_FLAG_VREF    0x20U
#define UI_HOME_FLAGS_DEFAULT (UI_HOME_FLAG_METER | UI_HOME_FLAG_GAINS | UI_HOME_FLAG_HEALTH | UI_HOME_FLAG_RFEN | UI_HOME_FLAG_HINTS | UI_HOME_FLAG_VREF)

/* ────────────── Mode Enumeration ────────────── */
/* Note: the _14MHZ suffixes in these legacy enum names are historical only.
 * Runtime carrier selection now comes from the band/frequency model. */
typedef enum {
    /* Production SSB (FROZEN baseline — render math is not modified) */
    MOD_CFG_PROD_SSB_14MHZ = 0,

    /* Enhanced experimental SSB modes */
    MOD_CFG_ENHANCED_SSB_14MHZ,
    MOD_CFG_RAW_DIRECT_14MHZ,

    /* Filtered SSB (legacy, kept for comparison) */
    MOD_CFG_FILTERED_STD_14MHZ,
    MOD_CFG_FILTERED_NARROW_14MHZ,

    /* FM mode */
    MOD_CFG_FM_NARROW_14MHZ,

    /* AM mode */
    MOD_CFG_AM_14MHZ,

    /* CW mode */
    MOD_CFG_CW_14MHZ,

    /* Internal test/diagnostic modes */
    MOD_CFG_INT_1KHZ_TONE,
    MOD_CFG_INT_2TONE_IMD,
    MOD_CFG_INT_2TONE_BRANCH_A,
    MOD_CFG_INT_2TONE_BRANCH_C,
    MOD_CFG_INT_SWEEP,

    /* RAW LEGACY SSB baseline (reuses the old compatibility enum slot) */
    MOD_CFG_BASELINE_27MHZ,

    /* Internal SSB isolation modes */
    MOD_CFG_INT_SSB_1KHZ,
    MOD_CFG_INT_SSB_2TONE,
    MOD_CFG_INT_SSB_SWEEP,
    MOD_CFG_INT_THETA_SWEEP,

    /* Direct RF carrier TX test: actual HRTIM carrier, CW-paddle gated */
    MOD_CFG_TX_CARRIER_TEST,

    MOD_CFG_COUNT
} mod_config_t;

/* ────────────── Audio Level Metering ────────────── */
typedef struct {
    uint16_t peak_raw;
    uint16_t peak_hold;
    int16_t  peak_db;        /* dBFS × 10 */
    float    rms;
} audio_meter_t;

/* Legacy v2-v19 persisted calibration payload.  These bytes are retained for
 * explicit conversion only and are never applied directly. */
typedef struct {
    float static_trim;
    float envelope_table[PHASE_CAL_BINS];
    float amp_trim_a;
    float amp_trim_c;
} phase_cal_legacy_t;

/* Typed SSB calibration domains.  Values are normalized half-cycle phase
 * units: 1.0 is pi radians.  differential_skew is added to branch A and
 * subtracted from branch C.  Common phase is applied equally to both. */
typedef struct {
    float differential_skew;
    float common_phase_pd[PHASE_CAL_BINS];
    float reserved[2];
} phase_cal_t;

_Static_assert(sizeof(phase_cal_t) == sizeof(phase_cal_legacy_t),
               "typed calibration must preserve the v19 settings footprint");

#define SSB_CAL_FLAG_VALIDATED          0x0001U
#define SSB_CAL_FLAG_APPLY              0x0002U
#define SSB_CAL_FLAG_LEGACY_REVALIDATE  0x0004U
#define SSB_CAL_FLAGS_MASK              0x0007U

/* ────────────── TX State ────────────── */
typedef enum {
    TX_STATE_RX      = 0,   /* muted / receive */
    TX_STATE_TX      = 1,   /* actively transmitting */
    TX_STATE_WAIT    = 2,   /* armed, waiting for audio */
    TX_STATE_SILENCE = 3    /* stream present but silent */
} tx_state_t;

typedef enum {
    MOD_PIPELINE_STAGE_NONE = 0,
    MOD_PIPELINE_STAGE_USB,
    MOD_PIPELINE_STAGE_PCM,
    MOD_PIPELINE_STAGE_RENDER,
    MOD_PIPELINE_STAGE_DMA,
    MOD_PIPELINE_STAGE_HRTIM,
    MOD_PIPELINE_STAGE_SCHEDULER,
    MOD_PIPELINE_STAGE_CONTROL,
    MOD_PIPELINE_STAGE_COUNT
} mod_pipeline_stage_t;

typedef enum {
    MOD_PIPELINE_REASON_NONE = 0,
    MOD_PIPELINE_REASON_USB_MISSING_PACKET,
    MOD_PIPELINE_REASON_USB_BAD_PACKET_SIZE,
    MOD_PIPELINE_REASON_USB_ZERO_PACKET,
    MOD_PIPELINE_REASON_USB_OUT_INCOMPLETE,
    MOD_PIPELINE_REASON_USB_REARM_FAILED,
    MOD_PIPELINE_REASON_USB_FEEDBACK_TX_FAILED,
    MOD_PIPELINE_REASON_USB_HW_ERROR,
    MOD_PIPELINE_REASON_USB_FRAME_SEQUENCE,
    MOD_PIPELINE_REASON_PCM_OVERFLOW,
    MOD_PIPELINE_REASON_PCM_RESET_DISCARD,
    MOD_PIPELINE_REASON_RENDER_UNDERRUN,
    MOD_PIPELINE_REASON_RENDER_OVERFLOW,
    MOD_PIPELINE_REASON_RENDER_RESET_DISCARD,
    MOD_PIPELINE_REASON_RENDER_SEQUENCE_GAP,
    MOD_PIPELINE_REASON_RENDER_DEADLINE,
    MOD_PIPELINE_REASON_DMA_ERROR,
    MOD_PIPELINE_REASON_DMA_START_FAILED,
    MOD_PIPELINE_REASON_DMA_SEQUENCE_GAP,
    MOD_PIPELINE_REASON_DMA_CNDTR_ANOMALY,
    MOD_PIPELINE_REASON_DMA_FLAGS_COALESCED,
    MOD_PIPELINE_REASON_DMA_REFILL_LATE,
    MOD_PIPELINE_REASON_DMA_CHANNEL_SKEW,
    MOD_PIPELINE_REASON_HRTIM_COMPARE_RANGE,
    MOD_PIPELINE_REASON_HRTIM_FAULT,
    MOD_PIPELINE_REASON_RATE_PLAN_INVALID,
    MOD_PIPELINE_REASON_SCHED_IO_DEFERRED,
    MOD_PIPELINE_REASON_SCHED_UI_DEFERRED,
    MOD_PIPELINE_REASON_CONTROLLED_STREAM_RESET,
    MOD_PIPELINE_REASON_MODE_NOT_AVAILABLE,
    MOD_PIPELINE_REASON_COUNT
} mod_pipeline_reason_t;

typedef enum {
    MOD_PIPELINE_SEVERITY_NONE = 0,
    MOD_PIPELINE_SEVERITY_WARNING,
    MOD_PIPELINE_SEVERITY_CONCEALED,
    MOD_PIPELINE_SEVERITY_HARD
} mod_pipeline_severity_t;

typedef enum {
    MOD_USB_PIPE_EVENT_ZERO_PACKET = 0,
    MOD_USB_PIPE_EVENT_OUT_INCOMPLETE,
    MOD_USB_PIPE_EVENT_REARM_FAILED,
    MOD_USB_PIPE_EVENT_FEEDBACK_TX_FAILED,
    MOD_USB_PIPE_EVENT_STREAM_ACTIVE,
    MOD_USB_PIPE_EVENT_STREAM_INACTIVE
} mod_usb_pipeline_event_t;

typedef struct {
    uint32_t total_events;
    uint32_t soft_fault_events;
    uint32_t controlled_reset_events;
    uint32_t sticky_stage_mask;
    uint32_t stage_events[MOD_PIPELINE_STAGE_COUNT];
    uint32_t reason_events[MOD_PIPELINE_REASON_COUNT];
    uint32_t last_tick_ms;
    uint32_t last_value;
    uint32_t first_tick_ms;
    uint32_t first_value;
    uint8_t  soft_fault_latched;
    uint8_t  last_stage;
    uint8_t  last_reason;
    uint8_t  last_severity;
    uint8_t  first_stage;
    uint8_t  first_reason;
    uint8_t  first_severity;
    uint8_t  sticky_latched;
    uint8_t  hard_lockout;
    uint8_t  rearm_pending;
    uint8_t  reserved0;
} mod_pipeline_health_t;

typedef enum {
    MOD_BUILD_FLAVOR_PROD = 0,
    MOD_BUILD_FLAVOR_DIAG
} mod_build_flavor_t;

typedef enum {
    MOD_STORAGE_FLASH = 0,
    MOD_STORAGE_SD    = 1
} mod_storage_backend_t;

typedef enum {
    MOD_CARRIER_APPLY_PATH_NONE = 0,
    MOD_CARRIER_APPLY_PATH_FULL_RECONFIG,
    MOD_CARRIER_APPLY_PATH_TIMING_UPDATE_STOPPED,
    MOD_CARRIER_APPLY_PATH_TIMING_UPDATE_LIVE
} mod_carrier_apply_path_t;

typedef enum {
    MOD_CLOCK_POLICY_FIXED_170 = 0,
    MOD_CLOCK_POLICY_SAFE_DYNAMIC,
    MOD_CLOCK_POLICY_PRECISION_DYNAMIC,
    /* The default since v8.04: receive plan first, then the smallest TX NCO
     * residual; better clocks are taken as idle-time upgrades (mod_band.c).
     * Appended so stored values 0..2 keep their meaning. */
    MOD_CLOCK_POLICY_AUTO,
    MOD_CLOCK_POLICY_COUNT
} mod_clock_policy_t;

typedef enum {
    MOD_RETUNE_IDLE = 0,
    MOD_RETUNE_PENDING,
    MOD_RETUNE_WAITING_FOR_IDLE,
    MOD_RETUNE_APPLYING,
    MOD_RETUNE_FAILED,           /* reserved: unrecovered non-fallback failure */
    MOD_RETUNE_FAILED_RECOVERED,
    MOD_RETUNE_FAILED_FALLBACK
} mod_retune_state_t;

typedef enum {
    MOD_CAPABILITY_VALID = 0,
    MOD_CAPABILITY_LIMITED,
    MOD_CAPABILITY_COMPRESSED,
    MOD_CAPABILITY_NOT_RECOMMENDED
} mod_capability_t;

typedef enum {
    MOD_OUTPHASE_FALLBACK_COMPRESS = 0,
    MOD_OUTPHASE_FALLBACK_WARN,
    MOD_OUTPHASE_FALLBACK_BLOCK,
    MOD_OUTPHASE_FALLBACK_COUNT
} mod_outphase_fallback_t;

/* ────────────── Status Report ────────────── */
typedef struct {
    /* PCM ring buffer */
    uint32_t pcm_count;
    uint32_t pcm_capacity;
    uint32_t pcm_peak;
    uint32_t pcm_drop_samples;
    uint32_t pcm_drop_during_pop_samples;

    /* Render FIFO */
    uint32_t render_count;
    uint32_t render_capacity;
    uint32_t render_peak;
    uint32_t render_low_water;
    uint32_t render_underruns;
    uint32_t render_overflows;

    /* USB transport */
    uint32_t usb_packets;
    uint32_t usb_last_submit_count;
    uint32_t usb_nonzero_packets;
    uint32_t usb_gap_resets;
    uint32_t usb_sof_count;
    uint32_t usb_missing_frames;
    uint32_t usb_packet_size_anomalies;
    uint32_t usb_rx_samples;
    uint32_t usb_packet_size_min;
    uint32_t usb_packet_size_max;
    uint32_t transport_resyncs;

    /* Timing */
    uint32_t last_audio_tick_ms;
    uint32_t last_usb_tick_ms;
    uint32_t last_usb_audio_tick_ms;
    uint32_t tim6_arr_last;
    int32_t  servo_pending;

    /* Carrier/clock diagnostics */
    uint32_t carrier_requested_hz;
    uint32_t carrier_actual_hz;
    int32_t  carrier_error_hz;
    uint32_t hrtim_input_hz;
    uint32_t hrtim_effective_mhz;
    uint32_t tim6_input_hz;
    uint16_t carrier_period_ticks;
    uint16_t carrier_phase_step_mdeg;
    uint32_t hrtim_tick_ps;
    uint32_t deadtime_tick_ps;
    uint8_t  clock_policy;
    uint8_t  retune_state;
    uint8_t  retune_last_status;
    uint8_t  retune_last_reason;
    uint8_t  retune_in_progress;
    uint8_t  retune_fault_latched;
    uint8_t  carrier_apply_pending;
    uint8_t  carrier_tx_ready;
    uint16_t reserved_clock_state;
    uint32_t retune_requested_count;
    uint32_t retune_applied_count;
    uint32_t retune_deferred_count;
    uint32_t retune_rejected_count;
    uint32_t retune_failed_count;
    uint32_t retune_last_old_clock_hz;
    uint32_t retune_last_new_clock_hz;
    int32_t  retune_last_error_before_hz;
    int32_t  retune_last_error_after_hz;
    uint16_t pll_m;
    uint16_t pll_n;
    uint16_t pll_r;
    uint16_t reserved_clock_diag;
    float    outphase_hw_theta_deg;
    float    outphase_req_theta_deg;
    float    outphase_eff_theta_deg;
    uint8_t  outphase_capability;
    uint8_t  outphase_limited;
    uint32_t outphase_limit_count;
    uint32_t low_env_inhibit_count;
    uint16_t outphase_reserved0;

    /* State flags */
    uint8_t transport_started;
    uint8_t internal_source;
    uint8_t audio_seen;
    uint8_t usb_stream_active;
    uint8_t usb_audio_seen;
    uint8_t audio_critical_reason;
    mod_pipeline_health_t pipeline;
    uint8_t usb_clean_mode;
    uint8_t usb_audio_policy;
    uint8_t usb_channel_mode;
    uint8_t debug_branch_mode;
    uint8_t debug_cal_bypass;

    /* Active configuration snapshot */
    mod_type_t mod_type;
    sideband_t sideband;
    mod_config_t config;
    audio_profile_t audio_profile;
    uint8_t cordic_enabled;
    uint8_t dsp_math_mode;
    uint8_t rf_metric_mode;
    uint8_t cpu_scope_mode;
    uint8_t ssb_pipeline_mode;
    uint8_t rate_plan_decimation;
    uint8_t rate_plan_interpolation;
    uint32_t rate_plan_input_hz;
    uint32_t rate_plan_dsp_hz;
    uint32_t rate_plan_rf_hz;
    uint16_t rate_plan_hilbert_taps;
    uint8_t rate_plan_low_samples;
    uint8_t rate_plan_rf_samples;
    uint8_t rate_plan_strict_frame;
    uint8_t rate_plan_fault;

    /* Audio metering */
    audio_meter_t meter;
    float    usb_gain;
    uint16_t usb_peak_gain_raw;
    float    usb_rms_gain;
    uint32_t usb_clip_count;
    uint8_t  usb_activity;

    /* Branch diagnostics */
    uint32_t branch_a_last_ticks;
    uint32_t branch_c_last_ticks;
    float    last_envelope;
    float    last_theta_deg;
    float    last_phi_deg;

    /* Live DSP telemetry (for UI meters) */
    float    comp_gain_db;      /* instantaneous compressor gain (negative = attenuation) */
    float    alc_gain_db;       /* look-ahead ALC gain in dB */
    float    mic_agc_gain_db;   /* mic AGC applied gain in dB */
    uint8_t  gate_active;       /* 1 if noise gate is currently closing the signal */
    uint8_t  cessb_clipping;    /* diagnostic clip/filter experiment only */
    uint32_t cessb_clip_count;
    float    cessb_peak_pre;
    float    cessb_peak_post_clip;
    float    cessb_peak_post_filter;
    float    cessb_overshoot;

    /* AM/FM telemetry */
    float    fm_input_gain;
    float    fm_deviation_peak_hz;
    uint32_t fm_limiter_count;
    uint32_t fm_silent_frame_count;
    uint8_t  fm_test_tone;
    uint8_t  fm_deviation_preset;
    uint8_t  am_profile;
    uint32_t am_clamp_count;
    float    am_pos_peak;
    float    am_neg_peak;

    /* TX / mute state */
    uint8_t  tx_muted;          /* 1 = software PTT OFF (mute engaged) */
    uint8_t  carrier_values_capped; /* 1 if carrier change safety-clamped DT/A-C/duty */
    uint8_t  last_carrier_apply_path; /* full reconfigure vs timing-only live/stopped */
    uint32_t cordic_timeouts;   /* cumulative hardware CORDIC timeout fallbacks */
    uint32_t hrtim_mcr;
    uint32_t hrtim_oenr;
    uint32_t hrtim_mcmp1r;
    uint32_t hrtim_mcmp2r;
    uint32_t hrtim_dta_dtxr;
    uint32_t hrtim_dtc_dtxr;
    uint32_t tim6_cr1;
    uint32_t dma_ch1_cndtr;
    uint32_t dma_ch2_cndtr;
    uint32_t dma_ht_count;
    uint32_t dma_tc_count;
    uint32_t dma_ch1_error_count;
    uint32_t dma_ch2_error_count;
    uint32_t dma_start_fail_count;
    uint32_t transport_dma_faults;
    uint32_t apply_config_count;
    uint32_t error_handler_count;
    tx_state_t tx_state;        /* current logical TX state */
    uint8_t  cw_paddle_dit_active; /* live-or-latched DIT input for low-rate UI redraws */
    uint8_t  cw_paddle_dah_active; /* live-or-latched DAH input for low-rate UI redraws */
    uint8_t  cw_paddle_guard_state; /* 0 wait-open, 1 wait-both, 2 wait-release, 3 armed, 4 stuck */
    uint8_t  cw_paddle_armed;       /* 1 after plug/open, both press, both release arming sequence */
    uint8_t  cw_rf_active;         /* CW RF gate state after practice/mute logic */
} modulator_status_t;

typedef struct {
    uint32_t render_count;
    uint32_t render_capacity;
    uint32_t last_audio_tick_ms;
    uint32_t last_usb_tick_ms;
    uint32_t last_usb_audio_tick_ms;
    uint8_t  transport_started;
    uint8_t  internal_source;
    uint8_t  usb_audio_seen;
    mod_type_t mod_type;
    mod_config_t config;
} modulator_status_fast_t;

typedef enum {
    MOD_CPU_DIAG_SERVICE_BG = 0,
    MOD_CPU_DIAG_RENDER_EXT,
    MOD_CPU_DIAG_USB_SUBMIT,
    MOD_CPU_DIAG_DMA_POP,
    MOD_CPU_DIAG_LCD_SERVICE,
    MOD_CPU_DIAG_UI_BUILD,
    MOD_CPU_DIAG_STATUS_FULL,
    MOD_CPU_DIAG_STORAGE_SAVE,
    MOD_CPU_DIAG_RETUNE,
    MOD_CPU_DIAG_COUNT
} mod_cpu_diag_id_t;

typedef struct {
    uint32_t calls;
    uint32_t last_cycles;
    uint32_t avg_cycles;
    uint32_t max_cycles;
} mod_cpu_diag_section_t;

typedef struct {
    uint8_t enabled;
    uint8_t dwt_available;
    uint32_t cpu_hz;
    mod_cpu_diag_section_t section[MOD_CPU_DIAG_COUNT];
} mod_cpu_diag_snapshot_t;


/* ────────────── Backward Compatibility Aliases ────────────── */
typedef mod_config_t mod_test_config_t;
#define MOD_TEST_CFG_BASELINE_RAW_LIMITED_14MHZ  MOD_CFG_PROD_SSB_14MHZ
#define MOD_TEST_CFG_BASELINE_RAW_DIRECT_14MHZ   MOD_CFG_RAW_DIRECT_14MHZ
#define MOD_TEST_CFG_FILTERED_STD_14MHZ          MOD_CFG_FILTERED_STD_14MHZ
#define MOD_TEST_CFG_FILTERED_NARROW_14MHZ       MOD_CFG_FILTERED_NARROW_14MHZ
#define MOD_TEST_CFG_INT_1KHZ_TONE               MOD_CFG_INT_1KHZ_TONE
#define MOD_TEST_CFG_INT_2TONE_IMD               MOD_CFG_INT_2TONE_IMD
#define MOD_TEST_CFG_INT_2TONE_BRANCH_A          MOD_CFG_INT_2TONE_BRANCH_A
#define MOD_TEST_CFG_INT_2TONE_BRANCH_C          MOD_CFG_INT_2TONE_BRANCH_C
#define MOD_TEST_CFG_BASELINE_27MHZ              MOD_CFG_BASELINE_27MHZ
#define MOD_TEST_CFG_COUNT                       MOD_CFG_COUNT

/* ────────────── Public API ────────────── */

/* Init */
void MOD_Init(void);
void MOD_StartDMA(void);
uint8_t MOD_RfDmaStaticIdleParked(void);
/* The RF DMA and its renderer are running (a transmission, or the tail of
 * one); IRQ-safe. */
uint8_t MOD_RfDmaStreamsRunning(void);
/* The RF DMA progress watchdog's last trip (RENDER_DEADLINE): the pacing
 * chain's registers at that moment, so a stall explains itself. */
typedef struct {
    uint32_t tick_ms, epoch, hrtim_mcr, timf_dier;
    uint16_t count, dma_ccr, dma_cndtr, tim6_cr1;
    uint8_t ticks, radio_state, config, render_count;
} mod_rf_stall_t;
void MOD_GetRfStall(mod_rf_stall_t *out);
/* Stream stops the main loop deferred because the transmitter was still up. */
uint32_t MOD_TransportStopDeferred(void);

/* Audio input — USB ISR and foreground-only local SD ingress. */
void MOD_SubmitUsbPacket(const int16_t *mono, uint32_t count, uint16_t usb_frame_number);
void MOD_RecordUsbPacketAnomaly(uint16_t usb_frame_number, uint32_t byte_count);
void MOD_RecordUsbPipelineEvent(mod_usb_pipeline_event_t event, uint32_t value);
void MOD_RecordUsbHardwareFault(uint16_t usb_frame_number, uint16_t usb_status);
void MOD_UsbSofTick(uint16_t usb_frame_number);
void MOD_RecordHrtimFault(uint32_t hrtim_status);
void MOD_UsbStreamReset(void);
void MOD_RadioResetTransport(void); /* foreground board reconfiguration */
uint32_t MOD_GetUsbFeedback10_14(void);
uint8_t MOD_LocalAudioAcquire(void);
uint8_t MOD_LocalAudioAcquireSource(mod_audio_source_t source);
void MOD_LocalAudioRelease(void);
uint8_t MOD_LocalAudioSubmit(const int16_t *mono, uint32_t count);
uint8_t MOD_LocalAudioArm(void);
void MOD_LocalAudioEndOfStream(void);
uint8_t MOD_LocalAudioDrainComplete(void);
uint32_t MOD_LocalAudioQueuedSamples(void);
uint32_t MOD_LocalAudioPcmFreeSamples(void);
mod_audio_source_t MOD_GetAudioSource(void);
uint8_t MOD_LocalAudioIoBegin(void);
void MOD_LocalAudioIoEnd(void);
uint8_t MOD_LocalAudioIoScopeActive(void);
void MOD_LocalAudioSetClean(uint8_t clean);

/* Mode control */
void MOD_SetTestConfig(mod_config_t cfg);
mod_config_t MOD_GetTestConfig(void);
void MOD_ResetDSPState(void);
void MOD_HandleLongPress(void);
uint8_t MOD_ModeHasInternalSource(void);

/* Sideband control */
void MOD_SetSideband(sideband_t sb);
sideband_t MOD_GetSideband(void);

/* Typed SSB calibration. Value edits clear VALIDATED but preserve the explicit
 * APPLY state so a bench operator can observe adjustments while calibrating. */
void MOD_SetCalibrationDifferentialSkew(float correction);
float MOD_GetCalibrationDifferentialSkew(void);
void MOD_SetCalibrationCommonPhaseBin(uint8_t bin, float correction);
float MOD_GetCalibrationCommonPhaseBin(uint8_t bin);
void MOD_SetCalibrationThetaBin(uint8_t bin, float correction);
float MOD_GetCalibrationThetaBin(uint8_t bin);
void MOD_SetCalibrationValidated(uint8_t validated);
uint8_t MOD_GetCalibrationValidated(void);
void MOD_SetCalibrationApply(uint8_t apply);
uint8_t MOD_GetCalibrationApply(void);
uint8_t MOD_GetCalibrationLegacyPending(void);
void MOD_ResetCalibration(void);
const phase_cal_t *MOD_GetCalibration(void);

/* Background service (main loop) */
void MOD_ServiceBackground(uint32_t now);
void MOD_RequestRealtimeRender(void);
void MOD_RealtimeRenderWorker(void);
void MOD_RealtimeHealthTick(void);
void mod_dma_irq_channel1(void);
void mod_dma_irq_channel2(void);
uint8_t MOD_BlockingIoAllowed(uint32_t now);
uint8_t MOD_BackgroundIoAllowed(uint32_t now);
void MOD_FillAllNullBuffers(void);
void MOD_TxCarrierTestStop(void);
uint8_t MOD_TxCarrierTestPaddleActive(void);
void MOD_ApplyRequestedConfigNow(void);

/* Status */
void MOD_GetStatus(modulator_status_t *status);
void MOD_GetStatusFast(modulator_status_fast_t *status);
void MOD_ClearPipelineHealth(void);
void MOD_GetPipelineHealthSnapshot(mod_pipeline_health_t *snapshot);
/* Short names for the pipeline enums and the RADIO_FAULT_* bits. Shared by the
 * LCD fault banner and the USB console so both read identically. */
const char *MOD_PipelineStageName(uint8_t stage);
const char *MOD_PipelineReasonName(uint8_t reason);
const char *MOD_PipelineSeverityName(uint8_t severity);
const char *MOD_RadioFaultName(uint8_t bit);

/* Recent pipeline events, newest first.
 *
 * pipeline_health keeps only the first and the last event, which says what
 * broke but not in what order -- and order is usually the diagnosis. This is a
 * small ring of the last few, enough to see a sequence without keeping a log.
 * Routine scheduler deferrals are not recorded: they are bookkeeping and would
 * push the interesting entries out.
 *
 * Stored packed at 8 bytes an entry, because the deep-diagnostic flavour has
 * about 60 bytes of SRAM left above its policy floor and this has to fit in
 * that. `age_ms` is held as 16 bits, so it is exact for the first 65 s after
 * an event and wraps after that; this answers "what just happened", not "what
 * happened an hour ago". The struct below is what a caller receives, unpacked
 * onto its own stack, so its size costs nothing. */
#define MOD_PIPELINE_LOG_DEPTH 6U
typedef struct {
    uint32_t value;
    uint16_t age_ms;
    uint8_t  stage, reason, severity;
} mod_pipeline_event_t;
/* age index 0 is the most recent. Returns 0 once the ring has no older entry. */
uint8_t MOD_GetPipelineEvent(uint8_t index, mod_pipeline_event_t *out);
/* Entries logged since boot, wrapping at 256: (seq - seen) of the newest
 * are new to a reader that saw `seen` (the ages wrap, so they cannot say). */
uint8_t MOD_PipelineEventSeq(void);
void MOD_RequestPipelineFaultAcknowledge(void);
uint8_t MOD_PipelineRfLockedOut(void);
uint8_t MOD_PipelineNotificationPending(void);
mod_build_flavor_t MOD_GetBuildFlavor(void);

/* CPU profiling diagnostics; disabled by default and inactive unless enabled. */
#if MOD_DEEP_DIAG_ENABLE
#define MOD_CPU_DIAG_DECLARE(name_, id_) uint32_t name_ = MOD_CpuDiagBegin((id_))
#define MOD_CPU_DIAG_FINISH(id_, name_)  MOD_CpuDiagEnd((id_), (name_))
void MOD_SetCpuDiagEnable(uint8_t en);
uint8_t MOD_GetCpuDiagEnable(void);
void MOD_ResetCpuDiag(void);
uint32_t MOD_CpuDiagBegin(mod_cpu_diag_id_t id);
void MOD_CpuDiagEnd(mod_cpu_diag_id_t id, uint32_t start_cycles);
void MOD_GetCpuDiagSnapshot(mod_cpu_diag_snapshot_t *snapshot);
void MOD_SetCpuScopeMode(mod_cpu_scope_mode_t mode);
mod_cpu_scope_mode_t MOD_GetCpuScopeMode(void);
const char *MOD_GetCpuScopeModeLabel(mod_cpu_scope_mode_t mode);
uint8_t MOD_CpuScopeModeActive(mod_cpu_scope_mode_t mode);
void MOD_CpuScopeBegin(mod_cpu_scope_mode_t mode);
void MOD_CpuScopeEnd(mod_cpu_scope_mode_t mode);
#else
#define MOD_CPU_DIAG_DECLARE(name_, id_)
#define MOD_CPU_DIAG_FINISH(id_, name_)  ((void)0)
static inline void MOD_SetCpuDiagEnable(uint8_t en) { (void)en; }
static inline uint8_t MOD_GetCpuDiagEnable(void) { return 0U; }
static inline void MOD_ResetCpuDiag(void) { }
static inline uint32_t MOD_CpuDiagBegin(mod_cpu_diag_id_t id) { (void)id; return 0U; }
static inline void MOD_CpuDiagEnd(mod_cpu_diag_id_t id, uint32_t start_cycles)
{ (void)id; (void)start_cycles; }
static inline void MOD_GetCpuDiagSnapshot(mod_cpu_diag_snapshot_t *snapshot)
{
    if (snapshot == 0) return;
    snapshot->enabled = 0U;
    snapshot->dwt_available = 0U;
    snapshot->cpu_hz = 0U;
    for (uint32_t i = 0U; i < (uint32_t)MOD_CPU_DIAG_COUNT; i++) {
        snapshot->section[i].calls = 0U;
        snapshot->section[i].last_cycles = 0U;
        snapshot->section[i].avg_cycles = 0U;
        snapshot->section[i].max_cycles = 0U;
    }
}
static inline void MOD_SetCpuScopeMode(mod_cpu_scope_mode_t mode) { (void)mode; }
static inline mod_cpu_scope_mode_t MOD_GetCpuScopeMode(void) { return MOD_CPU_SCOPE_OFF; }
static inline const char *MOD_GetCpuScopeModeLabel(mod_cpu_scope_mode_t mode)
{ (void)mode; return "OFF"; }
static inline uint8_t MOD_CpuScopeModeActive(mod_cpu_scope_mode_t mode)
{ (void)mode; return 0U; }
static inline void MOD_CpuScopeBegin(mod_cpu_scope_mode_t mode) { (void)mode; }
static inline void MOD_CpuScopeEnd(mod_cpu_scope_mode_t mode) { (void)mode; }
#endif

/* Mode metadata */
const char *MOD_GetModeLabel(mod_config_t cfg);
mod_type_t MOD_GetModeType(mod_config_t cfg);
uint8_t MOD_ModeIsFrozenBaseline(mod_config_t cfg);
uint8_t MOD_ModeIsProduction(mod_config_t cfg);
uint8_t MOD_ModeAvailableInBuild(mod_config_t cfg);

/* Carrier frequency / HRTIM clock */
void MOD_SetCarrierFreqMHz(float mhz);
void MOD_SetCarrierFreqHz(uint32_t hz);
float MOD_GetCarrierFreqMHz(void);
uint32_t MOD_GetCarrierRequestedHz(void);
float MOD_GetActualCarrierFreqMHz(void);
/* The carrier grid the HRTIM makes (period and fraction), and its distance
 * from the dial, before the TX NCO removes it; both from the corrected
 * reference. */
uint32_t MOD_GetActualCarrierFreqHz(void);
int32_t MOD_GetCarrierErrorHz(void);
int64_t MOD_GetCarrierErrorMilliHz(void);
/* The carrier fraction: returns N, the periods per master period, and puts
 * k, the ticks the master adds, in *extra (2 and 0: a whole period). */
uint8_t MOD_GetCarrierFraction(int8_t *extra);
/* Level of the strongest image the carrier grid leaves, dBc: the TX NCO's at
 * +-(phase-update rate), or the fraction's nudge. */
int32_t MOD_GetCarrierImageDbc(void);
/* SYSCLK as it really runs: PLL setting in use, REF CAL applied, micro-Hz. */
uint64_t MOD_SysclkMicroHz(void);
/* TX phase/NCO update rate in Hz (192 kHz, or 384 kHz with PHASE 384K): the
 * running stream's, or the one the next stream will use. _Next is always
 * the latter: what the carrier plan is made for. */
uint32_t MOD_TxPhaseUpdateHz(void);
uint32_t MOD_TxPhaseUpdateHzNext(void);
/* A PLL switch is under way or failed without fallback: nothing may run on
 * the clock tree. A pending frequency or clock change is not unsettled. */
uint8_t MOD_CarrierClockUnsettled(void);
/* AUTO has a better PLL setting queued for the next idle moment. */
uint8_t MOD_CarrierClockUpgradePending(void);
/* 1 keeps that upgrade waiting (the TX TEST page: no PLL move under a bench
 * measurement); 0 lets it go. It holds the key-down hop too. */
void MOD_CarrierUpgradeHold(uint8_t hold);
/* AUTO's key-down hop: a transmission about to start, nothing of it on the
 * air or in the DMA yet, moves the PLL to the setting CLOCK_ChooseTx prefers
 * (mostly 12 and 15 m), dropping any frames rendered on the old one, and
 * queues the way back for once the radio is idle. Not under a TX TEST grid
 * preset or with a retune or frequency change pending. 1 if it moved. */
uint8_t MOD_TxClockHop(void);
/* TX TEST's grid presets: 1..MOD_TxTestDiagPresetCount() puts the dial on
 * the running clock's carrier grid (GRID, no TX NCO step), just short of
 * halfway to the next grid point (HALF, fraction 1/2) or where the carrier
 * plan's image cost peaks (WORST) on 20/17/15/12/10 m; 0 clears it (the
 * dial stays). The label is NULL for 0. */
uint8_t MOD_TxTestDiagPresetCount(void);
void MOD_TxTestDiagPreset(uint8_t preset);
uint8_t MOD_TxTestDiagPresetActive(void);
const char *MOD_TxTestDiagLabel(void);
/* What `clock` on the console reports about the carrier clock. */
typedef struct {
    uint8_t  policy, pll_m, pll_n, pll_r;
    uint8_t  retune_state, last_reason, upgrade_pending, apply_pending;
    uint8_t  fault_latched, tx_timers_parked;
    uint16_t period_ticks;
    uint32_t applied, failed, deferred, rejected, tx_hops;
} mod_clock_snapshot_t;
void MOD_GetClockSnapshot(mod_clock_snapshot_t *s);
/* Re-plan the carrier for the current dial, e.g. after REF CAL changed. */
void MOD_CarrierReplan(void);
/* Stop (1) or restart (0) the TX HRTIM timers while nothing transmits; see
 * RADIO_CLOCK_FLAG_PARK_TX. Parking refuses while a TX stream runs. */
void MOD_TxTimersPark(uint8_t park);
uint8_t MOD_TxTimersParked(void);
uint32_t MOD_GetHrtimInputClockHz(void);
uint64_t MOD_GetHrtimEffectiveClockHz(void);
uint32_t MOD_GetTim6InputClockHz(void);
uint16_t MOD_GetCarrierPeriodTicks(void);
uint16_t MOD_GetCarrierHalfPeriodTicks(void);
uint8_t MOD_GetActiveBandIndex(void);
void MOD_SetCarrierClockPolicy(mod_clock_policy_t policy);
mod_clock_policy_t MOD_GetCarrierClockPolicy(void);
const char *MOD_GetCarrierClockPolicyLabel(mod_clock_policy_t policy);
mod_retune_state_t MOD_GetCarrierRetuneState(void);
void MOD_ServiceCarrierClockRetune(void);
uint8_t MOD_CarrierClockCanRetuneNow(void);
uint8_t MOD_CarrierFrequencyApplyPending(void);
uint8_t MOD_CarrierFrequencyReadyForTx(void);
/* Schedule a policy/clock revalidation. This does not by itself mark the
 * carrier unsafe; callers that defer a real carrier-frequency apply must
 * also set carrier-frequency apply-pending via the carrier-frequency API. */
void MOD_ScheduleCarrierClockRetune(uint32_t hz, uint32_t delay_ms);

/* Audio processing profile */
void MOD_SetAudioProfile(audio_profile_t profile);
audio_profile_t MOD_GetAudioProfile(void);
const char *MOD_GetAudioProfileLabel(audio_profile_t profile);

/* USB audio front-end policy */
void MOD_SetUsbCleanMode(uint8_t en);
uint8_t MOD_GetUsbCleanMode(void);
void MOD_SetUsbChannelMode(usb_audio_channel_mode_t mode);
usb_audio_channel_mode_t MOD_GetUsbChannelMode(void);

/* Runtime math / implementation policy */
void MOD_SetDspMathMode(dsp_math_mode_t mode);
dsp_math_mode_t MOD_GetDspMathMode(void);
void MOD_SetGainTelemetryMode(gain_telemetry_mode_t mode);
gain_telemetry_mode_t MOD_GetGainTelemetryMode(void);
void MOD_SetCompMathMode(comp_math_mode_t mode);
comp_math_mode_t MOD_GetCompMathMode(void);
void MOD_SetRfMetricMode(rf_metric_mode_t mode);
rf_metric_mode_t MOD_GetRfMetricMode(void);
void MOD_SetSsbPipelineMode(ssb_pipeline_mode_t mode);
ssb_pipeline_mode_t MOD_GetSsbPipelineMode(void);
const char *MOD_GetSsbPipelineModeLabel(ssb_pipeline_mode_t mode);
const mod_rate_plan_t *MOD_GetActiveRatePlan(void);
uint8_t MOD_NoncriticalUiAllowed(uint32_t now);

/* Hardware-aware outphasing policy. Values are fractional half-cycle theta
 * units: 0.05 = 9 electrical degrees of branch separation per side. */
void  MOD_SetOutphaseSsbMaxTheta(float theta);
float MOD_GetOutphaseSsbMaxTheta(void);
void  MOD_SetOutphaseAmMaxTheta(float theta);
float MOD_GetOutphaseAmMaxTheta(void);
void  MOD_SetOutphaseFmTheta(float theta);
float MOD_GetOutphaseFmTheta(void);
void  MOD_SetOutphaseCwTheta(float theta);
float MOD_GetOutphaseCwTheta(void);
void  MOD_SetOutphaseTestTheta(float theta);
float MOD_GetOutphaseTestTheta(void);
void MOD_SetOutphaseFallbackPolicy(mod_outphase_fallback_t policy);
mod_outphase_fallback_t MOD_GetOutphaseFallbackPolicy(void);
const char *MOD_GetOutphaseFallbackPolicyLabel(mod_outphase_fallback_t policy);
float MOD_GetOutphaseHardwareThetaLimit(void);
float MOD_GetOutphaseRequestedThetaLimit(mod_type_t mode);
float MOD_GetOutphaseEffectiveThetaLimit(mod_type_t mode);
mod_capability_t MOD_GetOutphaseCapability(mod_type_t mode);
const char *MOD_GetOutphaseCapabilityLabel(mod_capability_t cap);

/* Runtime diagnostics capture */
void MOD_SetRuntimeDiagEnable(uint8_t en);
uint8_t MOD_GetRuntimeDiagEnable(void);

/* UI preferences (flash-backed with the main settings blob) */
void MOD_SetUiHomeLayout(ui_home_layout_t layout);
ui_home_layout_t MOD_GetUiHomeLayout(void);
void MOD_SetUiHomeFlags(uint8_t flags);
uint8_t MOD_GetUiHomeFlags(void);
void MOD_SetUiShortcutUp(ui_shortcut_action_t action);
ui_shortcut_action_t MOD_GetUiShortcutUp(void);
void MOD_SetUiShortcutDown(ui_shortcut_action_t action);
ui_shortcut_action_t MOD_GetUiShortcutDown(void);

/* Acceleration / implementation status */
uint8_t MOD_GetCordicEnabled(void);
uint8_t MOD_RunCordicSelfTest(void);

/* Board reference status. Boot measures VDDA through the STM32 VREFINT ADC
 * channel once, then keeps the value for UI/boot diagnostics without runtime ADC
 * polling in the audio/RF path. */
uint8_t  MOD_GetVrefEnabled(void);
uint8_t  MOD_GetVrefReady(void);
uint8_t  MOD_GetVrefFault(void);
uint16_t MOD_GetVrefMillivolts(void);
uint32_t MOD_GetResetCsr(void);
const char *MOD_GetResetReasonLabel(void);

/* Runtime parameters */
void MOD_SetDrive(float d);   float MOD_GetDrive(void);
void MOD_SetGate(float g);    float MOD_GetGate(void);
void MOD_SetKnee(float k);    float MOD_GetKnee(void);
void MOD_SetFmDeviation(float d); float MOD_GetFmDeviation(void);
void MOD_SetFmDeviationPreset(fm_deviation_preset_t preset);
fm_deviation_preset_t MOD_GetFmDeviationPreset(void);
void MOD_SetFmTestTone(uint8_t en); uint8_t MOD_GetFmTestTone(void);
void MOD_SetFmInputGain(float g); float MOD_GetFmInputGain(void);
void MOD_SetAmCarrier(float c);   float MOD_GetAmCarrier(void);
void MOD_SetAmDepth(float d);     float MOD_GetAmDepth(void);
void MOD_SetAmProfile(am_profile_t profile);
am_profile_t MOD_GetAmProfile(void);
void MOD_SetUsbAudioPolicy(usb_audio_policy_t policy);
usb_audio_policy_t MOD_GetUsbAudioPolicy(void);
void MOD_SetUsbInputGain(float g); float MOD_GetUsbInputGain(void);
/* USB audio transmit gate, as QMX: with USB VOX off USB audio transmits only
 * while CAT transmit is on (MOD_SetUsbCatPtt, from TX;/TQ1;). USB VOX on
 * keys on any arriving audio, as before v8.10. TRANSMIT MUTE wins over both. */
void MOD_SetUsbVox(uint8_t on);     uint8_t MOD_GetUsbVox(void);
void MOD_SetUsbCatPtt(uint8_t on);  uint8_t MOD_GetUsbCatPtt(void);
/* DIGI TONE (default ON): USB audio under DIGI CLEAN is transmitted the way
 * QMX does it -- the tone is measured and sent as a constant carrier at full
 * drive, whatever the PC's level. OFF: the linear SSB modulator.
 * MOD_DigiToneHz: the carrier's offset while DIGI TONE renders, else 0. */
void MOD_SetDigiTone(uint8_t on);   uint8_t MOD_GetDigiTone(void);
float MOD_DigiToneHz(void);

/* Unvalidated CESSB-like clip/filter experiment. Production builds force this
 * off; it is available only in the RF-inhibited deep-diagnostic image. */
void MOD_SetCessbEnable(uint8_t en); uint8_t MOD_GetCessbEnable(void);
void MOD_SetCessbClip(float c);      float   MOD_GetCessbClip(void);

/* Experimental: TX Compression */
void MOD_SetCompEnable(uint8_t en); uint8_t MOD_GetCompEnable(void);
void MOD_SetCompTarget(float t);    float   MOD_GetCompTarget(void);
void MOD_SetCompRatio(float r);     float   MOD_GetCompRatio(void);

/* Experimental: TX EQ */
void MOD_SetEqEnable(uint8_t en); uint8_t MOD_GetEqEnable(void);
void MOD_SetEqBass(float b);      float   MOD_GetEqBass(void);
void MOD_SetEqTreble(float t);    float   MOD_GetEqTreble(void);

/* Unvalidated nonlinear RF-envelope controls. Production builds force bias
 * and CE enable off; they are available only in the RF-inhibited diagnostic
 * image for comparison. They are not calibration corrections or CESSB. */
void MOD_SetSsbPhaseBias(float b);   float   MOD_GetSsbPhaseBias(void);

/* ────────────── Extended SSB controls ────────────── */
void MOD_SetSsbAnalyticBackend(ssb_analytic_backend_t v);
ssb_analytic_backend_t MOD_GetSsbAnalyticBackend(void);
void MOD_SetSsbCeEnable(uint8_t en);
uint8_t MOD_GetSsbCeEnable(void);
void MOD_SetSsbCeMix(float v);
float MOD_GetSsbCeMix(void);
void MOD_SetSsbCeFloor(float v);
float MOD_GetSsbCeFloor(void);
void MOD_SetSsbCeKnee(float v);
float MOD_GetSsbCeKnee(void);
void MOD_SetSsbLowCut(float v);
float MOD_GetSsbLowCut(void);
void MOD_SetTxBwPreset(tx_bw_preset_t v);
tx_bw_preset_t MOD_GetTxBwPreset(void);
void MOD_SetOutphaseLaw(outphase_law_t v);
outphase_law_t MOD_GetOutphaseLaw(void);

/* ────────────── Debug isolation controls ────────────── */
void MOD_SetDebugBranchMode(debug_branch_mode_t v);
debug_branch_mode_t MOD_GetDebugBranchMode(void);
void MOD_SetDebugCalBypass(uint8_t en);
uint8_t MOD_GetDebugCalBypass(void);
float MOD_GetRfEnAssertMetric(void);
void MOD_SetRfEnAssertMetric(float v);
float MOD_GetRfEnReleaseMetric(void);
void MOD_SetRfEnReleaseMetric(float v);
float MOD_GetRfEnHoldFrames(void);
void MOD_SetRfEnHoldFrames(uint8_t n);

/* ────────────── CW keyer / break-in ────────────── */
void MOD_SetCwKeyerMode(cw_keyer_mode_t mode);
cw_keyer_mode_t MOD_GetCwKeyerMode(void);
void MOD_SetCwWpm(float wpm);
float MOD_GetCwWpm(void);
void MOD_SetCwSwap(uint8_t en);
uint8_t MOD_GetCwSwap(void);
void MOD_SetCwWeight(float pct);
float MOD_GetCwWeight(void);
void MOD_SetCwAutoSpace(uint8_t en);
uint8_t MOD_GetCwAutoSpace(void);
void MOD_SetCwAutoSpaceDits(uint8_t dits);
uint8_t MOD_GetCwAutoSpaceDits(void);
void MOD_SetCwCurtisBDitPct(float pct);
float MOD_GetCwCurtisBDitPct(void);
void MOD_SetCwCurtisBDahPct(float pct);
float MOD_GetCwCurtisBDahPct(void);
void MOD_SetCwLatencyEighths(float eighths);
float MOD_GetCwLatencyEighths(void);
void MOD_SetCwBreakInMode(cw_breakin_mode_t mode);
cw_breakin_mode_t MOD_GetCwBreakInMode(void);
void MOD_SetCwSemiQskTail(float dits);
float MOD_GetCwSemiQskTail(void);
void MOD_SetCwRampMs(float ms);
float MOD_GetCwRampMs(void);
void MOD_SetCwDrive(float drive);
float MOD_GetCwDrive(void);
void MOD_SetCwThetaTrim(float trim);
float MOD_GetCwThetaTrim(void);
void MOD_SetCwThetaModel(cw_theta_model_t model);
cw_theta_model_t MOD_GetCwThetaModel(void);
void MOD_SetCwPractice(uint8_t en);
uint8_t MOD_GetCwPractice(void);
uint8_t MOD_CwMonitorActive(void);
float MOD_CwMonitorEnvelope(void);
/* CW that cannot radiate -- practice, TX muted, TX not permitted -- keys the
 * sidetone and the Morse trainer from the headphone renderer. Once per
 * headphone half: whether the keyer has anything to do; then one call per
 * 48 kHz sample, returning the shaped envelope. PendSV only. */
uint8_t MOD_CwKeyerMonitorActive(void);
float MOD_CwKeyerMonitorStep(void);
void MOD_SetCwDebounceMs(float ms);
float MOD_GetCwDebounceMs(void);
void MOD_SetCwPaddleProtectMode(cw_paddle_protect_mode_t mode);
/* The once-per-power-up CW arming: 0 the key must open first, 1 hold it
 * closed a second, 2 release it, 3 armed. Until 3 the key sends nothing. */
uint8_t MOD_CwBootArmStage(void);
/* The sidetone-only keyer (TX MUTE, practice) is sending, or in its
 * break-in hang: the receiver pauses as under real TX. */
uint8_t MOD_CwMonitorKeying(void);
void MOD_CwMonitorIdle(void);
cw_paddle_protect_mode_t MOD_GetCwPaddleProtectMode(void);
/* The USB console's virtual paddle: bit 0 DIT, bit 1 DAH, ORed with the KEY
 * jack after CW SWAP. Main loop only. */
void     MOD_CwSetVirtualPaddle(uint8_t levers);
uint8_t  MOD_CwVirtualPaddle(void);
/* Keyer elements started since boot; wraps. */
uint32_t MOD_CwElementsStarted(void);
/* The 1 ms paddle sampler: SysTick only. */
void     MOD_CwInputTick(void);
/* The trainer playing a prompt: the keyer reads both levers open. */
void     MOD_CwSetTrainGate(uint8_t on);
/* The trainer's keying speed while it owns the keyer (0: the radio's). The
 * radio's CW SPEED is untouched. */
void     MOD_CwSetTrainingWpm(float wpm);
/* The trainer owns the keyer: it drives the sidetone only, never RF. Off
 * takes effect once both levers have been open 40 ms and the keyer is at
 * rest, so nothing keyed in the trainer reaches the air. Never saved. */
void     MOD_CwSetTrainInhibit(uint8_t on);
uint8_t  MOD_CwTrainInhibited(void);
/* SPOT: the sidetone at the pitch for 4 s, never RF; a second call stops
 * it. 1 when it started. */
uint8_t  MOD_CwSpot(void);
/* Radio memories: send `len` codes (MORSE_ParseText, 0 a word space; at
 * most 48) through the keyer as if keyed -- the same timing, break-in and
 * RF path, and the same holds (arming, protection, TX MUTE, practice). A
 * lever touched ends it. 0 if the keyer cannot take it now. */
uint8_t  MOD_CwMemorySend(const uint8_t *code, uint8_t len);
void     MOD_CwMemoryStop(void);
/* Sending, or waiting MEMORY REPEAT S to send again. */
uint8_t  MOD_CwMemoryBusy(void);
void     MOD_SetCwMemoryRepeatS(float s);
float    MOD_GetCwMemoryRepeatS(void);
/* The levers as the sampler reads them now (bit 0 DIT, bit 1 DAH, after
 * SWAP), whatever the keyer is allowed to see. */
uint8_t  MOD_CwLevers(void);
void MOD_SetCwPaddleArmOpenMs(float ms);
float MOD_GetCwPaddleArmOpenMs(void);
void MOD_SetCwPaddleArmPressMs(float ms);
float MOD_GetCwPaddleArmPressMs(void);
void MOD_SetCwPaddleArmReleaseMs(float ms);
float MOD_GetCwPaddleArmReleaseMs(void);
void MOD_SetCwBothPolicy(cw_both_policy_t policy);
cw_both_policy_t MOD_GetCwBothPolicy(void);
void MOD_SetCwPaddleStuckMs(float ms);
float MOD_GetCwPaddleStuckMs(void);
void MOD_SetTxTestBothPolicy(tx_test_both_policy_t policy);
tx_test_both_policy_t MOD_GetTxTestBothPolicy(void);
void MOD_SetTxTestPaddleStuckMs(float ms);
float MOD_GetTxTestPaddleStuckMs(void);

/* ────────────── Software PTT / Mute ────────────── */
/* Software PTT. When muted, renderer outputs null carrier and drive is
 * cross-faded over ~10 ms to avoid clicks. */
void     MOD_SetMuted(uint8_t muted);
uint8_t  MOD_GetMuted(void);
void     MOD_ToggleMute(void);

/* ────────────── A/B mode toggle ────────────── */
/* Remembers the previous mode so UI can ping-pong A/B quickly. */
void         MOD_StoreLastMode(mod_config_t cfg);
mod_config_t MOD_GetLastMode(void);
void         MOD_SwapWithLastMode(void);

/* ────────────── Mic AGC (front-end) ────────────── */
void    MOD_SetMicAgcEnable(uint8_t en); uint8_t MOD_GetMicAgcEnable(void);
void    MOD_SetMicAgcTarget(float t);    float   MOD_GetMicAgcTarget(void);
void    MOD_SetMicAgcMaxGain(float g);   float   MOD_GetMicAgcMaxGain(void);

/* ────────────── Noise Gate (front-end) ────────────── */
void    MOD_SetNoiseGateEnable(uint8_t en); uint8_t MOD_GetNoiseGateEnable(void);
void    MOD_SetNoiseGateThresh(float t);    float   MOD_GetNoiseGateThresh(void);

/* ────────────── Look-ahead ALC (pre-modulator) ────────────── */
void    MOD_SetAlcEnable(uint8_t en); uint8_t MOD_GetAlcEnable(void);
void    MOD_SetAlcCeiling(float c);   float   MOD_GetAlcCeiling(void);

/* ────────────── Diagnostic CESSB-like experiment iterations (0..3) ────────────── */
void    MOD_SetCessbIterations(uint8_t n); uint8_t MOD_GetCessbIterations(void);

/* ────────────── Runtime dead-time ──────────────
 * Compatibility raw-tick access is retained, but the authoritative storage
 * model is physical time and the active HRTIM DTG hardware runs in fine MUL8
 * mode (about 735 ps/tick at the default 170 MHz input clock; runtime-clock dependent in SAFE/PREC modes).
 *
 * Legacy MOD_GetDeadTime*() getters report the currently applied hardware
 * timing (quantized). Use the explicit Requested/Applied helpers below when
 * the UI or diagnostics need to distinguish operator intent from hardware
 * reality. */
void    MOD_SetDeadTimeTicks(uint16_t ticks); uint16_t MOD_GetDeadTimeTicks(void);
void    MOD_SetDeadTimeRiseTicks(uint16_t ticks); uint16_t MOD_GetDeadTimeRiseTicks(void);
void    MOD_SetDeadTimeFallTicks(uint16_t ticks); uint16_t MOD_GetDeadTimeFallTicks(void);
void    MOD_SetDeadTimeNs(float ns); float MOD_GetDeadTimeNs(void);
void    MOD_SetDeadTimeRiseNs(float ns); float MOD_GetDeadTimeRiseNs(void);
void    MOD_SetDeadTimeFallNs(float ns); float MOD_GetDeadTimeFallNs(void);
float   MOD_GetDeadTimeRequestedNs(void);
float   MOD_GetDeadTimeRiseRequestedNs(void);
float   MOD_GetDeadTimeFallRequestedNs(void);
float   MOD_GetDeadTimeAppliedNs(void);
float   MOD_GetDeadTimeRiseAppliedNs(void);
float   MOD_GetDeadTimeFallAppliedNs(void);
uint16_t MOD_GetDeadTimeRiseAppliedTicks(void);
uint16_t MOD_GetDeadTimeFallAppliedTicks(void);
float   MOD_GetDeadTimeStepNs(void);
float   MOD_GetDeadTimeMaxNs(void);
mod_carrier_apply_path_t MOD_GetLastCarrierApplyPath(void);

/* ────────────── Runtime branch A↔C offset (HRTIM ticks) ──────────────
 * Adds a static relative phase skew between branch A and branch C using
 * exact HRTIM compare ticks. Positive = branch C lags branch A; negative =
 * branch C leads. The offset is split approximately symmetrically between
 * the two branches so the common phase is preserved as much as possible.
 *
 * This is intended for precise inter-branch delay trimming without touching
 * the audio/DSP pipeline. One HRTIM tick is one output phase tick at the active carrier timebase;
 * its physical time depends on the current HRTIM input clock and carrier
 * period. At the default 170 MHz MUL32 clock it is about 184 ps.
 */
void    MOD_SetBranchAcOffsetTicks(int16_t ticks); int16_t MOD_GetBranchAcOffsetTicks(void);
void    MOD_SetPaGeometry(pa_geometry_t geometry);
pa_geometry_t MOD_GetPaGeometry(void);

/* ────────────── Runtime duty-cycle offset (HRTIM ticks) ──────────────
 * Compensates gate-driver asymmetric rise/fall (e.g. UCC27424).
 * Positive = longer TA1/TC1 high time, negative = shorter.
 * Takes effect immediately — triggers a HRTIM reconfig on change. */
void    MOD_SetDutyOffset(int16_t ticks); int16_t MOD_GetDutyOffset(void);

/* ────────────── CORDIC hardware acceleration ────────────── */
void    MOD_SetCordicEnable(uint8_t en);   /* 1 = use CORDIC, 0 = libm */

/* ────────────── Persistence: the SD card's settings store ────────────── */
/* Settings and the active band/sideband/analytic calibration context live in
 * the store in the card's system area (mod_sd_store.c). Load runs at boot. */
uint8_t MOD_SaveSettings(void);
uint8_t MOD_LoadSettings(void);
/* No usable store on the card (none, blank, another radio's, damaged, bad
 * layout), or its settings did not read, and the operator has not chosen to
 * run without one: boot stops. */
uint8_t MOD_StorageCardRequired(void);
/* The store's settings did not read before any were loaded or saved (the
 * running ones are defaults): nothing saves them; the boot hold reads again. */
uint8_t MOD_StorageSettingsUnread(void);
uint8_t MOD_StorageRereadSettings(void);
/* Main loop: write changed settings once they have settled; remount a lost
 * card; one step of the store's background check. */
void MOD_StorageAutosaveService(uint32_t now);
/* Event log on the SD card (LOGS/EVENTS.LOG): queue from any context, the
 * main-loop service writes it. */
enum { MOD_EVENT_FAULT = 1, MOD_EVENT_PIPELINE, MOD_EVENT_STORAGE, MOD_EVENT_SD };
void MOD_EventLog(uint8_t kind, uint8_t code, uint16_t aux, uint32_t v1, uint32_t v2);
void MOD_EventLogCounts(uint32_t *queued, uint32_t *lost);
void MOD_StorageLogService(uint32_t now);
/* Re-read a band's calibration from the store once blocking I/O is allowed;
 * until it reads, TX is locked (CAL NOT LOADED). */
void MOD_CalReloadService(uint32_t now);
uint8_t MOD_CalNotLoaded(void);
/* The card initialised again since the settings store read it (or not ready
 * after an error): the store looks at it again, FatFs remounted when it is
 * another card. 1 when this radio's store is usable. */
uint8_t MOD_StorageReconnect(void);
void    MOD_FactoryReset(void);
uint8_t MOD_GetStorageSdMissing(void);
const char *MOD_GetStorageStatusLabel(void);
/* The home screen's storage warning, NULL when there is none. */
const char *MOD_StorageWarning(void);
/* The operator's decisions about a card (NO SD CARD hold, SD menu, console). */
uint8_t MOD_StorageRunWithoutCard(void);
uint8_t MOD_StorageRunsWithoutCard(void);
uint8_t MOD_StorageCreate(void);
uint8_t MOD_StorageAdopt(void);
/* v8.08's flash dump (SYSTEM/NVM808.BIN) and card files into the store;
 * `require_nvm` refuses without the dump. Available: 2 dump, 1 files, 0. */
uint8_t MOD_StorageImport(uint8_t require_nvm);
uint8_t MOD_StorageImportAvailable(void);
/* An import (1) or a restore (2) was cut short (the store's IMPORTING /
 * RESTORING mark): the boot holds. MOD_StorageResume runs it again where its
 * source is still on the card, otherwise keeps the store as it is
 * (MOD_StorageKeep); MOD_StorageResumeAction says which. */
uint8_t MOD_StorageIncomplete(void);
uint8_t MOD_StorageKeep(void);
const char *MOD_StorageResumeAction(void);
uint8_t MOD_StorageResume(void);
void MOD_StorageImportSummary(uint8_t *nvm, uint8_t *files, uint8_t *settings_from,
                              uint16_t *cal_flash, uint16_t *cal_card, uint16_t *failed);
/* SYSTEM/STOREBAK.BIN from the store; a backup file (NULL: SYSTEM/RESTORE.BIN)
 * back into it. */
uint8_t MOD_StorageBackup(void);
uint8_t MOD_StorageRestore(const char *path, uint16_t *restored);
/* The Morse trainer's learner record (store slot SD_STORE_SLOT_TRAINER):
 * read into the trainer on entry, written at its checkpoints -- a lesson
 * change, a pause, a summary, the exit -- never while keying. A record
 * this image cannot read (a later firmware's) is never overwritten: the
 * trainer then runs on a fresh record it does not save. */
uint8_t MOD_TrainerRecordLoad(void);
/* With SAVE PROGRESS off (mtrain_prefs_t nosave) nothing is written and
 * the card's record is read again before the next session. */
uint8_t MOD_TrainerRecordSave(void);
/* DELETE PROGRESS: a fresh record written over the card's, whatever SAVE
 * PROGRESS says, and over one a later firmware wrote. 0: no card now. */
uint8_t MOD_TrainerRecordDelete(void);
/* The record will not be saved (unreadable, or no store): the page says so. */
uint8_t MOD_TrainerRecordUnsaved(void);
/* A backup entry for the trainer's slot is a record this image takes. */
uint8_t MOD_TrainerRecordAccept(const void *data, uint32_t len);
/* The store changed in a way worth a backup (calibration, an import): the
 * main loop writes STOREBAK.BIN in the background a minute later, at most
 * every ten minutes (MOD_StorageBackupService, from the autosave). */
void MOD_StorageBackupRequest(void);
void MOD_StorageBackupService(uint32_t now, uint8_t io_allowed);
void MOD_StorageBackupStatus(uint8_t *pending, uint8_t *running, uint16_t *slot, uint32_t *ok, uint32_t *failed);

/* ────────────── Band preset table ────────────── */
typedef struct {
    const char *name;
    float       mhz;
} mod_band_t;
uint8_t           MOD_GetBandCount(void);
const mod_band_t *MOD_GetBand(uint8_t idx);
void              MOD_SelectBand(uint8_t idx);
uint8_t           MOD_AudioRealtimeCritical(uint32_t now);
/* Whether the TX renderer committed a radiating frame within `ms` of `now`:
 * the transmitter is starting (the TX request follows once the frame reaches
 * the DMA). IRQ-safe. */
uint8_t           MOD_TxRfRenderedWithin(uint32_t now, uint32_t ms);
/* Milliseconds since that last radiating frame, 255 at most. IRQ-safe. */
uint32_t          MOD_TxRfRenderedAgeMs(uint32_t now);

#ifdef __cplusplus
}
#endif

#endif /* MODULATOR_H */
