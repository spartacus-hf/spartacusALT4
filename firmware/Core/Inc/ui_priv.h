#ifndef UI_PRIV_H
#define UI_PRIV_H

#include <stdint.h>
#include "modulator.h"
#include "mod_morse_trainer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SET_PROFILE = 0,
    SET_SIDEBAND,
    SET_FREQ,
    SET_DRIVE,
    SET_TX_BW,
    SET_MUTE,
    SET_SAVE,
    SET_CW_MODE,
    SET_CW_WPM,
    SET_CW_SWAP,
    SET_CW_WEIGHT,
    SET_CW_AUTOSPACE,
    SET_CW_CURTIS_DIT,
    SET_CW_CURTIS_DAH,
    SET_CW_LATENCY,
    SET_CW_BREAKIN,
    SET_CW_TAIL,
    SET_CW_RAMP,
    SET_CW_DRIVE,
    SET_CW_THETA,
    SET_CW_THETA_MODEL,
    SET_CW_PRACTICE,
    SET_CW_DEBOUNCE,
    SET_CW_PDL_PROT,
    SET_CW_ARM_OPEN,
    SET_CW_ARM_PRESS,
    SET_CW_ARM_RELEASE,
    SET_CW_BOTH_POLICY,
    SET_CW_STUCK_MS,
    /* The Morse trainer (v8.10 took over the old trainer's ids, so the
     * console's numbers stay put): */
    SET_TRAIN_ACTIVITY,
    SET_TRAIN_CONTENT,
    SET_TRAIN_GROUP,
    SET_TRAIN_LESSON,
    SET_TRAIN_RETRIES,
    SET_TRAIN_BOOST,
    SET_TRAIN_WAIT,
    SET_TRAIN_TONE,
    SET_TRAIN_COURSE,
    SET_TRAIN_OPEN,
    SET_TXTEST_BOTH_POLICY,
    SET_TXTEST_STUCK_MS,
    SET_SSB_BIAS,
    SET_SSB_ANA,
    SET_SSB_CE,
    SET_BRANCH_LAW,
    SET_TH_LAW,
    SET_CE_MIX,
    SET_CE_FLOOR,
    SET_CE_KNEE,
    SET_LOW_CUT,
    SET_GATE,
    SET_OUTPH_SSB_CAP,
    SET_FM_DEV,
    SET_FM_PRESET,
    SET_FM_GAIN,
    SET_FM_TEST_TONE,
    SET_OUTPH_FM_THETA,
    SET_AM_PROFILE,
    SET_AM_CARRIER,
    SET_AM_DEPTH,
    SET_OUTPH_AM_CAP,
    SET_CORDIC,
    SET_USB_CLEAN,
    SET_USB_GAIN,
    SET_USB_CHAN,
    SET_DSP_MATH,
    SET_GAIN_TELEM,
    SET_RF_METRIC,
    SET_CESSB,
    SET_CESSB_CLIP,
    SET_CESSB_ITER,
    SET_COMP,
    SET_COMP_MATH,
    SET_COMP_TGT,
    SET_COMP_RATIO,
    SET_EQ,
    SET_EQ_BASS,
    SET_EQ_TREBLE,
    SET_MICAGC,
    SET_MICAGC_TGT,
    SET_MICAGC_MAX,
    SET_NG,
    SET_NG_THR,
    SET_ALC,
    SET_ALC_CEIL,
    SET_CAL_SKEW,
    SET_CAL_VALID,
    SET_CAL_APPLY,
    SET_DEADTIME,
    SET_DEADTIME_RISE,
    SET_DEADTIME_FALL,
    SET_AC_OFFSET,
    SET_DUTY,
    SET_PA_GEOMETRY,
    SET_RFEN_ON,
    SET_RFEN_OFF,
    SET_RFEN_HOLD,
    SET_OUTPH_CW_THETA,
    SET_OUTPH_TEST_THETA,
    SET_OUTPH_FALLBACK,
    SET_CLK_POLICY,
    SET_HOME_LAYOUT,
    SET_HOME_METER,
    SET_HOME_GAINS,
    SET_HOME_HEALTH,
    SET_HOME_RFEN,
    SET_HOME_VREF,
    SET_HOME_HINTS,
    SET_SHORTCUT_UP,
    SET_SHORTCUT_DOWN,
    SET_RT_DIAG,
    SET_CPU_DIAG,
    SET_CPU_SCOPE,
    SET_SSB_PIPELINE,
    SET_DBG_BRANCH,
    SET_DBG_CAL,
    SET_REV1_DRIVER_A,
    SET_REV1_DRIVER_B,
    SET_REV1_DRAIN_A,
    SET_REV1_DRAIN_B,
    SET_REV1_BUTTONS,
    SET_REV1_SW4,
    SET_REV1_SW5,
    SET_REV1_ENC_A,
    SET_REV1_ENC_B,
    SET_REV1_ENC_SW,
    SET_REV1_ENC_HIGH,
    SET_REV1_ENC_REVERSE,
    SET_REV1_ENC_NOPULL,
    SET_REV1_ENC_DETENT,
    SET_REV1_BACKLIGHT,
    SET_REV1_VOLUME,
    SET_REV1_RX_MODE,
    SET_REV1_MIC,
    SET_REV1_MIC_GAIN,
    SET_REV1_USB_IQ,
    SET_REV1_ENV_MODE,
    SET_REV1_BIAS,
    SET_REV1_BIAS_LIMIT,
    SET_REV1_VIN_LIMIT,
    SET_REV1_ENV_LIMIT,
    SET_REV1_ENV_A,
    SET_REV1_ENV_B,
    SET_REV1_SWR,
    SET_REV1_FWD_ZERO,
    SET_REV1_REV_ZERO,
    SET_REV1_FWD_GAIN,
    SET_REV1_REV_GAIN,
    SET_REV1_AUX,
    SET_REV1_PD,
    SET_REV1_PD_MV,
    SET_REV1_PD_MAX,
    SET_REV1_PD_MA,
    SET_REV1_PD_RD,
    SET_REV1_PD_FIX,
    SET_REV1_IQ_I,
    SET_REV1_IQ_Q,
    SET_REV1_IQ_PHASE,
    SET_REV1_IQ_INVERT,
    SET_REV1_LPF0,
    SET_REV1_LPF1,
    SET_REV1_LPF2,
    SET_REV1_LPF3,
    SET_REV1_LPF4,
    SET_REV1_BPF0,
    SET_REV1_BPF1,
    SET_REV1_BPF2,
    SET_REV1_BPF3,
    SET_REV1_QUAL,
    SET_REV1_CONFIRM,
    SET_REV1_CLEAR, SET_REV1_OFF,
    SET_REV1_MONITOR, SET_REV1_PD_INFO, SET_REV1_CAL_CODE, SET_REV1_CAL_BIAS,
    SET_REV1_CAL_A, SET_REV1_CAL_B, SET_REV1_CAL_RX, SET_REV1_CAL_STOP,
    SET_REV1_BPF_SWEEP, SET_REV1_BPF_APPLY, SET_REV1_BPF_QMX,
    SET_REV1_LPF_SWEEP, SET_REV1_RF_SWEEP,
    SET_REV1_AF_SWEEP, SET_REV1_IMAGE_SWEEP, SET_REV1_IQ_SCOPE,
    SET_REV1_RXCAL, SET_REV1_RXCAL_APPLY,
    SET_REV1_LPF_TX0, SET_REV1_LPF_TX1, SET_REV1_LPF_TX2, SET_REV1_LPF_TX3, SET_REV1_LPF_TX4,
    SET_REV1_LPF_QMXP,
    SET_REV1_SELFTEST, SET_REV1_SELFTEST_PANEL, SET_REV1_SELFTEST_ARM,
    SET_REV1_USB_DIAG,
    /* Landscape UI (v8.00). Appended so the console's numeric ids stay put. */
    SET_UI_MODE,      /* unified USB/LSB/CW/AM/FM */
    SET_UI_BAND,      /* band preset */
    SET_UI_STEP,      /* tuning step */
    SET_UI_ROTATION,  /* LCD landscape 90 / 270 degrees */
    SET_UI_FKEY_PIN,  /* FUNC key spare pin, default PC14 */
    /* Clock tree (v8.04), appended for the same reason. */
    SET_REF_PPM,      /* reference oscillator correction, ppm */
    SET_CLK_PARK_TX,  /* park the TX HRTIM timers while receiving */
    SET_CLK_WIDE,     /* AUTO may use SYSCLK down to 150 MHz */
    SET_CLK_PHASE384, /* TX phase/NCO updates at 384 kHz */
    SET_RX_QCOMB,     /* cancel the PCM1821-locked comb in receive Q */
    /* CW receive (settings v26), appended for the same reason. */
    SET_CW_PITCH,     /* sidetone and CW beat note, Hz */
    SET_CW_RX_BW,     /* CW channel filter */
    SET_CW_AGC,       /* CW hang AGC: SLOW / FAST */
    SET_CW_DECODE,    /* receive CW decoder on the home screen */
    SET_SSB_RX_BW,    /* SSB receive filter width (v8.08) */
    SET_AM_RX_BW,     /* AM receive filter width */
    SET_RX_AGC,       /* voice AGC: OFF / SLOW / MED / FAST */
    SET_FM_SQUELCH,   /* FM squelch level, 0 off */
    /* Power and SWR meter (settings v28), appended for the same reason. */
    SET_TX_PANEL,     /* TX panel on the home screen */
    SET_METER_UPDATE, /* meter reading interval, ms */
    SET_METER_HANG,   /* TX panel kept after TX, ms */
    SET_TUNE_PCT,     /* TX TEST drain cap, % (QMX Tune %) */
    /* USB CAT ports (v8.10), appended so no console id moves. */
    SET_USB_VOX,      /* USB audio keys TX without a CAT command (QMX VOX) */
    SET_DIGI_TONE,    /* DIGI CLEAN USB audio sent as QMX does: measured tone */
    SET_REF_SOURCE,   /* the reference's long-term figure: USB host's SOF or own oscillator */
    /* The Morse trainer and the keyer presets (v8.10), appended likewise. */
    SET_TRAIN_LISTEN_WPM, /* character speed of what the trainer plays */
    SET_TRAIN_PACE_WPM,   /* its overall speed: Farnsworth spacing */
    SET_TRAIN_KEY_WPM,    /* the keyer's speed while training */
    SET_TRAIN_CAROUSEL,   /* LICW entry point */
    SET_TRAIN_MANUAL,     /* spacing in dits instead of Farnsworth */
    SET_TRAIN_ICS,        /* dits between characters */
    SET_TRAIN_IWS,        /* dits between words */
    SET_TRAIN_THINK,      /* seconds to start an answer */
    SET_TRAIN_SHOW,       /* echo prompt by sound, text or both */
    SET_TRAIN_ADAPT,      /* listen speed follows the answers */
    SET_TRAIN_MINUTES,    /* session length */
    SET_CW_PRESET_M32,    /* Iambic B, Curtis 75/45 (Morserino-32) */
    SET_CW_PRESET_CLASSIC,/* Iambic B, Curtis 0/0: every squeeze memory */
    SET_CW_SPOT,          /* the sidetone at the pitch, to zero-beat by ear */
    SET_CW_MEM1,          /* radio memories: line 1..8 of CW/MEMORIES.TXT */
    SET_CW_MEM2, SET_CW_MEM3, SET_CW_MEM4, SET_CW_MEM5, SET_CW_MEM6, SET_CW_MEM7, SET_CW_MEM8,
    SET_CW_MEM_REPEAT,    /* seconds between repeats of a memory, 0 once */
    SET_TRAIN_SAVE,       /* the trainer writes the learner's record (v8.12) */
    SET_COUNT
} setting_id_t;

typedef enum {
    STY_NUMERIC = 0,
    STY_TOGGLE,
    STY_PROFILE,
    STY_SIDEBAND,
    STY_ANALYTIC,
    STY_TXBW,
    STY_BRANCH,
    STY_BRANCH_LAW,
    STY_OUTPHASE,
    STY_PA_GEOMETRY,
    STY_USBCHAN,
    STY_USBPOLICY,
    STY_FM_PRESET,
    STY_AM_PROFILE,
    STY_DSPMATH,
    STY_GAINTELEM,
    STY_COMPMATH,
    STY_RFMETRIC,
    STY_CWKEYER,
    STY_CWBREAKIN,
    STY_CWPDLPROT,
    STY_CWBOTH,
    STY_CWACS,
    STY_CWTHETA,
    STY_TXTESTBOTH,
    STY_MTRAIN_ACT,
    STY_MTRAIN_SRC,
    STY_MTRAIN_SHOW,
    STY_MTRAIN_COURSE,
    STY_HOME_LAYOUT,
    STY_SHORTCUT,
    STY_CORDIC_RO,
    STY_MS_TIMEOUT,
    STY_OUTPH_FALLBACK,
    STY_CLK_POLICY,
    STY_CPUSCOPE,
    STY_SSBPIPE,
    STY_DRIVER, STY_DRAIN, STY_SWITCH, STY_RXMODE, STY_MIC, STY_ENVELOPE,
    STY_ENCPIN,
    STY_ACTION,
    /* Appended after STY_ACTION: `set list` prints these numbers. */
    STY_UMODE, STY_BAND, STY_STEP, STY_ROTATION,
    STY_CWBW, STY_CWAGC,
    STY_SSBBW, STY_AMBW, STY_RXAGC, STY_SQUELCH,
    STY_REFSRC
} setting_style_t;

typedef float (*getf_t)(void);
typedef void  (*setf_t)(float);

/* A setting's knob step, a byte in the table (UI_SettingStep reads the
 * value): a float there cost four bytes a row and its padding. */
typedef enum {
    STEP_0, STEP_0_001, STEP_0_002, STEP_0_005, STEP_0_01, STEP_0_02, STEP_0_05,
    STEP_0_1, STEP_0_5, STEP_1, STEP_5, STEP_10, STEP_16, STEP_20, STEP_50,
    STEP_100, STEP_250, STEP_500,
    STEP_DEADTIME,  /* MOD_GetDeadTimeStepNs(): one HRTIM tick at this clock */
    STEP_COUNT
} setting_step_t;

typedef struct {
    getf_t          get;
    setf_t          set;
    uint8_t         step;      /* setting_step_t */
    uint8_t         decimals;
    setting_style_t style;
} setting_item_t;
float UI_SettingStep(const setting_item_t *it);

typedef enum {
    AUDIO_GROUP_QUICK = 0,
    AUDIO_GROUP_CW,
    AUDIO_GROUP_CW_TRAIN,
    AUDIO_GROUP_CW_RF,
    AUDIO_GROUP_SSB,
    AUDIO_GROUP_FM,
    AUDIO_GROUP_AM,
    AUDIO_GROUP_USB_INPUT,
    AUDIO_GROUP_DSP_ENGINE,
    AUDIO_GROUP_VOICE,
    AUDIO_GROUP_PA_RF,
    AUDIO_GROUP_UI,
    AUDIO_GROUP_DEBUG,
    AUDIO_GROUP_BOARD,
    /* The REV1 board group is split into task-sized pages reached from the
     * BOARD / HARDWARE menu entry; AUDIO_GROUP_BOARD itself is no longer
     * listed and stays only so stored per-group cursors keep their meaning. */
    AUDIO_GROUP_BOARD_ASSEMBLY,
    AUDIO_GROUP_BOARD_CONTROLS,
    AUDIO_GROUP_BOARD_FILTERS,
    AUDIO_GROUP_BOARD_BIAS,
    AUDIO_GROUP_BOARD_RXTRIM,
    AUDIO_GROUP_BOARD_POWER,
    AUDIO_GROUP_BOARD_CAL,
    /* Everyday operating controls: the menu's first category. */
    AUDIO_GROUP_OPERATE,
    /* v8.10: ADVANCED CW and ADVANCED TRAINING, each a list in sections
     * (UI_MenuRowAt), and their sections. */
    AUDIO_GROUP_CW_ADVANCED,
    AUDIO_GROUP_CW_TIMING,
    AUDIO_GROUP_CW_BREAKIN,
    AUDIO_GROUP_CW_RECEIVE,
    AUDIO_GROUP_CW_SHAPING,
    AUDIO_GROUP_TRAIN_ADVANCED,
    AUDIO_GROUP_TRAIN_COURSE,
    AUDIO_GROUP_TRAIN_SPACING,
    AUDIO_GROUP_TRAIN_ANSWER,
    AUDIO_GROUP_TRAIN_SESSION,
    AUDIO_GROUP_TRAIN_BASIC,    /* the trainer list's SETTINGS section */
    AUDIO_GROUP_CW_MEMORIES,    /* CW > MEMORIES: send a line, its repeat */
    AUDIO_GROUP_COUNT
} audio_group_t;

typedef struct {
    const char        *title;
    const setting_id_t *items;
    uint8_t            count;
} audio_group_info_t;

#define AUDIO_GROUP_NONE 0xFFU
#define UI_AUDIO_SCOPE_ALL         0U
#define UI_AUDIO_SCOPE_TOP_MENU    1U
#define UI_AUDIO_SCOPE_MODULATION  2U
#define UI_AUDIO_SCOPE_BOARD       3U

extern uint8_t ui_debug_visible;

uint8_t UI_GetProdModeCount(void);
mod_config_t UI_GetProdModeAt(uint8_t idx);
uint8_t UI_GetTestModeCount(void);
mod_config_t UI_GetTestModeAt(uint8_t idx);
uint8_t UI_FindProdMode(mod_config_t cfg);
const char *UI_ProdModeHelp(mod_config_t cfg);

uint8_t UI_GetAudioGroupVisibleCount(void);
uint8_t UI_GetAudioGroupAt(uint8_t visible_idx);
uint8_t UI_GetModulationGroupVisibleCount(void);
uint8_t UI_GetModulationGroupAt(uint8_t visible_idx);
uint8_t UI_GetBoardGroupVisibleCount(void);
uint8_t UI_GetBoardGroupAt(uint8_t visible_idx);
/* Group list for a UI_AUDIO_SCOPE_*; shared by navigation and rendering. */
uint8_t UI_GroupListCount(uint8_t scope);
uint8_t UI_GroupListAt(uint8_t scope, uint8_t index);
const audio_group_info_t *UI_GetAudioGroupInfo(uint8_t group);
const char *UI_AudioGroupHelp(uint8_t group);
uint8_t UI_AudioGroupTotalItems(uint8_t group);
const setting_item_t *UI_AudioGroupSetting(uint8_t group, uint8_t cursor);
uint8_t UI_AudioGroupSettingId(uint8_t group, uint8_t cursor, setting_id_t *out_id);

const setting_item_t *UI_GetSetting(setting_id_t id);
/* A setting's name ("CW SPEED WPM"), byte-pair coded in flash
 * (ui_setting_names.def): valid until the second call after this one. */
const char *UI_SettingName(setting_id_t id);
/* Line n (0..7) of CW/MEMORIES.TXT (its first 511 bytes) into out, without
 * its line end; 0 if the card cannot be read now. Main loop: reads the card. */
uint8_t UI_CwMemoryText(uint8_t n, char *out, uint16_t cap);
const char *UI_SettingHelp(setting_id_t id);

uint8_t UI_SettingRange(setting_id_t id, float *min_value, float *max_value);
float UI_ClampSettingValue(const setting_item_t *it, float value);
int32_t UI_SettingDiscreteCount(setting_style_t style);
uint8_t UI_SsbAnalyticChoiceCount(void);
ssb_analytic_backend_t UI_SsbAnalyticChoiceAt(uint8_t index);
uint8_t UI_SettingIsDiscrete(setting_style_t style);
uint8_t UI_SettingIsBandScoped(setting_id_t id);

/* Setting value as the operator reads it ("USB", "ON", "12.50"), appended to
 * dst, which must hold 24 more characters. */
void UI_FormatSettingValue(const setting_item_t *it, float value, char *dst);

#ifdef __cplusplus
}
#endif

#endif
