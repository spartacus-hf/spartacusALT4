#include "mod_priv.h"
#include "math_pos.h"
#include "mod_alc_logic.h"
#include "mod_dsp_tables_priv.h"
#include "mod_analytic_logic.h"
#include "mod_calibration_logic.h"
#include "mod_cessb_policy.h"
#include "mod_mode_math.h"
#include "mod_ssb_envelope_logic.h"
#include "mod_ssb_gate_logic.h"
#include "mod_digi_tone_logic.h"

/* The SSB analytic backends beside WEAVER and H12P -- the 48 kHz Hilbert,
 * the matched pairs at 24 and 12 ksps and the strict QMX12 pipeline -- are
 * offered only by the deep-diagnostic image
 * (MOD_SsbAnalyticBackendBuildAvailable; storage sanitises them to WEAVER
 * elsewhere), so only that image has them, and elsewhere the extended SSB
 * path is H12P alone. */
#define SSB_EXT_ALL_BACKENDS MOD_DEEP_DIAG_ENABLE
/* The SSB comparison renderers -- ENH SSB, RAW DIR, FLT STD and FLT NAR --
 * are offered only by the deep-diagnostic image's mode list and
 * MOD_ModeAvailableInBuild, and only that image has them. */
#define SSB_COMPARE_RENDERERS MOD_DEEP_DIAG_ENABLE

#define INV_PI               (1.0f / MOD_PI_F)
#define TWO_PI               (2.0f * MOD_PI_F)

/* Production baseline */
#define PHASE_HOLD_FLOOR           0.020f
#define RAW_LEGACY_PHASE_HOLD_FLOOR 0.012f
#define SOFT_RANGE                 (1.0f - SOFT_KNEE)

/* Enhanced SSB */
#define SAMPLE_DRIVE_DIRECT  1.0f
#define ENH_SOFT_KNEE        0.85f
#define ENH_SOFT_RANGE       (1.0f - ENH_SOFT_KNEE)
#define WEAVER_FREQ_HZ       1550.0f
#define WEAVER_FREQ_ENH_HZ   1550.0f
#define WEAVER_GAIN          2.0f

/* Test tones */
#define TEST_TONE_1_HZ       1000.0f
#define TEST_TONE_2A_HZ       700.0f
#define TEST_TONE_2B_HZ      1900.0f
#define TEST_TONE_1_AMP        0.35f
#define TEST_TONE_2_AMP        0.15f
#define SWEEP_START_HZ        300.0f
#define SWEEP_END_HZ         3000.0f
#define SWEEP_PERIOD_FRAMES  2400U
#define SWEEP_AMP              0.30f

/* FM */
#define FM_PREEMPH_TAU      (50.0e-6f)
#define FM_AUDIO_LIMIT        0.92f

/* AM */
#define AM_AUDIO_GAIN         2.00f
#define AM_ENV_MIN            0.05f

#define USE_CORDIC              1
#define USE_FAST_ATAN2          1

/* Look-ahead ALC */
#define ALC_ATTACK_COEFF        0.20f
#define ALC_RELEASE_COEFF       0.0025f

/* Mic AGC */
#define MIC_AGC_ATTACK          0.020f
#define MIC_AGC_RELEASE         0.0008f

/* Noise gate */
#define NG_HYST_FACTOR          1.4f
#define NG_HOLD_SAMPLES         1200U
#define NG_FADE_SAMPLES         96U

/* Mute / PTT fade */
#define MUTE_FADE_SAMPLES       480U
#define MUTE_STEP               (1.0f / (float)MUTE_FADE_SAMPLES)

static inline float biquad_process(const BiquadCoeffs *c, BiquadState *s, float x);
static float biquad_chain(const BiquadCoeffs *c, BiquadState *s, uint32_t n, float x);
static float telemetry_gain_to_db(float gain);


/* DSP state - production Weaver */
static BiquadState weaver_i_state[WEAVER_LPF_SECTIONS];
static BiquadState weaver_q_state[WEAVER_LPF_SECTIONS];
#if SSB_COMPARE_RENDERERS
static BiquadState speech_hpf_state[SPEECH_HPF_SECTIONS];
static BiquadState speech_lpf_state[SPEECH_LPF_SECTIONS];
static BiquadState weaver_filt_i_state[WEAVER_LPF_SECTIONS];
static BiquadState weaver_filt_q_state[WEAVER_LPF_SECTIONS];

/* DSP state - enhanced */
static BiquadState enh_hpf_state[ENH_HPF_SECTIONS];
static BiquadState enh_lpf_state[ENH_LPF_SECTIONS];
static BiquadState weaver_enh_i_state[WEAVER_ENH_LPF_SECTIONS];
static BiquadState weaver_enh_q_state[WEAVER_ENH_LPF_SECTIONS];
#endif
static BiquadState am_hpf_state[ENH_HPF_SECTIONS];
static BiquadState am_lpf_state[ENH_LPF_SECTIONS];

/* DSP state - extended SSB path */
static BiquadState txbw_hpf_state[TX_BW_HPF_STATE_SECTIONS];
static BiquadState txbw_lpf_state[TX_BW_LPF_STATE_SECTIONS];
#if SSB_EXT_ALL_BACKENDS
static BiquadState qmx12_aa_lpf_state[QMX12_AA_LPF_SECTIONS];
static BiquadState weaver_ext_i_state[WEAVER_ENH_LPF_SECTIONS];
static BiquadState weaver_ext_q_state[WEAVER_ENH_LPF_SECTIONS];
static BiquadState hilb24_hpf_state[TX_BW_HPF_STATE_SECTIONS];
static BiquadState hilb24_lpf_state[TX_BW_LPF_STATE_SECTIONS];
static BiquadState hilb12a_hpf_state[TX_BW_HPF_STATE_SECTIONS];
static BiquadState hilb12a_lpf_state[TX_BW_LPF_STATE_SECTIONS];
static BiquadState hilb12b_hpf_state[TX_BW_HPF_STATE_SECTIONS];
static BiquadState hilb12b_lpf_state[TX_BW_LPF_STATE_SECTIONS];

typedef struct {
    float hist[HILB_NUM_TAPS];
    uint32_t wr_idx;
} HilbertLegacyState;
#endif

/* Analytic backends are mutually exclusive and backend changes reset the
 * pipeline.  Sharing their state saves more RAM than the production H12P
 * core adds. DIGI TONE runs only in place of PROD (Weaver), which keeps
 * nothing here. */
typedef union {
#if SSB_EXT_ALL_BACKENDS
    HilbertLegacyState legacy;
    HilbertExperimentState experimental;
#endif
#if MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE
    mod_analytic12_state_t production12;
#endif
    mod_digi_tone_state_t tone;
} HilbertWorkspace;

static HilbertWorkspace hilbert_workspace;
#if MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE
static mod_analytic12_coeffs_t analytic12_coeffs;   /* mod_tx_pipeline_init */
#endif
#if SSB_EXT_ALL_BACKENDS
static uint8_t qmx12_interp_valid = 0U;
static float qmx12_prev_phi = 0.0f;
static float qmx12_prev_env = 0.0f;
static float qmx12_prev_theta = 0.0f;
#endif

/* Production NCO (1550 Hz) */
static float osc_re = 1.0f, osc_im = 0.0f;
static float osc_step_re = 1.0f, osc_step_im = 0.0f;

#if SSB_COMPARE_RENDERERS
/* Enhanced NCO - same carrier as PROD (1550 Hz), wider Weaver LPF */
static float osc_enh_re = 1.0f, osc_enh_im = 0.0f;
static float osc_enh_step_re = 1.0f, osc_enh_step_im = 0.0f;
#endif

#if SSB_EXT_ALL_BACKENDS
/* Extended SSB path oscillator (its Weaver branch) */
static float osc_ext_re = 1.0f, osc_ext_im = 0.0f;
static float osc_ext_step_re = 1.0f, osc_ext_step_im = 0.0f;
#endif

/* Test signal NCOs: FM's TEST TONE uses the 1 kHz one; the rest belong to
 * the deep-diagnostic image's internal test signals. */
static ComplexOsc test_osc_1k;
#if MOD_DEEP_DIAG_ENABLE
static ComplexOsc test_osc_700;
static ComplexOsc test_osc_1900;
static float    test_phase_sweep = 0.0f;
static uint32_t sweep_frame_counter = 0U;
#endif

/* FM state */
static float fm_preemph_state = 0.0f;
static BiquadState dc_block_fm_state[DC_BLOCK_SECTIONS];
static BiquadState fm_lpf_state[FM_LPF_SECTIONS];

/* Audio metering */
static volatile uint16_t meter_peak_raw = 0U;
static volatile uint16_t meter_peak_hold = 0U;
static volatile float    meter_rms_sq_acc = 0.0f;
static volatile uint32_t meter_sample_count = 0U;

/* TX carrier-test paddle gate */
static uint8_t tx_test_paddle_dit = 0U;
static uint8_t tx_test_paddle_dah = 0U;
static uint8_t tx_test_gate_active = 0U;
typedef struct {
    uint8_t stable;
    uint8_t last_raw;
    uint8_t count;
} TxTestDebounceState;
static TxTestDebounceState tx_test_dit_db = {0U, 0U, 0U};
static TxTestDebounceState tx_test_dah_db = {0U, 0U, 0U};
#define TX_TEST_DEBOUNCE_FRAMES 2U

/* Unvalidated clip/filter experiment: diagnostic image only. */
#if MOD_CESSB_EXPERIMENT_ENABLE
static BiquadState cessb_i_state[WEAVER_ENH_LPF_SECTIONS];
static BiquadState cessb_q_state[WEAVER_ENH_LPF_SECTIONS];
#endif

/* Experimental: Compression */
static float   comp_gain       = 1.0f;
#define COMP_ATTACK_COEFF   0.0042f
#define COMP_RELEASE_COEFF  0.00015f
#define COMP_LUT_SIZE       257U
#define COMP_LUT_MAX_ABS    4.0f
static HOT_BUFFER_BSS float comp_gain_lut[2][COMP_LUT_SIZE];
static float   comp_lut_inv_step = 0.0f;
static float   comp_lut_cached_target = -1.0f;
static float   comp_lut_cached_ratio = -1.0f;
static volatile uint8_t comp_lut_active = 0U;
static uint8_t comp_lut_valid = 0U;

/* Experimental: TX EQ */
static BiquadCoeffs eq_bass_coeffs   = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
static BiquadCoeffs eq_treble_coeffs = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
static BiquadState  eq_bass_state;
static BiquadState  eq_treble_state;

/* DC-block states (one per renderer) */
static BiquadState dc_block_prod_state;
static BiquadState dc_block_ext_state;
static BiquadState dc_block_am_state;

static mod_ssb_gate_state_t gate_prod, gate_ext;
static mod_ssb_gate_fade_state_t prod_phase_state = {0};
#if SSB_COMPARE_RENDERERS
static BiquadState dc_block_raw_state;
static BiquadState dc_block_enh_state;
static BiquadState dc_block_filt_state;
static mod_ssb_gate_state_t gate_raw, gate_enh, gate_filt;
static mod_ssb_gate_fade_state_t raw_phase_state  = {0};
static mod_ssb_gate_fade_state_t enh_phase_state  = {0};
static mod_ssb_gate_fade_state_t filt_phase_state = {0};
#endif
#if MOD_DEEP_DIAG_ENABLE
static float raw_legacy_last_phi = 0.0f;
#endif
static mod_ssb_gate_fade_state_t ext_phase_state  = {0};

static float   mic_agc_env = 0.0f;
static float   mic_agc_gain = 1.0f;
typedef struct {
    float    env;
    float    gain;
    uint16_t hang;
    uint8_t  open;
} noise_gate_state_t;
static noise_gate_state_t ng = { 0.0f, 0.0f, 0U, 0U };
static mod_alc_state_t alc_state;
static volatile uint8_t cessb_clipping_flag = 0U;

/* Live telemetry */
static volatile float  live_comp_gain_db    = 0.0f;
static volatile float  live_alc_gain_db     = 0.0f;
static volatile float  live_mic_agc_gain_db = 0.0f;
static volatile uint8_t live_gate_active    = 0U;

static inline uint8_t mode_uses_sideband(mod_type_t type)
{
    return (type == MOD_TYPE_SSB) ? 1U : 0U;
}


static inline float soft_clip_audio(float x)
{
    if (x >  1.0f) return  1.0f;
    if (x < -1.0f) return -1.0f;
    return x;
}

static inline float soft_limit(float r, float knee, float range)
{
    if (r <= knee) return r;
    float x = (r - knee) / range;
    return knee + range * x / (1.0f + x);
}

static inline float soft_limit_enh(float r)
{
    return soft_limit(r, ENH_SOFT_KNEE, ENH_SOFT_RANGE);
}

/* Polynomial atan2 — accurate to ~0.005 rad, ~5x faster than libm atan2f.
 * Fallback when CORDIC is disabled. */
static inline float fast_atan2(float y, float x)
{
    if (x == 0.0f)
    {
        if (y > 0.0f) return  0.5f * MOD_PI_F;
        if (y < 0.0f) return -0.5f * MOD_PI_F;
        return 0.0f;
    }
    float ax = fabsf(x), ay = fabsf(y);
    float a, r;
    if (ax >= ay)
    {
        a = ay / ax;
        r = a * (0.25f * MOD_PI_F) - a * (a - 1.0f) * (0.2447f + 0.0663f * a);
    }
    else
    {
        a = ax / ay;
        r = (0.5f * MOD_PI_F) - (a * (0.25f * MOD_PI_F) - a * (a - 1.0f) * (0.2447f + 0.0663f * a));
    }
    if (x < 0.0f) r = MOD_PI_F - r;
    if (y < 0.0f) r = -r;
    return r;
}

/* ═══════════════ CORDIC hardware accelerator ═══════════════
 *
 * STM32G4 CORDIC, circular mode, 6 cycles (24 iterations) = ~20-bit precision.
 * We configure once at init; each call is a write-read pair that takes
 * roughly 30-40 cycles end-to-end at 170 MHz. Per ST's own measurements
 * the firmware uses it to avoid libm atan2f/sqrtf cost on exact paths.
 *
 * Inputs and outputs use q1.31 format. Angles are encoded as angle/π so
 * the full range (-1.0, 1.0) maps to (-π, π) — which is exactly our
 * internal "half-cycle" phase convention.
 *
 * Note: CORDIC "phase" function takes x,y inputs and returns BOTH the
 * angle (in RDATA) and the modulus (in second read) — we get atan2 and
 * sqrt(x²+y²) in a single transaction.
 */

uint8_t cordic_ready = 0U;
static volatile uint32_t cordic_timeout_count = 0U;

static void cordic_hw_init(void)
{
#if USE_CORDIC
    __HAL_RCC_CORDIC_CLK_ENABLE();
    /* Phase (x,y -> atan2, modulus), 24 iterations, two arguments, two results. */
    CORDIC->CSR = (2U << CORDIC_CSR_FUNC_Pos)
                | (6U << CORDIC_CSR_PRECISION_Pos)
                | CORDIC_CSR_NARGS
                | CORDIC_CSR_NRES;
    cordic_ready = 1U;
#endif
}

/* Convert a float in range [-1,1] to q1.31 */
static inline int32_t f_to_q31(float f)
{
    if (f >=  0.99999999f) return  0x7FFFFFFF;
    if (f <= -0.99999999f) return -0x7FFFFFFF;
    return (int32_t)(f * 2147483648.0f);
}

/* Convert q1.31 back to float in [-1,1] */
static inline float q31_to_f(int32_t q)
{
    return (float)q * (1.0f / 2147483648.0f);
}

/* CORDIC phase + modulus: x,y in any range — we scale so both fit in [-1,1]
 * with a safe margin, then rescale modulus on return. */
#if USE_CORDIC
#define CORDIC_SAFE_MAG       0.97f
#define CORDIC_SAFE_AXIS      (CORDIC_SAFE_MAG * 0.70710678f)
#define CORDIC_RRDY_TIMEOUT   128U
HOT_INLINE void cordic_phase_mag(float x, float y, float *phi_norm, float *mag)
{
    /* ST notes that rectangular-to-polar results become false when the
     * modulus exceeds 1.0 in q1.31. Avoid a hot-path sqrtf() for the common
     * scaling decision: if both axes are within the inscribed square of the
     * safe-radius circle, the vector is guaranteed safe already. */
    float scale = 1.0f;
    float ax = fabsf(x);
    float ay = fabsf(y);
    if ((ax > CORDIC_SAFE_AXIS) || (ay > CORDIC_SAFE_AXIS)) {
        float mag_bound = (ax > ay) ? ax : ay;
        if (mag_bound > CORDIC_SAFE_MAG) {
            scale = mag_bound / CORDIC_SAFE_MAG;
            x /= scale;
            y /= scale;
        } else {
            float mag2 = x * x + y * y;
            if (mag2 > (CORDIC_SAFE_MAG * CORDIC_SAFE_MAG)) {
                float m = sqrtf(mag2);
                if (m > CORDIC_SAFE_MAG) {
                    scale = m / CORDIC_SAFE_MAG;
                    x /= scale;
                    y /= scale;
                }
            }
        }
    }

    /* Phase function: RES1 = phase angle/π, RES2 = modulus. CSR is fixed at init. */
    CORDIC->WDATA = (uint32_t)f_to_q31(x);
    CORDIC->WDATA = (uint32_t)f_to_q31(y);
    uint32_t wait = CORDIC_RRDY_TIMEOUT;
    while (((CORDIC->CSR & CORDIC_CSR_RRDY) == 0U) && (wait > 0U)) {
        wait--;
    }
    if (wait == 0U) {
        cordic_timeout_count++;
        *mag = sqrtf(x * x + y * y) * scale;
        *phi_norm = atan2f(y, x) * INV_PI;
        return;
    }
    int32_t rphase = (int32_t)CORDIC->RDATA;
    int32_t rmag   = (int32_t)CORDIC->RDATA;

    *phi_norm = q31_to_f(rphase);
    *mag      = q31_to_f(rmag) * scale;
}
#endif

uint8_t MOD_RunCordicSelfTest(void)
{
#if USE_CORDIC
    if (cordic_ready == 0U) return 0U;
    float phi = 0.0f;
    float mag = 0.0f;
    uint32_t before = cordic_timeout_count;
    cordic_phase_mag(0.5f, 0.5f, &phi, &mag);
    if (cordic_timeout_count != before) return 0U;
    return ((phi > 0.23f) && (phi < 0.27f) &&
            (mag > 0.68f) && (mag < 0.74f)) ? 1U : 0U;
#else
    return 1U;
#endif
}

/* Unified magnitude/phase extraction — uses CORDIC if enabled, otherwise
 * a precise software fallback. LEGACY deliberately keeps the exact libm
 * reference so audio changes are not hidden behind approximation changes. */
HOT_INLINE void extract_mag_phase(float I, float Q, float *mag, float *phi_norm)
{
#if USE_CORDIC
    if (cfg_cordic_enable && cordic_ready) {
        cordic_phase_mag(I, Q, phi_norm, mag);
        return;
    }
#endif
    *mag = sqrtf(I * I + Q * Q);
    *phi_norm = atan2f(Q, I) * INV_PI;
}

/* Runtime-selectable DSP math policy used by all live SSB/test renderers.
 *
 * LEGACY: exact libm path, no hardware acceleration. This is the easiest
 *         rollback reference when checking whether newer math changed audio.
 * SAFE  : accelerated-exact path. Uses CORDIC when enabled, otherwise the
 *         same precise libm fallback as LEGACY.
 * FAST  : keeps exact magnitude but allows a faster phase approximation.
 *         This is intentionally opt-in because it can trade a little phase
 *         accuracy for CPU headroom.
 */
static __attribute__((noinline, optimize("O3"), hot)) void extract_mag_phase_runtime(
    float I,
    float Q,
    float *mag,
    float *phi_norm)
{
    mod_control_finite_pair_or_zero(&I, &Q);
    if (cfg_dsp_math_mode == DSP_MATH_LEGACY) {
        *mag = sqrtf(I * I + Q * Q);
        *phi_norm = atan2f(Q, I) * INV_PI;
        return;
    }

    if (cfg_dsp_math_mode == DSP_MATH_FAST) {
        *mag = sqrtf(I * I + Q * Q);
#if USE_FAST_ATAN2
        *phi_norm = fast_atan2(Q, I) * INV_PI;
#else
        *phi_norm = atan2f(Q, I) * INV_PI;
#endif
        return;
    }

    extract_mag_phase(I, Q, mag, phi_norm);
}

/* Per-frame latches for the per-sample helpers, set by render_dispatch.
 * The values behind them cannot change inside a frame -- the main loop does
 * not run during a render, and a new audio source from the USB interrupt
 * takes effect one frame later -- but their volatile reads, asked for four
 * to eight times a sample, were a sixth of the SSB renderer's time, and USB
 * audio SSB fell behind real time. */
static uint8_t frame_usb_clean;
static uint8_t frame_voice_gain_bypass;
static uint8_t frame_cal_active;
static float frame_ssb_theta_cap;

static inline uint8_t usb_clean_processing_compute(void)
{
    if (MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source)) return local_audio_clean;
    return (uint8_t)((!active_internal_source) &&
                     (cfg_usb_clean_mode == (uint8_t)USB_AUDIO_POLICY_DIGI_CLEAN));
}

static inline uint8_t usb_clean_processing_active(void)
{
    return frame_usb_clean;
}

/* Whether SSB would run the speech tools with the settings as they are now
 * (not the frame's latch): the clock planner's transmit load class. */
uint8_t mod_tx_voice_processing(void)
{
    return (uint8_t)!usb_clean_processing_compute();
}

static inline float usb_voice_gain_process(float x)
{
    if (frame_voice_gain_bypass != 0U) return x;
    x *= cfg_usb_input_gain;
    if (x > 1.0f) {
        return 1.0f;
    }
    if (x < -1.0f) {
        return -1.0f;
    }
    return x;
}

/* ═══════════════ Mic AGC ═══════════════
 *
 * Fast-attack, slow-release envelope follower that targets a nominal
 * RMS level. Important: max_gain is capped so the AGC can only amplify
 * quiet speakers — it cannot quiet a loud one (that's the compressor's
 * job). This is exactly QMX's "mic AGC" policy.
 */
static inline uint8_t mic_agc_bypassed(void)
{
    return (uint8_t)(usb_clean_processing_active() || !cfg_mic_agc_enable);
}

/* The gain elements are split into the per-sample state update (_step, on
 * explicit state) and the telemetry it leaves (_telemetry): the block
 * renderer runs the step over a frame on state held in locals and publishes
 * the telemetry once, since nothing reads it before the frame ends -- per
 * sample it cost a log10f while the element was working, with GAIN
 * TELEMETRY LIVE. */
static inline float mic_agc_step(float x, float *env, float *gain,
                                 float target, float max_gain)
{
    float a = fabsf(x);
    if (a > *env) *env += MIC_AGC_ATTACK  * (a - *env);
    else          *env += MIC_AGC_RELEASE * (a - *env);

    /* Only adjust if envelope is above noise floor — prevents AGC from
     * pumping during silence */
    if (*env > 0.005f) {
        float want = target / *env;
        if (want > max_gain) want = max_gain;
        if (want < 1.0f)     want = 1.0f;
        /* Smooth toward target */
        if (want > *gain) *gain += MIC_AGC_ATTACK  * (want - *gain);
        else              *gain += MIC_AGC_RELEASE * (want - *gain);
    }
    return x * *gain;
}

static inline float mic_agc_core(float x)
{
    return mic_agc_step(x, &mic_agc_env, &mic_agc_gain,
                        cfg_mic_agc_target, cfg_mic_agc_max_gain);
}

static inline void mic_agc_telemetry(void)
{
    if (cfg_gain_telemetry_mode == GAIN_TELEMETRY_LIVE) {
        live_mic_agc_gain_db = telemetry_gain_to_db(mic_agc_gain);
    } else {
        live_mic_agc_gain_db = 0.0f;
    }
}

static inline float mic_agc_process(float x)
{
    if (mic_agc_bypassed()) {
        mic_agc_gain = 1.0f;
        live_mic_agc_gain_db = 0.0f;
        return x;
    }
    x = mic_agc_core(x);
    mic_agc_telemetry();
    return x;
}

/* ═══════════════ Noise Gate ═══════════════
 *
 * Hysteretic gate on the *audio* (not the envelope), with hold time.
 * Purpose: kill room/fan noise during speech pauses which the compressor
 * would otherwise amplify.
 */
static inline uint8_t noise_gate_bypassed(void)
{
    return (uint8_t)(usb_clean_processing_active() || !cfg_ng_enable);
}

static inline float noise_gate_step(noise_gate_state_t *g, float x, float thresh)
{
    float a = fabsf(x);
    g->env += 0.1f * (a - g->env);
    float open_th  = thresh * NG_HYST_FACTOR;
    float close_th = thresh;
    if (!g->open && g->env > open_th) {
        g->open = 1U;
        g->hang = NG_HOLD_SAMPLES;
    } else if (g->open && g->env < close_th) {
        if (g->hang == 0U) g->open = 0U;
        else               g->hang--;
    } else if (g->open) {
        g->hang = NG_HOLD_SAMPLES;
    }
    float target = g->open ? 1.0f : 0.0f;
    float step = 1.0f / (float)NG_FADE_SAMPLES;
    if (target > g->gain) { g->gain += step; if (g->gain > 1.0f) g->gain = 1.0f; }
    else                  { g->gain -= step; if (g->gain < 0.0f) g->gain = 0.0f; }
    return x * g->gain;
}

static inline void noise_gate_telemetry(void)
{
    live_gate_active = (ng.gain < 0.99f) ? 1U : 0U;
}

static inline float noise_gate_process(float x)
{
    if (noise_gate_bypassed()) {
        ng.gain = 1.0f;
        return x;
    }
    x = noise_gate_step(&ng, x, cfg_ng_thresh);
    noise_gate_telemetry();
    return x;
}

/* ═══════════════ Look-ahead ALC ═══════════════
 *
 * Delays the audio by MOD_ALC_LOOKAHEAD_SAMPLES, tracks the peak of the
 * window ahead of the current read point, and applies attenuation such
 * that the delayed output never exceeds the ceiling. Because we see the
 * peak in advance, attenuation begins before the transient and is
 * perceptually inaudible.
 *
 * This is an independent peak-containment stage. It makes no CESSB or
 * average-power claim; those require a separately validated algorithm.
 */
static inline uint8_t alc_bypassed(void)
{
    return (uint8_t)(usb_clean_processing_active() || !cfg_alc_enable);
}

/* Out of line: one copy of the look-ahead for every renderer, over a frame
 * for the production renderer and a sample at a time for the others. */
static __attribute__((noinline, optimize("O3"), hot)) void alc_block(float *x, uint32_t n)
{
    mod_alc_process_block(&alc_state, x, n, cfg_alc_ceiling,
                          ALC_ATTACK_COEFF, ALC_RELEASE_COEFF);
}

static inline void alc_telemetry(void)
{
    if (cfg_gain_telemetry_mode == GAIN_TELEMETRY_LIVE) {
        live_alc_gain_db = telemetry_gain_to_db(alc_state.gain);
    } else {
        live_alc_gain_db = 0.0f;
    }
}

static inline float alc_process(float x)
{
    if (alc_bypassed()) {
        alc_state.gain = 1.0f;
        live_alc_gain_db = 0.0f;
        return x;
    }
    alc_block(&x, 1U);
    alc_telemetry();
    return x;
}


static __attribute__((noinline)) float telemetry_gain_to_db(float gain)
{
    if (gain <= 0.0f) return -60.0f;
    if (gain > 1.001f) return 20.0f * log10f(gain);
    if (gain < 0.999f) return 20.0f * log10f(gain);
    return 0.0f;
}

static void comp_lut_rebuild(void)
{
    float target = clampf(cfg_comp_target, 0.1f, 0.9f);
    float ratio = clampf(cfg_comp_ratio, 1.0f, 20.0f);
    float exponent = -(1.0f - 1.0f / ratio);
    float step = COMP_LUT_MAX_ABS / (float)(COMP_LUT_SIZE - 1U);
    uint8_t next_lut = (uint8_t)((comp_lut_active ^ 1U) & 1U);
    float *lut = comp_gain_lut[next_lut];

    if (step <= 0.0f) step = 1.0f;
    for (uint32_t i = 0U; i < COMP_LUT_SIZE; i++) {
        float abs_x = step * (float)i;
        float gain = 1.0f;
        if (abs_x > target) {
            float over = abs_x / (target + 1.0e-9f);
            gain = powf_pos(over, exponent);
        }
        lut[i] = gain;
    }

    uint32_t pm = enter_critical();
    comp_lut_valid = 0U;
    __DMB();
    comp_lut_inv_step = 1.0f / step;
    comp_lut_cached_target = target;
    comp_lut_cached_ratio = ratio;
    comp_lut_active = next_lut;
    __DMB();
    comp_lut_valid = 1U;
    exit_critical(pm);
}

__attribute__((cold)) void mod_tx_pipeline_comp_lut_sync(void)
{
    if ((cfg_comp_math_mode == COMP_MATH_LUT) && (cfg_comp_enable != 0U)) {
        comp_lut_rebuild();
    } else {
        uint32_t pm = enter_critical();
        comp_lut_valid = 0U;
        comp_lut_cached_target = -1.0f;
        comp_lut_cached_ratio = -1.0f;
        comp_lut_inv_step = 0.0f;
        exit_critical(pm);
    }
}

static inline float comp_desired_gain_exact(float abs_x)
{
    if (abs_x <= cfg_comp_target) return 1.0f;
    {
        float over = abs_x / (cfg_comp_target + 1.0e-9f);
        return powf_pos(over, -(1.0f - 1.0f / fmaxf(cfg_comp_ratio, 1.0f)));
    }
}

static inline float comp_desired_gain_lut(float abs_x)
{
    uint8_t valid = comp_lut_valid;
    float cached_target = comp_lut_cached_target;
    float cached_ratio = comp_lut_cached_ratio;
    uint8_t lut_active = comp_lut_active;
    float inv_step = comp_lut_inv_step;

    if ((valid == 0U) ||
        (fabsf(cached_target - cfg_comp_target) > 1.0e-6f) ||
        (fabsf(cached_ratio - cfg_comp_ratio) > 1.0e-6f) ||
        (inv_step <= 0.0f)) {
        return comp_desired_gain_exact(abs_x);
    }

    if (abs_x <= 0.0f) return 1.0f;
    if (abs_x >= COMP_LUT_MAX_ABS) {
        return comp_desired_gain_exact(abs_x);
    }

    /* Preserve the exact compressor knee around the threshold. A pure
     * linear interpolation LUT smears the 1.0 -> compressed transition,
     * which slightly changes onset behaviour at high ratios. Keep a narrow
     * exact window around the knee; the LUT still removes the expensive
     * powf_pos() from the general hot path. */
    if (abs_x <= (cfg_comp_target + (2.0f / inv_step))) {
        return comp_desired_gain_exact(abs_x);
    }

    {
        float idx_f = abs_x * inv_step;
        uint32_t idx = (uint32_t)idx_f;
        float frac = idx_f - (float)idx;
        const float *lut = comp_gain_lut[lut_active & 1U];
        if (idx >= (COMP_LUT_SIZE - 1U)) return lut[COMP_LUT_SIZE - 1U];
        return lut[idx] + (lut[idx + 1U] - lut[idx]) * frac;
    }
}

static __attribute__((noinline)) uint8_t ssb_speech_tools_enabled(void)
{
    if (usb_clean_processing_active()) return 0U;
    return (uint8_t)((cfg_comp_enable != 0U) ||
                     (cfg_eq_enable   != 0U) ||
                     (cfg_alc_enable  != 0U));
}

static inline float speech_compressor_process(float x)
{
    if (usb_clean_processing_active() || !cfg_comp_enable)
    {
        comp_gain = 1.0f;
        live_comp_gain_db = 0.0f;
        return x;
    }

    float abs_x = fabsf(x);
    float desired_gain = (cfg_comp_math_mode == COMP_MATH_LUT)
                       ? comp_desired_gain_lut(abs_x)
                       : comp_desired_gain_exact(abs_x);
    float coeff = (desired_gain < comp_gain) ? COMP_ATTACK_COEFF : COMP_RELEASE_COEFF;
    comp_gain += (desired_gain - comp_gain) * coeff;
    comp_gain  = clampf(comp_gain, 0.20f, 1.0f);
    if (cfg_gain_telemetry_mode == GAIN_TELEMETRY_LIVE) {
        live_comp_gain_db = telemetry_gain_to_db(comp_gain);
    } else {
        live_comp_gain_db = 0.0f;
    }
    return x * comp_gain;
}

static inline float speech_eq_process(float x)
{
    if (usb_clean_processing_active() || !cfg_eq_enable) return x;
    x = biquad_process(&eq_bass_coeffs,   &eq_bass_state,   x);
    x = biquad_process(&eq_treble_coeffs, &eq_treble_state, x);
    return soft_clip_audio(x);
}

/* Out of line: one copy for the diagnostic image's SSB renderers (the
 * production ones run ssb_speech_block), not the compressor's powf and
 * log10f paths inlined into each. */
#if MOD_DEEP_DIAG_ENABLE
static __attribute__((noinline)) float ssb_speech_postprocess(float x)
{
    x = speech_compressor_process(x);
    x = speech_eq_process(x);
    x = alc_process(x);
    return x;
}
#endif

#if MOD_CESSB_EXPERIMENT_ENABLE
static __attribute__((noinline)) void ssb_apply_optional_cessb(float *I, float *Q,
                                                               const BiquadCoeffs *coeffs,
                                                               BiquadState *state_i,
                                                               BiquadState *state_q,
                                                               uint32_t sections)
{
    if (!(cfg_cessb_enable && (cfg_cessb_iter > 0U))) return;

    float clip2 = cfg_cessb_clip * cfg_cessb_clip;
    uint8_t n = cfg_cessb_iter;
    if (n > CESSB_ITER_MAX) n = CESSB_ITER_MAX;

    for (uint8_t iter = 0U; iter < n; iter++) {
        float mag2 = (*I) * (*I) + (*Q) * (*Q);
        if (mag2 > cessb_peak_pre) cessb_peak_pre = mag2;
        if (mag2 > clip2) {
            float pre = sqrtf(mag2);
            float scale = cfg_cessb_clip / pre;
            *I *= scale;
            *Q *= scale;
            mag2 = clip2;
            cessb_clipping_flag = 1U;
            cessb_clip_count++;
        }
        if (mag2 > cessb_peak_post_clip) cessb_peak_post_clip = mag2;
        *I = biquad_chain(coeffs, state_i, sections, *I);
        *Q = biquad_chain(coeffs, state_q, sections, *Q);
        {
            float post_filter2 = (*I) * (*I) + (*Q) * (*Q);
            if (post_filter2 > cessb_peak_post_filter) cessb_peak_post_filter = post_filter2;
        }
    }
}

static inline void cessb_frame_reset(void)
{
    cessb_clipping_flag = 0U;
    cessb_peak_pre = 0.0f;
    cessb_peak_post_clip = 0.0f;
    cessb_peak_post_filter = 0.0f;
    cessb_overshoot = 0.0f;
}

static inline void cessb_frame_finalize(void)
{
    if (cessb_peak_pre > 0.0f) {
        cessb_peak_pre = sqrtf(cessb_peak_pre);
    }
    if (cessb_peak_post_clip > 0.0f) {
        cessb_peak_post_clip = sqrtf(cessb_peak_post_clip);
    }
    if (cessb_peak_post_filter > 0.0f) {
        float peak = sqrtf(cessb_peak_post_filter);
        cessb_peak_post_filter = peak;
        cessb_overshoot = (peak > cfg_cessb_clip) ? (peak - cfg_cessb_clip) : 0.0f;
    }
}
#else
/* The discarded arguments are deliberately not evaluated, so production has
 * neither a transform nor references to the diagnostic-only filter state. */
#define ssb_apply_optional_cessb(I, Q, coeffs, state_i, state_q, sections) \
    do { (void)(I); (void)(Q); } while (0)
#define cessb_frame_reset()    do { } while (0)
#define cessb_frame_finalize() do { } while (0)
#endif

__attribute__((cold)) void sanitize_ssb_runtime_settings(void)
{
    cfg_cessb_enable = mod_cessb_enable_for_build(cfg_cessb_enable);
    if (((uint32_t)cfg_ssb_analytic_backend >= (uint32_t)SSB_ANALYTIC_COUNT) ||
        (MOD_SsbAnalyticBackendBuildAvailable(cfg_ssb_analytic_backend) == 0U)) {
        cfg_ssb_analytic_backend = SSB_ANALYTIC_WEAVER;
    }
    if ((uint32_t)cfg_tx_bw_preset >= (uint32_t)TX_BW_COUNT) {
        cfg_tx_bw_preset = TX_BW_200_2900;
    }

    cfg_ssb_phase_bias = mod_ssb_phase_bias_for_build(
        mod_control_sanitize(cfg_ssb_phase_bias, 0.0f,
                             0.0f, MOD_SSB_PHASE_BIAS_MAX));
    cfg_ssb_ce_enable = mod_ssb_ce_enable_for_build(cfg_ssb_ce_enable);
    cfg_ssb_ce_mix = mod_control_sanitize(cfg_ssb_ce_mix, 0.35f,
                                          0.0f, MOD_SSB_CE_MIX_MAX);
    cfg_ssb_ce_floor = mod_control_sanitize(cfg_ssb_ce_floor, 0.06f,
                                            0.0f, MOD_SSB_CE_FLOOR_MAX);
    cfg_ssb_ce_knee = mod_control_sanitize(cfg_ssb_ce_knee, 0.50f,
                                           MOD_SSB_CE_KNEE_MIN, MOD_SSB_CE_KNEE_MAX);
    cfg_ssb_low_cut = mod_control_sanitize(cfg_ssb_low_cut, 0.004f,
                                           0.0f, MOD_SSB_LOW_CUT_MAX);

    if (cfg_ssb_ce_floor < cfg_ssb_low_cut) cfg_ssb_ce_floor = cfg_ssb_low_cut;
    if (cfg_ssb_ce_floor > cfg_ssb_ce_knee - 0.05f) cfg_ssb_ce_floor = cfg_ssb_ce_knee - 0.05f;
    if (cfg_ssb_ce_floor < cfg_ssb_low_cut) cfg_ssb_ce_floor = cfg_ssb_low_cut;
    if (cfg_ssb_ce_knee < cfg_ssb_ce_floor + 0.05f) cfg_ssb_ce_knee = cfg_ssb_ce_floor + 0.05f;
    if (cfg_ssb_ce_knee > MOD_SSB_CE_KNEE_MAX) {
        cfg_ssb_ce_knee = MOD_SSB_CE_KNEE_MAX;
        if (cfg_ssb_ce_floor > cfg_ssb_ce_knee - 0.05f) cfg_ssb_ce_floor = cfg_ssb_ce_knee - 0.05f;
    }
}


void sanitize_rf_en_settings(void)
{
    cfg_rf_en_assert_metric = mod_control_sanitize(
        cfg_rf_en_assert_metric, RF_EN_GPIO_ASSERT_METRIC_DEFAULT,
        RF_EN_GPIO_ASSERT_METRIC_MIN, RF_EN_GPIO_ASSERT_METRIC_MAX);
    cfg_rf_en_release_metric = mod_control_sanitize(
        cfg_rf_en_release_metric, RF_EN_GPIO_RELEASE_METRIC_DEFAULT,
        RF_EN_GPIO_RELEASE_METRIC_MIN, RF_EN_GPIO_RELEASE_METRIC_MAX);
    if (cfg_rf_en_release_metric > cfg_rf_en_assert_metric) {
        cfg_rf_en_release_metric = cfg_rf_en_assert_metric;
    }
    if (cfg_rf_en_hold_frames > RF_EN_GPIO_HOLD_FRAMES_MAX) {
        cfg_rf_en_hold_frames = RF_EN_GPIO_HOLD_FRAMES_MAX;
    }
}


static inline float ssb_theta_env_map(float env)
{
    return mod_ssb_theta_envelope_map(env,
                                      cfg_ssb_low_cut,
                                      cfg_ssb_phase_bias,
                                      cfg_ssb_ce_enable,
                                      cfg_ssb_ce_mix,
                                      cfg_ssb_ce_floor,
                                      cfg_ssb_ce_knee);
}

static inline float get_theta_pd_correction(float env)
{
    if (frame_cal_active == 0U) return 0.0f;
    return mod_calibration_interp(theta_linearity_pd, env);
}

/* mod_outphase_limit_theta_for_mode with a given cap, counting the samples
 * it limits: the block renderer publishes the count once a frame. */
static inline float ssb_limit_theta_counted(float theta, float cap, uint32_t *limited)
{
    if (!isfinite(theta)) theta = 0.0f;
    theta = clampf(theta, 0.0f, OUTPHASE_THETA_HARD_MAX);
    if (theta > cap) {
        theta = cap;
        (*limited)++;
    }
    return theta;
}

static inline void ssb_publish_theta_limits(uint32_t limited)
{
    if (limited != 0U) {
        outphase_limit_latched = 1U;
        outphase_limit_count += limited;
    }
}

/* mod_outphase_limit_theta_for_mode with the frame's SSB cap. */
static inline float ssb_limit_theta(float theta)
{
    if (!isfinite(theta)) theta = 0.0f;
    theta = clampf(theta, 0.0f, OUTPHASE_THETA_HARD_MAX);
    if (theta > frame_ssb_theta_cap) {
        theta = frame_ssb_theta_cap;
        outphase_limit_latched = 1U;
        outphase_limit_count++;
    }
    return theta;
}

static inline float ssb_theta_command(float env_theta)
{
    /* mod_outphase_theta_for_env(MOD_TYPE_SSB, env_theta), inlined. */
    const float env = clampf(sanitize_nan_default(env_theta, 0.0f), 0.0f, 0.999f);
    float theta = ssb_limit_theta(outphase_theta(env));
    theta += get_theta_pd_correction(env_theta);
    return ssb_limit_theta(theta);
}

static inline void ssb_note_low_env_inhibit(void)
{
    low_env_inhibit_count++;
}

static inline const BiquadCoeffs *tx_bw_hpf_coeffs(void)
{
    switch (cfg_tx_bw_preset) {
        case TX_BW_100_3000: return enh_hpf_100hz;
        case TX_BW_200_2900: return speech_hpf_200hz;
        case TX_BW_300_2400:
        case TX_BW_300_2700:
        default:             return speech_hpf_std;
    }
}

static const BiquadCoeffs *frame_tx_bw_hpf;
static const BiquadCoeffs *frame_tx_bw_lpf;
static uint8_t frame_tx_bw_lpf_two;
static uint8_t frame_tx_bw_bypass;

static inline float tx_bw_hpf_filter_sample(
    BiquadState state[TX_BW_HPF_STATE_SECTIONS], float x)
{
    const BiquadCoeffs *coeffs = frame_tx_bw_hpf;
    return biquad_process(&coeffs[0], &state[0], x);
}

static inline const BiquadCoeffs *tx_bw_lpf_coeffs(void)
{
    switch (cfg_tx_bw_preset) {
        case TX_BW_100_3000: return enh_lpf_3000hz;
        case TX_BW_200_2900: return speech_lpf_2900hz;
        case TX_BW_300_2400: return speech_lpf_narrow;
        case TX_BW_300_2700:
        default:             return speech_lpf_std;
    }
}

static inline float tx_bw_lpf_filter_sample(
    BiquadState state[TX_BW_LPF_STATE_SECTIONS], float x)
{
    const BiquadCoeffs *coeffs = frame_tx_bw_lpf;
    x = biquad_process(&coeffs[0], &state[0], x);
    if (frame_tx_bw_lpf_two != 0U) {
        x = biquad_process(&coeffs[1], &state[1], x);
    }
    return x;
}

static inline uint8_t tx_bw_filter_bypass_active(void)
{
    return (uint8_t)((usb_clean_processing_active() != 0U) &&
                     (active_audio_profile == AUDIO_PROFILE_USB_HIFI));
}

static inline float tx_bw_filter_sample(float x)
{
    if (frame_tx_bw_bypass != 0U) return x;
    x = tx_bw_hpf_filter_sample(txbw_hpf_state, x);
    x = tx_bw_lpf_filter_sample(txbw_lpf_state, x);
    return x;
}

#if SSB_EXT_ALL_BACKENDS
static inline float qmx12_input_filter_sample(float x)
{
    x = tx_bw_filter_sample(x);
    x = biquad_chain(qmx12_aa_lpf, qmx12_aa_lpf_state, QMX12_AA_LPF_SECTIONS, x);
    return x;
}
#endif

static inline float ssb_theta_env_map_legacy_raw(float env)
{
    env = clampf(env, 0.0f, 0.999f);
    if (env <= cfg_ssb_low_cut) return 0.0f;
    return soft_limit(env, cfg_knee, 1.0f - cfg_knee);
}

uint8_t ssb_backend_is_hilbert(ssb_analytic_backend_t backend)
{
    return (uint8_t)(backend != SSB_ANALYTIC_WEAVER);
}

uint8_t cal_backend_storage_key(uint8_t backend)
{
    if (backend == (uint8_t)SSB_ANALYTIC_WEAVER) return (uint8_t)SSB_ANALYTIC_WEAVER;
    if (backend == (uint8_t)SSB_ANALYTIC_H12P) return (uint8_t)SSB_ANALYTIC_H12P;
    return (uint8_t)SSB_ANALYTIC_HILBERT;
}

#if SSB_EXT_ALL_BACKENDS
static inline float hilbert_prefilter_sample(ssb_analytic_backend_t backend, float x)
{
    if (backend == SSB_ANALYTIC_HILBERT_PAIR_24) {
        x = tx_bw_hpf_filter_sample(hilb24_hpf_state, x);
        x = tx_bw_lpf_filter_sample(hilb24_lpf_state, x);
        return x;
    }
#if MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE
    if ((backend == SSB_ANALYTIC_HILBERT_PAIR_12) ||
        (backend == SSB_ANALYTIC_H12P)) {
#else
    if (backend == SSB_ANALYTIC_HILBERT_PAIR_12) {
#endif
        x = tx_bw_hpf_filter_sample(hilb12a_hpf_state, x);
        x = tx_bw_lpf_filter_sample(hilb12a_lpf_state, x);
        x = tx_bw_hpf_filter_sample(hilb12b_hpf_state, x);
        x = tx_bw_lpf_filter_sample(hilb12b_lpf_state, x);
        return x;
    }
    return x;
}

static inline void hilbert_process_sample_legacy(float x, float *i_sig, float *q_sig)
{
    HilbertLegacyState *st = &hilbert_workspace.legacy;
    st->hist[st->wr_idx] = x;

    float q = 0.0f;
    for (uint32_t t = 0U; t < HILB_NUM_TAPS; t++) {
        int32_t idx = (int32_t)st->wr_idx - (int32_t)t;
        while (idx < 0) idx += (int32_t)HILB_NUM_TAPS;
        q += hilb_coeffs[t] * st->hist[(uint32_t)idx];
    }

    int32_t didx = (int32_t)st->wr_idx - (int32_t)HILB_GROUP_DELAY;
    while (didx < 0) didx += (int32_t)HILB_NUM_TAPS;
    *i_sig = st->hist[(uint32_t)didx];
    *q_sig = active_sideband_sign * q;

    st->wr_idx++;
    if (st->wr_idx >= HILB_NUM_TAPS) st->wr_idx = 0U;
}

static inline void hilbert_process_sample_pair(HilbertExperimentState *st, float x,
                                                const float *coeffs, uint32_t num_taps,
                                                uint32_t group_delay, uint8_t decim,
                                                float *i_sig, float *q_sig)
{
    if (st == NULL) {
        *i_sig = 0.0f;
        *q_sig = 0.0f;
        return;
    }

    if (st->decim_phase == 0U) {
        st->hist[st->wr_idx] = x;
        float q = 0.0f;
        int32_t idx = (int32_t)st->wr_idx;
        for (uint32_t t = 0U; t < num_taps; t++) {
            q += coeffs[t] * st->hist[(uint32_t)idx];
            if (idx == 0) idx = (int32_t)num_taps - 1;
            else          idx--;
        }

        int32_t didx = (int32_t)st->wr_idx - (int32_t)group_delay;
        while (didx < 0) didx += (int32_t)num_taps;
        float d = st->hist[(uint32_t)didx];
        st->wr_idx++;
        if (st->wr_idx >= num_taps) st->wr_idx = 0U;

        float q_side = active_sideband_sign * q;
        float i_low = INV_SQRT2_F * (d - q_side);
        float q_low = INV_SQRT2_F * (d + q_side);

        if (!st->primed) {
            st->primed = 1U;
            st->last_i = i_low;
            st->last_q = q_low;
            st->seg_i0 = i_low;
            st->seg_q0 = q_low;
            st->seg_i1 = i_low;
            st->seg_q1 = q_low;
        } else {
            st->seg_i0 = st->last_i;
            st->seg_q0 = st->last_q;
            st->seg_i1 = i_low;
            st->seg_q1 = q_low;
            st->last_i = i_low;
            st->last_q = q_low;
        }
        st->interp_phase = 0U;
    }

    if (!st->primed) {
        *i_sig = 0.0f;
        *q_sig = 0.0f;
    } else {
        float w = (float)st->interp_phase / (float)decim;
        *i_sig = st->seg_i0 + (st->seg_i1 - st->seg_i0) * w;
        *q_sig = st->seg_q0 + (st->seg_q1 - st->seg_q0) * w;
        if (st->interp_phase + 1U < decim) st->interp_phase++;
    }

    st->decim_phase++;
    if (st->decim_phase >= decim) st->decim_phase = 0U;
}

static inline void hilbert_process_sample_lowrate(HilbertExperimentState *st, float x,
                                                   float *i_sig, float *q_sig)
{
    if (st == NULL) {
        *i_sig = 0.0f;
        *q_sig = 0.0f;
        return;
    }

    st->hist[st->wr_idx] = x;
    float q = 0.0f;
    int32_t idx = (int32_t)st->wr_idx;
    for (uint32_t t = 0U; t < HILB_PAIR12_NUM_TAPS; t++) {
        q += hilb_pair12_coeffs[t] * st->hist[(uint32_t)idx];
        if (idx == 0) idx = (int32_t)HILB_PAIR12_NUM_TAPS - 1;
        else          idx--;
    }

    int32_t didx = (int32_t)st->wr_idx - (int32_t)HILB_PAIR12_GROUP_DELAY;
    while (didx < 0) didx += (int32_t)HILB_PAIR12_NUM_TAPS;
    float d = st->hist[(uint32_t)didx];
    st->wr_idx++;
    if (st->wr_idx >= HILB_PAIR12_NUM_TAPS) st->wr_idx = 0U;

    float q_side = active_sideband_sign * q;
    *i_sig = INV_SQRT2_F * (d - q_side);
    *q_sig = INV_SQRT2_F * (d + q_side);
    st->primed = 1U;
}

/* The diagnostic renderers' analytic step, a sample at a time. */
static __attribute__((noinline)) void hilbert_process_sample_runtime(ssb_analytic_backend_t backend,
                                                                     float x,
                                                                     float *i_sig, float *q_sig)
{
    if (backend == SSB_ANALYTIC_HILBERT_PAIR_24) {
        x = hilbert_prefilter_sample(backend, x);
        hilbert_process_sample_pair(&hilbert_workspace.experimental, x,
                                    hilb_pair24_coeffs, HILB_PAIR24_NUM_TAPS,
                                    HILB_PAIR24_GROUP_DELAY, HILB_PAIR24_DECIM,
                                    i_sig, q_sig);
        return;
    }
    if (backend == SSB_ANALYTIC_HILBERT_PAIR_12) {
        x = hilbert_prefilter_sample(backend, x);
        hilbert_process_sample_pair(&hilbert_workspace.experimental, x,
                                    hilb_pair12_coeffs, HILB_PAIR12_NUM_TAPS,
                                    HILB_PAIR12_GROUP_DELAY, HILB_PAIR12_DECIM,
                                    i_sig, q_sig);
        return;
    }
#if MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE
    if (backend == SSB_ANALYTIC_H12P) {
        x = tx_bw_filter_sample(x);
        mod_analytic12_process(&hilbert_workspace.production12, x,
                               active_sideband_sign, i_sig, q_sig);
        return;
    }
#endif
    hilbert_process_sample_legacy(x, i_sig, q_sig);
}

#endif /* SSB_EXT_ALL_BACKENDS */

static inline uint8_t ssb_ext_active(void)
{
    return ssb_backend_is_hilbert(cfg_ssb_analytic_backend);
}

#if SSB_EXT_ALL_BACKENDS
static __attribute__((noinline)) uint8_t ssb_strict_qmx12_active(void)
{
    return (uint8_t)((MOD_GetSsbPipelineMode() == SSB_PIPELINE_STRICT_QMX12_EXP) &&
                     (cfg_ssb_analytic_backend == SSB_ANALYTIC_HILBERT_PAIR_12));
}
#endif

void refresh_active_cal_ptr(void)
{
    active_cal = &phase_cal_active;
}

/* ═══════════════ Unified audio front-end ═══════════════
 * Applied to PROD, RAW DIR, ENH, FILT paths before Weaver.
 * Order: DC-block -> mute fade -> mic AGC -> noise gate -> (mode-specific filters).
 */
HOT_INLINE float mute_gain_advance(float gain, float target)
{
    if (gain < target) { gain += MUTE_STEP; if (gain > target) gain = target; }
    else if (gain > target) { gain -= MUTE_STEP; if (gain < target) gain = target; }
    return gain;
}

float mute_gain_step(void)
{
    mute_gain = mute_gain_advance(mute_gain, mute_target);
    return mute_gain;
}

void mute_gain_step_n(uint32_t samples)
{
    while (samples-- > 0U) {
        (void)mute_gain_step();
    }
}

static inline float apply_mute_gain(float x)
{
    return x * mute_gain_step();
}

#if MOD_DEEP_DIAG_ENABLE   /* the production renderers run ssb_front_end_block */
static __attribute__((noinline)) float audio_front_end(float x, BiquadState *dc_state)
{
    x = usb_voice_gain_process(x);
    x = biquad_chain(dc_block, dc_state, DC_BLOCK_SECTIONS, x);
    x = apply_mute_gain(x);
    x = mic_agc_process(x);
    x = noise_gate_process(x);
    return x;
}
#endif

/* ═══════════════ NCO helpers ═══════════════ */

static inline void osc_renorm(float *re, float *im)
{
    float m2 = (*re) * (*re) + (*im) * (*im);
    if (isfinite(m2) && (m2 > 0.0f)) { float inv = 1.0f / sqrtf(m2); *re *= inv; *im *= inv; }
    else           { *re = 1.0f; *im = 0.0f; }
}

static void osc_init(ComplexOsc *o, float hz)
{
    o->re = 1.0f; o->im = 0.0f;
    o->step_re = cosf(TWO_PI * hz / 48000.0f);
    o->step_im = sinf(TWO_PI * hz / 48000.0f);
}

static inline void osc_advance(ComplexOsc *o)
{
    float nr = o->re * o->step_re - o->im * o->step_im;
    float ni = o->im * o->step_re + o->re * o->step_im;
    o->re = nr; o->im = ni;
}

static inline void osc_fix(ComplexOsc *o) { osc_renorm(&o->re, &o->im); }

__attribute__((cold)) void mod_tx_pipeline_init(void)
{
    cordic_hw_init();
    osc_step_re     = cosf(TWO_PI * WEAVER_FREQ_HZ     / 48000.0f);
    osc_step_im     = sinf(TWO_PI * WEAVER_FREQ_HZ     / 48000.0f);
#if SSB_COMPARE_RENDERERS
    osc_enh_step_re = cosf(TWO_PI * WEAVER_FREQ_ENH_HZ / 48000.0f);
    osc_enh_step_im = sinf(TWO_PI * WEAVER_FREQ_ENH_HZ / 48000.0f);
#endif
#if SSB_EXT_ALL_BACKENDS
    osc_ext_step_re = cosf(TWO_PI * WEAVER_FREQ_ENH_HZ / 48000.0f);
    osc_ext_step_im = sinf(TWO_PI * WEAVER_FREQ_ENH_HZ / 48000.0f);
#endif

    osc_init(&test_osc_1k,   TEST_TONE_1_HZ);
#if MOD_DEEP_DIAG_ENABLE
    osc_init(&test_osc_700,  TEST_TONE_2A_HZ);
    osc_init(&test_osc_1900, TEST_TONE_2B_HZ);
#endif
#if MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE
    mod_analytic12_coeffs_load(&analytic12_coeffs);
#endif
}

static inline float calibration_differential_skew(void)
{
    if (frame_cal_active == 0U) return 0.0f;
    return active_cal->differential_skew;
}

static inline float calibration_common_phase(float env)
{
    if (frame_cal_active == 0U) return 0.0f;
    return mod_calibration_interp(active_cal->common_phase_pd, env);
}

/* ═══════════════ DSP helpers ═══════════════ */

HOT_INLINE float biquad_process(const BiquadCoeffs *c, BiquadState *s, float x)
{
    float y = c->b0 * x + s->z1;
    s->z1 = c->b1 * x - c->a1 * y + s->z2;
    s->z2 = c->b2 * x - c->a2 * y;
    return y;
}

HOT_INLINE float biquad_chain_1(const BiquadCoeffs *c, BiquadState *s, float x)
{
    return biquad_process(&c[0], &s[0], x);
}

HOT_INLINE float biquad_chain_2(const BiquadCoeffs *c, BiquadState *s, float x)
{
    x = biquad_process(&c[0], &s[0], x);
    return biquad_process(&c[1], &s[1], x);
}

HOT_INLINE float biquad_chain_3(const BiquadCoeffs *c, BiquadState *s, float x)
{
    x = biquad_process(&c[0], &s[0], x);
    x = biquad_process(&c[1], &s[1], x);
    return biquad_process(&c[2], &s[2], x);
}

HOT_CODE static float biquad_chain(const BiquadCoeffs *c, BiquadState *s, uint32_t n, float x)
{
    switch (n) {
        case 0U: return x;
        case 1U: return biquad_chain_1(c, s, x);
        case 2U: return biquad_chain_2(c, s, x);
        case 3U: return biquad_chain_3(c, s, x);
        default:
            for (uint32_t i = 0; i < n; i++) x = biquad_process(&c[i], &s[i], x);
            return x;
    }
}

/* ═══════════════ EQ shelf computation (RBJ Cookbook) ═══════════════ */

static void compute_shelf(BiquadCoeffs *c, float fc, float dB, int high_shelf)
{
    if (fabsf(dB) < 0.001f)
    {
        /* Identity: pass-through, avoid wasted multiplies */
        c->b0 = 1.0f; c->b1 = 0.0f; c->b2 = 0.0f;
        c->a1 = 0.0f; c->a2 = 0.0f;
        return;
    }
    float A    = powf_pos(10.0f, dB / 40.0f);
    float w0   = TWO_PI * fc / 48000.0f;
    float cw   = cosf(w0);
    float sw   = sinf(w0);
    float alpha = sw * 0.5f * 1.41421356f;          /* shelf slope S=1 */
    float beta  = 2.0f * sqrtf(A) * alpha;
    float Ap1   = A + 1.0f;
    float Am1   = A - 1.0f;
    float a0;

    if (high_shelf)
    {
        c->b0 =  A * (Ap1 + Am1 * cw + beta);
        c->b1 = -2.0f * A * (Am1 + Ap1 * cw);
        c->b2 =  A * (Ap1 + Am1 * cw - beta);
        a0    =        Ap1 - Am1 * cw + beta;
        c->a1 =  2.0f *     (Am1 - Ap1 * cw);
        c->a2 =        Ap1 - Am1 * cw - beta;
    }
    else
    {
        c->b0 =  A * (Ap1 - Am1 * cw + beta);
        c->b1 =  2.0f * A * (Am1 - Ap1 * cw);
        c->b2 =  A * (Ap1 - Am1 * cw - beta);
        a0    =        Ap1 + Am1 * cw + beta;
        c->a1 = -2.0f *     (Am1 + Ap1 * cw);
        c->a2 =        Ap1 + Am1 * cw - beta;
    }
    float inv = 1.0f / a0;
    c->b0 *= inv; c->b1 *= inv; c->b2 *= inv;
    c->a1 *= inv; c->a2 *= inv;
}

__attribute__((cold)) void recompute_eq_coeffs(void)
{
    compute_shelf(&eq_bass_coeffs,   200.0f, cfg_eq_bass,   0);
    compute_shelf(&eq_treble_coeffs, 2600.0f, cfg_eq_treble, 1);
}

/* ═══════════════ Profile management ═══════════════ */

__attribute__((cold)) void apply_audio_profile(audio_profile_t profile)
{
    switch (profile)
    {
        case AUDIO_PROFILE_CLEAN:
            /* CLEAN stays natural-first: no aggressive envelope lifting, but
             * keep a tiny phase-bias headroom so weak speech does not fall
             * straight into the worst tiny-theta region. */
            cfg_mic_agc_enable = 0U;
            cfg_usb_clean_mode  = (uint8_t)USB_AUDIO_POLICY_DIGI_CLEAN;
            cfg_ng_enable      = 0U;
            cfg_alc_enable     = 1U;
            cfg_comp_enable    = 0U;
            cfg_cessb_enable   = 0U;
            cfg_eq_enable      = 0U;
            cfg_comp_target    = 0.60f;
            cfg_comp_ratio     = 2.0f;
            cfg_alc_ceiling    = 0.97f;
            cfg_cessb_clip     = 0.94f;
            cfg_eq_bass        = 0.0f;
            cfg_eq_treble      = 0.0f;
            cfg_ssb_phase_bias = 0.00f;
            cfg_ssb_ce_enable  = 0U;
            cfg_tx_bw_preset   = TX_BW_200_2900;
            cfg_ssb_ce_mix     = 0.18f;
            cfg_ssb_ce_floor   = 0.035f;
            cfg_ssb_ce_knee    = 0.44f;
            cfg_ssb_low_cut    = 0.004f;
            break;
        case AUDIO_PROFILE_ENHANCED:
            /* Processed speech preset. Its legacy nonlinear RF-envelope
             * requests resolve to OFF in production. */
            cfg_mic_agc_enable = 0U;
            cfg_usb_clean_mode  = (uint8_t)USB_AUDIO_POLICY_VOICE_PROC;
            cfg_ng_enable      = 0U;
            cfg_alc_enable     = 1U;
            cfg_comp_enable    = 1U;
            cfg_cessb_enable   = 0U;
            cfg_eq_enable      = 1U;
            cfg_comp_target    = 0.68f;
            cfg_comp_ratio     = 2.0f;
            cfg_cessb_clip     = 0.94f;
            cfg_eq_bass        = -0.5f;
            cfg_eq_treble      = 2.0f;
            cfg_ssb_phase_bias = mod_ssb_phase_bias_for_build(0.05f);
            cfg_ssb_ce_enable  = mod_ssb_ce_enable_for_build(1U);
            cfg_tx_bw_preset   = TX_BW_200_2900;
            cfg_ssb_ce_mix     = 0.28f;
            cfg_ssb_ce_floor   = 0.045f;
            cfg_ssb_ce_knee    = 0.52f;
            cfg_ssb_low_cut    = 0.002f;
            break;
        case AUDIO_PROFILE_DX:
            /* DX favors punch and readability without chopping syllable tails
             * too early by default. */
            cfg_mic_agc_enable = 1U;
            cfg_usb_clean_mode  = (uint8_t)USB_AUDIO_POLICY_VOICE_PROC;
            cfg_ng_enable      = 1U;
            cfg_alc_enable     = 1U;
            cfg_comp_enable    = 1U;
            cfg_cessb_enable   = 0U;
            cfg_eq_enable      = 1U;
            cfg_comp_target    = 0.54f;
            cfg_comp_ratio     = 4.5f;
            cfg_cessb_clip     = 0.91f;
            cfg_eq_bass        = -2.0f;
            cfg_eq_treble      = 3.8f;
            cfg_ssb_phase_bias = mod_ssb_phase_bias_for_build(0.10f);
            cfg_ssb_ce_enable  = mod_ssb_ce_enable_for_build(1U);
            cfg_tx_bw_preset   = TX_BW_300_2700;
            cfg_ssb_ce_mix     = 0.42f;
            cfg_ssb_ce_floor   = 0.070f;
            cfg_ssb_ce_knee    = 0.58f;
            cfg_ssb_low_cut    = 0.0035f;
            break;
        case AUDIO_PROFILE_QRP:
            /* The legacy diagnostic image retains its clip/filter experiment
             * for comparison; production policy resolves this request to OFF. */
            cfg_mic_agc_enable = 1U;
            cfg_usb_clean_mode  = (uint8_t)USB_AUDIO_POLICY_VOICE_PROC;
            cfg_ng_enable      = 1U;
            cfg_alc_enable     = 1U;
            cfg_comp_enable    = 1U;
            cfg_cessb_enable   = mod_cessb_enable_for_build(1U);
            cfg_eq_enable      = 1U;
            cfg_comp_target    = 0.56f;
            cfg_comp_ratio     = 4.0f;
            cfg_cessb_clip     = 0.89f;
            cfg_eq_bass        = -1.5f;
            cfg_eq_treble      = 3.5f;
            cfg_ssb_phase_bias = mod_ssb_phase_bias_for_build(0.16f);
            cfg_ssb_ce_enable  = mod_ssb_ce_enable_for_build(1U);
            cfg_tx_bw_preset   = TX_BW_300_2700;
            cfg_ssb_ce_mix     = 0.55f;
            cfg_ssb_ce_floor   = 0.090f;
            cfg_ssb_ce_knee    = 0.62f;
            cfg_ssb_low_cut    = 0.0045f;
            break;
        case AUDIO_PROFILE_USB_HIFI:
            cfg_usb_clean_mode  = (uint8_t)USB_AUDIO_POLICY_DIGI_CLEAN;
            cfg_mic_agc_enable = 0U;
            cfg_ng_enable      = 0U;
            cfg_alc_enable     = 0U;
            cfg_comp_enable    = 0U;
            cfg_cessb_enable   = 0U;
            cfg_eq_enable      = 0U;
            cfg_comp_target    = 0.80f;
            cfg_comp_ratio     = 1.0f;
            cfg_alc_ceiling    = 0.99f;
            cfg_cessb_clip     = 1.00f;
            cfg_eq_bass        = 0.0f;
            cfg_eq_treble      = 0.0f;
            cfg_ssb_phase_bias = 0.00f;
            cfg_ssb_ce_enable  = 0U;
            cfg_tx_bw_preset   = TX_BW_100_3000;
            cfg_ssb_ce_mix     = 0.0f;
            cfg_ssb_ce_floor   = 0.0f;
            cfg_ssb_ce_knee    = 0.25f;
            cfg_ssb_low_cut    = 0.0f;
            break;
        case AUDIO_PROFILE_CUSTOM:
        default:
            break;
    }
    sanitize_ssb_runtime_settings();
    recompute_eq_coeffs();
    active_audio_profile = profile;
    mod_tx_pipeline_comp_lut_sync();
}

void mark_profile_custom(void)
{
    active_audio_profile = AUDIO_PROFILE_CUSTOM;
}

static void fill_null_frame_no_rf_side_effect(render_cmp_t *o1, render_cmp_t *o2)
{
    uint32_t ta, tc;
    build_idle_branch_pair(&ta, &tc);
    for (uint32_t i = 0; i < AUDIO_FRAME_SIZE; i++) { o1[i] = ta; o2[i] = tc; }
}

static void fill_null_frame(render_cmp_t *o1, render_cmp_t *o2)
{
    fill_null_frame_no_rf_side_effect(o1, o2);
    rf_en_force_idle();
}

/* Transport/DMA/render FIFO implementation lives in mod_transport.c. */


/* ═══════════════ Audio metering ═══════════════ */

static uint16_t meter_update(const int16_t *m, uint32_t n)
{
    if (!m || !n) return 0U;
    uint32_t pk = 0U;
    uint32_t gain_pk = 0U;
    float    acc = 0.0f;
    float    gain_acc = 0.0f;
    uint32_t clip_inc = 0U;
    for (uint32_t i = 0; i < n; i++) {
        int32_t  s = m[i];
        uint32_t a = (uint32_t)(s < 0 ? -s : s);
        if (a > pk) pk = a;
        float fs = (float)s / 32768.0f;
        acc += fs * fs;
        float gs = fs * cfg_usb_input_gain;
        if (gs > 1.0f) {
            gs = 1.0f;
            clip_inc++;
        } else if (gs < -1.0f) {
            gs = -1.0f;
            clip_inc++;
        }
        uint32_t ga = (uint32_t)(fabsf(gs) * 32768.0f);
        if (ga > 32767U) ga = 32767U;
        if (ga > gain_pk) gain_pk = ga;
        gain_acc += gs * gs;
    }
    meter_peak_raw = (uint16_t)pk;
    meter_rms_sq_acc = acc;
    meter_sample_count = n;
    usb_input_peak_gain_raw = (uint16_t)gain_pk;
    usb_input_rms_gain = sqrtf(gain_acc / (float)n);
    usb_input_clip_count += clip_inc;
    usb_input_activity = (pk > USB_AUDIO_ACTIVITY_THRESHOLD) ? 1U : 0U;
    if      (pk > meter_peak_hold)   meter_peak_hold = (uint16_t)pk;
    else if (meter_peak_hold > 16U)  meter_peak_hold -= 16U;
    else                             meter_peak_hold  = 0U;
    return (uint16_t)pk;
}

static inline uint8_t tx_test_debounce_update(TxTestDebounceState *db, uint8_t raw)
{
    raw = raw ? 1U : 0U;
    if (raw == db->stable) {
        db->last_raw = raw;
        db->count = 0U;
        return db->stable;
    }
    if (raw != db->last_raw) {
        db->last_raw = raw;
        db->count = 1U;
    } else if (db->count < TX_TEST_DEBOUNCE_FRAMES) {
        db->count++;
    }
    if (db->count >= TX_TEST_DEBOUNCE_FRAMES) {
        db->stable = raw;
        db->count = 0U;
    }
    return db->stable;
}

static inline void tx_test_debounce_reset_one(TxTestDebounceState *db)
{
    db->stable = 0U;
    db->last_raw = 0U;
    db->count = 0U;
}

static uint8_t tx_test_gate_update(void)
{
    uint8_t raw_dit = 0U;
    uint8_t raw_dah = 0U;
    (void)cw_get_paddles_for_tx_test(&raw_dit, &raw_dah);

    tx_test_paddle_dit = tx_test_debounce_update(&tx_test_dit_db, raw_dit);
    tx_test_paddle_dah = tx_test_debounce_update(&tx_test_dah_db, raw_dah);
    tx_test_gate_active = (uint8_t)((tx_test_paddle_dit || tx_test_paddle_dah) ? 1U : 0U);
    return tx_test_gate_active;
}

void mod_tx_test_gate_reset(void)
{
    tx_test_paddle_dit = 0U;
    tx_test_paddle_dah = 0U;
    tx_test_gate_active = 0U;
    tx_test_debounce_reset_one(&tx_test_dit_db);
    tx_test_debounce_reset_one(&tx_test_dah_db);
}

/* ═══════════════ Test signal renderer (NCO based) ═══════════════ */

static void render_tx_carrier_test(const int16_t *mono, uint32_t count, render_cmp_t *out1, render_cmp_t *out2)
{
    (void)mono;
    if (count > AUDIO_FRAME_SIZE) count = AUDIO_FRAME_SIZE;

    uint8_t paddle_active = tx_test_gate_update();
    uint8_t tx_active = (uint8_t)((paddle_active != 0U) && (!cfg_muted));

    if (!tx_active) {
        fill_null_frame_no_rf_side_effect(out1, out2);
        cw_render_frame_tx_live = 0U;
        cw_render_frame_rf_active = 0U;
        DIAG_CAPTURE(out1[0], out2[0], 0.0f, 0.0f, 0.0f);
        return;
    }

    /* TX TEST is a PA current/load test: use a strong, low-outphase carrier
     * and let PC10/paddle gating be the RF on/off control. */
    float body_drive = sanitize_nan_default(cfg_cw_drive, CW_DEFAULT_DRIVE);
    body_drive = clampf(body_drive, CW_DRIVE_MIN, CW_DRIVE_MAX);
    const float theta = mod_outphase_limit_theta_for_mode(MOD_TYPE_TEST,
                                                          cfg_outphase_test_theta + cfg_cw_theta_trim);
    const float env_dbg = clampf(body_drive, 0.0f, 1.0f);
    float cal = calibration_differential_skew();
    float a_delta, c_delta;
    outphase_branch_deltas(theta, cal, &a_delta, &c_delta);

    uint32_t ta, tc;
    build_branch_pair_from_relative(calibration_common_phase(env_dbg),
                                    a_delta, c_delta, &ta, &tc);

    for (uint32_t i = 0U; i < AUDIO_FRAME_SIZE; i++) {
        out1[i] = ta;
        out2[i] = tc;
    }

    cw_render_frame_tx_live = 1U;
    cw_render_frame_rf_active = 1U;
    g_last_cw_tick = HAL_GetTick();
    DIAG_CAPTURE(ta, tc, env_dbg, theta, 0.0f);
}

__attribute__((cold)) void MOD_TxCarrierTestStop(void)
{
    mod_tx_test_gate_reset();
    cw_paddle_dit = 0U;
    cw_paddle_dah = 0U;
    cw_key_sample_active = 0U;
    cw_tx_live = 0U;
    cw_rf_enabled = 0U;
    rf_en_force_idle();
}

uint8_t MOD_TxCarrierTestPaddleActive(void)
{
    return (uint8_t)((tx_test_paddle_dit || tx_test_paddle_dah) ? 1U : 0U);
}

/* Out-of-line copies of the per-sample helpers for the experimental and
 * internal-test renderers: one copy each instead of every renderer inlining
 * them several times (about 5 KB of flash). The production renderers -- SSB
 * (its own frame forms), AM, FM, CW and the carrier test -- keep them
 * inline. */
#if MOD_DEEP_DIAG_ENABLE
static __attribute__((noinline)) void rf_en_track_ssb_call(float metric, float *frame_peak,
                                                         uint8_t *frame_active)
{
    rf_en_track_ssb_sample(metric, frame_peak, frame_active);
}

static __attribute__((noinline)) void build_branch_pair_call(float common_phase, float a_relative,
                                                           float c_relative, uint32_t *ta, uint32_t *tc)
{
    build_branch_pair_from_relative(common_phase, a_relative, c_relative, ta, tc);
}

static __attribute__((noinline)) render_cmp_t null_compare_tick_call(void)
{
    return mod_null_compare_tick();
}
#endif

/* The internal test signals and RAW LEG are offered only by the deep-diagnostic
 * build (MOD_ModeAvailableInBuild): no other build can select them, so no
 * other build has their renderers. */
#if MOD_DEEP_DIAG_ENABLE
static void render_test_linear(const int16_t *mono, uint32_t count, render_cmp_t *out1, render_cmp_t *out2)
{
    (void)mono;
    if (count > AUDIO_FRAME_SIZE) count = AUDIO_FRAME_SIZE;
    mod_config_t cfg = (mod_config_t)active_config;
    float sign = (active_sideband_enum == SIDEBAND_LSB) ? 1.0f : -1.0f;
    float frame_rf_peak = 0.0f;
    uint8_t frame_rf_active = 0U;

    for (uint32_t i = 0; i < count; i++)
    {
        float mute = mute_gain_step();
        float I = 0.0f, Q = 0.0f;

        if (cfg == MOD_CFG_INT_1KHZ_TONE)
        {
            I = TEST_TONE_1_AMP *  test_osc_1k.re;
            Q = TEST_TONE_1_AMP *  test_osc_1k.im * sign;
            osc_advance(&test_osc_1k);
            if ((i & 31U) == 31U) osc_fix(&test_osc_1k);
        }
        else if (cfg == MOD_CFG_INT_2TONE_IMD ||
                 cfg == MOD_CFG_INT_2TONE_BRANCH_A ||
                 cfg == MOD_CFG_INT_2TONE_BRANCH_C)
        {
            I = TEST_TONE_2_AMP * (test_osc_700.re  + test_osc_1900.re);
            Q = TEST_TONE_2_AMP * (test_osc_700.im  + test_osc_1900.im) * sign;
            osc_advance(&test_osc_700);
            osc_advance(&test_osc_1900);
            if ((i & 31U) == 31U) { osc_fix(&test_osc_700); osc_fix(&test_osc_1900); }
        }
        else if (cfg == MOD_CFG_INT_SWEEP)
        {
            float p  = (float)(sweep_frame_counter % SWEEP_PERIOD_FRAMES) / (float)SWEEP_PERIOD_FRAMES;
            float hz = SWEEP_START_HZ + p * (SWEEP_END_HZ - SWEEP_START_HZ);
            float inc = TWO_PI * hz / 48000.0f;
            I = SWEEP_AMP * cosf(test_phase_sweep);
            Q = SWEEP_AMP * sinf(test_phase_sweep) * sign;
            test_phase_sweep += inc;
            if (test_phase_sweep > TWO_PI)  test_phase_sweep -= TWO_PI;
            if (i == 0U) sweep_frame_counter++;
        }

        I *= mute; Q *= mute;
        float r_raw, phi;
        extract_mag_phase_runtime(I, Q, &r_raw, &phi);
        if (r_raw > 0.999f) r_raw = 0.999f;
        float r = clampf(r_raw, 0.0f, 0.999f);
        float theta = ssb_theta_command(r);
        float rf_metric = rf_en_metric_from_env_theta(r, theta);
        if (rf_metric > frame_rf_peak) frame_rf_peak = rf_metric;
        frame_rf_active = 1U;
        float cal   = calibration_differential_skew();
        float a_delta, c_delta;
        outphase_branch_deltas(theta, cal, &a_delta, &c_delta);
        uint32_t ta, tc;
        build_branch_pair_call(phi + calibration_common_phase(r),
                                        a_delta, c_delta, &ta, &tc);
        route_branch_pair(&out1[i], &out2[i], ta, tc);

        if (i == count - 1U) {
            DIAG_CAPTURE(ta, tc, r_raw, theta, phi);
        }
    }
    uint32_t idle_a, idle_c;
    build_idle_branch_pair(&idle_a, &idle_c);
    for (uint32_t i = count; i < AUDIO_FRAME_SIZE; i++) {
        out1[i] = idle_a;
        out2[i] = idle_c;
    }
    rf_en_commit_frame(frame_rf_peak, frame_rf_active);
}

static inline float debug_internal_mono_sample(mod_config_t cfg, float *sweep_phase)
{
    switch (cfg)
    {
        case MOD_CFG_INT_SSB_1KHZ:
            {
                float x = 0.35f * test_osc_1k.re;
                osc_advance(&test_osc_1k);
                osc_fix(&test_osc_1k);
                return x;
            }
        case MOD_CFG_INT_SSB_2TONE:
            {
                float x = 0.18f * (test_osc_700.re + test_osc_1900.re);
                osc_advance(&test_osc_700);
                osc_advance(&test_osc_1900);
                osc_fix(&test_osc_700);
                osc_fix(&test_osc_1900);
                return x;
            }
        case MOD_CFG_INT_SSB_SWEEP:
        default:
            {
                float p  = (float)(sweep_frame_counter % SWEEP_PERIOD_FRAMES) / (float)SWEEP_PERIOD_FRAMES;
                float hz = SWEEP_START_HZ + p * (SWEEP_END_HZ - SWEEP_START_HZ);
                float inc = TWO_PI * hz / 48000.0f;
                float x = 0.30f * cosf(*sweep_phase);
                *sweep_phase += inc;
                if (*sweep_phase > TWO_PI) *sweep_phase -= TWO_PI;
                return x;
            }
    }
}

/* Minimal SSB isolation renderer: mono test tone -> Weaver analytic ->
 * mag/phase -> outphasing. No AGC, no gate, no speech EQ, no CESSB, no CE,
 * and calibration can be bypassed independently. */
static void render_ssb_debug_internal(const int16_t *mono, uint32_t count, render_cmp_t *out1, render_cmp_t *out2)
{
    (void)mono;
    if (count > AUDIO_FRAME_SIZE) count = AUDIO_FRAME_SIZE;

    float nre = osc_re, nim = osc_im;
    float sign = (active_sideband_enum == SIDEBAND_LSB) ? 1.0f : -1.0f;
    float frame_rf_peak = 0.0f;
    uint8_t frame_rf_active = 0U;

    for (uint32_t i = 0; i < count; i++)
    {
        float x = debug_internal_mono_sample((mod_config_t)active_config, &test_phase_sweep);
        if (i == 0U && ((mod_config_t)active_config == MOD_CFG_INT_SSB_SWEEP)) sweep_frame_counter++;
        x *= mute_gain_step();

        float I = 0.0f, Q = 0.0f;
        if (ssb_backend_is_hilbert(cfg_ssb_analytic_backend)) {
            hilbert_process_sample_runtime(cfg_ssb_analytic_backend, x, &I, &Q);
        } else {
            float mi = x * nre;
            float mq = -x * nim;
            float bi = biquad_chain(weaver_lpf_prod, weaver_i_state, WEAVER_LPF_SECTIONS, mi);
            float bq = biquad_chain(weaver_lpf_prod, weaver_q_state, WEAVER_LPF_SECTIONS, mq);
            I  = WEAVER_GAIN * (bi * nre - bq * nim);
            Q  = WEAVER_GAIN * (bi * nim + bq * nre);
        }

        float r_raw, phi_unscaled;
        extract_mag_phase_runtime(I, Q, &r_raw, &phi_unscaled);
        float phi = ssb_backend_is_hilbert(cfg_ssb_analytic_backend) ? phi_unscaled : (sign * phi_unscaled);
        float r = clampf(r_raw, 0.0f, 0.999f);

        if (r < 0.0008f) {
            ssb_note_low_env_inhibit();
            out1[i] = null_compare_tick_call();
            out2[i] = null_compare_tick_call();
            rf_en_track_ssb_call(0.0f, &frame_rf_peak, &frame_rf_active);
        } else {
            float theta = ssb_theta_command(r);
            float rf_metric = rf_en_metric_from_env_theta(r, theta);
            rf_en_track_ssb_call(rf_metric, &frame_rf_peak, &frame_rf_active);
            float cal   = calibration_differential_skew();
            float a_delta, c_delta;
            outphase_branch_deltas(theta, cal, &a_delta, &c_delta);
            uint32_t ta, tc;
            build_branch_pair_call(phi + calibration_common_phase(r),
                                            a_delta, c_delta, &ta, &tc);
            route_branch_pair(&out1[i], &out2[i], ta, tc);
            if (i == count - 1U) {
                DIAG_CAPTURE(ta, tc, r_raw, theta, phi);
            }
        }

        float nr = nre * osc_step_re - nim * osc_step_im;
        float ni = nim * osc_step_re + nre * osc_step_im;
        nre = nr; nim = ni;
        if ((i & 7U) == 7U) osc_renorm(&nre, &nim);
    }

    osc_renorm(&nre, &nim);
    osc_re = nre; osc_im = nim;
    for (uint32_t i = count; i < AUDIO_FRAME_SIZE; i++) {
        out1[i] = null_compare_tick_call();
        out2[i] = null_compare_tick_call();
    }
    rf_en_commit_frame(frame_rf_peak, frame_rf_active);
}

static void render_theta_debug(const int16_t *mono, uint32_t count, render_cmp_t *out1, render_cmp_t *out2)
{
    (void)mono;
    if (count > AUDIO_FRAME_SIZE) count = AUDIO_FRAME_SIZE;
    float frame_rf_peak = 0.0f;
    uint8_t frame_rf_active = 0U;

    for (uint32_t i = 0; i < count; i++)
    {
        float mute = mute_gain_step();
        float p = (float)((sweep_frame_counter + i) % SWEEP_PERIOD_FRAMES) / (float)SWEEP_PERIOD_FRAMES;
        float tri = (p < 0.5f) ? (p * 2.0f) : ((1.0f - p) * 2.0f);
        float theta = 0.5f * tri;
        theta = 0.5f - mute * (0.5f - theta);
        theta = mod_outphase_limit_theta_for_mode(MOD_TYPE_TEST, theta);
        float env = rf_output_metric_from_theta(theta);
        if (env > frame_rf_peak) frame_rf_peak = env;
        if (count > 0U) frame_rf_active = 1U;
        float a_delta, c_delta;
        outphase_branch_deltas(theta, 0.0f, &a_delta, &c_delta);
        uint32_t uta, utc;
        build_branch_pair_call(0.0f, a_delta, c_delta, &uta, &utc);
        route_branch_pair(&out1[i], &out2[i], uta, utc);
        if (i == count - 1U) {
            DIAG_CAPTURE(uta, utc, env, theta, 0.0f);
        }
    }
    sweep_frame_counter++;
    for (uint32_t i = count; i < AUDIO_FRAME_SIZE; i++) {
        out1[i] = null_compare_tick_call();
        out2[i] = null_compare_tick_call();
    }
    rf_en_commit_frame(frame_rf_peak, frame_rf_active);
}

/* ═══════════════════════════════════════════════════════════
 *  RENDER: RAW LEGACY SSB
 *
 *  Purpose: keep a simple compatibility Weaver path available as an explicit
 *  comparison mode without regressing the newer renderers. This path keeps
 *  only the parts that remain broadly useful today:
 *    - mute ramp
 *    - current outphasing law selection
 *    - current branch-routing / calibration infrastructure
 *
 *  It deliberately avoids the newer stateful speech processing, gate-fade
 *  logic, and low-envelope phase blending that have correlated with the
 *  grainy SSB reports.
 * ═══════════════════════════════════════════════════════════ */

HOT_RENDER static void render_ssb_raw_legacy(const int16_t *mono, uint32_t count, render_cmp_t *out1, render_cmp_t *out2)
{
    if (count > AUDIO_FRAME_SIZE) count = AUDIO_FRAME_SIZE;
    if ((!active_internal_source) && (mono == NULL)) { fill_null_frame(out1, out2); return; }

    float nre = osc_re, nim = osc_im;
    float frame_rf_peak = 0.0f;
    uint8_t frame_rf_active = 0U;
    float raw_gate = (cfg_gate > cfg_ssb_low_cut) ? cfg_gate : cfg_ssb_low_cut;
    cessb_frame_reset();

    for (uint32_t i = 0; i < count; i++)
    {
        float x = active_internal_source ? 0.0f : ((float)mono[i] / 32768.0f) * active_sample_drive;

        float I = 0.0f, Q = 0.0f;
        if (ssb_backend_is_hilbert(cfg_ssb_analytic_backend)) {
            hilbert_process_sample_runtime(cfg_ssb_analytic_backend, x, &I, &Q);
        } else {
            float mi = x * nre, mq = -x * nim;
            float bi = biquad_chain(weaver_lpf_raw_legacy, weaver_i_state, WEAVER_LPF_SECTIONS, mi);
            float bq = biquad_chain(weaver_lpf_raw_legacy, weaver_q_state, WEAVER_LPF_SECTIONS, mq);
            I = WEAVER_GAIN * (bi * nre - bq * nim);
            Q = WEAVER_GAIN * (bi * nim + bq * nre);
        }

        float r_raw, phi_raw;
        extract_mag_phase_runtime(I, Q, &r_raw, &phi_raw);
        if (r_raw < raw_gate)
        {
            ssb_note_low_env_inhibit();
            out1[i] = null_compare_tick_call();
            out2[i] = null_compare_tick_call();
            rf_en_track_ssb_call(0.0f, &frame_rf_peak, &frame_rf_active);
            float nr = nre * osc_step_re - nim * osc_step_im;
            float ni = nim * osc_step_re + nre * osc_step_im;
            nre = nr; nim = ni;
            if ((i & 15U) == 15U) osc_renorm(&nre, &nim);
            continue;
        }

        float phi;
        if (r_raw > RAW_LEGACY_PHASE_HOLD_FLOOR) {
            phi = ssb_backend_is_hilbert(cfg_ssb_analytic_backend) ? phi_raw : (active_sideband_sign * phi_raw);
            raw_legacy_last_phi = phi;
        } else {
            phi = raw_legacy_last_phi;
        }

        float env_for_theta = ssb_theta_env_map_legacy_raw(r_raw);
        if (env_for_theta <= 0.0f) {
            ssb_note_low_env_inhibit();
            out1[i] = null_compare_tick_call();
            out2[i] = null_compare_tick_call();
            rf_en_track_ssb_call(0.0f, &frame_rf_peak, &frame_rf_active);
            float nr = nre * osc_step_re - nim * osc_step_im;
            float ni = nim * osc_step_re + nre * osc_step_im;
            nre = nr; nim = ni;
            if ((i & 15U) == 15U) osc_renorm(&nre, &nim);
            continue;
        }

        float theta = ssb_theta_command(env_for_theta);
        float rf_metric = rf_en_metric_from_env_theta(env_for_theta, theta);
        rf_en_track_ssb_call(rf_metric, &frame_rf_peak, &frame_rf_active);
        float cal = calibration_differential_skew();
        float a_delta, c_delta;
        outphase_branch_deltas(theta, cal, &a_delta, &c_delta);
        uint32_t ta, tc;
        build_branch_pair_call(phi + calibration_common_phase(env_for_theta),
                                        a_delta, c_delta, &ta, &tc);
        route_branch_pair(&out1[i], &out2[i], ta, tc);

        if (i == count - 1U) {
            DIAG_CAPTURE(ta, tc, r_raw, theta, phi);
        }

        float nr = nre * osc_step_re - nim * osc_step_im;
        float ni = nim * osc_step_re + nre * osc_step_im;
        nre = nr; nim = ni;
        if ((i & 15U) == 15U) osc_renorm(&nre, &nim);
    }

    osc_renorm(&nre, &nim);
    osc_re = nre; osc_im = nim;
    for (uint32_t i = count; i < AUDIO_FRAME_SIZE; i++) {
        out1[i] = null_compare_tick_call();
        out2[i] = null_compare_tick_call();
    }
    cessb_frame_finalize();
    rf_en_commit_frame(frame_rf_peak, frame_rf_active);
}
#endif /* MOD_DEEP_DIAG_ENABLE: internal test signals and RAW LEG */

/* ═══════════════════════════════════════════════════════════
 *  RENDER: Production SSB (FROZEN — empirical baseline)
 * ═══════════════════════════════════════════════════════════
 *
 * The production renderer runs each stage over the whole frame rather than
 * the whole chain once a sample. A stage's state and the frame's settings
 * stay in registers: per sample, every float store to filter state made the
 * compiler reload the settings (a float store may alias any float setting)
 * and the carrier geometry (a compare store may alias period_ticks), and the
 * branch law went through calls into mod_outphase.c. USB-audio SSB needed
 * about 115k cycles a frame for this, which left no margin at the WIDE
 * clocks or with TX PHASE 384K (docs/REV1.md).
 *
 * Every stage keeps the per-sample helpers' arithmetic in the same order, so
 * the compares, gates, counters and telemetry are bit-exact with the
 * per-sample chain (host harness tools/bench/ssb_equiv, 40k frames across
 * carriers, sidebands, bandwidths, speech tools, laws, math modes and
 * calibration). Gain telemetry is published once a frame instead of once a
 * sample: nothing reads it while a frame renders. */

/* One section over a frame, state and coefficients in registers. Out of
 * line: the renderer runs up to fourteen sections a frame, and a call each
 * costs nothing next to 48 samples. O2, not O3: the loop is a recurrence, so
 * O3's unrolling bought nothing on the bench (worst case 89.4-89.7 % either
 * way) for 606 more bytes. */
static __attribute__((noinline, optimize("O2"), hot)) void biquad_block(
    const BiquadCoeffs *c, BiquadState *s, float *x, uint32_t n)
{
    const float b0 = c->b0, b1 = c->b1, b2 = c->b2, a1 = c->a1, a2 = c->a2;
    float z1 = s->z1, z2 = s->z2;
    for (uint32_t i = 0U; i < n; i++) {
        const float in = x[i];
        const float y = b0 * in + z1;
        z1 = b1 * in - a1 * y + z2;
        z2 = b2 * in - a2 * y;
        x[i] = y;
    }
    s->z1 = z1;
    s->z2 = z2;
}

/* The compressor's per-frame constants: which law comp_desired_gain_lut or
 * _exact would take, and their operands. comp_plan_gain returns what those
 * return for every abs_x. */
typedef struct {
    const float *lut;
    float inv_step;
    float knee_hi;
    float target;
    float target_eps;
    float exponent;
    uint8_t use_lut;
} comp_plan_t;

static inline void comp_plan_latch(comp_plan_t *p)
{
    p->target = cfg_comp_target;
    p->target_eps = cfg_comp_target + 1.0e-9f;
    p->exponent = -(1.0f - 1.0f / fmaxf(cfg_comp_ratio, 1.0f));
    p->use_lut = 0U;
    p->lut = comp_gain_lut[0];
    p->inv_step = 0.0f;
    p->knee_hi = 0.0f;
    if (cfg_comp_math_mode != COMP_MATH_LUT) return;

    const uint8_t valid = comp_lut_valid;
    const float cached_target = comp_lut_cached_target;
    const float cached_ratio = comp_lut_cached_ratio;
    const uint8_t lut_active = comp_lut_active;
    const float inv_step = comp_lut_inv_step;
    if ((valid == 0U) ||
        (fabsf(cached_target - cfg_comp_target) > 1.0e-6f) ||
        (fabsf(cached_ratio - cfg_comp_ratio) > 1.0e-6f) ||
        (inv_step <= 0.0f)) {
        return;
    }
    p->use_lut = 1U;
    p->lut = comp_gain_lut[lut_active & 1U];
    p->inv_step = inv_step;
    p->knee_hi = cfg_comp_target + (2.0f / inv_step);
}

HOT_INLINE float comp_plan_exact(const comp_plan_t *p, float abs_x)
{
    if (abs_x <= p->target) return 1.0f;
    return powf_pos(abs_x / p->target_eps, p->exponent);
}

HOT_INLINE float comp_plan_gain(const comp_plan_t *p, float abs_x)
{
    if (p->use_lut == 0U) return comp_plan_exact(p, abs_x);
    if (abs_x <= 0.0f) return 1.0f;
    if (abs_x >= COMP_LUT_MAX_ABS) return comp_plan_exact(p, abs_x);
    if (abs_x <= p->knee_hi) return comp_plan_exact(p, abs_x);
    const float idx_f = abs_x * p->inv_step;
    const uint32_t idx = (uint32_t)idx_f;
    const float frac = idx_f - (float)idx;
    if (idx >= (COMP_LUT_SIZE - 1U)) return p->lut[COMP_LUT_SIZE - 1U];
    return p->lut[idx] + (p->lut[idx + 1U] - p->lut[idx]) * frac;
}

/* audio_front_end over a frame: voice gain, DC block, mute fade, mic AGC,
 * noise gate. */
static __attribute__((noinline, optimize("O2"), hot)) void ssb_front_end_block(const int16_t *mono, float *x, uint32_t n,
                                    BiquadState *dc_state)
{
    if (active_internal_source) {
        for (uint32_t i = 0U; i < n; i++) x[i] = 0.0f;
    } else {
        for (uint32_t i = 0U; i < n; i++) x[i] = (float)mono[i] / 32768.0f;
    }

    if (frame_voice_gain_bypass == 0U) {
        const float g = cfg_usb_input_gain;
        for (uint32_t i = 0U; i < n; i++) {
            float v = x[i] * g;
            if (v > 1.0f) v = 1.0f;
            else if (v < -1.0f) v = -1.0f;
            x[i] = v;
        }
    }

    _Static_assert(DC_BLOCK_SECTIONS == 1U, "the DC block runs one section");
    biquad_block(&dc_block[0], dc_state, x, n);

    {
        float mg = mute_gain;
        const float mt = mute_target;
        for (uint32_t i = 0U; i < n; i++) {
            mg = mute_gain_advance(mg, mt);
            x[i] *= mg;
        }
        mute_gain = mg;
    }

    if (n == 0U) return;
    if (mic_agc_bypassed()) {
        mic_agc_gain = 1.0f;
        live_mic_agc_gain_db = 0.0f;
    } else {
        float env = mic_agc_env, gain = mic_agc_gain;
        const float target = cfg_mic_agc_target, max_gain = cfg_mic_agc_max_gain;
        for (uint32_t i = 0U; i < n; i++) {
            x[i] = mic_agc_step(x[i], &env, &gain, target, max_gain);
        }
        mic_agc_env = env;
        mic_agc_gain = gain;
        mic_agc_telemetry();
    }

    if (noise_gate_bypassed()) {
        ng.gain = 1.0f;
    } else {
        noise_gate_state_t g = ng;
        const float thresh = cfg_ng_thresh;
        for (uint32_t i = 0U; i < n; i++) x[i] = noise_gate_step(&g, x[i], thresh);
        ng = g;
        noise_gate_telemetry();
    }
}

/* ssb_speech_postprocess over a frame: compressor, EQ, ALC. */
static __attribute__((noinline, optimize("O2"), hot)) void ssb_speech_block(float *x, uint32_t n)
{
    if (n == 0U) return;
    if (usb_clean_processing_active() || !cfg_comp_enable) {
        comp_gain = 1.0f;
        live_comp_gain_db = 0.0f;
    } else {
        comp_plan_t plan;
        comp_plan_latch(&plan);
        float g = comp_gain;
        for (uint32_t i = 0U; i < n; i++) {
            const float desired_gain = comp_plan_gain(&plan, fabsf(x[i]));
            const float coeff = (desired_gain < g) ? COMP_ATTACK_COEFF : COMP_RELEASE_COEFF;
            g += (desired_gain - g) * coeff;
            g  = clampf(g, 0.20f, 1.0f);
            x[i] *= g;
        }
        comp_gain = g;
        if (cfg_gain_telemetry_mode == GAIN_TELEMETRY_LIVE) {
            live_comp_gain_db = telemetry_gain_to_db(comp_gain);
        } else {
            live_comp_gain_db = 0.0f;
        }
    }

    if (!(usb_clean_processing_active() || !cfg_eq_enable)) {
        biquad_block(&eq_bass_coeffs,   &eq_bass_state,   x, n);
        biquad_block(&eq_treble_coeffs, &eq_treble_state, x, n);
        for (uint32_t i = 0U; i < n; i++) x[i] = soft_clip_audio(x[i]);
    }

    if (alc_bypassed()) {
        alc_state.gain = 1.0f;
        live_alc_gain_db = 0.0f;
    } else {
        alc_block(x, n);
        alc_telemetry();
    }
}

/* The PROD Weaver over a chunk, in place: re[] carries the audio in and I
 * out, im[] receives Q. The oscillator's sequence depends only on the sample
 * index, so it is run twice (mix, then remix) rather than kept in a table;
 * *osc carries it from chunk to chunk. Chunks start at multiples of 8, so the
 * renormalisation falls on the same samples as over a whole frame. */
HOT_INLINE void ssb_weaver_block(float *re, float *im, uint32_t n, ComplexOsc *osc)
{
    const float sre = osc->step_re, sim = osc->step_im;
    float nre = osc->re, nim = osc->im;
    for (uint32_t i = 0U; i < n; i++) {
        const float x = re[i];
        re[i] = x * nre;
        im[i] = -x * nim;
        const float nr = nre * sre - nim * sim;
        const float ni = nim * sre + nre * sim;
        nre = nr; nim = ni;
        if ((i & 7U) == 7U) osc_renorm(&nre, &nim);
    }
    for (uint32_t s = 0U; s < WEAVER_LPF_SECTIONS; s++) {
        biquad_block(&weaver_lpf_prod[s], &weaver_i_state[s], re, n);
        biquad_block(&weaver_lpf_prod[s], &weaver_q_state[s], im, n);
    }
    nre = osc->re; nim = osc->im;
    for (uint32_t i = 0U; i < n; i++) {
        const float bi = re[i], bq = im[i];
        re[i] = WEAVER_GAIN * (bi * nre - bq * nim);
        im[i] = WEAVER_GAIN * (bi * nim + bq * nre);
        const float nr = nre * sre - nim * sim;
        const float ni = nim * sre + nre * sim;
        nre = nr; nim = ni;
        if ((i & 7U) == 7U) osc_renorm(&nre, &nim);
    }
    osc->re = nre;
    osc->im = nim;
}

/* extract_mag_phase_runtime over a frame, in place: re[] becomes the
 * magnitude, im[] the phase in units of pi. The production law (SAFE with the
 * CORDIC) runs inline; the libm and FAST diagnostic laws go through
 * extract_mag_phase_runtime a sample. */
HOT_INLINE void ssb_mag_phase_block(float *re, float *im, uint32_t n)
{
#if USE_CORDIC
    if ((cfg_dsp_math_mode == DSP_MATH_SAFE) && cfg_cordic_enable && cordic_ready) {
        for (uint32_t i = 0U; i < n; i++) {
            float I = re[i], Q = im[i], mag, phi;
            mod_control_finite_pair_or_zero(&I, &Q);
            cordic_phase_mag(I, Q, &phi, &mag);
            re[i] = mag;
            im[i] = phi;
        }
        return;
    }
#endif
    for (uint32_t i = 0U; i < n; i++) {
        float mag, phi;
        extract_mag_phase_runtime(re[i], im[i], &mag, &phi);
        re[i] = mag;
        im[i] = phi;
    }
}

/* Samples per pass through the stages: a multiple of 8 (the Weaver's
 * renormalisation cadence) that keeps the two scratch arrays inside the
 * realtime stack budget (tools/stack_usage_report.py). The diagnostic
 * build's CESSB and capture hooks take more of the frame, and it transmits
 * nothing, so it trades a few passes for 64 bytes. */
#if MOD_DEEP_DIAG_ENABLE
#define SSB_RENDER_CHUNK 8U
#else
#define SSB_RENDER_CHUNK 16U
#endif
_Static_assert((SSB_RENDER_CHUNK % 8U) == 0U, "chunks must keep the Weaver renorm cadence");

/* O2, not HOT_RENDER's O3: O3 unrolls the fixed-length chunk loops into
 * 12 KB of flash for no measurable gain; the filter sections (biquad_block,
 * O2: unrolling a recurrence cost 600 B and no cycles) and the ALC
 * (alc_block, O3) run out of line. */
__attribute__((optimize("O2"), hot)) static void render_ssb_prod(const int16_t *mono, uint32_t count, render_cmp_t *out1, render_cmp_t *out2)
{
    if (count > AUDIO_FRAME_SIZE) count = AUDIO_FRAME_SIZE;
    if ((!active_internal_source) && (mono == NULL)) { fill_null_frame(out1, out2); return; }

    cessb_frame_reset();

    /* The frame's settings, carrier and branch law. */
    const float drive = active_sample_drive;
    const uint8_t speech_tools = ssb_speech_tools_enabled();
    const float gate_threshold = cfg_gate;
    const float sideband_sign = active_sideband_sign;
    const uint8_t cal_on = frame_cal_active;
    const phase_cal_t *cal = active_cal;
    const float skew = cal_on ? cal->differential_skew : 0.0f;
    const float theta_cap = frame_ssb_theta_cap;
    const uint8_t swap = (uint8_t)(cfg_outphase_law == OUTPHASE_LAW_CLASSIC_SWAP);
    const uint8_t metric_is_env = (uint8_t)((cfg_outphase_law == OUTPHASE_LAW_CLASSIC) ||
                                            (cfg_outphase_law == OUTPHASE_LAW_CLASSIC_SWAP));
    const float low_cut = cfg_ssb_low_cut, phase_bias = cfg_ssb_phase_bias;
    const uint8_t ce_enable = cfg_ssb_ce_enable;
    const float ce_mix = cfg_ssb_ce_mix, ce_floor = cfg_ssb_ce_floor, ce_knee = cfg_ssb_ce_knee;
    const uint16_t period = active_carrier->period_ticks;
    const render_cmp_t null_tick = mod_null_compare_tick();
    const uint8_t route_both = (uint8_t)(mod_effective_branch_mode() == BRANCH_MODE_BOTH);
    const uint8_t ac_offset = (uint8_t)(route_both && cal_on && (cfg_branch_ac_offset_ticks != 0));

    ComplexOsc osc = { osc_re, osc_im, osc_step_re, osc_step_im };
    mod_ssb_gate_state_t gate = gate_prod;
    mod_ssb_gate_fade_state_t fade = prod_phase_state;
    float follow = rf_en_follow_metric;
    float frame_rf_peak = 0.0f;
    uint8_t frame_rf_active = 0U;
    uint32_t inhibited = 0U, limited = 0U;

    for (uint32_t base = 0U; base < count; base += SSB_RENDER_CHUNK) {
        const uint32_t n = ((count - base) < SSB_RENDER_CHUNK) ? (count - base) : SSB_RENDER_CHUNK;
        /* In place through the chain: audio, then I and Q, then magnitude
         * and phase. */
        float re[SSB_RENDER_CHUNK], im[SSB_RENDER_CHUNK];

        /* Shared speech front-end: DC-block, mute fade, mic AGC, noise gate. */
        ssb_front_end_block((mono != NULL) ? (mono + base) : NULL, re, n, &dc_block_prod_state);
        for (uint32_t i = 0U; i < n; i++) re[i] *= drive;
        if (frame_tx_bw_bypass == 0U) {
            biquad_block(&frame_tx_bw_hpf[0], &txbw_hpf_state[0], re, n);
            biquad_block(&frame_tx_bw_lpf[0], &txbw_lpf_state[0], re, n);
            if (frame_tx_bw_lpf_two != 0U) {
                biquad_block(&frame_tx_bw_lpf[1], &txbw_lpf_state[1], re, n);
            }
        }
        if (speech_tools) {
            ssb_speech_block(re, n);
        }

        ssb_weaver_block(re, im, n, &osc);
#if MOD_CESSB_EXPERIMENT_ENABLE
        for (uint32_t i = 0U; i < n; i++) {
            ssb_apply_optional_cessb(&re[i], &im[i], weaver_lpf_prod,
                                     cessb_i_state, cessb_q_state, WEAVER_LPF_SECTIONS);
        }
#endif
        /* Keep the PROD baseline phase path conservative: CORDIC when ON,
         * exact atan2f/sqrtf when OFF. */
        ssb_mag_phase_block(re, im, n);

        for (uint32_t k = 0U; k < n; k++)
        {
            const uint32_t i = base + k;
            const float r_raw = re[k];
            const float phi_unscaled = im[k];

            /* PROD gate: use hysteretic gate_process for open/close decision,
             * but keep the smooth gate_fade amplitude ramp on close so there
             * is no hard click at syllable endings. gate_process supplies hold
             * time + hysteresis; gate_fade supplies the actual amplitude taper. */
            uint8_t rendering = mod_ssb_gate_process(&gate, r_raw, gate_threshold);
            float r = clampf(r_raw, 0.0f, 0.999f);
            float r_use = mod_ssb_gate_fade_update(&fade, rendering, r);
            uint8_t good_phase = (r_raw > PHASE_HOLD_FLOOR);
            float phi_live = sideband_sign * phi_unscaled;
            float phi = mod_ssb_phase_stabilize_low_env(r_raw, gate.last_phi, phi_live);
            mod_ssb_gate_update_phi(&gate, phi, good_phase);
            float phi_use = good_phase ? phi : gate.last_phi;

            /* Below the null envelope, or mapped to nothing: inhibit. */
            float env_for_theta = 0.0f;
            if (!(r_use < MOD_SSB_GATE_NULL_ENV)) {
                env_for_theta = mod_ssb_theta_envelope_map(r_use, low_cut, phase_bias, ce_enable,
                                                           ce_mix, ce_floor, ce_knee);
            }
            float rf_metric = 0.0f;
            if (env_for_theta <= 0.0f) {
                inhibited++;
                out1[i] = null_tick;
                out2[i] = null_tick;
            } else {
                float env_theta = clampf(env_for_theta, 0.0f, 0.999f);

                /* ssb_theta_command */
                const float env = clampf(sanitize_nan_default(env_theta, 0.0f), 0.0f, 0.999f);
                float theta = ssb_limit_theta_counted(outphase_theta_lookup(env), theta_cap, &limited);
                theta += cal_on ? mod_calibration_interp(theta_linearity_pd, env_theta) : 0.0f;
                theta = ssb_limit_theta_counted(theta, theta_cap, &limited);

                rf_metric = metric_is_env ? clampf(env_theta, 0.0f, 1.0f)
                                          : rf_output_metric_from_theta(theta);

                const mod_outphase_pair_t pair = mod_outphase_branch_pair_pi(theta, skew, swap);
                const float common = phi_use + (cal_on ? mod_calibration_interp(cal->common_phase_pd, env_theta)
                                                       : 0.0f);
                uint32_t ta = phase_to_ticks_period(period, common + pair.branch_a_pi);
                uint32_t tc = phase_to_ticks_period(period, common + pair.branch_c_pi);
                if (ac_offset) apply_branch_ac_offset(&ta, &tc);
                if (route_both) {
                    out1[i] = (render_cmp_t)ta;
                    out2[i] = (render_cmp_t)tc;
                } else {
                    route_branch_pair(&out1[i], &out2[i], ta, tc);
                }

                if (i == count - 1U) {
                    DIAG_CAPTURE(ta, tc, r_raw, theta, phi_use);
                }
            }
            rf_en_track_step(&follow, rf_metric, &frame_rf_peak, &frame_rf_active);
        }
    }

    osc_renorm(&osc.re, &osc.im);
    osc_re = osc.re; osc_im = osc.im;
    gate_prod = gate;
    prod_phase_state = fade;
    rf_en_follow_metric = follow;
    if (inhibited != 0U) low_env_inhibit_count += inhibited;
    ssb_publish_theta_limits(limited);

    for (uint32_t i = count; i < AUDIO_FRAME_SIZE; i++) {
        out1[i] = null_tick;
        out2[i] = null_tick;
    }
    cessb_frame_finalize();
    rf_en_commit_frame(frame_rf_peak, frame_rf_active);
}

#if MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE
/* The H12P core over a chunk, in place (re[] in, I out; im[] gets Q): about
 * a third of the renderer's frame, 26k cycles. Out of line, so the
 * renderer's layout does not move its loops. From CCM it took 25k, but CCM
 * keeps a 4 KB reserve (tools/memory_budget_check.py); its coefficients in
 * SRAM rather than flash save 4k (bench, 2026-10-03). */
static __attribute__((noinline, optimize("O2"), hot)) void h12p_analytic_chunk(float *re, float *im,
                                                                             uint32_t n,
                                                                             float sideband_sign)
{
    mod_analytic12_block(&hilbert_workspace.production12, &analytic12_coeffs, re, re, im, n,
                         sideband_sign);
}

/* ═══════════════ RENDER: SSB through the H12P analytic core ═══════════════
 *
 * render_ssb_prod's frame form with the 48 -> 12 -> 48 ksps Hilbert core
 * (mod_analytic12_block) in place of the Weaver. H12P used to run through
 * render_ssb_ext, the whole chain once a sample, the core's circular
 * indexing and the out-of-line helpers included: ~177k cycles a frame, more
 * than a 1 ms frame has, so every USB-audio over emptied the render queue
 * ~0.08 s in and the underrun latched FAULT_DMA and a hard lockout (bench,
 * 2026-10-03). This form takes ~75k at the default settings and
 * keeps up at the worst case (WIDE CLOCK, TX PHASE 384K, every speech tool)
 * at 164 MHz.
 *
 * The stages keep that renderer's order and state -- the speech tools before
 * the TX bandwidth filter, the sideband in the Hilbert pair rather than in
 * the phase, its own DC block, gate and fade -- and its arithmetic, so the
 * compares, counters and telemetry are bit-exact with it (host harness
 * tools/bench/ssb_equiv, both backends). The per-sample back end is
 * render_ssb_prod's. */
__attribute__((optimize("O2"), hot)) static void render_ssb_h12p(const int16_t *mono, uint32_t count,
                                                                 render_cmp_t *out1, render_cmp_t *out2)
{
    if (count > AUDIO_FRAME_SIZE) count = AUDIO_FRAME_SIZE;
    if (mono == NULL) { fill_null_frame(out1, out2); return; }

    cessb_frame_reset();

    /* The frame's settings, carrier and branch law. */
    const float drive = active_sample_drive;
    const uint8_t speech_tools = ssb_speech_tools_enabled();
    const float gate_threshold = cfg_gate;
    const float sideband_sign = active_sideband_sign;
    const uint8_t cal_on = frame_cal_active;
    const phase_cal_t *cal = active_cal;
    const float skew = cal_on ? cal->differential_skew : 0.0f;
    const float theta_cap = frame_ssb_theta_cap;
    const uint8_t swap = (uint8_t)(cfg_outphase_law == OUTPHASE_LAW_CLASSIC_SWAP);
    const uint8_t metric_is_env = (uint8_t)((cfg_outphase_law == OUTPHASE_LAW_CLASSIC) ||
                                            (cfg_outphase_law == OUTPHASE_LAW_CLASSIC_SWAP));
    const float low_cut = cfg_ssb_low_cut, phase_bias = cfg_ssb_phase_bias;
    const uint8_t ce_enable = cfg_ssb_ce_enable;
    const float ce_mix = cfg_ssb_ce_mix, ce_floor = cfg_ssb_ce_floor, ce_knee = cfg_ssb_ce_knee;
    const uint16_t period = active_carrier->period_ticks;
    const render_cmp_t null_tick = mod_null_compare_tick();
    const uint8_t route_both = (uint8_t)(mod_effective_branch_mode() == BRANCH_MODE_BOTH);
    const uint8_t ac_offset = (uint8_t)(route_both && cal_on && (cfg_branch_ac_offset_ticks != 0));

    mod_ssb_gate_state_t gate = gate_ext;
    mod_ssb_gate_fade_state_t fade = ext_phase_state;
    float follow = rf_en_follow_metric;
    float frame_rf_peak = 0.0f;
    uint8_t frame_rf_active = 0U;
    uint32_t inhibited = 0U, limited = 0U;

    for (uint32_t base = 0U; base < count; base += SSB_RENDER_CHUNK) {
        const uint32_t n = ((count - base) < SSB_RENDER_CHUNK) ? (count - base) : SSB_RENDER_CHUNK;
        /* In place through the chain: audio, then I and Q, then magnitude
         * and phase. */
        float re[SSB_RENDER_CHUNK], im[SSB_RENDER_CHUNK];

        ssb_front_end_block(mono + base, re, n, &dc_block_ext_state);
        for (uint32_t i = 0U; i < n; i++) re[i] *= drive;
        if (speech_tools) {
            ssb_speech_block(re, n);
        }
        if (frame_tx_bw_bypass == 0U) {
            biquad_block(&frame_tx_bw_hpf[0], &txbw_hpf_state[0], re, n);
            biquad_block(&frame_tx_bw_lpf[0], &txbw_lpf_state[0], re, n);
            if (frame_tx_bw_lpf_two != 0U) {
                biquad_block(&frame_tx_bw_lpf[1], &txbw_lpf_state[1], re, n);
            }
        }

        h12p_analytic_chunk(re, im, n, sideband_sign);
#if MOD_CESSB_EXPERIMENT_ENABLE
        for (uint32_t i = 0U; i < n; i++) {
            ssb_apply_optional_cessb(&re[i], &im[i], weaver_lpf_enh,
                                     cessb_i_state, cessb_q_state, WEAVER_ENH_LPF_SECTIONS);
        }
#endif
        ssb_mag_phase_block(re, im, n);

        for (uint32_t k = 0U; k < n; k++)
        {
            const uint32_t i = base + k;
            const float r_raw = re[k];
            const float phi_live = im[k];   /* the sideband is in the Hilbert pair */

            uint8_t rendering = mod_ssb_gate_process(&gate, r_raw, gate_threshold);
            float r = clampf(r_raw, 0.0f, 0.999f);
            float r_use = mod_ssb_gate_fade_update(&fade, rendering, r);
            uint8_t good_phase = (r_raw > PHASE_HOLD_FLOOR);
            float phi = mod_ssb_phase_stabilize_low_env(r_raw, gate.last_phi, phi_live);
            mod_ssb_gate_update_phi(&gate, phi, good_phase);
            float phi_use = good_phase ? phi : gate.last_phi;

            /* Below the null envelope, or mapped to nothing: inhibit. */
            float env_for_theta = 0.0f;
            if (!(r_use < MOD_SSB_GATE_NULL_ENV)) {
                env_for_theta = mod_ssb_theta_envelope_map(r_use, low_cut, phase_bias, ce_enable,
                                                           ce_mix, ce_floor, ce_knee);
            }
            float rf_metric = 0.0f;
            if (env_for_theta <= 0.0f) {
                inhibited++;
                out1[i] = null_tick;
                out2[i] = null_tick;
            } else {
                float env_theta = clampf(env_for_theta, 0.0f, 0.999f);

                /* ssb_theta_command */
                const float env = clampf(sanitize_nan_default(env_theta, 0.0f), 0.0f, 0.999f);
                float theta = ssb_limit_theta_counted(outphase_theta_lookup(env), theta_cap, &limited);
                theta += cal_on ? mod_calibration_interp(theta_linearity_pd, env_theta) : 0.0f;
                theta = ssb_limit_theta_counted(theta, theta_cap, &limited);

                rf_metric = metric_is_env ? clampf(env_theta, 0.0f, 1.0f)
                                          : rf_output_metric_from_theta(theta);

                const mod_outphase_pair_t pair = mod_outphase_branch_pair_pi(theta, skew, swap);
                const float common = phi_use + (cal_on ? mod_calibration_interp(cal->common_phase_pd, env_theta)
                                                       : 0.0f);
                uint32_t ta = phase_to_ticks_period(period, common + pair.branch_a_pi);
                uint32_t tc = phase_to_ticks_period(period, common + pair.branch_c_pi);
                if (ac_offset) apply_branch_ac_offset(&ta, &tc);
                if (route_both) {
                    out1[i] = (render_cmp_t)ta;
                    out2[i] = (render_cmp_t)tc;
                } else {
                    route_branch_pair(&out1[i], &out2[i], ta, tc);
                }

                if (i == count - 1U) {
                    DIAG_CAPTURE(ta, tc, r_raw, theta, phi_use);
                }
            }
            rf_en_track_step(&follow, rf_metric, &frame_rf_peak, &frame_rf_active);
        }
    }

    gate_ext = gate;
    ext_phase_state = fade;
    rf_en_follow_metric = follow;
    if (inhibited != 0U) low_env_inhibit_count += inhibited;
    ssb_publish_theta_limits(limited);

    for (uint32_t i = count; i < AUDIO_FRAME_SIZE; i++) {
        out1[i] = null_tick;
        out2[i] = null_tick;
    }
    cessb_frame_finalize();
    rf_en_commit_frame(frame_rf_peak, frame_rf_active);
}
#endif

/* ═══════════════ RENDER: DIGI TONE (QMX's way) ═══════════════ */

/* DIGI CLEAN from the USB sound card transmitted the way QMX transmits it:
 * the tone is measured, not modulated (mod_digi_tone_logic.h), and sent as a
 * constant-envelope carrier at full drive whatever the PC's level. It stands
 * in for PROD (Weaver), which keeps nothing in hilbert_workspace. */
static inline uint8_t mod_digi_tone_selected(void)
{
    return (uint8_t)((cfg_digi_tone != 0U) && (active_render_fn == render_ssb_prod) &&
                     (active_audio_source == MOD_AUDIO_SOURCE_USB) &&
                     (usb_clean_processing_compute() != 0U));
}

/* The DIGI TONE carrier's offset from the dial while it is the renderer in
 * use (0 Hz otherwise or before a tone), for the console. */
__attribute__((cold)) float MOD_DigiToneHz(void)
{
    if (!mod_digi_tone_selected()) return 0.0f;
    return mod_digi_tone_hz(&hilbert_workspace.tone);
}

/* PROD's per-sample tail (theta, calibration, branch ticks) for a constant
 * envelope: no gate, no low-envelope phase hold, the phase is always good.
 * A new stream (pcm_reset_seq) starts from idle. Os, 2.2 KB where O2 took
 * 3.4 KB: on the bench (FT8 into a TX-blocked dial, DWT PC samples) it and
 * its helpers held 26 % of the CPU where PROD held 38 %. */
__attribute__((noinline, optimize("Os"), hot)) static void render_digi_tone(const int16_t *mono, uint32_t count,
                                                               render_cmp_t *out1, render_cmp_t *out2)
{
    if (count > AUDIO_FRAME_SIZE) count = AUDIO_FRAME_SIZE;
    if (mono == NULL) { fill_null_frame(out1, out2); return; }

    mod_digi_tone_state_t *s = &hilbert_workspace.tone;
    const uint32_t seq = pcm_reset_seq;
    if (s->seq != seq) {
        memset(s, 0, sizeof(*s));
        s->seq = seq;
    }

    const float drive = active_sample_drive;
    const float sideband_sign = active_sideband_sign;
    const uint8_t cal_on = frame_cal_active;
    const phase_cal_t *cal = active_cal;
    const float skew = cal_on ? cal->differential_skew : 0.0f;
    const float theta_cap = frame_ssb_theta_cap;
    const uint8_t swap = (uint8_t)(cfg_outphase_law == OUTPHASE_LAW_CLASSIC_SWAP);
    const uint8_t metric_is_env = (uint8_t)((cfg_outphase_law == OUTPHASE_LAW_CLASSIC) ||
                                            (cfg_outphase_law == OUTPHASE_LAW_CLASSIC_SWAP));
    const float low_cut = cfg_ssb_low_cut, phase_bias = cfg_ssb_phase_bias;
    const uint8_t ce_enable = cfg_ssb_ce_enable;
    const float ce_mix = cfg_ssb_ce_mix, ce_floor = cfg_ssb_ce_floor, ce_knee = cfg_ssb_ce_knee;
    const uint16_t period = active_carrier->period_ticks;
    const render_cmp_t null_tick = mod_null_compare_tick();
    const uint8_t route_both = (uint8_t)(mod_effective_branch_mode() == BRANCH_MODE_BOTH);
    const uint8_t ac_offset = (uint8_t)(route_both && cal_on && (cfg_branch_ac_offset_ticks != 0));

    float follow = rf_en_follow_metric;
    float frame_rf_peak = 0.0f;
    uint8_t frame_rf_active = 0U;
    uint32_t inhibited = 0U, limited = 0U;

    for (uint32_t base = 0U; base < count; base += SSB_RENDER_CHUNK) {
        const uint32_t n = ((count - base) < SSB_RENDER_CHUNK) ? (count - base) : SSB_RENDER_CHUNK;
        float x[SSB_RENDER_CHUNK];

        /* DC block and the mute fade: a muted carrier falls like a tone
         * that stops. */
        ssb_front_end_block(mono + base, x, n, &dc_block_prod_state);

        for (uint32_t k = 0U; k < n; k++) {
            const uint32_t i = base + k;
            mod_digi_tone_sample(s, x[k]);
            const float r = clampf(drive * mod_digi_tone_envelope(s), 0.0f, 0.999f);

            float env_for_theta = 0.0f;
            if (!(r < MOD_SSB_GATE_NULL_ENV)) {
                env_for_theta = mod_ssb_theta_envelope_map(r, low_cut, phase_bias, ce_enable,
                                                           ce_mix, ce_floor, ce_knee);
            }
            float rf_metric = 0.0f;
            if (env_for_theta <= 0.0f) {
                inhibited++;
                out1[i] = null_tick;
                out2[i] = null_tick;
            } else {
                const float env = clampf(env_for_theta, 0.0f, 0.999f);
                float theta = ssb_limit_theta_counted(outphase_theta_lookup(env), theta_cap, &limited);
                theta += cal_on ? mod_calibration_interp(theta_linearity_pd, env) : 0.0f;
                theta = ssb_limit_theta_counted(theta, theta_cap, &limited);

                rf_metric = metric_is_env ? env : rf_output_metric_from_theta(theta);

                const mod_outphase_pair_t pair = mod_outphase_branch_pair_pi(theta, skew, swap);
                const float common = sideband_sign * s->phase +
                                     (cal_on ? mod_calibration_interp(cal->common_phase_pd, env) : 0.0f);
                uint32_t ta = phase_to_ticks_period(period, common + pair.branch_a_pi);
                uint32_t tc = phase_to_ticks_period(period, common + pair.branch_c_pi);
                if (ac_offset) apply_branch_ac_offset(&ta, &tc);
                if (route_both) {
                    out1[i] = (render_cmp_t)ta;
                    out2[i] = (render_cmp_t)tc;
                } else {
                    route_branch_pair(&out1[i], &out2[i], ta, tc);
                }

                if (i == count - 1U) {
                    DIAG_CAPTURE(ta, tc, r, theta, s->phase);
                }
            }
            rf_en_track_step(&follow, rf_metric, &frame_rf_peak, &frame_rf_active);
        }
    }

    rf_en_follow_metric = follow;
    if (inhibited != 0U) low_env_inhibit_count += inhibited;
    ssb_publish_theta_limits(limited);

    for (uint32_t i = count; i < AUDIO_FRAME_SIZE; i++) {
        out1[i] = null_tick;
        out2[i] = null_tick;
    }
    rf_en_commit_frame(frame_rf_peak, frame_rf_active);
}

#if SSB_COMPARE_RENDERERS
/* ═══════════════ RENDER: Enhanced SSB ═══════════════ */

HOT_RENDER static void render_ssb_enh(const int16_t *mono, uint32_t count, render_cmp_t *out1, render_cmp_t *out2)
{
    if (count > AUDIO_FRAME_SIZE) count = AUDIO_FRAME_SIZE;
    if (mono == NULL) { fill_null_frame(out1, out2); return; }

    float nre = osc_enh_re, nim = osc_enh_im;
    float frame_rf_peak = 0.0f;
    uint8_t frame_rf_active = 0U;
    cessb_frame_reset();

    for (uint32_t i = 0; i < count; i++)
    {
        float x = ((float)mono[i] / 32768.0f);

        /* Shared speech front-end: DC-block, mute fade, mic AGC, noise gate. */
        x = audio_front_end(x, &dc_block_enh_state);
        x *= active_sample_drive;

        /* Front-end speech shaping */
        x = biquad_chain(enh_hpf_100hz,  enh_hpf_state, ENH_HPF_SECTIONS, x);
        x = biquad_chain(enh_lpf_3000hz, enh_lpf_state, ENH_LPF_SECTIONS, x);

        if (ssb_speech_tools_enabled()) {
            x = ssb_speech_postprocess(x);
        }

        /* Weaver SSB generation — ENH keeps its own wider LPF shape. */
        float mi = x * nre, mq = -x * nim;
        float bi = biquad_chain(weaver_lpf_enh, weaver_enh_i_state, WEAVER_ENH_LPF_SECTIONS, mi);
        float bq = biquad_chain(weaver_lpf_enh, weaver_enh_q_state, WEAVER_ENH_LPF_SECTIONS, mq);
        float I  = WEAVER_GAIN * (bi * nre - bq * nim);
        float Q  = WEAVER_GAIN * (bi * nim + bq * nre);

        ssb_apply_optional_cessb(&I, &Q,
                                 weaver_lpf_enh,
                                 cessb_i_state,
                                 cessb_q_state,
                                 WEAVER_ENH_LPF_SECTIONS);

        /* CORDIC-accelerated magnitude + phase */
        float r_raw, phi_unscaled;
        extract_mag_phase_runtime(I, Q, &r_raw, &phi_unscaled);
        float phi = active_sideband_sign * phi_unscaled;

        /* Hysteretic gate with phase memory */
        uint8_t rendering = mod_ssb_gate_process(&gate_enh, r_raw, cfg_gate);
        float r = clampf(r_raw, 0.0f, 0.999f);
        float r_use = mod_ssb_gate_fade_update(&enh_phase_state, rendering, r);
        uint8_t good_phase = (r_raw > PHASE_HOLD_FLOOR);
        phi = mod_ssb_phase_stabilize_low_env(r_raw, gate_enh.last_phi, phi);
        mod_ssb_gate_update_phi(&gate_enh, phi, good_phase);
        float phi_use = good_phase ? phi : gate_enh.last_phi;

        if (r_use < MOD_SSB_GATE_NULL_ENV) {
            ssb_note_low_env_inhibit();
            out1[i] = null_compare_tick_call();
            out2[i] = null_compare_tick_call();
            rf_en_track_ssb_call(0.0f, &frame_rf_peak, &frame_rf_active);
            float nr = nre * osc_enh_step_re - nim * osc_enh_step_im;
            float ni = nim * osc_enh_step_re + nre * osc_enh_step_im;
            nre = nr; nim = ni;
            if ((i & 7U) == 7U) osc_renorm(&nre, &nim);
            continue;
        }

        float env_for_theta = ssb_theta_env_map(r_use);
        if (env_for_theta <= 0.0f) {
            ssb_note_low_env_inhibit();
            out1[i] = null_compare_tick_call();
            out2[i] = null_compare_tick_call();
            rf_en_track_ssb_call(0.0f, &frame_rf_peak, &frame_rf_active);
            float nr = nre * osc_enh_step_re - nim * osc_enh_step_im;
            float ni = nim * osc_enh_step_re + nre * osc_enh_step_im;
            nre = nr; nim = ni;
            if ((i & 7U) == 7U) osc_renorm(&nre, &nim);
            continue;
        }
        float env_theta = clampf(env_for_theta, 0.0f, 0.999f);
        float theta = ssb_theta_command(env_theta);
        float rf_metric = rf_en_metric_from_env_theta(env_theta, theta);
        rf_en_track_ssb_call(rf_metric, &frame_rf_peak, &frame_rf_active);
        float cal   = calibration_differential_skew();

        float a_delta, c_delta;
        outphase_branch_deltas(theta, cal, &a_delta, &c_delta);
        uint32_t ta, tc;
        build_branch_pair_call(phi_use + calibration_common_phase(env_theta),
                                        a_delta, c_delta, &ta, &tc);
        route_branch_pair(&out1[i], &out2[i], ta, tc);

        if (i == count - 1U) {
            DIAG_CAPTURE(ta, tc, r_raw, theta, phi_use);
        }

        float nr = nre * osc_enh_step_re - nim * osc_enh_step_im;
        float ni = nim * osc_enh_step_re + nre * osc_enh_step_im;
        nre = nr; nim = ni;
        if ((i & 7U) == 7U) osc_renorm(&nre, &nim);
    }

    osc_renorm(&nre, &nim);
    osc_enh_re = nre; osc_enh_im = nim;
    for (uint32_t i = count; i < AUDIO_FRAME_SIZE; i++) {
        out1[i] = null_compare_tick_call();
        out2[i] = null_compare_tick_call();
    }
    cessb_frame_finalize();
    rf_en_commit_frame(frame_rf_peak, frame_rf_active);
}

/* ═══════════════ RENDER: Filtered SSB (legacy) ═══════════════ */

HOT_RENDER static void render_ssb_filt(const int16_t *mono, uint32_t count, render_cmp_t *out1, render_cmp_t *out2)
{
    if (count > AUDIO_FRAME_SIZE) count = AUDIO_FRAME_SIZE;
    if (mono == NULL) { fill_null_frame(out1, out2); return; }

    float nre = osc_re, nim = osc_im;
    float frame_rf_peak = 0.0f;
    uint8_t frame_rf_active = 0U;
    cessb_frame_reset();
    const BiquadCoeffs *hpf = speech_hpf_std;
    const BiquadCoeffs *lpf = ((mod_config_t)active_config == MOD_CFG_FILTERED_NARROW_14MHZ)
                              ? speech_lpf_narrow : speech_lpf_std;

    for (uint32_t i = 0; i < count; i++)
    {
        float x = ((float)mono[i] / 32768.0f);

        /* Shared speech front-end. */
        x = audio_front_end(x, &dc_block_filt_state);
        x *= active_sample_drive;

        x = biquad_chain(hpf, speech_hpf_state, SPEECH_HPF_SECTIONS, x);
        x = biquad_chain(lpf, speech_lpf_state, SPEECH_LPF_SECTIONS, x);
        if (ssb_speech_tools_enabled()) {
            x = ssb_speech_postprocess(x);
        }

        float mi = x * nre, mq = -x * nim;
        float bi = biquad_chain(weaver_lpf_prod, weaver_filt_i_state, WEAVER_LPF_SECTIONS, mi);
        float bq = biquad_chain(weaver_lpf_prod, weaver_filt_q_state, WEAVER_LPF_SECTIONS, mq);
        float I  = WEAVER_GAIN * (bi * nre - bq * nim);
        float Q  = WEAVER_GAIN * (bi * nim + bq * nre);
        ssb_apply_optional_cessb(&I, &Q,
                                 weaver_lpf_prod,
                                 cessb_i_state,
                                 cessb_q_state,
                                 WEAVER_LPF_SECTIONS);

        float r_raw, phi_unscaled;
        extract_mag_phase_runtime(I, Q, &r_raw, &phi_unscaled);
        float phi = active_sideband_sign * phi_unscaled;

        uint8_t rendering = mod_ssb_gate_process(&gate_filt, r_raw, cfg_gate);
        float r = clampf(r_raw, 0.0f, 0.999f);
        float r_use = mod_ssb_gate_fade_update(&filt_phase_state, rendering, r);
        uint8_t good_phase = (r_raw > PHASE_HOLD_FLOOR);
        phi = mod_ssb_phase_stabilize_low_env(r_raw, gate_filt.last_phi, phi);
        mod_ssb_gate_update_phi(&gate_filt, phi, good_phase);
        float phi_use = good_phase ? phi : gate_filt.last_phi;

        if (r_use < MOD_SSB_GATE_NULL_ENV) {
            ssb_note_low_env_inhibit();
            out1[i] = null_compare_tick_call();
            out2[i] = null_compare_tick_call();
            rf_en_track_ssb_call(0.0f, &frame_rf_peak, &frame_rf_active);
            float nr = nre * osc_step_re - nim * osc_step_im;
            float ni = nim * osc_step_re + nre * osc_step_im;
            nre = nr; nim = ni;
            if ((i & 7U) == 7U) osc_renorm(&nre, &nim);
            continue;
        }

        float env_for_theta = ssb_theta_env_map(r_use);
        if (env_for_theta <= 0.0f) {
            ssb_note_low_env_inhibit();
            out1[i] = null_compare_tick_call();
            out2[i] = null_compare_tick_call();
            rf_en_track_ssb_call(0.0f, &frame_rf_peak, &frame_rf_active);
            float nr = nre * osc_step_re - nim * osc_step_im;
            float ni = nim * osc_step_re + nre * osc_step_im;
            nre = nr; nim = ni;
            if ((i & 7U) == 7U) osc_renorm(&nre, &nim);
            continue;
        }
        float env_theta = clampf(env_for_theta, 0.0f, 0.999f);
        float theta = ssb_theta_command(env_theta);
        float rf_metric = rf_en_metric_from_env_theta(env_theta, theta);
        rf_en_track_ssb_call(rf_metric, &frame_rf_peak, &frame_rf_active);
        float cal   = calibration_differential_skew();

        float a_delta, c_delta;
        outphase_branch_deltas(theta, cal, &a_delta, &c_delta);
        uint32_t ta, tc;
        build_branch_pair_call(phi_use + calibration_common_phase(env_theta),
                                        a_delta, c_delta, &ta, &tc);
        route_branch_pair(&out1[i], &out2[i], ta, tc);

        if (i == count - 1U) {
            DIAG_CAPTURE(ta, tc, r_raw, theta, phi_use);
        }

        float nr = nre * osc_step_re - nim * osc_step_im;
        float ni = nim * osc_step_re + nre * osc_step_im;
        nre = nr; nim = ni;
        if ((i & 7U) == 7U) osc_renorm(&nre, &nim);
    }

    osc_renorm(&nre, &nim);
    osc_re = nre; osc_im = nim;
    for (uint32_t i = count; i < AUDIO_FRAME_SIZE; i++) {
        out1[i] = null_compare_tick_call();
        out2[i] = null_compare_tick_call();
    }
    cessb_frame_finalize();
    rf_en_commit_frame(frame_rf_peak, frame_rf_active);
}

/* ═══════════════ RENDER: Raw Direct SSB (empirical best) ═══════════════ */

HOT_RENDER static void render_ssb_raw_direct(const int16_t *mono, uint32_t count, render_cmp_t *out1, render_cmp_t *out2)
{
    if (count > AUDIO_FRAME_SIZE) count = AUDIO_FRAME_SIZE;
    if (mono == NULL) { fill_null_frame(out1, out2); return; }

    float nre = osc_re, nim = osc_im;
    float frame_rf_peak = 0.0f;
    uint8_t frame_rf_active = 0U;
    cessb_frame_reset();
    for (uint32_t i = 0; i < count; i++)
    {
        float x = ((float)mono[i] / 32768.0f);

        /* RAW DIR still uses the shared front-end for DC blocking and mute
         * fade. AGC/gate stay off by default so CLEAN preserves raw sound. */
        x = audio_front_end(x, &dc_block_raw_state);
        x *= active_sample_drive;
        x = tx_bw_filter_sample(x);

        if (ssb_speech_tools_enabled())
        {
            x = ssb_speech_postprocess(x);
        }

        float mi = x * nre, mq = -x * nim;
        float bi = biquad_chain(weaver_lpf_prod, weaver_i_state, WEAVER_LPF_SECTIONS, mi);
        float bq = biquad_chain(weaver_lpf_prod, weaver_q_state, WEAVER_LPF_SECTIONS, mq);
        float I  = WEAVER_GAIN * (bi * nre - bq * nim);
        float Q  = WEAVER_GAIN * (bi * nim + bq * nre);
        ssb_apply_optional_cessb(&I, &Q,
                                 weaver_lpf_prod,
                                 cessb_i_state,
                                 cessb_q_state,
                                 WEAVER_LPF_SECTIONS);

        float r_raw, phi_unscaled;
        extract_mag_phase_runtime(I, Q, &r_raw, &phi_unscaled);

        /* RAW gate: same hybrid approach as PROD — hysteretic decision,
         * smooth amplitude taper on close. */
        uint8_t rendering = mod_ssb_gate_process(&gate_raw, r_raw, cfg_gate);
        float r = clampf(r_raw, 0.0f, 0.999f);
        float r_use = mod_ssb_gate_fade_update(&raw_phase_state, rendering, r);
        uint8_t good_phase = (r_raw > PHASE_HOLD_FLOOR);
        float phi_live = active_sideband_sign * phi_unscaled;
        float phi = mod_ssb_phase_stabilize_low_env(r_raw, gate_raw.last_phi, phi_live);
        mod_ssb_gate_update_phi(&gate_raw, phi, good_phase);
        float phi_use = good_phase ? phi : gate_raw.last_phi;

        if (r_use < MOD_SSB_GATE_NULL_ENV) {
            ssb_note_low_env_inhibit();
            out1[i] = null_compare_tick_call();
            out2[i] = null_compare_tick_call();
            rf_en_track_ssb_call(0.0f, &frame_rf_peak, &frame_rf_active);
            float nr = nre * osc_step_re - nim * osc_step_im;
            float ni = nim * osc_step_re + nre * osc_step_im;
            nre = nr; nim = ni;
            if ((i & 7U) == 7U) osc_renorm(&nre, &nim);
            continue;
        }
        float env_for_theta = ssb_theta_env_map(r_use);
        if (env_for_theta <= 0.0f) {
            ssb_note_low_env_inhibit();
            out1[i] = null_compare_tick_call();
            out2[i] = null_compare_tick_call();
            rf_en_track_ssb_call(0.0f, &frame_rf_peak, &frame_rf_active);
            float nr = nre * osc_step_re - nim * osc_step_im;
            float ni = nim * osc_step_re + nre * osc_step_im;
            nre = nr; nim = ni;
            if ((i & 7U) == 7U) osc_renorm(&nre, &nim);
            continue;
        }
        float env_theta = clampf(env_for_theta, 0.0f, 0.999f);
        float theta = ssb_theta_command(env_theta);
        float rf_metric = rf_en_metric_from_env_theta(env_theta, theta);
        rf_en_track_ssb_call(rf_metric, &frame_rf_peak, &frame_rf_active);
        float cal   = calibration_differential_skew();

        float a_delta, c_delta;
        outphase_branch_deltas(theta, cal, &a_delta, &c_delta);
        uint32_t ta, tc;
        build_branch_pair_call(phi_use + calibration_common_phase(env_theta),
                                        a_delta, c_delta, &ta, &tc);
        route_branch_pair(&out1[i], &out2[i], ta, tc);

        if (i == count - 1U) {
            DIAG_CAPTURE(ta, tc, r_raw, theta, phi_use);
        }

        float nr = nre * osc_step_re - nim * osc_step_im;
        float ni = nim * osc_step_re + nre * osc_step_im;
        nre = nr; nim = ni;
        if ((i & 7U) == 7U) osc_renorm(&nre, &nim);
    }
    osc_renorm(&nre, &nim);
    osc_re = nre; osc_im = nim;
    for (uint32_t i = count; i < AUDIO_FRAME_SIZE; i++) {
        out1[i] = null_compare_tick_call();
        out2[i] = null_compare_tick_call();
    }
    cessb_frame_finalize();
    rf_en_commit_frame(frame_rf_peak, frame_rf_active);
}

#endif /* SSB_COMPARE_RENDERERS */

/* ═══════════════ RENDER: Extended SSB ═══════════════ */

#if SSB_EXT_ALL_BACKENDS
static __attribute__((noinline, optimize("O3"), hot)) float qmx12_diag_gate_envelope(
    float raw_envelope)
{
    uint8_t rendering = mod_ssb_gate_process(&gate_ext, raw_envelope, cfg_gate);
    float bounded = clampf(raw_envelope, 0.0f, 0.999f);
    return mod_ssb_gate_fade_update(&ext_phase_state, rendering, bounded);
}

static __attribute__((noinline, optimize("O3"), hot)) float qmx12_diag_phase_update(
    float raw_envelope,
    float phase_pi)
{
    uint8_t good_phase = (raw_envelope > PHASE_HOLD_FLOOR);
    float phase = mod_ssb_phase_stabilize_low_env(raw_envelope,
                                                   gate_ext.last_phi,
                                                   phase_pi);
    mod_ssb_gate_update_phi(&gate_ext, phase, good_phase);
    return good_phase ? phase : gate_ext.last_phi;
}

static __attribute__((noinline, optimize("O3"), hot)) void qmx12_diag_finalize_control(
    float control_envelope,
    float live_phase_pi,
    float *phase_pi,
    float *theta_pi)
{
    float env_for_theta = ssb_theta_env_map(control_envelope);
    *theta_pi = (env_for_theta > 0.0f)
              ? ssb_theta_command(clampf(env_for_theta, 0.0f, 0.999f))
              : 0.0f;
    *phase_pi = mod_ssb_phase_wrap_norm(
        live_phase_pi + calibration_common_phase(env_for_theta));
}

static __attribute__((noinline, optimize("O3"), hot)) void qmx12_diag_control_update(
    float i_sig,
    float q_sig,
    float *raw_envelope,
    float *control_envelope,
    float *phase_pi,
    float *theta_pi)
{
    float r_raw = 0.0f;
    float phi_unscaled = 0.0f;
    extract_mag_phase_runtime(i_sig, q_sig, &r_raw, &phi_unscaled);
    float env_ctrl = qmx12_diag_gate_envelope(r_raw);
    float phi_cur = qmx12_diag_phase_update(r_raw, phi_unscaled);

    *raw_envelope = r_raw;
    *control_envelope = env_ctrl;
    qmx12_diag_finalize_control(env_ctrl, phi_cur, phase_pi, theta_pi);
}

static __attribute__((noinline, optimize("O3"), hot)) void qmx12_diag_interpolate(
    uint32_t subphase,
    float control_envelope,
    float phase_pi,
    float theta_pi,
    float *interpolated_envelope,
    float *interpolated_phase,
    float *interpolated_theta)
{
    float weight = qmx12_interp_valid
                 ? ((float)(subphase + 1U) / (float)HILB_PAIR12_DECIM)
                 : 1.0f;
    *interpolated_envelope =
        qmx12_prev_env + (control_envelope - qmx12_prev_env) * weight;
    *interpolated_theta =
        qmx12_prev_theta + (theta_pi - qmx12_prev_theta) * weight;
    *interpolated_phase = qmx12_interp_valid
                        ? mod_ssb_phase_blend_norm(qmx12_prev_phi,
                                                   phase_pi, weight)
                        : phase_pi;
}

HOT_RENDER __attribute__((noinline)) static void render_ssb_qmx12_strict(const int16_t *mono,
                                                                         uint32_t count,
                                                                         render_cmp_t *out1,
                                                                         render_cmp_t *out2)
{
    if (count > AUDIO_FRAME_SIZE) count = AUDIO_FRAME_SIZE;
    if ((mono == NULL) || (count < AUDIO_FRAME_SIZE)) { fill_null_frame(out1, out2); return; }

    float frame_rf_peak = 0.0f;
    uint8_t frame_rf_active = 0U;
    cessb_frame_reset();

    for (uint32_t k = 0U; k < (AUDIO_FRAME_SIZE / HILB_PAIR12_DECIM); k++)
    {
        float acc = 0.0f;
        for (uint32_t j = 0U; j < HILB_PAIR12_DECIM; j++) {
            uint32_t i = k * HILB_PAIR12_DECIM + j;
            float x = ((float)mono[i] / 32768.0f);
            x = audio_front_end(x, &dc_block_ext_state);
            x *= active_sample_drive;
            if (ssb_speech_tools_enabled()) {
                x = ssb_speech_postprocess(x);
            }
            x = qmx12_input_filter_sample(x);
            acc += x;
        }

        float x_low = acc * (1.0f / (float)HILB_PAIR12_DECIM);

        float I = 0.0f, Q = 0.0f;
        hilbert_process_sample_lowrate(&hilbert_workspace.experimental, x_low, &I, &Q);
        ssb_apply_optional_cessb(&I, &Q,
                                 weaver_lpf_enh_12k,
                                 cessb_i_state,
                                 cessb_q_state,
                                 WEAVER_ENH_LPF_SECTIONS);

        float r_raw;
        float env_ctrl;
        float phi_cur;
        float theta_cur;
        qmx12_diag_control_update(I, Q, &r_raw, &env_ctrl, &phi_cur,
                                  &theta_cur);

        for (uint32_t j = 0U; j < HILB_PAIR12_DECIM; j++) {
            uint32_t i = k * HILB_PAIR12_DECIM + j;
            float env_i;
            float theta_i;
            float phi_i;
            qmx12_diag_interpolate(j, env_ctrl, phi_cur, theta_cur,
                                   &env_i, &phi_i, &theta_i);

            float env_theta_i = ssb_theta_env_map(env_i);
            if ((env_i < MOD_SSB_GATE_NULL_ENV) || (env_theta_i <= 0.0f)) {
                ssb_note_low_env_inhibit();
                out1[i] = null_compare_tick_call();
                out2[i] = null_compare_tick_call();
                rf_en_track_ssb_call(0.0f, &frame_rf_peak, &frame_rf_active);
                continue;
            }

            float rf_metric = rf_en_metric_from_env_theta(clampf(env_theta_i, 0.0f, 0.999f), theta_i);
            rf_en_track_ssb_call(rf_metric, &frame_rf_peak, &frame_rf_active);
            float a_delta, c_delta;
            outphase_branch_deltas(theta_i, calibration_differential_skew(),
                                   &a_delta, &c_delta);
            uint32_t ta, tc;
            build_branch_pair_call(phi_i, a_delta, c_delta, &ta, &tc);
            route_branch_pair(&out1[i], &out2[i], ta, tc);

            if (i == AUDIO_FRAME_SIZE - 1U) {
                DIAG_CAPTURE(ta, tc, r_raw, theta_i, phi_i);
            }
        }

        qmx12_prev_phi = phi_cur;
        qmx12_prev_env = env_ctrl;
        qmx12_prev_theta = theta_cur;
        qmx12_interp_valid = 1U;
    }

    cessb_frame_finalize();
    rf_en_commit_frame(frame_rf_peak, frame_rf_active);
}

/* The deep-diagnostic image's extended SSB backends (the Hilbert pairs, the
 * 48 kHz Hilbert, QMX12) and its Weaver comparison, a sample at a time. H12P
 * has render_ssb_h12p; this one runs it only between a switch from another
 * backend and the pipeline reset that changes renderers. */
HOT_RENDER static void render_ssb_ext(const int16_t *mono, uint32_t count, render_cmp_t *out1, render_cmp_t *out2)
{
    if (ssb_strict_qmx12_active() != 0U) {
        render_ssb_qmx12_strict(mono, count, out1, out2);
        return;
    }

    if (count > AUDIO_FRAME_SIZE) count = AUDIO_FRAME_SIZE;
    if (mono == NULL) { fill_null_frame(out1, out2); return; }

    float nre = osc_ext_re, nim = osc_ext_im;
    float frame_rf_peak = 0.0f;
    uint8_t frame_rf_active = 0U;
    cessb_frame_reset();

    for (uint32_t i = 0U; i < count; i++)
    {
        float x = ((float)mono[i] / 32768.0f);
        x = audio_front_end(x, &dc_block_ext_state);
        x *= active_sample_drive;
        if (cfg_ssb_analytic_backend == SSB_ANALYTIC_HILBERT) {
            x = tx_bw_filter_sample(x);
            if (ssb_speech_tools_enabled()) {
                x = ssb_speech_postprocess(x);
            }
        } else if (ssb_backend_is_hilbert(cfg_ssb_analytic_backend)) {
            if (ssb_speech_tools_enabled()) {
                x = ssb_speech_postprocess(x);
            }
        } else {
            x = tx_bw_filter_sample(x);
            if (ssb_speech_tools_enabled()) {
                x = ssb_speech_postprocess(x);
            }
        }

        float I = 0.0f, Q = 0.0f;
        if (ssb_backend_is_hilbert(cfg_ssb_analytic_backend)) {
            hilbert_process_sample_runtime(cfg_ssb_analytic_backend, x, &I, &Q);
        } else {
            float mi = x * nre, mq = -x * nim;
            float bi = biquad_chain(weaver_lpf_enh, weaver_ext_i_state, WEAVER_ENH_LPF_SECTIONS, mi);
            float bq = biquad_chain(weaver_lpf_enh, weaver_ext_q_state, WEAVER_ENH_LPF_SECTIONS, mq);
            I = WEAVER_GAIN * (bi * nre - bq * nim);
            Q = WEAVER_GAIN * (bi * nim + bq * nre);
            float nr = nre * osc_ext_step_re - nim * osc_ext_step_im;
            float ni = nim * osc_ext_step_re + nre * osc_ext_step_im;
            nre = nr; nim = ni;
            if ((i & 7U) == 7U) osc_renorm(&nre, &nim);
        }

        ssb_apply_optional_cessb(&I, &Q,
                                 weaver_lpf_enh,
                                 cessb_i_state,
                                 cessb_q_state,
                                 WEAVER_ENH_LPF_SECTIONS);

        float r_raw, phi_unscaled;
        extract_mag_phase_runtime(I, Q, &r_raw, &phi_unscaled);
        uint8_t rendering = mod_ssb_gate_process(&gate_ext, r_raw, cfg_gate);
        float r = clampf(r_raw, 0.0f, 0.999f);
        float env_ctrl = mod_ssb_gate_fade_update(&ext_phase_state, rendering, r);
        uint8_t good_phase = (r_raw > PHASE_HOLD_FLOOR);
        float phi_live = ssb_backend_is_hilbert(cfg_ssb_analytic_backend)
                       ? phi_unscaled
                       : (active_sideband_sign * phi_unscaled);
        float phi = mod_ssb_phase_stabilize_low_env(r_raw, gate_ext.last_phi, phi_live);
        mod_ssb_gate_update_phi(&gate_ext, phi, good_phase);
        float phi_use = good_phase ? phi : gate_ext.last_phi;

        float env_for_theta = ssb_theta_env_map(env_ctrl);
        if ((env_ctrl < MOD_SSB_GATE_NULL_ENV) || (env_for_theta <= 0.0f)) {
            ssb_note_low_env_inhibit();
            out1[i] = null_compare_tick_call();
            out2[i] = null_compare_tick_call();
            rf_en_track_ssb_call(0.0f, &frame_rf_peak, &frame_rf_active);
            continue;
        }

        float env_theta = clampf(env_for_theta, 0.0f, 0.999f);
        float theta = ssb_theta_command(env_theta);
        float rf_metric = rf_en_metric_from_env_theta(env_theta, theta);
        rf_en_track_ssb_call(rf_metric, &frame_rf_peak, &frame_rf_active);
        float cal = calibration_differential_skew();

        float a_delta, c_delta;
        outphase_branch_deltas(theta, cal, &a_delta, &c_delta);
        uint32_t ta, tc;
        build_branch_pair_call(phi_use + calibration_common_phase(env_theta),
                                        a_delta, c_delta, &ta, &tc);
        route_branch_pair(&out1[i], &out2[i], ta, tc);

        if (i == count - 1U) {
            DIAG_CAPTURE(ta, tc, r_raw, theta, phi_use);
        }
    }

    osc_renorm(&nre, &nim);
    osc_ext_re = nre;
    osc_ext_im = nim;
    for (uint32_t i = count; i < AUDIO_FRAME_SIZE; i++) {
        out1[i] = null_compare_tick_call();
        out2[i] = null_compare_tick_call();
    }
    cessb_frame_finalize();
    rf_en_commit_frame(frame_rf_peak, frame_rf_active);
}
#endif /* SSB_EXT_ALL_BACKENDS */

/* ═══════════════ RENDER: FM ═══════════════ */

HOT_RENDER static void render_fm(const int16_t *mono, uint32_t count, render_cmp_t *out1, render_cmp_t *out2)
{
    if (count > AUDIO_FRAME_SIZE) count = AUDIO_FRAME_SIZE;
    if (mono == NULL) { fill_null_frame(out1, out2); return; }

    const float theta = mod_fixed_theta_for_mode(MOD_TYPE_FM);
    const float frame_rf_peak = rf_output_metric_from_theta(theta);
    float frame_dev_peak = 0.0f;
    uint8_t frame_has_deviation = 0U;

    for (uint32_t i = 0; i < count; i++)
    {
        float x;
        if (cfg_fm_test_tone != 0U) {
            x = TEST_TONE_1_AMP * test_osc_1k.re;
            osc_advance(&test_osc_1k);
            if ((i & 31U) == 31U) osc_fix(&test_osc_1k);
        } else {
            x = ((float)mono[i] / 32768.0f);
            x = biquad_chain(dc_block, dc_block_fm_state, DC_BLOCK_SECTIONS, x);
        }
        x = apply_mute_gain(x);
        x *= cfg_fm_input_gain;
        x = biquad_chain(fm_lpf_3000hz, fm_lpf_state, FM_LPF_SECTIONS, x);

        float xpe = mod_fm_preemphasis_bounded(x, &fm_preemph_state,
                                               AUDIO_RATE_HZ, FM_PREEMPH_TAU,
                                               1.5f);
        if ((xpe > FM_AUDIO_LIMIT) || (xpe < -FM_AUDIO_LIMIT)) {
            fm_limiter_count++;
            xpe = clampf(xpe, -FM_AUDIO_LIMIT, FM_AUDIO_LIMIT);
        }
        float dev_hz = fabsf(xpe) * cfg_fm_dev;
        if (dev_hz > frame_dev_peak) frame_dev_peak = dev_hz;
        if (dev_hz > 1.0f) frame_has_deviation = 1U;

        fm_phase_accum += mod_fm_phase_step_pi(xpe, cfg_fm_dev, AUDIO_RATE_HZ);
        while (fm_phase_accum >  1.0f) fm_phase_accum -= 2.0f;
        while (fm_phase_accum < -1.0f) fm_phase_accum += 2.0f;

        float a_delta, c_delta;
        outphase_branch_deltas(theta, 0.0f, &a_delta, &c_delta);
        uint32_t ta, tc;
        build_branch_pair_from_relative(fm_phase_accum, a_delta, c_delta, &ta, &tc);
        route_branch_pair(&out1[i], &out2[i], ta, tc);

        if (i == count - 1U) {
            DIAG_CAPTURE(ta, tc, 1.0f, theta, fm_phase_accum);
        }
    }

    uint32_t hold_a, hold_c;
    if (count > 0U) {
        hold_a = out1[count - 1U];
        hold_c = out2[count - 1U];
    } else {
        build_idle_branch_pair(&hold_a, &hold_c);
    }
    for (uint32_t i = count; i < AUDIO_FRAME_SIZE; i++) { out1[i] = hold_a; out2[i] = hold_c; }
    {
        uint32_t pm = enter_critical();
        fm_hold_mcmp1 = hold_a;
        fm_hold_mcmp2 = hold_c;
        fm_hold_valid = (count > 0U) ? 1U : 0U;
        exit_critical(pm);
    }
    fm_deviation_peak_hz = frame_dev_peak;
    if (frame_has_deviation == 0U) fm_silent_frame_count++;
    rf_en_commit_frame(frame_rf_peak, (uint8_t)(count > 0U));
}


/* ═══════════════ RENDER: AM ═══════════════ */

HOT_RENDER static void render_am(const int16_t *mono, uint32_t count, render_cmp_t *out1, render_cmp_t *out2)
{
    if (count > AUDIO_FRAME_SIZE) count = AUDIO_FRAME_SIZE;
    if (mono == NULL) { fill_null_frame(out1, out2); return; }

    float frame_rf_peak = 0.0f;
    uint8_t frame_rf_active = 0U;
    float frame_pos_peak = 0.0f;
    float frame_neg_peak = 1.0f;
    /* The AM theta limit, fixed while the frame renders. */
    const float theta_cap = MOD_GetOutphaseEffectiveThetaLimit(MOD_TYPE_AM);

    for (uint32_t i = 0; i < count; i++)
    {
        float x = (float)mono[i] / 32768.0f;
        /* DC-block the raw input, then track the mute envelope separately.
         * audio_front_end() applies mute to the audio signal but cannot
         * reach the carrier term in env — so we capture mute_gain here and
         * apply it to the whole envelope after construction. */
        x = biquad_chain(dc_block, &dc_block_am_state, DC_BLOCK_SECTIONS, x);
        float env_mute = mute_gain_step();   /* advance mute ramp this sample */
        x = mic_agc_process(x);
        x = noise_gate_process(x);
        x = biquad_chain(enh_hpf_100hz,  am_hpf_state, ENH_HPF_SECTIONS, x);
        x = biquad_chain(enh_lpf_3000hz, am_lpf_state, ENH_LPF_SECTIONS, x);
        x = soft_clip_audio(x * AM_AUDIO_GAIN);

        float env = mod_am_envelope_unclamped(x, cfg_am_carrier,
                                              cfg_am_depth, cfg_am_profile);
        env *= env_mute;
        if (env > 0.999f) {
            env = 0.999f;
            am_clamp_count++;
        } else if (env < 0.0f) {
            env = 0.0f;
            am_clamp_count++;
        }
        if (env > frame_pos_peak) frame_pos_peak = env;
        if (env < frame_neg_peak) frame_neg_peak = env;

        uint32_t ta, tc;
        float theta = 0.0f;
        if (env < 0.002f) {
            build_idle_branch_pair(&ta, &tc);
        } else {
            theta = mod_outphase_theta_for_env_cap(env, theta_cap);
            float rf_metric = rf_en_metric_from_env_theta(env, theta);
            if (rf_metric > frame_rf_peak) frame_rf_peak = rf_metric;
            frame_rf_active = 1U;
            float a_delta, c_delta;
            outphase_branch_deltas(theta, 0.0f, &a_delta, &c_delta);
            build_branch_pair_from_relative(0.0f, a_delta, c_delta, &ta, &tc);
        }

        route_branch_pair(&out1[i], &out2[i], ta, tc);

        if (i == count - 1U) {
            DIAG_CAPTURE(ta, tc, env, theta, 0.0f);
        }
    }

    uint32_t a, c;
    if (count > 0U) {
        a = out1[count - 1U];
        c = out2[count - 1U];
    } else {
        build_idle_branch_pair(&a, &c);
    }
    for (uint32_t i = count; i < AUDIO_FRAME_SIZE; i++) { out1[i] = a; out2[i] = c; }
    am_pos_peak = frame_pos_peak;
    am_neg_peak = frame_neg_peak;
    rf_en_commit_frame(frame_rf_peak, frame_rf_active);
}

__attribute__((cold)) void mod_tx_pipeline_reset(void)
{
#if SSB_COMPARE_RENDERERS
    memset(speech_hpf_state,    0, sizeof(speech_hpf_state));
    memset(speech_lpf_state,    0, sizeof(speech_lpf_state));
    memset(enh_hpf_state,       0, sizeof(enh_hpf_state));
    memset(enh_lpf_state,       0, sizeof(enh_lpf_state));
    memset(weaver_filt_i_state, 0, sizeof(weaver_filt_i_state));
    memset(weaver_filt_q_state, 0, sizeof(weaver_filt_q_state));
    memset(weaver_enh_i_state,  0, sizeof(weaver_enh_i_state));
    memset(weaver_enh_q_state,  0, sizeof(weaver_enh_q_state));
#endif
    memset(am_hpf_state,        0, sizeof(am_hpf_state));
    memset(am_lpf_state,        0, sizeof(am_lpf_state));
    memset(weaver_i_state,      0, sizeof(weaver_i_state));
    memset(weaver_q_state,      0, sizeof(weaver_q_state));
#if MOD_CESSB_EXPERIMENT_ENABLE
    memset(cessb_i_state,       0, sizeof(cessb_i_state));
    memset(cessb_q_state,       0, sizeof(cessb_q_state));
#endif
    memset(txbw_hpf_state,      0, sizeof(txbw_hpf_state));
    memset(txbw_lpf_state,      0, sizeof(txbw_lpf_state));
#if SSB_EXT_ALL_BACKENDS
    memset(qmx12_aa_lpf_state,  0, sizeof(qmx12_aa_lpf_state));
    memset(weaver_ext_i_state,  0, sizeof(weaver_ext_i_state));
    memset(weaver_ext_q_state,  0, sizeof(weaver_ext_q_state));
    memset(hilb24_hpf_state,    0, sizeof(hilb24_hpf_state));
    memset(hilb24_lpf_state,    0, sizeof(hilb24_lpf_state));
    memset(hilb12a_hpf_state,   0, sizeof(hilb12a_hpf_state));
    memset(hilb12a_lpf_state,   0, sizeof(hilb12a_lpf_state));
    memset(hilb12b_hpf_state,   0, sizeof(hilb12b_hpf_state));
    memset(hilb12b_lpf_state,   0, sizeof(hilb12b_lpf_state));
    qmx12_interp_valid = 0U;
    qmx12_prev_phi = 0.0f;
    qmx12_prev_env = 0.0f;
    qmx12_prev_theta = 0.0f;
#endif
    memset(&hilbert_workspace, 0, sizeof(hilbert_workspace));
    memset(&eq_bass_state,      0, sizeof(eq_bass_state));
    memset(&eq_treble_state,    0, sizeof(eq_treble_state));

    /* Reset shared DSP state. */
    memset(&dc_block_prod_state, 0, sizeof(dc_block_prod_state));
    memset(&dc_block_ext_state,  0, sizeof(dc_block_ext_state));
    memset(&dc_block_am_state,   0, sizeof(dc_block_am_state));
    memset(&gate_prod, 0, sizeof(gate_prod));
    memset(&gate_ext,  0, sizeof(gate_ext));
    memset(&prod_phase_state, 0, sizeof(prod_phase_state));
#if SSB_COMPARE_RENDERERS
    memset(&dc_block_raw_state,  0, sizeof(dc_block_raw_state));
    memset(&dc_block_enh_state,  0, sizeof(dc_block_enh_state));
    memset(&dc_block_filt_state, 0, sizeof(dc_block_filt_state));
    memset(&gate_raw,  0, sizeof(gate_raw));
    memset(&gate_enh,  0, sizeof(gate_enh));
    memset(&gate_filt, 0, sizeof(gate_filt));
    memset(&raw_phase_state,  0, sizeof(raw_phase_state));
    memset(&enh_phase_state,  0, sizeof(enh_phase_state));
    memset(&filt_phase_state, 0, sizeof(filt_phase_state));
#endif
#if MOD_DEEP_DIAG_ENABLE
    raw_legacy_last_phi = 0.0f;
#endif
    memset(&ext_phase_state,  0, sizeof(ext_phase_state));
    rf_en_reset_tracking();

    /* Mic AGC / noise gate / ALC state */
    mic_agc_env = 0.0f;
    mic_agc_gain = 1.0f;
    ng.open = 0U;
    ng.hang = 0U;
    ng.env = 0.0f;
    ng.gain = 0.0f;          /* gate starts closed — opens on first real audio */
    mod_alc_reset(&alc_state);
    live_comp_gain_db = 0.0f;
    live_alc_gain_db = 0.0f;
    live_mic_agc_gain_db = 0.0f;
    live_gate_active = 0U;

    osc_re     = 1.0f; osc_im     = 0.0f;
#if SSB_COMPARE_RENDERERS
    osc_enh_re = 1.0f; osc_enh_im = 0.0f;
#endif
#if SSB_EXT_ALL_BACKENDS
    osc_ext_re = 1.0f; osc_ext_im = 0.0f;
#endif
    fm_phase_accum   = 0.0f;
    fm_preemph_state = 0.0f;
    fm_hold_valid = 0U;
    memset(dc_block_fm_state,   0, sizeof(dc_block_fm_state));
    memset(fm_lpf_state,        0, sizeof(fm_lpf_state));
#if MOD_DEEP_DIAG_ENABLE
    sweep_frame_counter = 0U;
#endif
    comp_gain     = 1.0f;
    mod_tx_pipeline_comp_lut_sync();
    last_dma_valid = 0U;

    /* Test signal NCOs */
    osc_init(&test_osc_1k,   TEST_TONE_1_HZ);
#if MOD_DEEP_DIAG_ENABLE
    osc_init(&test_osc_700,  TEST_TONE_2A_HZ);
    osc_init(&test_osc_1900, TEST_TONE_2B_HZ);
    test_phase_sweep = 0.0f;
#endif

    tim6_servo_frame_counter = 0U;
    tim6_servo_bias = TRANSPORT_LOCKED_BIAS;
    tim6_servo_error_avg = 0;
    mod_tim6_scheduler_init(MOD_GetTim6InputClockHz(), AUDIO_RATE_HZ_U32);
    tim6_arr_last = mod_tim6_next_base_arr() + (uint32_t)TRANSPORT_LOCKED_BIAS;

    uint32_t pm = enter_critical();
    pcm_wr_idx = 0U; pcm_rd_idx = 0U; pcm_count = 0U;
    pcm_push_in_progress = 0U; pcm_pop_in_progress = 0U; pcm_reset_seq++;
    render_wr_idx = 0U; render_rd_idx = 0U; render_count = 0U;
    render_push_in_progress = 0U; render_pop_in_progress = 0U; render_reset_seq++;
    render_next_seq = 0U;
    render_dma_expected_seq = 0U;
    render_dma_seq_valid = 0U;
    transport_started   = active_internal_source ? 1U : 0U;
    pcm_peak = 0U; pcm_drop_samples = 0U; pcm_drop_during_pop_samples = 0U;
    render_peak = 0U; render_low_water = RENDER_QUEUE_FRAMES; render_underruns = 0U; render_overflows = 0U; transport_resyncs = 0U; transport_underrun_streak = 0U;
    outphase_limit_count = 0U; low_env_inhibit_count = 0U;
    fm_silent_frame_count = 0U; fm_limiter_count = 0U; fm_deviation_peak_hz = 0.0f;
    am_clamp_count = 0U; am_pos_peak = 0.0f; am_neg_peak = 0.0f;
    rate_plan_fault_latched = 0U;
    usb_input_clip_count = 0U; usb_input_peak_gain_raw = 0U; usb_input_rms_gain = 0.0f; usb_input_activity = 0U;
    cessb_clip_count = 0U; cessb_peak_pre = 0.0f; cessb_peak_post_clip = 0.0f; cessb_peak_post_filter = 0.0f; cessb_overshoot = 0.0f;
    usb_packets = 0U; usb_last_submit_count = 0U;
    usb_nonzero_packets = 0U; usb_gap_resets = 0U;
    usb_sof_count = 0U; usb_missing_frames = 0U;
    usb_packet_size_anomalies = 0U; usb_rx_samples = 0U;
    usb_packet_size_min = 0U; usb_packet_size_max = 0U;
    usb_stream_active = 0U; usb_missing_frame_streak = 0U;
    audio_seen          = active_internal_source ? 1U : 0U;
    usb_audio_seen      = 0U;
    audio_critical_reason = 0U;
    /* Internal generators should not look like recent USB activity; otherwise
     * the TX-critical holdoff logic freezes the UI for a while after entering
     * a test mode despite there being no live audio ingress. */
    g_last_audio_tick   = 0U;
    g_last_usb_tick     = 0U;
    g_last_usb_audio_tick = 0U;
    g_last_cw_tick      = 0U;
    exit_critical(pm);

    MOD_FillAllNullBuffers();
    rf_en_force_idle();
}

__attribute__((cold)) void mod_tx_pipeline_select_profile(mod_config_t cfg)
{
    if (cfg != MOD_CFG_TX_CARRIER_TEST) {
        MOD_TxCarrierTestStop();
    }
    active_internal_source = 0U;
    active_sample_drive    = cfg_drive;
    active_branch_mode     = BRANCH_MODE_BOTH;
    active_carrier         = &custom_carrier;
    active_mod_type        = MOD_TYPE_SSB;
    active_render_fn       = render_ssb_prod;
    refresh_active_cal_ptr();

    switch (cfg)
    {
        case MOD_CFG_PROD_SSB_14MHZ:
            active_render_fn = render_ssb_prod; break;
#if SSB_COMPARE_RENDERERS
        case MOD_CFG_ENHANCED_SSB_14MHZ:
            active_render_fn = render_ssb_enh;  break;
        case MOD_CFG_RAW_DIRECT_14MHZ:
            active_render_fn = render_ssb_raw_direct;
            active_sample_drive = SAMPLE_DRIVE_DIRECT;
            break;
        case MOD_CFG_FILTERED_STD_14MHZ:
        case MOD_CFG_FILTERED_NARROW_14MHZ:
            active_render_fn = render_ssb_filt; break;
#endif
        case MOD_CFG_FM_NARROW_14MHZ:
            active_render_fn = render_fm;
            active_mod_type  = MOD_TYPE_FM;
            break;
        case MOD_CFG_AM_14MHZ:
            active_render_fn = render_am;
            active_mod_type  = MOD_TYPE_AM;
            break;
        case MOD_CFG_CW_14MHZ:
            active_render_fn       = render_cw;
            active_internal_source = 1U;
            active_mod_type        = MOD_TYPE_CW;
            break;
        case MOD_CFG_TX_CARRIER_TEST:
            active_render_fn       = render_tx_carrier_test;
            active_internal_source = 1U;
            active_mod_type        = MOD_TYPE_TEST;
            active_branch_mode     = BRANCH_MODE_BOTH;
            break;
#if MOD_DEEP_DIAG_ENABLE
        case MOD_CFG_INT_1KHZ_TONE:
        case MOD_CFG_INT_2TONE_IMD:
        case MOD_CFG_INT_SWEEP:
            active_render_fn       = render_test_linear;
            active_internal_source = 1U;
            active_mod_type        = MOD_TYPE_TEST;
            break;
        case MOD_CFG_INT_SSB_1KHZ:
        case MOD_CFG_INT_SSB_2TONE:
        case MOD_CFG_INT_SSB_SWEEP:
            active_render_fn       = render_ssb_debug_internal;
            active_internal_source = 1U;
            active_mod_type        = MOD_TYPE_TEST;
            break;
        case MOD_CFG_INT_THETA_SWEEP:
            active_render_fn       = render_theta_debug;
            active_internal_source = 1U;
            active_mod_type        = MOD_TYPE_TEST;
            break;
        case MOD_CFG_INT_2TONE_BRANCH_A:
            active_render_fn       = render_test_linear;
            active_internal_source = 1U;
            active_mod_type        = MOD_TYPE_TEST;
            active_branch_mode     = BRANCH_MODE_A_ONLY;
            break;
        case MOD_CFG_INT_2TONE_BRANCH_C:
            active_render_fn       = render_test_linear;
            active_internal_source = 1U;
            active_mod_type        = MOD_TYPE_TEST;
            active_branch_mode     = BRANCH_MODE_C_ONLY;
            break;
        case MOD_CFG_BASELINE_27MHZ:
            active_render_fn = render_ssb_raw_legacy;
            active_carrier   = &custom_carrier;
            break;
#endif
        default:
            active_render_fn = render_ssb_prod;
            break;
    }

    if ((active_mod_type == MOD_TYPE_SSB) && ssb_ext_active() && (cfg != MOD_CFG_BASELINE_27MHZ)) {
#if MOD_SSB_ANALYTIC_EXPERIMENT_ENABLE
        if (cfg_ssb_analytic_backend == SSB_ANALYTIC_H12P) {
            active_render_fn = render_ssb_h12p;
        }
#endif
#if SSB_EXT_ALL_BACKENDS
        if (cfg_ssb_analytic_backend != SSB_ANALYTIC_H12P) {
            active_render_fn = render_ssb_ext;
        }
#endif
    }
    if (active_mod_type == MOD_TYPE_CW) {
        cw_reset_state();
    }
}

static inline void render_frame_latch(void)
{
    frame_usb_clean = usb_clean_processing_compute();
    frame_voice_gain_bypass = (uint8_t)((active_internal_source != 0U) ||
                                        MOD_AUDIO_SOURCE_IS_LOCAL(active_audio_source));
    frame_cal_active = mod_calibration_is_active(active_cal_flags, cfg_debug_cal_bypass);
    frame_ssb_theta_cap = MOD_GetOutphaseEffectiveThetaLimit(MOD_TYPE_SSB);
    frame_tx_bw_hpf = tx_bw_hpf_coeffs();
    frame_tx_bw_lpf = tx_bw_lpf_coeffs();
    frame_tx_bw_lpf_two = (uint8_t)((cfg_tx_bw_preset == TX_BW_300_2400) ||
                                    (cfg_tx_bw_preset == TX_BW_300_2700) ||
                                    ((uint32_t)cfg_tx_bw_preset >= (uint32_t)TX_BW_COUNT));
    frame_tx_bw_bypass = tx_bw_filter_bypass_active();
}

static inline void render_dispatch(const int16_t *mono, uint32_t count, render_cmp_t *out1, render_cmp_t *out2)
{
    render_frame_latch();
    if (!MOD_CarrierFrequencyReadyForTx()) {
        (void)mono;
        (void)count;
        fill_null_frame(out1, out2);
        cw_render_frame_rf_active = 0U;
        cw_render_frame_tx_live = 0U;
        return;
    }
    if (mod_digi_tone_selected()) render_digi_tone(mono, count, out1, out2);
    else if (active_render_fn)    active_render_fn(mono, count, out1, out2);
    else                          fill_null_frame(out1, out2);
}

static inline uint8_t render_current_frame_kind(void)
{
    if ((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST) {
        return RENDER_FRAME_KIND_TX_TEST;
    }
    return (active_mod_type == MOD_TYPE_CW) ? RENDER_FRAME_KIND_CW : RENDER_FRAME_KIND_MODULATED;
}

static inline uint8_t render_compare_frame_valid(const render_cmp_t *out1, const render_cmp_t *out2)
{
    const CarrierProfile *carrier = active_carrier;
    if ((carrier == NULL) || (carrier->period_ticks == 0U)) return 0U;
#if MOD_HRTIM_ATOMIC_EXPERIMENT
    /* Atomic values are master reset times (phase_compare_encode_raw_tick),
     * legal for every phase of the period. min/max_compare_ticks bound the
     * old local compares: checked against them, a phase above period - 100
     * failed, which P/2 plus the common phase is from 15 m up, and TX TEST,
     * CW and SSB tripped a hard RF CMP lockout at key-down on 15 m to 10 m. */
    const uint32_t period = carrier->period_ticks;
    for (uint32_t i = 0U; i < AUDIO_FRAME_SIZE; i++) {
        if (!mod_hrtim_phase_compare_legal(period, (uint32_t)out1[i]) ||
            !mod_hrtim_phase_compare_legal(period, (uint32_t)out2[i])) return 0U;
    }
#else
    const uint32_t lo = carrier->min_compare_ticks;
    const uint32_t hi = carrier->max_compare_ticks;

    for (uint32_t i = 0U; i < AUDIO_FRAME_SIZE; i++) {
        uint32_t a = (uint32_t)out1[i];
        uint32_t c = (uint32_t)out2[i];
        if ((a < lo) || (a > hi) || (c < lo) || (c > hi)) return 0U;
    }
#endif
    return 1U;
}

static inline void render_frame_prepare(RenderFrame *s)
{
    if (s == NULL) return;
    s->frame_kind = render_current_frame_kind();
    s->rf_active = 0U;
    s->tx_live = 0U;
    s->rf_gate_valid = 0U;
    cw_render_frame_rf_active = 0U;
    cw_render_frame_tx_live = 0U;
}

static inline void render_frame_finalize(RenderFrame *s)
{
    if (s == NULL) return;
    if ((s->frame_kind == RENDER_FRAME_KIND_CW) ||
        (s->frame_kind == RENDER_FRAME_KIND_TX_TEST) ||
        (s->frame_kind == RENDER_FRAME_KIND_MODULATED)) {
        s->rf_active = cw_render_frame_rf_active;
        s->tx_live = cw_render_frame_tx_live;
        if ((s->frame_kind == RENDER_FRAME_KIND_CW) &&
            (cw_sidetone_only() || (cfg_muted != 0U))) {
            s->rf_active = 0U;
        }
        s->rf_gate_valid = 1U;
    } else {
        s->rf_active = 0U;
        s->tx_live = 0U;
        s->rf_gate_valid = 0U;
    }
}

void mod_tx_pipeline_process_frame(RenderFrame *s, const int16_t *mono, uint32_t count)
{
    render_frame_prepare(s);
    mod_rate_plan_validate_active();
    if (rate_plan_fault_latched != 0U) {
        fill_null_frame_no_rf_side_effect(s->mcmp1, s->mcmp2);
        cw_render_frame_rf_active = 0U;
        cw_render_frame_tx_live = 0U;
        render_frame_finalize(s);
        return;
    }
    render_dispatch(mono, count, s->mcmp1, s->mcmp2);
    if (render_compare_frame_valid(s->mcmp1, s->mcmp2) == 0U) {
        mod_pipeline_note(MOD_PIPELINE_STAGE_HRTIM,
                          MOD_PIPELINE_REASON_HRTIM_COMPARE_RANGE,
                          (uint32_t)active_config);
        fill_null_frame_no_rf_side_effect(s->mcmp1, s->mcmp2);
        cw_render_frame_rf_active = 0U;
        cw_render_frame_tx_live = 0U;
    }
    render_frame_finalize(s);
}

void mod_tx_pipeline_render_ext(void)
{
    CPU_DIAG_DECLARE(cpu_diag_start, MOD_CPU_DIAG_RENDER_EXT);
    CPU_SCOPE_BEGIN(MOD_CPU_SCOPE_RENDER_EXT);
    int16_t m[AUDIO_FRAME_SIZE];
    RenderFrame *s = rfifo_begin();
    if (!s) {
        CPU_SCOPE_END(MOD_CPU_SCOPE_RENDER_EXT);
        CPU_DIAG_END(MOD_CPU_DIAG_RENDER_EXT, cpu_diag_start);
        return;
    }
    if (!pcm_pop_frame(m)) {
        rfifo_cancel();
        CPU_SCOPE_END(MOD_CPU_SCOPE_RENDER_EXT);
        CPU_DIAG_END(MOD_CPU_DIAG_RENDER_EXT, cpu_diag_start);
        return;
    }
    uint16_t input_peak = meter_update(m, AUDIO_FRAME_SIZE);
    if (input_peak != 0U) {
        uint32_t now = HAL_GetTick();
        uint32_t pm = enter_critical();
        g_last_audio_tick = now;
        audio_seen = 1U;
        exit_critical(pm);
    }
    mod_tx_pipeline_process_frame(s, m, AUDIO_FRAME_SIZE);
    rfifo_commit();
    CPU_SCOPE_END(MOD_CPU_SCOPE_RENDER_EXT);
    CPU_DIAG_END(MOD_CPU_DIAG_RENDER_EXT, cpu_diag_start);
}


void mod_tx_pipeline_render_int(void)
{
    RenderFrame *s = rfifo_begin();
    if (!s) return;
    mod_tx_pipeline_process_frame(s, NULL, AUDIO_FRAME_SIZE);
    rfifo_commit();
}

void MOD_GetStatusFast(modulator_status_fast_t *s)
{
    if (!s) return;
    uint32_t pm = enter_critical();
    s->render_count = render_count;
    s->render_capacity = RENDER_QUEUE_FRAMES;
    s->last_audio_tick_ms = g_last_audio_tick;
    s->last_usb_tick_ms = g_last_usb_tick;
    s->last_usb_audio_tick_ms = g_last_usb_audio_tick;
    s->transport_started = (uint8_t)transport_started;
    s->internal_source = active_internal_source;
    s->usb_audio_seen = usb_audio_seen;
    s->mod_type = active_mod_type;
    s->config = (mod_config_t)active_config;
    exit_critical(pm);
}

__attribute__((cold)) void MOD_GetStatus(modulator_status_t *s)
{
    if (!s) return;
    CPU_DIAG_DECLARE(cpu_diag_start, MOD_CPU_DIAG_STATUS_FULL);

    /* Snapshot queue and transport counters under critical section. */
    uint32_t pm = enter_critical();
    s->pcm_count          = pcm_count;
    s->pcm_capacity       = PCM_RING_SAMPLES;
    s->pcm_peak           = pcm_peak;
    s->pcm_drop_samples   = pcm_drop_samples;
    s->pcm_drop_during_pop_samples = pcm_drop_during_pop_samples;
    s->render_count       = render_count;
    s->render_capacity    = RENDER_QUEUE_FRAMES;
    s->render_peak        = render_peak;
    s->render_low_water   = render_low_water;
    s->render_underruns   = render_underruns;
    s->render_overflows   = render_overflows;
    s->usb_packets        = usb_packets;
    s->usb_last_submit_count = usb_last_submit_count;
    s->usb_nonzero_packets = usb_nonzero_packets;
    s->usb_gap_resets     = usb_gap_resets;
    s->usb_sof_count      = usb_sof_count;
    s->usb_missing_frames = usb_missing_frames;
    s->usb_packet_size_anomalies = usb_packet_size_anomalies;
    s->usb_rx_samples     = usb_rx_samples;
    s->usb_packet_size_min = usb_packet_size_min;
    s->usb_packet_size_max = usb_packet_size_max;
    s->transport_resyncs  = transport_resyncs;
    s->transport_started  = (uint8_t)transport_started;
    s->internal_source    = active_internal_source;
    s->audio_seen         = audio_seen;
    s->usb_stream_active  = usb_stream_active;
    s->usb_audio_seen     = usb_audio_seen;
    s->audio_critical_reason = audio_critical_reason;
    memcpy(&s->pipeline, (const void *)&pipeline_health, sizeof(s->pipeline));
    s->debug_branch_mode  = (uint8_t)cfg_debug_branch_mode;
    s->debug_cal_bypass   = cfg_debug_cal_bypass;
    s->last_audio_tick_ms = g_last_audio_tick;
    s->last_usb_tick_ms   = g_last_usb_tick;
    s->last_usb_audio_tick_ms = g_last_usb_audio_tick;
    s->tim6_arr_last      = tim6_arr_last;
    s->servo_pending      = tim6_servo_bias;
    uint16_t peak_hold_snap = meter_peak_hold;
    float meter_rms_sq_snap = meter_rms_sq_acc;
    uint32_t meter_sample_count_snap = meter_sample_count;
    float comp_gain_snap = comp_gain;
    float alc_gain_snap = alc_state.gain;
    float mic_agc_gain_snap = mic_agc_gain;
    gain_telemetry_mode_t gain_telem_mode_snap = cfg_gain_telemetry_mode;
#if MOD_DEEP_DIAG_ENABLE
    uint8_t runtime_diag_snap = cfg_runtime_diag_enable;
#else
    const uint8_t runtime_diag_snap = 0U;
#endif
    exit_critical(pm);

    s->carrier_requested_hz = MOD_GetCarrierRequestedHz();
    s->carrier_actual_hz = MOD_GetActualCarrierFreqHz();
    s->carrier_error_hz = MOD_GetCarrierErrorHz();
    s->hrtim_input_hz = MOD_GetHrtimInputClockHz();
    s->hrtim_effective_mhz = (uint32_t)(MOD_GetHrtimEffectiveClockHz() / 1000000ULL);
    s->tim6_input_hz = MOD_GetTim6InputClockHz();
    s->carrier_period_ticks = MOD_GetCarrierPeriodTicks();
    s->carrier_phase_step_mdeg = s->carrier_period_ticks ? (uint16_t)((360000UL + ((uint32_t)s->carrier_period_ticks / 2UL)) / (uint32_t)s->carrier_period_ticks) : 0U;
    s->hrtim_tick_ps = (uint32_t)((1000000000000ULL + (MOD_GetHrtimEffectiveClockHz() / 2ULL)) / MOD_GetHrtimEffectiveClockHz());
    s->deadtime_tick_ps = (uint32_t)((1000000000000ULL + (((uint64_t)MOD_GetHrtimInputClockHz() * MOD_HRTIM_DEADTIME_MUL) / 2ULL)) / ((uint64_t)MOD_GetHrtimInputClockHz() * MOD_HRTIM_DEADTIME_MUL));
    s->clock_policy = (uint8_t)MOD_GetCarrierClockPolicy();
    s->retune_state = (uint8_t)MOD_GetCarrierRetuneState();
    s->retune_last_status = carrier_retune_last_status;
    s->retune_last_reason = carrier_retune_last_reason;
    s->retune_in_progress = carrier_clock_retune_in_progress;
    s->retune_fault_latched = carrier_retune_fault_latched;
    s->carrier_apply_pending = carrier_frequency_apply_pending;
    s->carrier_tx_ready = MOD_CarrierFrequencyReadyForTx();
    s->reserved_clock_state = 0U;
    s->retune_requested_count = carrier_retune_requested_count;
    s->retune_applied_count = carrier_retune_applied_count;
    s->retune_deferred_count = carrier_retune_deferred_count;
    s->retune_rejected_count = carrier_retune_rejected_count;
    s->retune_failed_count = carrier_retune_failed_count;
    s->retune_last_old_clock_hz = carrier_retune_last_old_clock_hz;
    s->retune_last_new_clock_hz = carrier_retune_last_new_clock_hz;
    s->retune_last_error_before_hz = carrier_retune_last_error_before_hz;
    s->retune_last_error_after_hz = carrier_retune_last_error_after_hz;
    s->pll_m = carrier_retune_last_pll_m;
    s->pll_n = carrier_retune_last_pll_n;
    s->pll_r = carrier_retune_last_pll_r;
    s->reserved_clock_diag = 0U;
    s->outphase_hw_theta_deg = MOD_GetOutphaseHardwareThetaLimit() * 180.0f;
    s->outphase_req_theta_deg = MOD_GetOutphaseRequestedThetaLimit(active_mod_type) * 180.0f;
    s->outphase_eff_theta_deg = MOD_GetOutphaseEffectiveThetaLimit(active_mod_type) * 180.0f;
    s->outphase_capability = (uint8_t)MOD_GetOutphaseCapability(active_mod_type);
    s->outphase_limited = (uint8_t)((outphase_limit_latched != 0U) ||
                                    (s->outphase_capability != (uint8_t)MOD_CAPABILITY_VALID));
    s->outphase_limit_count = outphase_limit_count;
    s->low_env_inhibit_count = low_env_inhibit_count;
    s->outphase_reserved0 = 0U;
    s->mod_type           = active_mod_type;
    s->sideband           = active_sideband_enum;
    s->config             = (mod_config_t)active_config;
    s->audio_profile      = active_audio_profile;
    s->cordic_enabled     = (cfg_cordic_enable && cordic_ready) ? 1U : 0U;
    s->usb_clean_mode     = cfg_usb_clean_mode;
    s->usb_audio_policy   = (uint8_t)MOD_GetUsbAudioPolicy();
    s->usb_channel_mode   = (uint8_t)cfg_usb_channel_mode;
    s->dsp_math_mode      = (uint8_t)cfg_dsp_math_mode;
    s->rf_metric_mode     = (uint8_t)cfg_rf_metric_mode;
    s->cpu_scope_mode     = (uint8_t)MOD_GetCpuScopeMode();
    s->ssb_pipeline_mode  = (uint8_t)MOD_GetSsbPipelineMode();
    const mod_rate_plan_t *rp = MOD_GetActiveRatePlan();
    if (rp != NULL) {
        s->rate_plan_input_hz = rp->input_rate_hz;
        s->rate_plan_dsp_hz = rp->dsp_rate_hz;
        s->rate_plan_rf_hz = rp->rf_update_rate_hz;
        s->rate_plan_hilbert_taps = rp->hilbert_taps;
        s->rate_plan_decimation = rp->decimation;
        s->rate_plan_interpolation = rp->interpolation;
        s->rate_plan_low_samples = rp->low_rate_samples_per_frame;
        s->rate_plan_rf_samples = rp->rf_samples_per_frame;
        s->rate_plan_strict_frame = rp->strict_frame;
    } else {
        s->rate_plan_input_hz = 0U;
        s->rate_plan_dsp_hz = 0U;
        s->rate_plan_rf_hz = 0U;
        s->rate_plan_hilbert_taps = 0U;
        s->rate_plan_decimation = 0U;
        s->rate_plan_interpolation = 0U;
        s->rate_plan_low_samples = 0U;
        s->rate_plan_rf_samples = 0U;
        s->rate_plan_strict_frame = 0U;
    }
    s->rate_plan_fault = rate_plan_fault_latched;
    s->meter.peak_raw     = meter_peak_raw;
    s->meter.peak_hold    = meter_peak_hold;
    s->usb_gain           = cfg_usb_input_gain;
    s->usb_peak_gain_raw  = usb_input_peak_gain_raw;
    s->usb_rms_gain       = usb_input_rms_gain;
    s->usb_clip_count     = usb_input_clip_count;
    s->usb_activity       = usb_input_activity;
    if (runtime_diag_snap != 0U) {
        s->branch_a_last_ticks = diag_branch_a;
        s->branch_c_last_ticks = diag_branch_c;
        s->last_envelope       = diag_envelope;
        s->last_theta_deg      = diag_theta_deg;
        s->last_phi_deg        = diag_phi_deg;
    } else {
        s->branch_a_last_ticks = 0U;
        s->branch_c_last_ticks = 0U;
        s->last_envelope       = 0.0f;
        s->last_theta_deg      = 0.0f;
        s->last_phi_deg        = 0.0f;
    }

    /* Live DSP telemetry for UI and diagnostics. */
    if (gain_telem_mode_snap == GAIN_TELEMETRY_LIVE) {
        s->comp_gain_db     = live_comp_gain_db;
        s->alc_gain_db      = live_alc_gain_db;
        s->mic_agc_gain_db  = live_mic_agc_gain_db;
    } else {
        s->comp_gain_db     = 0.0f;
        s->alc_gain_db      = 0.0f;
        s->mic_agc_gain_db  = 0.0f;
    }
    s->gate_active      = live_gate_active;
    s->cessb_clipping   = cessb_clipping_flag;
    s->cessb_clip_count = cessb_clip_count;
    s->cessb_peak_pre = cessb_peak_pre;
    s->cessb_peak_post_clip = cessb_peak_post_clip;
    s->cessb_peak_post_filter = cessb_peak_post_filter;
    s->cessb_overshoot = cessb_overshoot;
    s->fm_input_gain = cfg_fm_input_gain;
    s->fm_deviation_peak_hz = fm_deviation_peak_hz;
    s->fm_limiter_count = fm_limiter_count;
    s->fm_silent_frame_count = fm_silent_frame_count;
    s->fm_test_tone = cfg_fm_test_tone ? 1U : 0U;
    s->fm_deviation_preset = (uint8_t)MOD_GetFmDeviationPreset();
    s->am_profile = (uint8_t)MOD_GetAmProfile();
    s->am_clamp_count = am_clamp_count;
    s->am_pos_peak = am_pos_peak;
    s->am_neg_peak = am_neg_peak;
    s->tx_muted         = cfg_muted;
    s->carrier_values_capped = carrier_values_capped;
    s->last_carrier_apply_path = last_carrier_apply_path;
    s->cordic_timeouts   = cordic_timeout_count;

    /* Derive TX state */
    uint32_t now = HAL_GetTick();
    if (cfg_muted) {
        s->tx_state = TX_STATE_RX;
    } else if (active_mod_type == MOD_TYPE_CW) {
        s->tx_state = ((!cw_sidetone_only()) && cw_tx_live) ? TX_STATE_TX : TX_STATE_RX;
    } else if ((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST) {
        s->tx_state = (cw_rf_enabled != 0U) ? TX_STATE_TX : TX_STATE_RX;
    } else if (active_internal_source) {
        s->tx_state = TX_STATE_TX;
    } else if (mod_usb_transport_recent_at(now, 300U) != 0U) {
        s->tx_state = (transport_started != 0U) ? TX_STATE_TX : TX_STATE_WAIT;
    } else if (usb_stream_active == 0U || g_last_usb_tick == 0U) {
        s->tx_state = TX_STATE_WAIT;
    } else if (!transport_started) {
        s->tx_state = TX_STATE_WAIT;
    } else if ((audio_seen == 0U) ||
               ((uint32_t)(now - g_last_audio_tick) > 120U)) {
        s->tx_state = TX_STATE_SILENCE;
    } else {
        s->tx_state = TX_STATE_TX;
    }
    uint32_t pm_latch = enter_critical();
    if (s->config == MOD_CFG_TX_CARRIER_TEST) {
        s->cw_paddle_dit_active = tx_test_paddle_dit;
        s->cw_paddle_dah_active = tx_test_paddle_dah;
        cw_ui_key_latch = 0U;
    } else {
        uint8_t visible = cw_ui_key_latch;
        cw_ui_key_latch = 0U;
        if (s->mod_type != MOD_TYPE_CW) visible = 0U;
        s->cw_paddle_dit_active = (s->mod_type == MOD_TYPE_CW)
                                ? (uint8_t)(cw_paddle_dit | ((visible & CW_UI_KEY_DIT) ? 1U : 0U))
                                : 0U;
        s->cw_paddle_dah_active = (s->mod_type == MOD_TYPE_CW)
                                ? (uint8_t)(cw_paddle_dah | ((visible & CW_UI_KEY_DAH) ? 1U : 0U))
                                : 0U;
    }
    s->cw_rf_active = ((s->mod_type == MOD_TYPE_CW) || (s->config == MOD_CFG_TX_CARRIER_TEST)) ? cw_rf_enabled : 0U;
    exit_critical(pm_latch);
    s->cw_paddle_guard_state = cw_paddle_guard_state();
    s->cw_paddle_armed = cw_paddle_guard_is_armed();

    s->meter.rms = (meter_sample_count_snap > 0U)
        ? sqrtf(meter_rms_sq_snap / (float)meter_sample_count_snap)
        : 0.0f;

    s->hrtim_mcr = HRTIM1->sMasterRegs.MCR;
    s->hrtim_oenr = HRTIM1->sCommonRegs.OENR;
    s->hrtim_mcmp1r = HRTIM1->sMasterRegs.MCMP1R;
    s->hrtim_mcmp2r = HRTIM1->sMasterRegs.MCMP2R;
    s->hrtim_dta_dtxr = HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_A].DTxR;
    s->hrtim_dtc_dtxr = HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_E].DTxR; /* branch C is Timer E */
    s->tim6_cr1 = TIM6->CR1;
    s->dma_ch1_cndtr = (hdma_tim6_up.Instance != NULL) ? hdma_tim6_up.Instance->CNDTR : 0U;
    s->dma_ch2_cndtr = (hdma_mcmp2.Instance != NULL) ? hdma_mcmp2.Instance->CNDTR : 0U;
    s->dma_ht_count = dma_ht_count;
    s->dma_tc_count = dma_tc_count;
    s->dma_ch1_error_count = dma_ch1_error_count;
    s->dma_ch2_error_count = dma_ch2_error_count;
    s->dma_start_fail_count = dma_start_fail_count;
    s->transport_dma_faults = transport_dma_faults;
    s->apply_config_count = apply_config_count;
    s->error_handler_count = g_error_handler_count;

    if (gain_telem_mode_snap == GAIN_TELEMETRY_UI) {
        s->comp_gain_db = telemetry_gain_to_db(comp_gain_snap);
        s->alc_gain_db = telemetry_gain_to_db(alc_gain_snap);
        s->mic_agc_gain_db = telemetry_gain_to_db(mic_agc_gain_snap);
    }

    /* Compute the dB outside the critical section — log10f is expensive */
    s->meter.peak_db = (peak_hold_snap > 0U)
        ? (int16_t)(20.0f * log10f((float)peak_hold_snap / 32768.0f) * 10.0f)
        : -600;

    CPU_DIAG_END(MOD_CPU_DIAG_STATUS_FULL, cpu_diag_start);
}
