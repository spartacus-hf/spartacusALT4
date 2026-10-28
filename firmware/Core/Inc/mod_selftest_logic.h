#ifndef MOD_SELFTEST_LOGIC_H
#define MOD_SELFTEST_LOGIC_H

#include <stdint.h>

/* Verdict rules and limits for the REV1 built-in self test.
 *
 * Everything here is pure arithmetic over measured numbers: no HAL, no board
 * state, no I/O. mod_selftest.c drives the hardware and hands the readings to
 * these functions, so the rev1_selftest host suite can exercise every limit
 * and every classification against modelled hardware. The split follows
 * radio_pd_policy.c and mod_transport_logic.c.
 *
 * The suite is modelled on the QRP Labs QMX hardware tests (operating manual
 * 1.03.002, sections 5.19 and 8.6). Where a test has a QMX counterpart the
 * comment names it. Nothing here transmits, and nothing here writes the
 * qualification mask: an LPF is qualified by a spectrum analyser, never by
 * firmware. */

typedef enum {
    ST_PENDING = 0, /* not run */
    ST_RUNNING,
    ST_PASS,
    ST_WARN,        /* usable, but outside the expected window */
    ST_FAIL,
    ST_SKIP         /* not applicable on this board or not reached */
} selftest_verdict_t;

/* Group bits double as the run mask passed to MOD_SelfTestStart. */
#define ST_GROUP_POWER    0x01U
#define ST_GROUP_CLOCK    0x02U
#define ST_GROUP_CONTROLS 0x04U
#define ST_GROUP_STORAGE  0x08U
#define ST_GROUP_FILTERS  0x10U
#define ST_GROUP_RX       0x20U
#define ST_GROUP_ANALOG   0x40U
/* Everything that needs neither an operator at the panel nor the energised
 * analog bench steps. This is what "run" does without arguments. */
#define ST_GROUP_DEFAULT  (ST_GROUP_POWER | ST_GROUP_CLOCK | ST_GROUP_STORAGE | \
                           ST_GROUP_FILTERS | ST_GROUP_RX)
#define ST_GROUP_ALL      0x7FU
#define ST_GROUP_COUNT    7U

/* id, group, short name, unit string. Values are integers in the unit shown;
 * a unit of "" means the value column is a plain count or code. */
/* id, group, short name, unit, low limit, high limit. Values are integers
 * in the unit shown; a unit of "" means the value column is a plain count
 * or code. The limits live here rather than in each result row so a run
 * costs 12 bytes per test instead of 20. */
#define SELFTEST_TABLE(X) \
    X(ST_VREF,         ST_GROUP_POWER,    "VREFINT / VDDA",      "MV",    2800,              3000) \
    X(ST_VIN,          ST_GROUP_POWER,    "VIN SUPPLY",          "MV",    6000,              ST_LIMIT_DYNAMIC) \
    X(ST_RAIL5,        ST_GROUP_POWER,    "5V RAIL LM1084",      "MV",    4400,              5500) \
    X(ST_RAIL_RIPPLE,  ST_GROUP_POWER,    "5V RAIL SPREAD",      "MV",    0,                 150) \
    X(ST_ADC_SCAN,     ST_GROUP_POWER,    "ADC2 DMA SCAN",       "HZ",    3600,              4400) \
    X(ST_FAULTS,       ST_GROUP_POWER,    "PROTECTION FAULTS",   "",      0,                 0) \
    X(ST_RESET,        ST_GROUP_POWER,    "LAST RESET CAUSE",    "",      0,                 0) \
    X(ST_PD,           ST_GROUP_POWER,    "USB-PD CONTRACT",     "MV",    4500,              14000) \
    X(ST_SYSCLK,       ST_GROUP_CLOCK,    "SYSCLK FROM HSE",     "KHZ",   165000,            170000) \
    X(ST_HRTIM,        ST_GROUP_CLOCK,    "HRTIM INPUT CLOCK",   "KHZ",   100000,            1000000) \
    X(ST_RF_GEOM,      ST_GROUP_CLOCK,    "CARRIER GEOMETRY",    "HZ",    1000000,           28333333) \
    X(ST_CORDIC,       ST_GROUP_CLOCK,    "CORDIC UNIT",         "",      1,                 1) \
    X(ST_MATH,         ST_GROUP_CLOCK,    "FPU / LIBM",          "",      1,                 1) \
    X(ST_RX_LO_COVER,  ST_GROUP_CLOCK,    "RX LO BAND COVER",    "PCT",   90,                100) \
    X(ST_CPU,          ST_GROUP_CLOCK,    "CPU SERVICE LOAD",    "CYC",   0,                 0) \
    X(ST_STACK,        ST_GROUP_CLOCK,    "STACK HEADROOM",      "B",     1024,              ST_LIMIT_NONE) \
    X(ST_BTN_POL,      ST_GROUP_CONTROLS, "BUTTON POLARITY",     "",      0,                 1) \
    X(ST_BTN_UP,       ST_GROUP_CONTROLS, "BUTTON UP PD2",       "",      1,                 1) \
    X(ST_BTN_DN,       ST_GROUP_CONTROLS, "BUTTON DOWN PD3",     "",      1,                 1) \
    X(ST_BTN_SEL,      ST_GROUP_CONTROLS, "BUTTON SELECT PD4",   "",      1,                 1) \
    X(ST_BTN_SW4,      ST_GROUP_CONTROLS, "SW4 PD5",             "",      1,                 1) \
    X(ST_BTN_SW5,      ST_GROUP_CONTROLS, "SW5 PD6",             "",      1,                 1) \
    X(ST_ENC_CW,       ST_GROUP_CONTROLS, "ENCODER CW",          "",      1,                 1) \
    X(ST_ENC_CCW,      ST_GROUP_CONTROLS, "ENCODER CCW",         "",      1,                 1) \
    X(ST_ENC_CLICK,    ST_GROUP_CONTROLS, "ENCODER CLICK",       "",      1,                 1) \
    X(ST_ENC_LONG,     ST_GROUP_CONTROLS, "ENCODER HOLD",        "",      1,                 1) \
    X(ST_SD_MOUNT,     ST_GROUP_STORAGE,  "SD CARD MOUNT",       "MIB",   1,                 ST_LIMIT_NONE) \
    X(ST_SD_RW,        ST_GROUP_STORAGE,  "SD WRITE/READ BACK",  "B",     4096,              4096) \
    X(ST_SD_SPEED,     ST_GROUP_STORAGE,  "SD THROUGHPUT",       "KIBS",  16,                ST_LIMIT_NONE) \
    X(ST_SETTINGS,     ST_GROUP_STORAGE,  "SETTINGS RECORD",     "",      0,                 0) \
    X(ST_LPF_OFF,      ST_GROUP_FILTERS,  "LPF ALL DESELECTED",  "MV",    2500,              ST_LIMIT_NONE) \
    X(ST_LPF0,         ST_GROUP_FILTERS,  "LPF 0 CONTINUITY",    "MV",    0,                 900) \
    X(ST_LPF1,         ST_GROUP_FILTERS,  "LPF 1 CONTINUITY",    "MV",    0,                 900) \
    X(ST_LPF2,         ST_GROUP_FILTERS,  "LPF 2 CONTINUITY",    "MV",    0,                 900) \
    X(ST_LPF3,         ST_GROUP_FILTERS,  "LPF 3 CONTINUITY",    "MV",    0,                 900) \
    X(ST_LPF4,         ST_GROUP_FILTERS,  "LPF 4 CONTINUITY",    "MV",    0,                 900) \
    X(ST_LPF_ONEHOT,   ST_GROUP_FILTERS,  "LPF SELECT ONE-HOT",  "",      1,                 1) \
    X(ST_BPF_SWEEP,    ST_GROUP_FILTERS,  "BPF SWEEP + FIT",     "",      4,                 4) \
    X(ST_PCM,          ST_GROUP_RX,       "PCM1821 SAMPLE RATE", "HZ",    171000,            178000) \
    X(ST_RX_FLOOR,     ST_GROUP_RX,       "RX NOISE FLOOR",      "DDB",   -1300,             -50) \
    X(ST_IQ_AMP,       ST_GROUP_RX,       "I/Q AMPLITUDE BAL",   "DDB",   -10,               10) \
    X(ST_IQ_PHASE,     ST_GROUP_RX,       "I/Q PHASE ERROR",     "DDEG",  -50,               50) \
    X(ST_IMAGE,        ST_GROUP_RX,       "IMAGE REJECTION",     "DDB",   400,               ST_LIMIT_NONE) \
    X(ST_OPP_SB,       ST_GROUP_RX,       "OPPOSITE SIDEBAND",   "DDB",   400,               ST_LIMIT_NONE) \
    X(ST_PHONES,       ST_GROUP_RX,       "HEADPHONE PWM PATH",  "CODE",  0,                 ST_LIMIT_NONE) \
    X(ST_MIC1,         ST_GROUP_RX,       "MIC1 PA2 ADC1",       "DDB",   0,                 0) \
    X(ST_MIC2,         ST_GROUP_RX,       "MIC2 PA3 ADC1",       "DDB",   0,                 0) \
    X(ST_MIC3,         ST_GROUP_RX,       "MIC3 PE9 ADC3",       "DDB",   0,                 0) \
    X(ST_FWD_ZERO,     ST_GROUP_ANALOG,   "FORWARD DET ZERO",    "MV",    0,                 200) \
    X(ST_REV_ZERO,     ST_GROUP_ANALOG,   "REFLECTED DET ZERO",  "MV",    0,                 200) \
    X(ST_BIAS_BUCK,    ST_GROUP_ANALOG,   "PIN BIAS SMPS",       "DMA",   ST_LIMIT_DYNAMIC,  ST_LIMIT_DYNAMIC) \
    X(ST_BIAS_SETTLE,  ST_GROUP_ANALOG,   "PIN BIAS SETTLING",   "MS",    1,                 100) \
    X(ST_ENV_A,        ST_GROUP_ANALOG,   "ENVELOPE A SLOPE",    "CPV",   200,               4000) \
    X(ST_ENV_B,        ST_GROUP_ANALOG,   "ENVELOPE B SLOPE",    "CPV",   200,               4000) \
    X(ST_ENV_LIN,      ST_GROUP_ANALOG,   "ENVELOPE LINEARITY",  "PPT",   990,               1000)

#define ST_ENUM(id, group, name, unit, low, high) id,
typedef enum { SELFTEST_TABLE(ST_ENUM) SELFTEST_COUNT } selftest_id_t;
#undef ST_ENUM

/* One row per test, indexed by selftest_id_t, so the id and the group are the
 * array position and need no storage. The limits are a property of the test
 * rather than of the run, so they live in flash (MOD_SelfTestLimits) instead
 * of being copied into every row: at 54 tests that is 432 bytes of SRAM this
 * board does not have to spare. 12 bytes per row. */
#define ST_FLAG_OFFERS  0x01U  /* value is a measurement a setting could take */
typedef struct {
    int32_t     value;      /* in the unit named by MOD_SelfTestUnit */
    const char *detail;     /* a string another module words, never a formatted
                             * buffer; NULL: the card's text below */
    uint8_t     verdict;    /* selftest_verdict_t */
    uint8_t     flags;      /* ST_FLAG_* */
    uint16_t    text;       /* card_text_t on the SD card (HELP.BIN), 0 for none */
} selftest_result_t;

/* The window a test applies, from a flash table. Two tests have limits that
 * depend on live configuration -- the VIN ceiling and the PIN bias target --
 * and report ST_LIMIT_DYNAMIC so mod_selftest.c can substitute them. */
#define ST_LIMIT_DYNAMIC  (-2147483647 - 1)
#define ST_LIMIT_NONE     2147483647  /* one-sided window */
void MOD_SelfTestLimits(uint8_t id, int32_t *low, int32_t *high);

/* Names and grouping. Index by selftest_id_t; out-of-range gives "?" / 0. */
const char *MOD_SelfTestName(uint8_t id);
const char *MOD_SelfTestUnit(uint8_t id);
uint8_t     MOD_SelfTestGroup(uint8_t id);
const char *MOD_SelfTestGroupName(uint8_t group);
const char *MOD_SelfTestVerdictName(uint8_t verdict);
/* Parses one group word ("power", "rx", "all", ...); 0 when unknown. */
uint8_t     MOD_SelfTestGroupFromName(const char *name);

/* Inclusive window. An empty window (low > high) is a programming error and
 * yields ST_FAIL rather than passing everything. */
uint8_t MOD_SelfTestRange(int32_t value, int32_t low, int32_t high);
/* As above, but outside the window is ST_WARN instead of ST_FAIL: used where
 * a reading is informative and a bench decision, not a defect. */
uint8_t MOD_SelfTestRangeWarn(int32_t value, int32_t low, int32_t high);

/* ---- LPF continuity, the receive-bias test (no QMX counterpart) ----
 *
 * QMX measures its low-pass filters by transmitting at reduced power
 * (operating manual 8.6.5). REV1 does not have to: with the bias buck off,
 * R74/R77 (1 k from the 5 V rail) push about 4 mA through whichever filter's
 * PIN diodes are selected, and TX_BIAS settles at the QRP Labs node model
 * 0.6741 V + 12.1 ohm x I with no current in the 33 ohm feeds, so
 *     V = (674.1 + 0.0121 x V5) / 1.0121
 * which is 710 mV at a 3.7 V rail and 732 mV at 5.5 V: nearly rail
 * independent, so the same window works during a USB-C 5 V cold start. With
 * no filter selected nothing conducts and TX_BIAS charges toward the rail
 * through the same 1 k.
 *
 * The thresholds come from what each failure actually reads:
 *   both branches conducting     710-732 mV   -> PASS
 *   ~50 ohm of extra path R         927 mV   -> marginal
 *   one branch open, one good       904 mV   -> marginal
 *   ~100 ohm of extra path R       1110 mV   -> marginal
 *   nothing conducting          rail (5 V)   -> open
 * A single open branch is the reason the conducting limit is 900 mV and not
 * the 1.1 V that a both-branches test alone would allow: with one branch
 * open its 1 k charges that diode node through its 33 ohm feed into TX_BIAS,
 * which lifts the common node to about 904 mV. Anything between the two
 * limits is reported, never silently passed. A reading far below the diode
 * drop means the node is shorted.
 *
 * Read that margin honestly. A fully open filter reads the rail and is
 * unambiguous; a single open branch clears the limit by only a few millivolts,
 * and the QRP Labs model is characterised from 10 to 50 mA, not at the 4 mA
 * this test runs at. So the window is placed to never fail a healthy filter
 * (which sits 170 mV clear of the limit) at the cost of reporting a
 * single-branch failure as marginal rather than certain. The LPF response
 * sweep, which pushes real RF through the filter, is what settles a marginal
 * result -- this test is the fast screen that needs no dummy load.
 *
 * Note the commanded bias overlaps this window below about 15 mA: at 10 mA
 * RADIO_BiasTargetMv is 986 mV. That is inherent -- QDX/QMX disable the buck
 * at 5 mA precisely because the passive bias is already about 4 mA -- so the
 * continuity test switches the buck off rather than trusting the level alone.
 *
 * C73 (470 uF) sets the settling: falling through the 33 ohm feeds takes
 * about 20 ms per time constant, rising through the 1 k about 235 ms, which
 * is why mod_selftest.c allows 400 ms per slot and 800 ms for the deselect
 * step. */
#define ST_LPF_SHORT_MV    400   /* below this the node cannot hold a diode drop */
#define ST_LPF_CONDUCT_MV  900   /* at or below: both branches conduct */
#define ST_LPF_OPEN_MV    2500   /* at or above: nothing conducts */
_Static_assert(ST_LPF_CONDUCT_MV == 900 && ST_LPF_OPEN_MV == 2500,
               "SELFTEST_TABLE spells these limits as literals; keep both in step");
typedef enum {
    ST_LPF_SHORTED = 0, ST_LPF_CONDUCTING, ST_LPF_MARGINAL, ST_LPF_OPEN
} selftest_lpf_state_t;
selftest_lpf_state_t MOD_SelfTestLpfState(int32_t bias_mv);
/* Verdict for a slot that is expected to conduct (selected) or not (deselected).
 * `fitted` is 0 for a slot with no configured upper edge, which is reported
 * ST_SKIP rather than ST_FAIL: an unfitted filter is a build choice. */
uint8_t     MOD_SelfTestLpfVerdict(int32_t bias_mv, uint8_t expect_conducting, uint8_t fitted);
/* Its detail: a card text (card_text_t, card_text.def). */
uint16_t    MOD_SelfTestLpfDetail(int32_t bias_mv, uint8_t expect_conducting, uint8_t fitted);
/* Exactly one of the five LPF select lines asserted. `bits` is PE2..PE6
 * right-aligned to bit 0. */
uint8_t MOD_SelfTestOneHot(uint32_t bits, uint8_t expected_slot);

/* ---- Envelope DAC ramp (QMX 8.6.6 "PA mod. test") ----
 *
 * Least squares over (dac_code, measured_mv) pairs. Returns the slope in DAC
 * counts per volt -- the unit radio_config_t.envelope_dac_per_volt uses -- and
 * the coefficient of determination in parts per thousand, so the caller can
 * refuse a fit that is not a straight line. Points at or above the ceiling are
 * excluded by the caller, not here. */
typedef struct {
    float   counts_per_volt;
    int32_t r2_ppt;        /* 0..1000 */
    int32_t intercept_mv;
    uint8_t valid;
} selftest_fit_t;
void MOD_SelfTestFit(const uint16_t *dac_code, const uint16_t *measured_mv,
                     unsigned count, selftest_fit_t *out);
#define ST_ENV_R2_MIN_PPT   990  /* below this the transfer is not linear */
#define ST_ENV_CPV_MIN      200  /* counts per volt; outside is a wiring fault */
#define ST_ENV_CPV_MAX     4000
_Static_assert(ST_ENV_CPV_MIN == 200 && ST_ENV_CPV_MAX == 4000 && ST_ENV_R2_MIN_PPT == 990,
               "SELFTEST_TABLE spells these limits as literals; keep both in step");

/* ---- Run roll-up ----
 * Any FAIL makes the run FAIL; otherwise any WARN makes it WARN; otherwise a
 * run with at least one PASS is PASS. SKIP and PENDING never stand in for a
 * result, so an all-skipped run reports ST_SKIP, not ST_PASS. */
uint8_t MOD_SelfTestRollup(const selftest_result_t *results, unsigned count);
/* Counts by verdict; any out pointer may be NULL. */
void MOD_SelfTestTally(const selftest_result_t *results, unsigned count,
                       unsigned *pass, unsigned *warn, unsigned *fail, unsigned *skip);

#endif
