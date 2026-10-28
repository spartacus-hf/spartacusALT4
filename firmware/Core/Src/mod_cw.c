#include "mod_priv.h"
#include "mod_calibration_logic.h"
#include "mod_mode_math.h"
#include "mod_morse_trainer.h"
#include "mod_cw_keyer.h"
#include "mod_cw_player.h"
#include "mod_morse_tables.h"
#include "radio.h"

static inline uint8_t cw_calibration_active(void)
{
    return mod_calibration_is_active(active_cal_flags, cfg_debug_cal_bypass);
}

static inline uint32_t cw_ms_to_samples(float ms)
{
    float samples = ms * (AUDIO_RATE_HZ / 1000.0f) + 0.5f;
    if (samples < 0.0f) samples = 0.0f;
    if (samples > 1000000.0f) samples = 1000000.0f;
    return (uint32_t)samples;
}

static inline uint8_t cw_gpio_active(GPIO_TypeDef *port, uint16_t pin)
{
    return ((port->IDR & pin) == 0U) ? 1U : 0U;
}


/* Configurable single-owner paddle safety interlock for wiring where an
 * unplugged jack grounds both inputs. The state machine is advanced only by
 * cw_paddle_guard_service() from non-ISR background context. Render and DMA
 * paths read cached guarded values only. */
#define CW_PADDLE_ARM_WAIT_OPEN        0U
#define CW_PADDLE_ARM_WAIT_BOTH_PRESS  1U
#define CW_PADDLE_ARM_WAIT_RELEASE     2U
#define CW_PADDLE_ARM_READY            3U
#define CW_PADDLE_ARM_STUCK_FAULT      4U

typedef struct {
    uint8_t running;
    uint32_t since_ms;
} CwGuardTimer;

#define CW_PADDLE_FAST_DIT         (1UL << 0)
#define CW_PADDLE_FAST_DAH         (1UL << 1)
#define CW_PADDLE_FAST_ARMED       (1UL << 2)
#define CW_PADDLE_FAST_FAULT       (1UL << 3)
#define CW_PADDLE_FAST_STATE_SHIFT 8U
#define CW_PADDLE_FAST_STATE_MASK  (0xFFUL << CW_PADDLE_FAST_STATE_SHIFT)

static uint8_t cw_paddle_arm_state = CW_PADDLE_ARM_WAIT_OPEN;
static CwGuardTimer cw_guard_open_timer = {0U, 0U};
static CwGuardTimer cw_guard_both_timer = {0U, 0U};
/* Fast-path paddle snapshot. One aligned 32-bit write publishes raw dit/dah,
 * armed/fault and UI state together, so render/DMA-adjacent readers never see
 * a mixed multi-field cache. */
static volatile uint32_t cw_paddle_fast_state =
    ((uint32_t)CW_PADDLE_ARM_WAIT_OPEN << CW_PADDLE_FAST_STATE_SHIFT);

static inline uint32_t cw_paddle_pack_state(uint8_t raw_dit, uint8_t raw_dah,
                                            uint8_t armed, uint8_t fault,
                                            uint8_t ui_state)
{
    return ((raw_dit != 0U) ? CW_PADDLE_FAST_DIT : 0U)
         | ((raw_dah != 0U) ? CW_PADDLE_FAST_DAH : 0U)
         | ((armed   != 0U) ? CW_PADDLE_FAST_ARMED : 0U)
         | ((fault   != 0U) ? CW_PADDLE_FAST_FAULT : 0U)
         | (((uint32_t)ui_state << CW_PADDLE_FAST_STATE_SHIFT) & CW_PADDLE_FAST_STATE_MASK);
}

static void cw_guard_timer_reset(CwGuardTimer *t)
{
    t->running = 0U;
    t->since_ms = 0U;
}

static uint8_t cw_guard_timer_elapsed(CwGuardTimer *t, uint8_t condition, uint32_t now, uint32_t required_ms)
{
    if (!condition) {
        cw_guard_timer_reset(t);
        return 0U;
    }
    if (!t->running) {
        t->running = 1U;
        t->since_ms = now;
        return (required_ms == 0U) ? 1U : 0U;
    }
    return ((uint32_t)(now - t->since_ms) >= required_ms) ? 1U : 0U;
}

/* Arm once per power-up: CW keys nothing until the key has been seen open,
 * then held closed CW_BOOT_ARM_HOLD_MS -- either lever, or a straight key --
 * then released. A bump at power-up never transmits, and a key that is
 * closed from the start (a paddle leaning on something, an unplugged jack
 * that grounds both inputs) never arms (the owner's request, 2026-10-01;
 * TX MUTE is off by default now). Practice and the Morse trainer key
 * without it, and TX TEST keeps its own arming (PADDLE GUARD). */
#define CW_BOOT_ARM_OPEN_MS 40U
#define CW_BOOT_ARM_HOLD_MS 1000U
static CwGuardTimer cw_boot_timer;
static volatile uint8_t cw_boot_stage; /* 0 wait open, 1 wait hold, 2 wait release, 3 armed */
/* CW keys nothing now: not armed, and neither practice nor the trainer.
 * The keyer and the transport read the levers as open meanwhile. */
static volatile uint8_t cw_boot_hold = 1U;

uint8_t MOD_CwBootArmStage(void) { return cw_boot_stage; }

static void cw_boot_arm_service(uint8_t any, uint32_t now)
{
    /* Frozen while the trainer owns the keyer: arming is the radio's, and
     * an unarmed radio stays unarmed after training. */
    if (cw_train_inhibit) {
        cw_guard_timer_reset(&cw_boot_timer);
        cw_boot_hold = 0U;
        return;
    }
    switch (cw_boot_stage) {
        case 0U:
            if (cw_guard_timer_elapsed(&cw_boot_timer, (uint8_t)!any, now, CW_BOOT_ARM_OPEN_MS)) {
                cw_boot_stage = 1U;
                cw_guard_timer_reset(&cw_boot_timer);
            }
            break;
        case 1U:
            if (cw_guard_timer_elapsed(&cw_boot_timer, any, now, CW_BOOT_ARM_HOLD_MS)) {
                cw_boot_stage = 2U;
                cw_guard_timer_reset(&cw_boot_timer);
            }
            break;
        case 2U:
            if (cw_guard_timer_elapsed(&cw_boot_timer, (uint8_t)!any, now, CW_BOOT_ARM_OPEN_MS)) {
                cw_boot_stage = 3U;
                /* The arming hold's own closures are spent: arming sends nothing. */
                cw_reset_state();
            }
            break;
        default:
            break;
    }
    cw_boot_hold = (uint8_t)(cw_boot_stage != 3U && !cfg_cw_practice);
}

static uint32_t cw_clamp_timeout_ms(float v, float min_v, float max_v)
{
    v = sanitize_nan_default(v, min_v);
    if (v < min_v) v = min_v;
    if (v > max_v) v = max_v;
    return (uint32_t)(v + 0.5f);
}

static uint8_t cw_both_timer_context_active(void)
{
    if (cfg_cw_paddle_protect_mode == CW_PADDLE_PROTECT_OFF) return 0U;
    if ((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST) {
        return (cfg_tx_test_both_policy == TXTEST_BOTH_FAULT) ? 1U : 0U;
    }
    if (cfg_cw_both_policy == CW_BOTH_TIMER ||
        cfg_cw_both_policy == CW_BOTH_FAULT) {
        return 1U;
    }
    if (cfg_cw_paddle_protect_mode == CW_PADDLE_PROTECT_ARM_STUCK) return 1U;
    return 0U;
}

static uint8_t cw_active_both_fault_policy(void)
{
    if (cfg_cw_paddle_protect_mode == CW_PADDLE_PROTECT_OFF) return 0U;
    if ((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST) {
        return (cfg_tx_test_both_policy == TXTEST_BOTH_FAULT) ? 1U : 0U;
    }
    return (cfg_cw_both_policy == CW_BOTH_FAULT) ? 1U : 0U;
}

static uint32_t cw_active_both_timeout_ms(void)
{
    if ((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST) {
        return cw_clamp_timeout_ms(cfg_tx_test_paddle_stuck_ms,
                                   TXTEST_PADDLE_STUCK_MS_MIN,
                                   TXTEST_PADDLE_STUCK_MS_MAX);
    }
    return cw_clamp_timeout_ms(cfg_cw_paddle_stuck_ms,
                               CW_PADDLE_STUCK_MS_MIN,
                               CW_PADDLE_STUCK_MS_MAX);
}

uint8_t cw_paddle_guard_state(void)
{
    uint32_t st = cw_paddle_fast_state;
    return (uint8_t)((st & CW_PADDLE_FAST_STATE_MASK) >> CW_PADDLE_FAST_STATE_SHIFT);
}

uint8_t cw_paddle_guard_is_armed(void)
{
    uint32_t st = cw_paddle_fast_state;
    return (uint8_t)((st & CW_PADDLE_FAST_ARMED) ? 1U : 0U);
}

uint8_t cw_paddle_guard_is_faulted(void)
{
    uint32_t st = cw_paddle_fast_state;
    return (uint8_t)((st & CW_PADDLE_FAST_FAULT) ? 1U : 0U);
}

void cw_paddle_guard_reset(void)
{
    cw_paddle_arm_state = (cfg_cw_paddle_protect_mode == CW_PADDLE_PROTECT_OFF)
                        ? CW_PADDLE_ARM_READY
                        : CW_PADDLE_ARM_WAIT_OPEN;
    cw_guard_timer_reset(&cw_guard_open_timer);
    cw_guard_timer_reset(&cw_guard_both_timer);
    cw_paddle_fast_state = cw_paddle_pack_state(0U, 0U,
                                                (cw_paddle_arm_state == CW_PADDLE_ARM_READY) ? 1U : 0U,
                                                0U,
                                                cw_paddle_arm_state);
}

/* The USB console's virtual paddle (usb_diag.c). Its levers join the KEY
 * jack here, before the guard and the cached fast state, so the keyer, the
 * paddle protection and the TX-test gate treat them exactly as the jack. They
 * join after CW SWAP: the console labels its keys DIT and DAH, and SWAP is
 * about which way round the jack is wired. Written only from the main loop. */
static volatile uint8_t cw_virtual_levers;
/* Elements the keyer has started, so a tap on the console can let go once
 * the keyer has taken it. */
static volatile uint32_t cw_elements_started;

void MOD_CwSetVirtualPaddle(uint8_t levers) { cw_virtual_levers = (uint8_t)(levers & 3U); }
uint8_t MOD_CwVirtualPaddle(void) { return cw_virtual_levers; }
uint32_t MOD_CwElementsStarted(void) { return cw_elements_started; }

/* ---- The paddle sampler -------------------------------------------------
 *
 * SysTick, every millisecond (MOD_CwInputTick): the KEY jack read, SWAP
 * applied, the console's levers joined, each lever debounced on press and on
 * release (CW DEBOUNCE MS) and its closures counted (CWK_SamplerTick). One
 * aligned word carries it all to the keyer, the guard and the transport.
 * Until v8.09 the levers were read in the main loop: an LCD frame or a card
 * read held them stale, and a tap shorter than the loop was never seen.
 * Nothing here decides protection, touches the card or RF. */
static cwk_sampler_t cw_sampler CCMRAM_BSS;
static volatile uint32_t cw_input_word;
static volatile uint8_t cw_input_debounce_ms = 3U;
/* SWAP as the sampler applies it: it turns only with both levers open, or
 * the lever held would read as the other one closing. */
static uint8_t cw_swap_live CCMRAM_BSS;
/* The trainer plays a prompt: the keyer reads the levers open. */
static volatile uint8_t cw_train_gate;
/* The trainer owns the keyer (MOD_CwSetTrainInhibit): sidetone only, never
 * RF. Set at once; cleared by the guard once both levers have been open
 * CW_TRAIN_RELEASE_MS with the keyer at rest. Never saved. */
#define CW_TRAIN_RELEASE_MS 40U
volatile uint8_t cw_train_inhibit;
static uint8_t cw_train_release;
static CwGuardTimer cw_train_open_timer;
/* The closures the keyer has taken (its context only), and those a reset
 * from the main loop left behind: neither starts a transmission. */
static cwk_reader_t cw_reader CCMRAM_BSS;
static volatile uint16_t cw_reset_closures;

void MOD_CwInputTick(void)
{
    uint8_t dit = cw_gpio_active(CW_DIT_GPIO_Port, CW_DIT_Pin);
    uint8_t dah = cw_gpio_active(CW_DAH_GPIO_Port, CW_DAH_Pin);
    if (!dit && !dah) cw_swap_live = cfg_cw_swap;
    if (cw_swap_live) {
        const uint8_t t = dit;
        dit = dah;
        dah = t;
    }
    const uint8_t v = cw_virtual_levers;
    const uint8_t raw = (uint8_t)((dit | (v & 1U)) | (uint8_t)((dah | ((v >> 1) & 1U)) << 1));
    cw_input_word = CWK_SamplerTick(&cw_sampler, raw, cw_input_debounce_ms);
}

void MOD_CwSetTrainGate(uint8_t on) { cw_train_gate = on ? 1U : 0U; }

void MOD_CwSetTrainInhibit(uint8_t on)
{
    if (on) {
        MOD_CwMemoryStop();
        cw_train_inhibit = 1U;
        cw_train_release = 0U;
        return;
    }
    if (cw_train_inhibit) {
        cw_train_release = 1U;
        cw_guard_timer_reset(&cw_train_open_timer);
    }
}

uint8_t MOD_CwTrainInhibited(void) { return cw_train_inhibit; }

/* SPOT: one 4 s mark at the pitch from the player -- shaped, and with no way
 * to RF -- to match a station's tone by ear; a second press ends it. The
 * trainer's prompts have the player first. */
uint8_t MOD_CwSpot(void)
{
    if (MTRAIN_Active()) return 0U;
    if (CWP_Busy()) { CWP_Stop(); return 0U; }
    cwp_prog_t p;
    memset(&p, 0, sizeof(p));
    p.text[0] = MORSE_Code('T');
    p.len = 1U;
    p.dah = 4U * 48000U;
    p.lead = 48U;
    p.ramp = 240U;
    p.level = 255U;
    return CWP_Start(&p);
}
uint8_t MOD_CwLevers(void) { return (uint8_t)CWK_WORD_RAW(cw_input_word); }

static uint8_t cw_keyer_at_rest(void);

/* The trainer has let go: RF back once nothing it keyed can reach the air. */
static void cw_train_release_service(uint8_t both_open, uint32_t now)
{
    if (!cw_train_release) return;
    if (!cw_guard_timer_elapsed(&cw_train_open_timer, both_open, now, CW_TRAIN_RELEASE_MS)) return;
    if (!cw_keyer_at_rest() || CWP_Busy()) return;
    cw_reset_state(); /* the trainer's closures key nothing */
    cw_train_inhibit = 0U;
    cw_train_release = 0U;
}

/* ---- Radio memories ------------------------------------------------------
 * A message sent through the keyer as if keyed on the paddle (CWK_Seq*):
 * the main loop hands over the codes (MOD_CwMemorySend), the keyer's
 * context sends them -- the transport sees a lever down, so arming,
 * protection, TX MUTE and practice hold it as they hold the paddle -- and
 * a lever touched ends it with the element under way. */
#define CW_MEM_MAX 48U
static uint8_t cw_mem_code[CW_MEM_MAX] CCMRAM_BSS;
static cwk_seq_t cw_mem_seq CCMRAM_BSS;
static uint8_t cw_mem_len;
static volatile uint8_t cw_mem_go;    /* main loop: start it at the next sample */
static volatile uint8_t cw_mem_run;   /* keyer: sending */
static volatile uint8_t cw_mem_end;   /* keyer: 1 sent to the end, 2 a lever took over */
static uint8_t cw_mem_again;          /* main loop: REPEAT due */
static uint32_t cw_mem_end_ms;


/* The radio keys CW now (not TX TEST): a memory may be handed over. */
static uint8_t cw_mem_mode_ok(void)
{
    return (uint8_t)(active_mod_type == MOD_TYPE_CW && (mod_config_t)active_config != MOD_CFG_TX_CARRIER_TEST);
}

/* The keyer may send now: CW, not training, armed, nothing in progress. */
static uint8_t cw_mem_start(void)
{
    if (!cw_mem_mode_ok() || cw_train_inhibit || cw_boot_hold || cw_mem_go || cw_mem_run) return 0U;
    if ((cfg_cw_paddle_protect_mode != CW_PADDLE_PROTECT_OFF) && !cw_paddle_guard_is_armed()) return 0U;
    cw_mem_end = 0U;
    cw_mem_go = 1U;
    return 1U;
}

uint8_t MOD_CwMemorySend(const uint8_t *code, uint8_t len)
{
    if (MOD_CwMemoryBusy() || len == 0U || len > CW_MEM_MAX) return 0U;
    memcpy(cw_mem_code, code, len);
    cw_mem_len = len;
    return cw_mem_start();
}

void MOD_CwMemoryStop(void)
{
    cw_mem_go = 0U;
    cw_mem_run = 0U;
    cw_mem_again = 0U;
}

uint8_t MOD_CwMemoryBusy(void) { return (uint8_t)(cw_mem_go | cw_mem_run | cw_mem_again); }

/* Main loop: a message sent to its end comes again after MEMORY REPEAT S,
 * unless a lever is down meanwhile; none outlives CW. */
static void cw_mem_service(uint8_t any, uint32_t now)
{
    if ((cw_mem_go | cw_mem_run | cw_mem_again) && !cw_mem_mode_ok()) MOD_CwMemoryStop();
    if (any) cw_mem_again = 0U;
    const uint8_t end = cw_mem_end;
    if (end) {
        cw_mem_end = 0U;
        cw_mem_again = (uint8_t)(end == 1U && cfg_cw_mem_repeat_s != 0U);
        cw_mem_end_ms = now;
    }
    if (!cw_mem_again) return;
    if (cfg_cw_mem_repeat_s == 0U) { cw_mem_again = 0U; return; }
    if ((uint32_t)(now - cw_mem_end_ms) < 1000UL * cfg_cw_mem_repeat_s) return;
    cw_mem_again = 0U;
    (void)cw_mem_start();
}

/* The levers as the sampler last read them, before debounce. */
uint8_t cw_read_paddles_logical_raw(uint8_t *dit_out, uint8_t *dah_out)
{
    const uint8_t raw = CWK_WORD_RAW(cw_input_word);
    if (dit_out) *dit_out = (uint8_t)(raw & 1U);
    if (dah_out) *dah_out = (uint8_t)((raw >> 1) & 1U);
    return (uint8_t)(raw != 0U);
}

__attribute__((cold)) void cw_paddle_guard_service(uint32_t now)
{
    uint8_t raw_dit = 0U;
    uint8_t raw_dah = 0U;
    (void)cw_read_paddles_logical_raw(&raw_dit, &raw_dah);

    uint8_t both_open = (uint8_t)(((raw_dit == 0U) && (raw_dah == 0U)) ? 1U : 0U);
    uint8_t both_pressed = (uint8_t)(((raw_dit != 0U) && (raw_dah != 0U)) ? 1U : 0U);
    cw_boot_arm_service((uint8_t)!both_open, now);
    cw_train_release_service(both_open, now);
    cw_mem_service((uint8_t)!both_open, now);

    if ((uint32_t)cfg_cw_paddle_protect_mode >= (uint32_t)CW_PADDLE_PROTECT_COUNT) {
        cfg_cw_paddle_protect_mode = CW_DEFAULT_PADDLE_PROTECT_MODE;
    }
    /* CW_PADDLE_PROTECT_CUSTOM is intentionally handled by this same configurable
     * arming state machine. Its distinct behavior comes from the user-selected
     * arm timers and both-paddle policy, not from a separate enum branch. */
    if (cfg_cw_paddle_protect_mode == CW_PADDLE_PROTECT_OFF) {
        cw_paddle_arm_state = CW_PADDLE_ARM_READY;
        cw_guard_timer_reset(&cw_guard_open_timer);
        cw_guard_timer_reset(&cw_guard_both_timer);
    } else {
        uint32_t open_ms = cw_clamp_timeout_ms(cfg_cw_paddle_arm_open_ms, CW_PADDLE_ARM_MS_MIN, CW_PADDLE_ARM_MS_MAX);
        uint32_t press_ms = cw_clamp_timeout_ms(cfg_cw_paddle_arm_press_ms, CW_PADDLE_ARM_MS_MIN, CW_PADDLE_ARM_MS_MAX);
        uint32_t release_ms = cw_clamp_timeout_ms(cfg_cw_paddle_arm_release_ms, CW_PADDLE_ARM_MS_MIN, CW_PADDLE_ARM_MS_MAX);

        switch (cw_paddle_arm_state) {
            case CW_PADDLE_ARM_READY: {
                if (both_pressed && (cw_both_timer_context_active() != 0U)) {
                    uint32_t stuck_ms = cw_active_both_timeout_ms();
                    uint8_t fault_policy = cw_active_both_fault_policy();
                    uint8_t enter_fault = 0U;

                    if (stuck_ms == 0U) {
                        /* A zero timeout is an explicit OFF setting for TIMER-style
                         * stuck detection, but it means immediate lockout for FAULT
                         * policies so the FAULT label cannot silently behave as IGNORE. */
                        enter_fault = fault_policy;
                        cw_guard_timer_reset(&cw_guard_both_timer);
                    } else {
                        enter_fault = cw_guard_timer_elapsed(&cw_guard_both_timer, 1U, now, stuck_ms);
                    }

                    if (enter_fault != 0U) {
                        cw_paddle_arm_state = CW_PADDLE_ARM_STUCK_FAULT;
                        cw_guard_timer_reset(&cw_guard_open_timer);
                        cw_guard_timer_reset(&cw_guard_both_timer);
                    }
                } else {
                    cw_guard_timer_reset(&cw_guard_both_timer);
                }
                break;
            }

            case CW_PADDLE_ARM_STUCK_FAULT:
            case CW_PADDLE_ARM_WAIT_OPEN:
                cw_guard_timer_reset(&cw_guard_both_timer);
                if (cw_guard_timer_elapsed(&cw_guard_open_timer, both_open, now, open_ms)) {
                    cw_paddle_arm_state = CW_PADDLE_ARM_WAIT_BOTH_PRESS;
                    cw_guard_timer_reset(&cw_guard_open_timer);
                }
                break;

            case CW_PADDLE_ARM_WAIT_BOTH_PRESS:
                cw_guard_timer_reset(&cw_guard_open_timer);
                if (cw_guard_timer_elapsed(&cw_guard_both_timer, both_pressed, now, press_ms)) {
                    cw_paddle_arm_state = CW_PADDLE_ARM_WAIT_RELEASE;
                    cw_guard_timer_reset(&cw_guard_both_timer);
                }
                break;

            case CW_PADDLE_ARM_WAIT_RELEASE:
            default:
                cw_guard_timer_reset(&cw_guard_both_timer);
                if (cw_guard_timer_elapsed(&cw_guard_open_timer, both_open, now, release_ms)) {
                    cw_paddle_arm_state = CW_PADDLE_ARM_READY;
                    cw_guard_timer_reset(&cw_guard_open_timer);
                }
                break;
        }
    }

    (void)now;
    /* Not armed since power-up: the keyer reads both levers open
     * (cw_boot_arm_service). Not on TX TEST, which keeps its own arming. */
    if (cw_boot_hold && (mod_config_t)active_config != MOD_CFG_TX_CARRIER_TEST) raw_dit = raw_dah = 0U;
    cw_paddle_fast_state = cw_paddle_pack_state(raw_dit, raw_dah,
                                                (cw_paddle_arm_state == CW_PADDLE_ARM_READY) ? 1U : 0U,
                                                (cw_paddle_arm_state == CW_PADDLE_ARM_STUCK_FAULT) ? 1U : 0U,
                                                cw_paddle_arm_state);
}

/* The levers for CW straight from the sampler -- fresher than the guard's
 * main-loop copy -- with a closure the keyer has not taken yet counting as
 * down, so a tap shorter than a main-loop pass still starts the stream that
 * will send it. TX TEST keeps the guard's copy and its own debounce. */
static uint8_t cw_live_levers(void)
{
    const uint32_t word = cw_input_word;
    const uint16_t reset = cw_reset_closures;
    uint8_t raw = CWK_WORD_RAW(word);
    if (CWK_WORD_DITS(word) != cw_reader.seen[0] && CWK_WORD_DITS(word) != (uint8_t)reset) raw |= 1U;
    if (CWK_WORD_DAHS(word) != cw_reader.seen[1] && CWK_WORD_DAHS(word) != (uint8_t)(reset >> 8)) raw |= 2U;
    /* A message being sent is a lever down to the transport. */
    if (cw_mem_go | cw_mem_run) raw |= 1U;
    return (cw_boot_hold || cw_train_gate) ? 0U : raw;
}

static uint8_t cw_cached_read(uint8_t for_tx_test, uint8_t *dit_out, uint8_t *dah_out)
{
    uint32_t st = cw_paddle_fast_state;
    const uint8_t live = for_tx_test ? 0U : cw_live_levers();
    uint8_t dit = (uint8_t)(for_tx_test ? ((st & CW_PADDLE_FAST_DIT) ? 1U : 0U) : (live & 1U));
    uint8_t dah = (uint8_t)(for_tx_test ? ((st & CW_PADDLE_FAST_DAH) ? 1U : 0U) : ((live >> 1) & 1U));

    if (cfg_cw_paddle_protect_mode != CW_PADDLE_PROTECT_OFF) {
        if (((st & CW_PADDLE_FAST_ARMED) == 0U) || ((st & CW_PADDLE_FAST_FAULT) != 0U)) {
            dit = 0U;
            dah = 0U;
        }
    }

    if (dit && dah && (cfg_cw_paddle_protect_mode != CW_PADDLE_PROTECT_OFF)) {
        if (for_tx_test) {
            if (cfg_tx_test_both_policy == TXTEST_BOTH_IGNORE ||
                cfg_tx_test_both_policy == TXTEST_BOTH_FAULT) {
                dit = 0U;
                dah = 0U;
            }
        } else {
            if (cfg_cw_both_policy == CW_BOTH_IGNORE || cfg_cw_both_policy == CW_BOTH_FAULT) {
                dit = 0U;
                dah = 0U;
            }
        }
    }

    if (dit_out) *dit_out = dit;
    if (dah_out) *dah_out = dah;
    return (uint8_t)((dit || dah) ? 1U : 0U);
}

uint8_t cw_get_paddles_for_cw(uint8_t *dit_out, uint8_t *dah_out)
{
    return cw_cached_read(0U, dit_out, dah_out);
}

uint8_t cw_get_paddles_for_tx_test(uint8_t *dit_out, uint8_t *dah_out)
{
    return cw_cached_read(1U, dit_out, dah_out);
}

/* ---- Envelope ------------------------------------------------------------ */

/* The Blackman-Harris key-edge ramp, 0..65535 for 0..1: half the CCM of the
 * float table, and a step of 1.5e-5 against the 1/512 the position is
 * quantised to anyway. */
static inline float cw_ramp_lookup(uint32_t pos, uint32_t total)
{
    if (total == 0U) return 1.0f;
    if (pos >= total) return 1.0f;
    uint32_t idx = (pos * CW_RAMP_LUT_SIZE) / total;
    if (idx > CW_RAMP_LUT_SIZE) idx = CW_RAMP_LUT_SIZE;
    return (float)cw_ramp_lut[idx] * (1.0f / 65535.0f);
}

/* ---- Timing, published at safe boundaries -----------------------------------
 *
 * A setting the operator turns while sending -- speed on the knob, weight,
 * Curtis, latency, ACS -- used to reset the keyer and zero the envelope in
 * the middle of a mark: a hard key-up on the air. Now the main loop works
 * the new timing out into cw_cfg_pending under a sequence count (odd while
 * it writes), and the keyer takes it over between marks: a mark always ends
 * at the speed it began. A new keyer mode waits until the keyer is idle
 * with both levers open, a new ramp until the envelope is down. */
typedef struct {
    cwk_timing_t k;
    uint32_t ramp;      /* key-edge ramp, samples */
    uint32_t tail;      /* SEMI break-in hang after the last element, samples */
} cw_rt_cfg_t;

static cw_rt_cfg_t cw_cfg_pending;
static volatile uint32_t cw_cfg_seq;
/* The keyer's own copy and state: render or headphone context only, in CCM. */
static cw_rt_cfg_t cw_rt CCMRAM_BSS;
static uint32_t cw_rt_seq CCMRAM_BSS;
static cwk_state_t cw_ks CCMRAM_BSS;
static uint8_t cw_straight_down CCMRAM_BSS;
/* A reset asked for from outside the keyer's context: taken at its next
 * sample, so the keyer never sees its state cleared under it. */
static volatile uint8_t cw_reset_req;
/* The trainer's keying speed while it owns the keyer; 0 the radio's. */
static float cw_train_wpm;

static inline uint8_t cw_levers_idle(void)
{
    return (uint8_t)(CWK_Idle(&cw_ks) && CWK_WORD_LEVEL(cw_input_word) == 0U && cw_straight_down == 0U);
}

static inline void cw_cfg_follow(void)
{
    const uint32_t seq = cw_cfg_seq;
    if (seq == cw_rt_seq || (seq & 1U) != 0U) return;
    if (cw_ks.phase == CWK_MARK) return;
    if ((cw_cfg_pending.k.mode != cw_rt.k.mode) && !cw_levers_idle()) return;
    if ((cw_cfg_pending.ramp != cw_rt.ramp) && (cw_env.target_on || cw_env.env > 0.0001f)) return;
    cw_rt = cw_cfg_pending;
    cw_rt_seq = seq;
}

static __attribute__((noinline, hot)) void cw_envelope_step(uint8_t target_on)
{
    const uint32_t ramp = cw_rt.ramp;
    if (target_on != cw_env.prev_target_on) {
        cw_env.prev_target_on = target_on;
        cw_env.target_on = target_on;
        if (ramp > 0U) {
            float match = target_on ? cw_env.env : (1.0f - cw_env.env);
            uint32_t lo = 0U, hi = ramp;
            while ((hi - lo) > 1U) {
                uint32_t mid = (lo + hi) / 2U;
                if (cw_ramp_lookup(mid, ramp) < match) lo = mid;
                else hi = mid;
            }
            cw_env.ramp_pos = lo;
        } else {
            cw_env.ramp_pos = 0U;
        }
    }

    if (cw_env.target_on) {
        if (cw_env.env < 0.9999f) {
            if (cw_env.ramp_pos < ramp) cw_env.ramp_pos++;
            cw_env.env = cw_ramp_lookup(cw_env.ramp_pos, ramp);
        } else {
            cw_env.env = 1.0f;
        }
    } else {
        if (cw_env.env > 0.0001f) {
            if (cw_env.ramp_pos < ramp) cw_env.ramp_pos++;
            cw_env.env = 1.0f - cw_ramp_lookup(cw_env.ramp_pos, ramp);
        } else {
            cw_env.env = 0.0f;
        }
    }
}


void cw_build_ramp_lut(void)
{
    for (uint32_t i = 0U; i <= CW_RAMP_LUT_SIZE; i++) {
        float x = (float)i / (float)CW_RAMP_LUT_SIZE;
        float v = mod_cw_blackman_harris_ramp(x) * 65535.0f + 0.5f;
        cw_ramp_lut[i] = (uint16_t)((v < 0.0f) ? 0.0f : ((v > 65535.0f) ? 65535.0f : v));
    }
    cw_ramp_lut[0] = 0U;
    cw_ramp_lut[CW_RAMP_LUT_SIZE] = 65535U;
    CWP_Init(cw_ramp_lut, CW_RAMP_LUT_SIZE);
}


/* Every float CW setting: its default when it is not a number, then its
 * range. One table instead of two calls per setting (-1 KB of flash). */
typedef struct {
    float *v;
    float def, lo, hi;
} cw_float_limit_t;

static const cw_float_limit_t cw_float_limits[] = {
    {&cfg_cw_wpm, CW_DEFAULT_WPM, CW_WPM_MIN, CW_WPM_MAX},
    {&cfg_cw_weight, CW_DEFAULT_WEIGHT, CW_WEIGHT_MIN, CW_WEIGHT_MAX},
    {&cfg_cw_semi_qsk_tail_dits, CW_DEFAULT_SEMI_QSK_TAIL_DITS, CW_SEMI_QSK_TAIL_MIN, CW_SEMI_QSK_TAIL_MAX},
    {&cfg_cw_ramp_ms, CW_DEFAULT_RAMP_MS, CW_RAMP_MS_MIN, CW_RAMP_MS_MAX},
    {&cfg_cw_drive, CW_DEFAULT_DRIVE, CW_DRIVE_MIN, CW_DRIVE_MAX},
    {&cfg_cw_theta_trim, CW_DEFAULT_THETA_TRIM, CW_THETA_TRIM_MIN, CW_THETA_TRIM_MAX},
    {&cfg_cw_debounce_ms, CW_DEFAULT_DEBOUNCE_MS, CW_DEBOUNCE_MS_MIN, CW_DEBOUNCE_MS_MAX},
    {&cfg_cw_curtis_b_dit_pct, CW_DEFAULT_CURTIS_B_DIT_PCT, CW_CURTIS_B_PCT_MIN, CW_CURTIS_B_PCT_MAX},
    {&cfg_cw_curtis_b_dah_pct, CW_DEFAULT_CURTIS_B_DAH_PCT, CW_CURTIS_B_PCT_MIN, CW_CURTIS_B_PCT_MAX},
    {&cfg_cw_paddle_arm_open_ms, CW_DEFAULT_PADDLE_ARM_OPEN_MS, CW_PADDLE_ARM_MS_MIN, CW_PADDLE_ARM_MS_MAX},
    {&cfg_cw_paddle_arm_press_ms, CW_DEFAULT_PADDLE_ARM_PRESS_MS, CW_PADDLE_ARM_MS_MIN, CW_PADDLE_ARM_MS_MAX},
    {&cfg_cw_paddle_arm_release_ms, CW_DEFAULT_PADDLE_ARM_RELEASE_MS, CW_PADDLE_ARM_MS_MIN, CW_PADDLE_ARM_MS_MAX},
    {&cfg_cw_paddle_stuck_ms, CW_DEFAULT_PADDLE_STUCK_MS, CW_PADDLE_STUCK_MS_MIN, CW_PADDLE_STUCK_MS_MAX},
    {&cfg_tx_test_paddle_stuck_ms, TXTEST_DEFAULT_PADDLE_STUCK_MS, TXTEST_PADDLE_STUCK_MS_MIN, TXTEST_PADDLE_STUCK_MS_MAX},
};

/* The settings sanitised, then the keyer's timing worked out and published
 * (main loop; the keyer takes it at its next safe boundary). */
__attribute__((cold)) void cw_recompute_timing(void)
{
    for (uint32_t i = 0U; i < sizeof(cw_float_limits) / sizeof(cw_float_limits[0]); ++i) {
        const cw_float_limit_t *l = &cw_float_limits[i];
        *l->v = clampf(sanitize_nan_default(*l->v, l->def), l->lo, l->hi);
    }
    if (cfg_cw_autospace == 1U) cfg_cw_autospace = 3U;
    if ((cfg_cw_autospace != 0U) && (cfg_cw_autospace != 2U) && (cfg_cw_autospace != 3U) && (cfg_cw_autospace != 4U)) cfg_cw_autospace = CW_DEFAULT_AUTOSPACE;
    if (cfg_cw_latency_eighths > CW_LATENCY_EIGHTHS_MAX) cfg_cw_latency_eighths = CW_DEFAULT_LATENCY_EIGHTHS;
    if ((uint32_t)cfg_cw_keyer_mode >= (uint32_t)CW_KEYER_COUNT) cfg_cw_keyer_mode = CW_DEFAULT_KEYER_MODE;
    if ((uint32_t)cfg_cw_breakin_mode >= (uint32_t)CW_BREAKIN_COUNT) cfg_cw_breakin_mode = CW_DEFAULT_BREAKIN;
    if ((uint32_t)cfg_cw_paddle_protect_mode >= (uint32_t)CW_PADDLE_PROTECT_COUNT) cfg_cw_paddle_protect_mode = CW_DEFAULT_PADDLE_PROTECT_MODE;
    if ((uint32_t)cfg_cw_both_policy >= (uint32_t)CW_BOTH_COUNT) cfg_cw_both_policy = CW_DEFAULT_BOTH_POLICY;
    if ((uint32_t)cfg_tx_test_both_policy >= (uint32_t)TXTEST_BOTH_COUNT) cfg_tx_test_both_policy = TXTEST_DEFAULT_BOTH_POLICY;

    const float wpm = (cw_train_wpm > 0.0f) ? cw_train_wpm : cfg_cw_wpm;
    ++cw_cfg_seq; /* odd: being written */
    __DMB();
    CWK_TimingInit(&cw_cfg_pending.k, wpm, cfg_cw_weight, cfg_cw_curtis_b_dit_pct, cfg_cw_curtis_b_dah_pct,
                   cfg_cw_latency_eighths, cfg_cw_autospace, (uint8_t)cfg_cw_keyer_mode);
    cw_cfg_pending.ramp = clampu32(cw_ms_to_samples(cfg_cw_ramp_ms), 1U, 2000U);
    cw_cfg_pending.tail = clampu32((uint32_t)((float)cw_cfg_pending.k.unit * cfg_cw_semi_qsk_tail_dits + 0.5f),
                                   cw_cfg_pending.ramp, 200000U);
    __DMB();
    ++cw_cfg_seq;
    /* The first timing of all goes straight in: nothing has keyed yet. */
    if (cw_rt_seq == 0U) {
        cw_rt = cw_cfg_pending;
        cw_rt_seq = cw_cfg_seq;
    }
    const float db = cfg_cw_debounce_ms + 0.5f;
    cw_input_debounce_ms = (uint8_t)((db < 1.0f) ? 0.0f : db);
}

void sanitize_cw_settings(void)
{
    cw_recompute_timing();
}

void MOD_CwSetTrainingWpm(float wpm)
{
    cw_train_wpm = (wpm > 0.0f) ? clampf(wpm, CW_WPM_MIN, CW_WPM_MAX) : 0.0f;
    cw_recompute_timing();
}

/* The keyer back to rest: asked from the main loop (a mode change, a fault,
 * practice turned on) and carried out by the keyer at its next sample. The
 * flags others read are cleared now. */
void cw_reset_state(void)
{
    const uint32_t word = cw_input_word;
    /* A memory not yet taken, or waiting to repeat, is dropped (a mode
     * change resets the keyer): it never starts later on its own. */
    cw_mem_go = 0U;
    cw_mem_again = 0U;
    cw_reset_closures = (uint16_t)(CWK_WORD_DITS(word) | ((uint16_t)CWK_WORD_DAHS(word) << 8));
    cw_reset_req = 1U;
    cw_key_sample_active = 0U;
    cw_paddle_dit = 0U;
    cw_paddle_dah = 0U;
    cw_ui_key_latch = 0U;
    cw_tx_live = 0U;
    cw_rf_enabled = 0U;
}

static void cw_rt_reset(void)
{
    if (cw_mem_run) cw_mem_end = 2U;
    cw_mem_run = 0U;
    cw_reset_req = 0U;
    CWK_Reset(&cw_ks);
    CWK_Discard(&cw_reader, cw_input_word);
    cw_straight_down = 0U;
    memset(&cw_env, 0, sizeof(cw_env));
    cw_rf_hold_samples = 0U;
    cw_key_sample_active = 0U;
}

/* ---- The keyer ----------------------------------------------------------- */

/* Whether the levers reach the keyer now: the paddle guard armed and not
 * faulted (with protection on), CW armed since power-up, both levers
 * together not refused by the both-key policy. */
static inline uint8_t cw_both_refused(void)
{
    return (uint8_t)((cfg_cw_paddle_protect_mode != CW_PADDLE_PROTECT_OFF) &&
                     ((cfg_cw_both_policy == CW_BOTH_IGNORE) || (cfg_cw_both_policy == CW_BOTH_FAULT)));
}

/* A memory being sent: its lever, unless the operator's took over. Out of
 * line: the keyer's sample path stays as it was for the paddle. */
static __attribute__((noinline)) void cw_mem_input(cwk_input_t *in)
{
    if (in->level | in->closed) {
        cw_mem_go = cw_mem_run = 0U;
        cw_mem_end = 2U;
        return;
    }
    if (cw_mem_go) {
        cw_mem_go = 0U;
        cw_mem_run = CWK_SeqStart(&cw_mem_seq, cw_mem_code, cw_mem_len);
    }
    if (cw_mem_run) in->level = CWK_SeqLevel(&cw_mem_seq);
}

static inline void cw_keyer_input(cwk_input_t *in)
{
    const uint32_t word = cw_input_word;
    if (cw_boot_hold || cw_train_gate) {
        CWK_Discard(&cw_reader, word);
        in->level = 0U;
        in->closed = 0U;
        in->last = CWK_DIT;
        return;
    }
    CWK_ReadInput(&cw_reader, word, in);
    if ((in->level == 3U) && cw_both_refused()) {
        in->level = 0U;
        in->closed = 0U;
    }
    if (cw_mem_go | cw_mem_run) cw_mem_input(in);
}

/* A straight key: the key follows the debounced lever. */
static inline uint8_t cw_straight_step(uint8_t level)
{
    const uint8_t key = (uint8_t)(level != 0U);
    if (key != cw_straight_down) {
        cw_straight_down = key;
        if (key) ++cw_elements_started;
        if (MTRAIN_Capturing() != 0U) MTRAIN_KeyEvent(key ? MTRAIN_EV_DOWN : MTRAIN_EV_UP, HAL_GetTick());
    }
    return key;
}

static __attribute__((noinline)) void cw_mem_after(uint8_t ev)
{
    if (CWK_SeqAfter(&cw_mem_seq, &cw_rt.k, ev)) return;
    cw_mem_run = 0U;
    cw_mem_end = 1U;
}

static inline uint8_t cw_core_step(const cwk_input_t *in)
{
    const uint8_t ev = CWK_Step(&cw_ks, &cw_rt.k, in);
    if (cw_mem_run) cw_mem_after(ev);
    if ((ev & (CWK_EV_START | CWK_EV_END)) != 0U) {
        if (ev & CWK_EV_START) ++cw_elements_started;
        if (MTRAIN_Capturing() != 0U) {
            const uint32_t ms = HAL_GetTick();
            if (ev & CWK_EV_END) MTRAIN_KeyEvent(MTRAIN_EV_UP, ms);
            if (ev & CWK_EV_START) MTRAIN_KeyEvent((ev & CWK_EV_DAH) ? MTRAIN_EV_DAH : MTRAIN_EV_DIT, ms);
        }
    }
    return (uint8_t)(ev & CWK_EV_KEY);
}

/* One 48 kHz keyer sample. Out of line: render_cw and the headphone renderer
 * (MOD_CwKeyerMonitorStep) share one copy, built for speed like render_cw. */
static HOT_RENDER __attribute__((noinline)) uint8_t cw_keyer_step(void)
{
    if (cw_reset_req) cw_rt_reset();
    if ((cfg_cw_paddle_protect_mode != CW_PADDLE_PROTECT_OFF) && (!cw_paddle_guard_is_armed())) {
        cw_rt_reset();
        cw_paddle_dit = 0U;
        cw_paddle_dah = 0U;
        return 0U;
    }
    cw_cfg_follow();
    cwk_input_t in;
    cw_keyer_input(&in);
    cw_paddle_dit = (uint8_t)(in.level & 1U);
    cw_paddle_dah = (uint8_t)((in.level >> 1) & 1U);
    if (in.closed) cw_ui_key_latch |= (uint8_t)(in.closed & (CW_UI_KEY_DIT | CW_UI_KEY_DAH));
    /* A straight key follows its lever; a memory is sent by the core, which
     * finishes its last gap before the key has the keyer back. */
    const uint8_t straight = (uint8_t)((cw_rt.k.mode == CWK_STRAIGHT) && !cw_mem_run && CWK_Idle(&cw_ks));
    const uint8_t key = straight ? cw_straight_step(in.level) : cw_core_step(&in);
    cw_key_sample_active = key;
    return key;
}

/* The keyer between elements and characters, its envelope down. */
static uint8_t cw_keyer_at_rest(void)
{
    return (uint8_t)(CWK_Idle(&cw_ks) && !cw_straight_down && cw_env.env <= 0.0f);
}

/* Nothing to step: no lever down or closed since the keyer last looked, the
 * keyer at rest and the envelope down. */
static uint8_t cw_fast_idle_eligible(void)
{
    const uint32_t word = cw_input_word;
    if (cw_reset_req || cw_mem_go || cw_mem_run) return 0U;
    /* Not armed since power-up (cw_boot_hold): the keyer takes no lever, so
     * stepping it for a lever held down only discarded the closures, sample
     * by sample -- ~25k cycles a headphone half with the receiver not paused
     * (nothing is sent). With a host recording that starved the main loop
     * and the watchdog reset the radio (bench, 2026-10-06). The closures go
     * when the hold ends: arming and PRACTICE both reset the keyer. TX TEST
     * keeps its own arming and keys through the hold (its TUNE carrier). */
    if (!cw_boot_hold || (mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST) {
        if ((CWK_WORD_RAW(word) | CWK_WORD_LEVEL(word)) != 0U) return 0U;
        if (CWK_Pending(&cw_reader, word)) return 0U;
        if ((cfg_cw_paddle_protect_mode != CW_PADDLE_PROTECT_OFF) &&
            (cw_paddle_guard_is_armed() == 0U)) {
            return 0U;
        }
    }
    if (!CWK_Idle(&cw_ks) || cw_straight_down) return 0U;
    if (cw_paddle_dit != 0U || cw_paddle_dah != 0U) return 0U;
    if (cw_key_sample_active != 0U) return 0U;
    if (cw_env.target_on != 0U || cw_env.prev_target_on != 0U) return 0U;
    if (cw_env.env > 0.0001f) return 0U;
    if (cw_rf_hold_samples != 0U) return 0U;
    return 1U;
}

/* The keyer cannot put RF on the air: practice mode, TX muted, or a radio
 * that would not permit TX now (a fault, an unready assembly, an unapplied
 * carrier). The TX stream would then only drive the HRTIM compares with idle
 * pairs while the receiver runs -- its DMA deadlines competing with receive
 * for no output -- and keep the main loop in its realtime branch, so the
 * trainer's page could not follow the paddle. The keyer runs from the
 * headphone renderer instead, at the same 48 kHz. */
uint8_t cw_keyer_monitor_only(void)
{
    return (uint8_t)(cw_sidetone_only() || (cfg_muted != 0U) || (RADIO_TxPermitted() == 0U));
}

uint8_t MOD_CwKeyerMonitorActive(void)
{
    if ((active_internal_source == 0U) || (active_mod_type != MOD_TYPE_CW) ||
        ((mod_config_t)active_config == MOD_CFG_TX_CARRIER_TEST)) return 0U;
    if (cw_keyer_monitor_only() == 0U) return 0U;
    /* An idle keyer with no paddle down steps nothing: its state would not
     * change, and the envelope is already zero. The player (SPOT, the
     * trainer's prompts) then plays alone (phone_keyer) and the receiver runs
     * on: SPOT is matched against a station by ear. */
    return (uint8_t)(cw_fast_idle_eligible() == 0U);
}

/* A keyer that only drives the sidetone (TX MUTE, practice) keys as a
 * transmitter would: the receiver pauses while it sends and through the
 * same break-in hang (MOD_CwMonitorKeying), as it does under real TX. With
 * both running, the keyer stepped in the headphone renderer (up to 25k
 * cycles a half) on top of the receive DSP (~45k) overran the 64k-cycle
 * half: most receive halves were lost, and the CW decoder read the
 * chopped noise as 80 WPM gibberish (bench, 2026-10-01). */
static volatile uint32_t cw_monitor_hold;

uint8_t MOD_CwMonitorKeying(void) { return (uint8_t)(cw_monitor_hold != 0U); }
/* The keyer stopped driving the sidetone (unmuted, mode change): no hold
 * may outlive it, or the receiver would stay paused. */
void MOD_CwMonitorIdle(void) { cw_monitor_hold = 0U; }

/* The sidetone: the keyer's envelope, or the trainer's player where it is
 * louder (a prompt and the paddle never sound together: the trainer holds
 * the levers off while it plays). */
float MOD_CwKeyerMonitorStep(void)
{
    cw_rf_hold_samples = 0U; /* no TX session to hold open */
    const uint8_t key = cw_keyer_step();
    cw_envelope_step(key);
    const float played = CWP_Step();
    /* A lever down is sending from its first sample, before the debounced
     * closure has keyed an element: the sampler that debounces it (SysTick)
     * waits behind this renderer, which with a host recording left it no
     * time -- the keyer stepped, never keyed, the receiver never paused, and
     * the watchdog reset the radio (bench, 2026-10-06). */
    if (key || CWK_WORD_RAW(cw_input_word) || cw_env.target_on || cw_env.env > 0.0001f ||
        cw_ks.phase != CWK_IDLE || CWP_Running()) {
        cw_monitor_hold = (cfg_cw_breakin_mode == CW_BREAKIN_SEMI) ? cw_rt.tail : CW_IDLE_HOLD_SAMPLE_MIN;
    } else if (cw_monitor_hold) {
        --cw_monitor_hold;
    }
    return (played > cw_env.env) ? played : cw_env.env;
}

static void cw_fill_idle_fast(uint32_t count, render_cmp_t *out1, render_cmp_t *out2)
{
    uint32_t ta, tc;
    if (count > AUDIO_FRAME_SIZE) count = AUDIO_FRAME_SIZE;
    build_idle_branch_pair(&ta, &tc);
    for (uint32_t i = 0U; i < AUDIO_FRAME_SIZE; i++) {
        out1[i] = ta;
        out2[i] = tc;
    }
    mute_gain_step_n(count);
    cw_render_frame_tx_live = 0U;
    cw_render_frame_rf_active = 0U;
    DIAG_CAPTURE(ta, tc, 0.0f, 0.0f, 0.0f);
}

/* One keyed sample's theta: the keyed-carrier law, the calibration's theta
 * linearity and the limit, against the frame's cap. Out of line so its
 * clamps stay out of render_cw's complexity; one call replaces the four
 * limit calls a sample it used to make, each a division. */
static __attribute__((noinline, optimize("O3"), hot)) float cw_sample_theta(float body_drive, float key_env,
                                                                            float env_dbg, float cap,
                                                                            uint8_t cal_on)
{
    const float theta = mod_keyed_carrier_effective_theta_cap(body_drive, key_env, cfg_cw_theta_trim, cap);
    return mod_outphase_limit_theta_cap(
        theta + (cal_on ? mod_calibration_interp(theta_linearity_pd, env_dbg) : 0.0f), cap);
}

HOT_RENDER void render_cw(const int16_t *mono, uint32_t count, render_cmp_t *out1, render_cmp_t *out2)
{
    (void)mono;
    if (count > AUDIO_FRAME_SIZE) count = AUDIO_FRAME_SIZE;

    if (cw_fast_idle_eligible()) {
        cw_fill_idle_fast(count, out1, out2);
        return;
    }

    uint8_t rf_frame_active = 0U;
    uint8_t tx_frame_live = 0U;
    uint32_t last_ta = 0U, last_tc = 0U;
    /* Fixed while the frame renders: the CW theta limit and whether the
     * calibration applies. Per sample they cost a division and a walk of the
     * configuration on each of four limit calls. */
    const float theta_cap = MOD_GetOutphaseEffectiveThetaLimit(MOD_TYPE_CW);
    const uint8_t cal_on = cw_calibration_active();
    /* Practice and the trainer key the sidetone only. */
    const uint8_t rf_allowed = (uint8_t)!cw_sidetone_only();

    for (uint32_t i = 0U; i < count; i++)
    {
        uint8_t key_on = cw_keyer_step();
        cw_envelope_step(key_on);

        /* Separate the logical TX session state from the PA/gate-driver enable.
         *
         * The keyer phase deliberately stays non-idle during inter-element gaps so
         * full/semi break-in can keep the transmitter path "live". But leaving the
         * external gate-driver helper enabled through those null gaps wastes power.
         *
         * rf_path_active tracks only the shaped RF envelope itself: key-down, ramp-up
         * and ramp-down keep the helper asserted, but once the shaped envelope has
         * fully decayed in the gap we drop the helper until the next element starts. */
        uint8_t rf_path_active = (uint8_t)((key_on != 0U)
                                        || (cw_env.target_on != 0U)
                                        || (cw_env.env > 0.0001f));
        uint8_t tx_session_active = (uint8_t)(rf_path_active
                                           || (cw_ks.phase != CWK_IDLE));
        if (tx_session_active) {
            cw_rf_hold_samples = (cfg_cw_breakin_mode == CW_BREAKIN_SEMI)
                               ? cw_rt.tail
                               : CW_IDLE_HOLD_SAMPLE_MIN;
        } else if (cw_rf_hold_samples > 0U) {
            cw_rf_hold_samples--;
        }

        uint8_t sample_tx_live = (uint8_t)((tx_session_active != 0U) || (cw_rf_hold_samples > 0U));

        float body_drive = cfg_cw_drive * mute_gain_step();
        if (!(body_drive == body_drive)) body_drive = 0.0f;
        body_drive = clampf(body_drive, 0.0f, 1.0f);

        uint8_t actual_output_active = (uint8_t)((rf_allowed != 0U)
                                              && (!cfg_muted)
                                              && (rf_path_active != 0U)
                                              && (body_drive >= 0.0005f)
                                              && (cw_env.env >= 0.0005f));
        if (sample_tx_live != 0U) tx_frame_live = 1U;
        if (actual_output_active != 0U) rf_frame_active = 1U;

        uint32_t ta, tc;
        float theta = 0.0f;
        float env_dbg = clampf(cw_env.env * body_drive, 0.0f, 1.0f);
        if (actual_output_active == 0U) {
            build_idle_branch_pair(&ta, &tc);
        } else {
            theta = cw_sample_theta(body_drive, cw_env.env, env_dbg, theta_cap, cal_on);

            /* Keep CW on the same calibrated branch path as SSB so the PA
             * alignment controls exposed in the CW menu actually influence the
             * keyed carrier. */
            float cal = cal_on ? active_cal->differential_skew : 0.0f;
            float a_delta, c_delta;
            outphase_branch_deltas(theta, cal, &a_delta, &c_delta);
            build_branch_pair_from_relative(cal_on ? mod_calibration_interp(active_cal->common_phase_pd, env_dbg)
                                                   : 0.0f,
                                            a_delta, c_delta, &ta, &tc);
        }

        route_branch_pair(&out1[i], &out2[i], ta, tc);
        last_ta = ta;
        last_tc = tc;
        if (i == count - 1U) {
            DIAG_CAPTURE(ta, tc, env_dbg, theta, 0.0f);
        }
    }

    if (count == 0U) {
        build_idle_branch_pair(&last_ta, &last_tc);
    }
    for (uint32_t i = count; i < AUDIO_FRAME_SIZE; i++) {
        out1[i] = last_ta;
        out2[i] = last_tc;
    }

    cw_render_frame_tx_live = tx_frame_live;
    cw_render_frame_rf_active = rf_frame_active;
    /* A radiating frame: the receiver gives way (RADIO_TxHandover). Not for
     * practice, which never radiates and keeps its sidetone. */
    if (rf_frame_active) mod_tx_handover_mark();

    if ((tx_frame_live != 0U) || (cw_paddle_dit != 0U) || (cw_paddle_dah != 0U)
        || (cw_ks.phase != CWK_IDLE) || (cw_env.env > 0.0001f)) {
        g_last_cw_tick = HAL_GetTick();
    }
}
