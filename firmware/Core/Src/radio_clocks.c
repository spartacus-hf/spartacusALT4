#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "radio_clocks.h"
#include "radio_config.h"
#include "board_rev1.h"
#include "radio.h"
#include "modulator.h"
#include "clock_plan.h"
#include <string.h>

static uint32_t rx_hz;
static uint64_t rx_uhz; /* the LO exactly, micro-hertz; 0 when stopped */
static uint8_t rx_quality;
/* The period planned for the dial (RADIO_RxClockStart, RADIO_RxRetuneTo).
 * The scanner hops the LO without changing it, so a running period that
 * differs means the LO is not the dial's and a seamless retune must not
 * build on it. */
static uint32_t rx_plan_period;
/* Whether the running plan has the ADC at ~96 kHz (RADIO_RxPlanLoAdc):
 * another rate needs the receiver restarted, not tuned in place. */
static uint8_t rx_plan_adc96;
uint8_t RADIO_RxAdc96(void) { return rx_plan_adc96; }

/* HRTIM tick rate (SYSCLK x 32) in Hz, from the exact clock tree with the
 * reference correction applied -- not SystemCoreClock's nominal image. */
static uint64_t hr_clock_hz(void)
{
    return (MOD_SysclkMicroHz() * 32ULL + 500000ULL) / 1000000ULL;
}

uint32_t RADIO_SysclkHz(void)
{
    return (uint32_t)((MOD_SysclkMicroHz() + 500000ULL) / 1000000ULL);
}
static radio_rx_geometry_t rx_geometry;

static uint32_t clocks_enter_critical(void)
{
#if defined(MOD_HOST_TEST_CRITICAL_STUBS)
    return 0U;
#else
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    return pm;
#endif
}

static void clocks_exit_critical(uint32_t pm)
{
#if defined(MOD_HOST_TEST_CRITICAL_STUBS)
    (void)pm;
#else
    __set_PRIMASK(pm);
#endif
}

void RADIO_TestClockStop(void)
{
    HRTIM1->sCommonRegs.ODISR = BOARD_TEST_OUTPUT;
    HRTIM1->sMasterRegs.MCR &= ~HRTIM_TIMERID_TIMER_D;
    GPIOB->BRR = GPIO_PIN_14;
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << 28U)) | (1UL << 28U);
}

/* QSD clocks off, both pins low GPIOs: where rx_clocks_program starts. */
static void rx_outputs_stop(void)
{
    HRTIM1->sCommonRegs.ODISR = BOARD_RX_OUTPUTS;
    HRTIM1->sMasterRegs.MCR &= ~(HRTIM_TIMERID_TIMER_B | HRTIM_TIMERID_TIMER_C);
    GPIOB->BRR = GPIO_PIN_12 | GPIO_PIN_13;
    GPIOB->MODER = (GPIOB->MODER & ~(15UL << 24U)) | (5UL << 24U);
}

void RADIO_ClocksStop(void)
{
    rx_outputs_stop();
    BOARD_SetRx(0U);
    RADIO_TestClockStop();
    rx_hz = 0U;
    rx_uhz = 0U;
    rx_plan_period = 0U;
}

static void timer_init(unsigned index, uint32_t period)
{
    HRTIM_Timerx_TypeDef *t = &HRTIM1->sTimerxRegs[index];
    t->TIMxCR = HRTIM_TIMCR_CONT; /* no preload: writes below are active now */
    t->PERxR = period; /* HRTIM PER = fHRCK / fPWM, no ARR-style -1. */
    t->TIMxDIER = 0U;
    t->RSTxR = 0U;
    t->OUTxR = 0U;
    t->FLTxR = 0U;
    t->REPxR = 0U;
    t->CMP1xR = period / 2U;
    t->CMP2xR = 3U * period / 4U;
    t->CNTxR = 0U;
}

/* Program and start the QSD clocks at period p (IRQs masked by the caller).
 * extra_timers start in the same MCR write, so their phase to Timer C is
 * fixed. */
static void rx_clocks_program(uint32_t p, uint32_t extra_timers)
{
    radio_rx_geometry_t g;
    RADIO_RxClockEdges(p, &g); /* balanced integer-tick quarters */
    /* Timer C: PB12 (CLK0 -> U11 S1) high for [0, half) from PER/CMP1.
     * PB13 (CLK1 -> U11 S0) must LEAD it by a quarter: set at C CMP2 =
     * half + rise (~3p/4), reset at ~p/4 by Timer B CMP2 (Timer C event 3 per
     * RM0440 table 234). A compare at p/4 itself would be below the 96-tick
     * minimum above 14.2 MHz, hence the helper timer. With REV1's cross-wired
     * U11 halves, U9 -> IN1 (left) and U12 -> IN2 (right), a leading CLK1
     * puts signals above the LO at positive frequency. */
    timer_init(HRTIM_TIMERINDEX_TIMER_C, p);
    HRTIM_Timerx_TypeDef *c = &HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_C];
    c->CMP1xR = g.half;
    c->CMP2xR = g.half + g.rise;
    c->SETx1R = HRTIM_OUTPUTSET_TIMPER;
    c->RSTx1R = HRTIM_OUTPUTRESET_TIMCMP1;
    c->SETx2R = HRTIM_OUTPUTSET_TIMCMP2;
    c->RSTx2R = HRTIM_OUTPUTRESET_TIMCEV3_TIMBCMP2;
    /* Timer B runs half a period ahead of C. Rather than resetting B from a
     * C compare (reset resolution is not specified), both counters start in
     * the same MCR write and B's first period is shortened to p - half; the
     * preloaded full period takes over at that first roll-over. Only the
     * high-resolution period and simultaneous enable are relied upon. B CMP2
     * at half + fall therefore lands at C time fall. */
    timer_init(HRTIM_TIMERINDEX_TIMER_B, p - g.half);
    HRTIM_Timerx_TypeDef *b = &HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_B];
    b->CMP2xR = g.half + g.fall;
    b->TIMxCR = HRTIM_TIMCR_CONT | HRTIM_TIMCR_PREEN | HRTIM_TIMCR_TREPU;
    b->PERxR = p; /* preload: applied at B's first repetition (roll-over) */
    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~(0xFFUL << 16U)) | (0xDDUL << 16U);
    GPIOB->OSPEEDR |= 15UL << 24U;
    HRTIM1->sMasterRegs.MCR |= HRTIM_TIMERID_TIMER_B | HRTIM_TIMERID_TIMER_C | extra_timers;
    /* Let the helper phase settle (one short B period) while both physical
     * outputs are still low GPIOs. A period is p / 32 core clocks (HRTIM runs
     * at 32 x SYSCLK) and every pass of this loop takes several, so p / 8
     * passes wait at least four full periods. The fixed 1000 passes it
     * replaces took ~60 us with interrupts masked, on every retune, for at
     * most ~1 us of period at 1 MHz. */
    for (volatile uint32_t i = 0U; i < p / 8U + 8U; ++i) __NOP();
    HRTIM1->sCommonRegs.OENR = BOARD_RX_OUTPUTS;
    GPIOB->MODER = (GPIOB->MODER & ~(15UL << 24U)) | (10UL << 24U);
    rx_geometry = g;
    rx_uhz = (MOD_SysclkMicroHz() * 32ULL + p / 2U) / p;
    rx_hz = (uint32_t)((rx_uhz + 500000ULL) / 1000000ULL);
}

uint8_t RADIO_RxClockStart(uint32_t hz, radio_rx_mode_t mode)
{
    RADIO_ClocksStop();
    if (hz < 1000000UL || hz > RADIO_RX_MAX_HZ) return 0U;
    /* Like QMX's 12 kHz IF superhet: place the LO so the channel sits near
     * 12 kHz, clear of DC, within the flat +-45 kHz I/Q span. The DSP NCO then
     * removes the exact residual.
     *
     * The plan is arithmetic on the arguments alone, so it runs before the
     * interrupts are masked. It walks every HRTIM period within +-50 kHz --
     * a count that grows as 1/f^2, each with a 64-bit division, ~40 us at
     * 1.435 MHz -- and together with the fixed settle loop in
     * rx_clocks_program it kept interrupts masked for ~100 us or more on
     * every retune. Coinciding with the other priority-1 interrupts, that
     * made the 4 kHz telemetry ADC interrupt miss a whole scan (latched
     * FAULT_ADC, "adc_late") and the audio DMA halves miss their deadlines:
     * within 10 retunes across the bands on v8.02. With the plan outside and
     * the settle sized to the period, the masked time is 11 us at 7 MHz and
     * 40 us at 1.435 MHz, and 290 retunes showed neither. */
    uint32_t p = 0U;
    int32_t offset = 0;
    uint8_t adc96 = 0U;
    const uint8_t quality = RADIO_RxPlanLoAdc(hr_clock_hz(), hz, mode, &p, &offset, &adc96);
    uint32_t pm = clocks_enter_critical();
    if (BOARD_RfActive() || BOARD_EmergencyLatched()) {
        clocks_exit_critical(pm);
        return 0U;
    }
    rx_quality = quality;
    if (!quality || p < 192U || p > 0xFFDCU) {
        clocks_exit_critical(pm);
        return 0U;
    }
    rx_clocks_program(p, 0U);
    rx_plan_period = p;
    rx_plan_adc96 = adc96;
    RADIO_RxAdcRunning96(adc96);
    clocks_exit_critical(pm);
    return 1U;
}

/* Seamless tuning (radio.c service_rx_clock): can the running receiver reach
 * `hz` without a restart? 1 keeps the running LO -- only the receive NCO moves;
 * 2 wants the LO moved to *period in place (RADIO_RxRetuneTo); 0 means a full
 * restart (no LO, a scanner's LO, or no plan here). Planning is arithmetic
 * only; nothing is touched. */
uint8_t RADIO_RxRetunePlan(uint32_t hz, radio_rx_mode_t mode, uint32_t *period)
{
    if (!rx_uhz || !rx_plan_period || rx_geometry.period != rx_plan_period) return 0U;
    /* `clock adc96` pinned the other rate: restart. */
    if ((RADIO_RxAdcPolicy() == RADIO_RX_ADC_96K && !rx_plan_adc96) ||
        (RADIO_RxAdcPolicy() == RADIO_RX_ADC_HIGH && rx_plan_adc96)) return 0U;
    if (hz < 1000000UL || hz > RADIO_RX_MAX_HZ) return 0U;
    int32_t lo = 0, hi = 0;
    RADIO_RxChannelSpan(mode, &lo, &hi);
    const int64_t d_uhz = (int64_t)hz * 1000000LL - (int64_t)rx_uhz;
    const int32_t d = (int32_t)(d_uhz / 1000000LL);
    if (RADIO_RxKeepLo(d, lo, hi)) return 1U;
    uint32_t p = 0U;
    int32_t offset = 0;
    uint8_t adc96 = 0U;
    if (RADIO_RxPlanLoAdc(hr_clock_hz(), hz, mode, &p, &offset, &adc96) != 2U) return 0U;
    if (adc96 != rx_plan_adc96) return 0U; /* another ADC rate: restart */
    if (p == rx_plan_period) return 1U; /* still the best there is here */
    if (period) *period = p;
    return 2U;
}

/* Move the dial's LO to `period` in place (RADIO_RxLoHop) and make it the
 * planned one. */
uint8_t RADIO_RxRetuneTo(uint32_t period)
{
    if (!RADIO_RxLoHop(period)) return 0U;
    rx_plan_period = period;
    rx_quality = 2U;
    return 1U;
}

uint64_t RADIO_HrClockHz(void) { return hr_clock_hz(); }

uint8_t RADIO_RxLoHop(uint32_t period)
{
    if (period < 192U || period > 0xFFDCU) return 0U;
    uint32_t pm = clocks_enter_critical();
    /* Only a running receive LO moves: never one that TX entry, a PLL switch
     * or a fault has just stopped, and never with RF live. */
    if (!rx_uhz || BOARD_RfActive() || BOARD_EmergencyLatched() || RADIO_State() != RADIO_RECEIVE) {
        clocks_exit_critical(pm);
        return 0U;
    }
    rx_outputs_stop();
    rx_clocks_program(period, 0U);
    clocks_exit_critical(pm);
    return 1U;
}

uint8_t RADIO_MeasureClocksStart(uint32_t hz)
{
    RADIO_ClocksStop();
    uint32_t pm = clocks_enter_critical();
    uint32_t p = hz ? (uint32_t)((hr_clock_hz() + hz / 2U) / hz) : 0U;
    if (BOARD_RfActive() || BOARD_EmergencyLatched() || RADIO_State() != RADIO_RECEIVE ||
        hz < 1000000UL || hz > RADIO_RX_MAX_HZ || p < 192U || p > 0xFFDCU) {
        clocks_exit_critical(pm);
        return 0U;
    }
    /* Timer D carries the test clock at the LO period, in phase with PB12.
     * Its output stays disabled (driven low) until the chop gate enables it. */
    timer_init(HRTIM_TIMERINDEX_TIMER_D, p);
    HRTIM_Timerx_TypeDef *d = &HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_D];
    d->SETx1R = HRTIM_OUTPUTSET_TIMPER;
    d->RSTx1R = HRTIM_OUTPUTRESET_TIMCMP1;
    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~(15UL << 24U)) | (13UL << 24U);
    GPIOB->OSPEEDR |= 3UL << 28U;
    rx_quality = 0U; /* not a receive plan */
    rx_clocks_program(p, HRTIM_TIMERID_TIMER_D);
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << 28U)) | (2UL << 28U);
    clocks_exit_critical(pm);
    return 1U;
}

void RADIO_TestOutput(uint8_t on)
{
    if (on && !BOARD_EmergencyLatched() && !BOARD_RfActive()) HRTIM1->sCommonRegs.OENR = BOARD_TEST_OUTPUT;
    else HRTIM1->sCommonRegs.ODISR = BOARD_TEST_OUTPUT;
}

uint32_t RADIO_RxClockHz(void) { return rx_hz; }

float RADIO_RxOffsetHz(uint32_t dial_hz)
{
    if (!rx_uhz) return 0.0f;
    /* Whole hertz and the micro-hertz rest apart: an int64 to float
     * conversion is a soft-float routine on this FPU (__aeabi_l2f). */
    const int64_t uhz = (int64_t)dial_hz * 1000000LL - (int64_t)rx_uhz;
    const int32_t hz = (int32_t)(uhz / 1000000LL);
    return (float)hz + (float)(int32_t)(uhz - (int64_t)hz * 1000000LL) * 1.0e-6f;
}

#if !defined(MOD_HOST_TEST_CRITICAL_STUBS)
/* A free-running 32-bit count of SYSCLK: the timebase `clock stamp` hands the
 * host, which times it against its NTP clock to measure the reference error.
 * TIM5 is otherwise unused; it keeps counting across PLL retunes, which the
 * stamp reports so the host can tell. */
void RADIO_RefTimerStart(void)
{
    __HAL_RCC_TIM5_CLK_ENABLE();
    TIM5->CR1 = 0U;
    TIM5->PSC = 0U;
    TIM5->ARR = 0xFFFFFFFFUL;
    TIM5->EGR = TIM_EGR_UG;
    TIM5->CR1 = TIM_CR1_CEN;
}
#endif

/* The reference disciplined to the USB host.
 *
 * REV1's 8 MHz reference is a DSC1003 MEMS oscillator. Against the USB host's
 * SOF (2026-10-02, every SOF stamped and read over SWD) it sat at +15 to
 * +18.5 ppm and moved by up to a ppm within a few hundred milliseconds, all
 * the time: +-12 Hz at 14 MHz, where FT8's tones are 6.25 Hz apart. The LO
 * (HRTIM), the PCM1821's clock and the TX carrier all come from it, and
 * through that wander not one FT8 signal decoded although many stood well
 * over the noise. QMX takes its LO from an Si5351 on a 25 MHz TCXO and never
 * sees this; QRP Labs warns of the same failure in digitally compensated
 * TCXOs ("frequency stepping"). KiwiSDR corrects its free-running ADC clock
 * the way this does, in its NCO, against GPS time.
 *
 * The host's SOF is its crystal divided down, 1 ms to a few ns, and it is
 * there whenever WSJT-X is. TIM5 counts SYSCLK; USB_LP_IRQHandler reads it
 * first thing whenever the SOF raised the interrupt (RADIO_RefSofEntry), so a
 * stamp is late only by the interrupt's entry -- a few cycles mostly, never
 * early. The tracker
 *   - predicts each SOF's stamp, by the host's frame number rather than a
 *     count of interrupts, so a missed interrupt costs nothing;
 *   - per REF_SOF_GROUP frames, takes the mean of the residuals within
 *     REF_SOF_BAND cycles of the earliest: the stamps taken on time, their
 *     one-cycle quantisation averaged;
 *   - drops a group further than REF_SOF_GATE cycles off: a masked section
 *     that held every SOF of a group, or the host itself (the bench laptop
 *     moves its SOF by ~5 us every ~15 s and slews back over 300 ms, which no
 *     received signal shows);
 *   - coasts on a group further than REF_SOF_STEP cycles off: no oscillator
 *     moves that far in 10 ms;
 *   - after a coast, takes nothing until REF_SOF_SETTLE groups in a row have
 *     agreed with the one before within REF_SOF_AGREE cycles; steady inside
 *     REF_SOF_STEP again, the loop goes on, steady anywhere else, the host's
 *     timing has stepped and the new origin is taken as it is, with nothing
 *     published moving (see below);
 *   - takes REF_SOF_ALPHA of the rest into its phase and REF_SOF_BETA into
 *     its frequency.
 * The NCOs follow that frequency plus the phase taken, spread over the next
 * group (RADIO_RefTrimPpb), so their phase follows the reference's own a
 * group late. Chosen against 20 m recordings with every SOF stamped: it
 * decoded what a non-causal fit of the same stamps decoded (30 signals in
 * four slots, none uncorrected); a smoother or slower loop only added lag.
 * What is left is the stamps' quantisation, about 0.5 rad at 14 MHz (less
 * now, below).
 *
 * The step gate (bench, 2026-10-02, 40 m EFHW against a QMX+): with the gate
 * at REF_SOF_GATE alone, ~2 % of the groups -- a burst of two to four every
 * ~1.02 s, on this laptop's xHCI -- sat 10-18 cycles late as a whole, the
 * NCOs jumped 10-20 ppm for 10 ms and back (9 rad at 14 MHz), and every
 * strong FT8 station grew a broadband streak each second in the waterfall;
 * with following off the streaks were gone. The groups' errors otherwise sit
 * at a median 0.9 cycles, 95 % under 4. With the gate the streaks went from
 * 6-14 events a slot to 0-1, as on the QMX+. Half of each group's phase
 * (ALPHA 0.5, a pole at 0.7, ~30 ms) then decoded more than all of it in each
 * of four interleaved 40 m rounds (26.9 against 23.6 a slot, the same SNR on
 * the stations both heard) -- the stamps' quantisation is filtered, the
 * MEMS's wander still followed; 0.3 was no better. `clock sof A S` (percent,
 * cycles) sets REF_SOF_ALPHA and REF_SOF_STEP until the restart; with BETA
 * 0.1 the loop stays damped down to an ALPHA of 0.3.
 *
 * Settling after a coast (bench, 2026-10-02, SWD trace of this struct at
 * 1.8 ms): the gate used to widen by a cycle a coasted group, to catch the
 * reference again. Every ~15 s the laptop's SOF jumped some hundreds of
 * cycles late and slewed back over ~200 ms, and the widened gate took the
 * slew's tail (~+20 cycles) as the reference: the NCOs ran +6 ppm for 10 ms
 * and +1.2 ppm for the next second -- 17 Hz at 14 MHz, against FT8's 6.25 Hz
 * tones, in receive and transmit -- until the frequency had walked off far
 * enough to coast 80 groups and acquire again (four times a minute). Waiting
 * for steady groups lets a slew pass at the right frequency, and takes a
 * step the host keeps without a jump in anything published.
 *
 * The internal oscillators cannot do this: HSI16 against SYSCLK moved
 * +-100 ppm from one second to the next; HSI48 and LSI are RC oscillators
 * too, and HSI48's only trim (CRS) is this same SOF.
 *
 * The host is no reference either, only the steadier one in seconds (bench,
 * 2026-10-04, against broadcast carriers): the laptop's SOF ran 4.5 ppm slow
 * as its CPU warmed by 17 C, and WSJT-X's decoding alone slanted FT8 overs by
 * 2.5 Hz rms at 14 MHz -- all of it passed on, while a hair dryer's +6 ppm
 * on the DSC1003 was cancelled whole. The MEMS's own random wander
 * (0.2-0.35 ppm over 1-20 s) matches the host's drift where an FT8 over
 * lives, so no split between the two (the MCU's temperature for the slow
 * part) does better. With a TCXO, REF SOURCE LOCAL turns the following off
 * (`follow`; RADIO_RefTrimPpb). */
#if !defined(MOD_HOST_TEST_CRITICAL_STUBS)
#define REF_SOF_GROUP   10U     /* host frames per update: 10 ms */
#define REF_SOF_BAND    6.0f    /* cycles over a group's earliest stamp still averaged */
#define REF_SOF_GATE    25.0f   /* cycles off the prediction: not the reference */
#define REF_SOF_BETA    0.1f    /* of a group's error, the share the frequency takes */
#define REF_SOF_ALPHA   0.5f    /* of a group's error, the share the phase takes */
#define REF_SOF_STEP    5.0f    /* cycles off the prediction: the stamps, not the reference */
#define REF_SOF_AGREE   4.0f    /* cycles between two groups after a coast: steady */
#define REF_SOF_SETTLE  3U      /* steady groups in a row before one is taken */
#define REF_SOF_ACQUIRE 20U     /* groups spanned by the acquiring slope: 0.2 s */
#define REF_SOF_COAST   80U     /* groups dropped in a row before acquiring again */
#define REF_SOF_HOLD_MS 60000U  /* no host: the last figure stands this long */
#define REF_SOF_RF_FRAMES 4U    /* an RF DMA interrupt this recently: the RF stream runs */
#define REF_SOF_PROBATION 10U   /* groups a far-off re-acquisition must pass before it is published */
#define REF_SOF_PROBATION_PPB 3000.0f /* far off: further than this from the figure published */
enum { REF_IDLE, REF_ACQUIRE, REF_LOCKED };
_Static_assert(REF_SOF_SETTLE <= 3U, "settle is two bits");
/* Packed: RAM is the budget here (tools/memory_budget_check.py). */
static struct {
    uint32_t nominal_q14;        /* SYSCLK cycles per host frame at 0 ppb, Q14 */
    uint32_t pllcfgr;            /* the clock tree the state was built on */
    uint32_t base_frame, base_stamp; /* the prediction's origin */
    float base_frac;
    float y;                     /* cycles per frame over nominal */
    float lo, sum;               /* this group: earliest residual, band sum */
    union {
        float acq_value;         /* acquiring: the first group's mean */
        float coast_m;           /* locked: the last group's mean */
    };
    uint32_t frame, last_frame;  /* host frames, unwrapped from the 11-bit FN */
    uint32_t tick;               /* HAL tick of the last group taken */
    volatile int32_t ppb_nco;    /* published: what the NCOs follow, ppb */
    uint32_t entry_stamp;        /* the SOF as USB_LP_IRQHandler saw it */
    uint16_t entry_fn, fn_last, dropped;
    uint8_t count, used, state, acq_n, acq_at, dropped_run, acquisitions;
    volatile uint8_t entry_valid;
    uint8_t follow;
    /* Written by the SOF interrupt, and `masked` by the RF DMA interrupts
     * too: the same priority, neither preempts the other. */
    uint8_t ever : 1, started : 1;
    uint8_t settle : 2;               /* after a coast: steady groups in a row */
    uint8_t sws : 2;                  /* the clock switch the state was built on */
    uint8_t masked : 1;               /* the pending SOF waited behind an RF DMA interrupt */
    uint8_t stepped;                  /* groups the step gate coasted (wraps) */
    uint8_t absorbed;                 /* host steps taken as a new origin (wraps) */
    uint8_t alpha_pc, step;           /* REF_SOF_ALPHA x 100 and REF_SOF_STEP: `clock sof A S` */
    uint8_t rf_frames;                /* host frames since an RF DMA interrupt ran (saturates) */
    uint8_t probation;                /* groups still to pass before an acquisition is published */
    uint32_t rf_tick;                 /* HAL tick of the last RF DMA interrupt */
} ref_sof = { .follow = 1U, .alpha_pc = (uint8_t)(REF_SOF_ALPHA * 100.0f), .step = (uint8_t)REF_SOF_STEP,
              .rf_frames = 0xFFU };
#if REF_SOF_BENCH
/* BENCH: every SOF's stamp by host frame, read over SWD (ref_sof_raw_frame is
 * the newest frame written). */
#define REF_SOF_RAW 512U
uint32_t ref_sof_raw[REF_SOF_RAW] __attribute__((section(".ccmram_bss")));
volatile uint32_t ref_sof_raw_frame;
#endif

void RADIO_RefSofEntry(uint32_t stamp, uint16_t fn)
{
    ref_sof.entry_stamp = stamp;
    ref_sof.entry_fn = fn;
    ref_sof.entry_valid = ref_sof.masked ? 2U : 1U;
    ref_sof.masked = 0U;
}

/* The RF DMA interrupts share USB's priority and come first when both are
 * pending (lower IRQ numbers). While the transport runs -- transmitting, or
 * with PTT held -- each spends ~140 us interleaving a frame, and the TX
 * DMA's rate follows the USB audio's, so a SOF that falls in that window
 * stays in it: every stamp of every group late by the interrupt, timed by
 * this board's own clock. The tracker took that for the reference (bench,
 * 2026-10-02, SWD PC samples): it coasted, acquired again on those stamps
 * and published -0.1 ppm for +17 for two seconds. A SOF still pending as an
 * RF DMA interrupt returns is marked, and its stamp is not taken. */
void RADIO_RefSofAfterIsr(void)
{
    if ((USB->ISTR & USB_ISTR_SOF) != 0U) ref_sof.masked = 1U;
    ref_sof.rf_frames = 0U;
    ref_sof.rf_tick = HAL_GetTick();
}

/* Every other interrupt that can hold the USB one off returns through here:
 * the ADC scan at priority 0 (every 250 us; it preempts USB too) and the
 * audio, LCD and USB-PD ones at USB's own priority. A SOF still pending then
 * waited behind it, and its stamp is not taken. The SOF walks through the
 * ADC interrupt's few microseconds once every 250 us / (XO - host), ~13 s at
 * +19 ppm, and stays in it ~300 ms: those groups came late as a whole and
 * coasted, moved the origin (`absorbed`, one every ~9 s in receive) or fed an
 * acquisition (bench, 2026-10-04). Not while the USB interrupt is itself
 * active: the ADC's preempts it after the stamp, and the SOF it would mark
 * is the next one. Unlike RADIO_RefSofAfterIsr it says nothing about the RF
 * stream. `masked` shares a byte with bits the USB interrupt writes, and
 * that is why: with USB inactive nothing below priority 0 is mid-way through
 * that byte except the RF DMA interrupts, which only ever set the same bit. */
void RADIO_RefSofIsrReturn(void)
{
    if ((USB->ISTR & USB_ISTR_SOF) != 0U &&
        (NVIC->IABR[(uint32_t)USB_LP_IRQn >> 5U] & (1UL << ((uint32_t)USB_LP_IRQn & 31U))) == 0U)
        ref_sof.masked = 1U;
}

/* The RF stream runs: its interrupts came within the last few host frames. */
static uint8_t ref_sof_rf_streaming(void) { return (uint8_t)(ref_sof.rf_frames < REF_SOF_RF_FRAMES); }

uint32_t RADIO_RefSofFrame(void) { return ref_sof.frame; }

static float ref_sof_nominal_f(void) { return (float)ref_sof.nominal_q14 * (1.0f / 16384.0f); }

/* SYSCLK per host frame at 0 ppb from the PLL as it is set (not the CMSIS
 * figure, which truncates 8 MHz / 3). */
static void ref_sof_nominal(void)
{
    const uint32_t c = RCC->PLLCFGR;
    const uint32_t m = ((c & RCC_PLLCFGR_PLLM) >> RCC_PLLCFGR_PLLM_Pos) + 1U;
    const uint32_t n = (c & RCC_PLLCFGR_PLLN) >> RCC_PLLCFGR_PLLN_Pos;
    const uint32_t r = 2U * (((c & RCC_PLLCFGR_PLLR) >> RCC_PLLCFGR_PLLR_Pos) + 1U);
    const uint8_t pll = (RCC->CFGR & RCC_CFGR_SWS) == RCC_CFGR_SWS_PLL;
    /* HSE x N / (M R) per second, a thousandth of it per frame; Q14 is
     * 0.4 ppb, which the loop's frequency takes up. */
    ref_sof.nominal_q14 = (uint32_t)(pll ? ((((uint64_t)(CLOCK_HSE_NOMINAL_HZ / 1000UL) * n) << 14) + (m * r) / 2U) /
                                               ((uint64_t)m * r)
                                         : (((uint64_t)SystemCoreClock << 14) + 500U) / 1000U);
    ref_sof.pllcfgr = c;
    ref_sof.sws = (uint8_t)((RCC->CFGR & RCC_CFGR_SWS) >> RCC_CFGR_SWS_Pos);
}

/* Frame `frame`'s predicted stamp: base_stamp + *whole, plus the return. */
static float ref_sof_predict(uint32_t frame, uint32_t *whole)
{
    const uint32_t k = frame - ref_sof.base_frame;
    const uint64_t acc = (uint64_t)k * ref_sof.nominal_q14;
    *whole = (uint32_t)(acc >> 14);
    return (float)(uint32_t)(acc & 0x3FFFU) * (1.0f / 16384.0f) + ref_sof.base_frac + (float)k * ref_sof.y;
}

/* Move the origin to `frame`, at its prediction plus `add` cycles. */
static void ref_sof_rebase(uint32_t frame, float add)
{
    uint32_t whole;
    const float f = ref_sof_predict(frame, &whole) + add;
    int32_t fl = (int32_t)f;            /* floor, without libm */
    if ((float)fl > f) --fl;
    ref_sof.base_stamp += whole + (uint32_t)fl;
    ref_sof.base_frac = f - (float)fl;
    ref_sof.base_frame = frame;
}

static void ref_sof_acquire(uint32_t frame, uint32_t stamp)
{
    ref_sof.state = REF_ACQUIRE;
    ref_sof.base_frame = frame;
    ref_sof.base_stamp = stamp;
    ref_sof.base_frac = 0.0f;
    ref_sof.acq_n = 0U;
    ref_sof.count = 0U;
    ref_sof.dropped_run = 0U;
    ++ref_sof.acquisitions;
}

static int32_t ref_sof_round(float v) { return (int32_t)(v >= 0.0f ? v + 0.5f : v - 0.5f); }

static void ref_sof_publish(float nco)
{
    if (ref_sof.probation) return;
    ref_sof.ppb_nco = ref_sof_round(nco * (1.0e9f / ref_sof_nominal_f()));
}

/* A group is complete: m is the mean of its residuals within REF_SOF_BAND of
 * the earliest, `end` its last frame. */
static void ref_sof_close(void)
{
    if (!ref_sof.count) return;
    const float m = ref_sof.sum / (float)ref_sof.used;
    const uint32_t end = ref_sof.last_frame;
    const uint8_t taken = ref_sof.count;
    ref_sof.count = 0U;
    if (ref_sof.state == REF_ACQUIRE) {
        /* The slope between the first group and one REF_SOF_ACQUIRE later
         * fixes the frequency; the frequency so far may be anything, so
         * nothing is gated yet. Each mean is taken at its group's middle:
         * a wrong frequency biases both the same way.
         *
         * A group that took fewer than half its frames' stamps starts the
         * acquisition again. Through a transmission most stamps come late
         * and are not taken (RADIO_RefSofAfterIsr): one acquisition there
         * ran 5.4 s on the few left and published -4.2 ppm for +19 (bench,
         * 2026-10-03, WSPR on 40 m: the carrier ~160 Hz off for a second,
         * ~135 Hz for two more). A first group that sparse can also close
         * more than 255 frames after the base, past acq_at's eight bits.
         * (Not `used`: the frequency is not known yet, so the stamps walk
         * out of the band within a group -- 3 cycles a frame at +19 ppm.) */
        if (taken < REF_SOF_GROUP / 2U) { ref_sof.state = REF_IDLE; return; }
        const uint32_t mid = end - (REF_SOF_GROUP - 1U) / 2U;
        if (ref_sof.acq_n++ == 0U) {
            ref_sof.acq_value = m;
            ref_sof.acq_at = (uint8_t)(mid - ref_sof.base_frame); /* the base stays put while acquiring */
            return;
        }
        if (ref_sof.acq_n <= REF_SOF_ACQUIRE) return;
        const float b = (m - ref_sof.acq_value) / (float)(mid - (ref_sof.base_frame + ref_sof.acq_at));
        ref_sof_rebase(end, m + b * (float)(end - mid));
        ref_sof.y += b;
        ref_sof.state = REF_LOCKED;
        /* Once a figure stands, a slope that lands far from it waits for
         * REF_SOF_PROBATION groups inside the step gate before anything
         * follows it. A slope fitted to stamps pinned behind an interrupt
         * is wrong by up to the whole offset (+19 ppm -- 270 Hz at 14 MHz)
         * and no later group agrees with it: it coasts until it is acquired
         * again, with the old figure still published. A right one costs
         * 100 ms. A real move that size only follows a transmission's heat
         * (+6 ppm in a minute, bench 2026-10-04), which the old figure held
         * through anyway. */
        const float moved = ref_sof.y * (1.0e9f / ref_sof_nominal_f()) - (float)ref_sof.ppb_nco;
        ref_sof.probation = (uint8_t)((ref_sof.ever && (moved > REF_SOF_PROBATION_PPB ||
                                                        moved < -REF_SOF_PROBATION_PPB))
                                      ? REF_SOF_PROBATION : 0U);
        ref_sof.ever = 1U;
        ref_sof.dropped_run = 0U;
        ref_sof.tick = HAL_GetTick();
        ref_sof_publish(ref_sof.y);
        return;
    }
    /* After a coast, a group counts once REF_SOF_SETTLE in a row have each
     * agreed with the one before: a slewing host never does. Locked, the
     * first group past the step gate coasts, whatever follows it. */
    const uint8_t coasting = (uint8_t)(ref_sof.dropped_run != 0U);
    const float d = m - ref_sof.coast_m;
    if (!coasting) ref_sof.settle = 0U;
    else if (d > REF_SOF_AGREE || d < -REF_SOF_AGREE) ref_sof.settle = 0U;
    else if (ref_sof.settle < REF_SOF_SETTLE) ++ref_sof.settle;
    ref_sof.coast_m = m;
    const uint8_t settled = (uint8_t)(coasting && ref_sof.settle >= REF_SOF_SETTLE);
    const float step = (float)ref_sof.step;
    if ((!coasting || settled) && m <= step && m >= -step) {
        const float phase = (float)ref_sof.alpha_pc * 0.01f * m;
        ref_sof_rebase(end, phase);
        ref_sof.y += REF_SOF_BETA * m * (1.0f / (float)REF_SOF_GROUP);
        ref_sof.dropped_run = 0U;
        ref_sof.tick = HAL_GetTick();
        if (ref_sof.probation) --ref_sof.probation;
        ref_sof_publish(ref_sof.y + phase * (1.0f / (float)REF_SOF_GROUP));
    } else if (settled) {
        /* Steady, but further off than the reference moves while coasting:
         * the host's SOF stepped. Its new timing is the origin from here;
         * the frequency and everything published stay as they were. */
        ref_sof_rebase(end, m);
        ++ref_sof.absorbed;
        ref_sof.dropped_run = 0U;
        ref_sof.tick = HAL_GetTick();
        ref_sof_publish(ref_sof.y);
    } else {
        if (m <= REF_SOF_GATE && m >= -REF_SOF_GATE) ++ref_sof.stepped;
        /* Coast on the frequency; a lock lost this long is acquired again
         * (a slope that a host step spoiled ends here too). */
        ref_sof_rebase(end, 0.0f);
        ++ref_sof.dropped;
        ref_sof_publish(ref_sof.y);
        if (ref_sof.dropped_run < 0xFFU) ++ref_sof.dropped_run;
        /* Not while the RF stream runs, though: then the SOF walks through
         * its interrupts once every ~50 s (the TX frames tick on this
         * board's clock, the SOF on the host's), every stamp comes late for
         * several seconds, and no slope taken from them is the reference.
         * The frequency holds; once the stream stops, the stamps decide. */
        if ((ref_sof.dropped_run >= REF_SOF_COAST) && !ref_sof_rf_streaming()) ref_sof.state = REF_IDLE;
    }
}

void RADIO_RefSofTick(uint16_t fn)
{
    uint32_t now;
    uint8_t late = 0U;
    if (ref_sof.entry_valid) {
        now = ref_sof.entry_stamp;
        fn = ref_sof.entry_fn;
        late = (uint8_t)(ref_sof.entry_valid == 2U);
        ref_sof.entry_valid = 0U;
    } else now = TIM5->CNT;
    if (ref_sof.started) ref_sof.frame += (uint32_t)((uint16_t)(fn - ref_sof.fn_last) & 0x7FFU);
    else { ref_sof.frame = fn; ref_sof.started = 1U; }
    ref_sof.fn_last = fn;
    const uint32_t frame = ref_sof.frame;
#if REF_SOF_BENCH
    ref_sof_raw[frame & (REF_SOF_RAW - 1U)] = now;
    ref_sof_raw_frame = frame;
#endif
    if (late) {
        /* No stamp from this frame (RADIO_RefSofAfterIsr): its group goes on
         * without it, and closes as it would. */
        if (ref_sof.count && frame / REF_SOF_GROUP != ref_sof.last_frame / REF_SOF_GROUP) ref_sof_close();
        ref_sof.last_frame = frame;
        if (frame % REF_SOF_GROUP == REF_SOF_GROUP - 1U) ref_sof_close();
        return;
    }
    if (ref_sof.rf_frames < 0xFFU) ++ref_sof.rf_frames;
    /* A PLL retune (TIM5 now counts another SYSCLK), the first SOF, a host
     * that stopped sending them, or a lock lost: acquire again. The error in
     * ppb is the same on every PLL setting, so the frequency carries over. */
    if (ref_sof.pllcfgr != RCC->PLLCFGR || ref_sof.sws != ((RCC->CFGR & RCC_CFGR_SWS) >> RCC_CFGR_SWS_Pos) ||
        ref_sof.nominal_q14 == 0U) {
        const float ppb = ref_sof.nominal_q14 ? ref_sof.y * (1.0e9f / ref_sof_nominal_f()) : 0.0f;
        ref_sof_nominal();
        ref_sof.y = ppb * ref_sof_nominal_f() * 1.0e-9f;
        ref_sof_acquire(frame, now);
    } else if (ref_sof.state != REF_LOCKED && ref_sof_rf_streaming()) {
        /* Nothing is acquired from stamps the RF stream delays (see
         * ref_sof_close): the last figure stands until it stops. */
        ref_sof.state = REF_IDLE;
        ref_sof.count = 0U;
        ref_sof.last_frame = frame;
        return;
    } else if (ref_sof.state == REF_IDLE || (frame - ref_sof.last_frame) > 100U) {
        ref_sof_acquire(frame, now);
    }
    /* A group ends at a multiple of REF_SOF_GROUP, or when a frame of the
     * next one arrives first. */
    if (ref_sof.count && frame / REF_SOF_GROUP != ref_sof.last_frame / REF_SOF_GROUP) ref_sof_close();
    ref_sof.last_frame = frame;
    uint32_t whole;
    const float p = ref_sof_predict(frame, &whole);
    const float r = (float)(int32_t)(now - ref_sof.base_stamp - whole) - p;
    /* The band mean without keeping the residuals: one further below than
     * the band starts it again; the on-time stamps sit within a few cycles
     * of each other, the late ones far above. */
    if (!ref_sof.count || r < ref_sof.lo - REF_SOF_BAND) { ref_sof.lo = r; ref_sof.sum = r; ref_sof.used = 1U; }
    else {
        if (r < ref_sof.lo) ref_sof.lo = r;
        if (r <= ref_sof.lo + REF_SOF_BAND) { ref_sof.sum += r; ++ref_sof.used; }
    }
    ++ref_sof.count;
    if (frame % REF_SOF_GROUP == REF_SOF_GROUP - 1U) ref_sof_close();
}

/* Whether the host's figure stands: locked once, heard within the hold, and
 * inside REF CAL's own limit. The transmit carrier follows it too, and no
 * band-edge or filter check sees the trim: a broken host or a spoiled slope
 * must not move the carriers further than REF CAL could. */
static uint8_t ref_sof_have(void)
{
    const int32_t ppb = ref_sof.ppb_nco;
    /* The hold runs from the later of the last group taken and the last RF
     * DMA interrupt. Through a transmission the tracker may take nothing for
     * long -- stamps late behind the RF interrupts, or a PLL retune at
     * key-down that leaves it idle until the stream stops -- and timing out
     * there dropped the carrier to REF CAL mid-over: +19 ppm, ~134 Hz at
     * 7 MHz, on a WSPR over's two minutes. After the stream the old figure
     * stands until the stamps decide again. */
    const uint32_t now = HAL_GetTick();
    return (uint8_t)(ref_sof.ever &&
                     ((now - ref_sof.tick) < REF_SOF_HOLD_MS || (now - ref_sof.rf_tick) < REF_SOF_HOLD_MS) &&
                     ppb <= RADIO_REF_PPB_LIMIT && ppb >= -RADIO_REF_PPB_LIMIT);
}

uint8_t RADIO_RefUsbPpb(int32_t *ppb)
{
    const uint8_t ok = ref_sof_have();
    if (ok && ppb) *ppb = ref_sof_round(ref_sof.y * (1.0e9f / ref_sof_nominal_f()));
    return ok;
}

void RADIO_RefFollowEnable(uint8_t on) { ref_sof.follow = on ? 1U : 0U; }

void RADIO_RefSofTune(uint16_t alpha_percent, uint16_t step_cycles)
{
    if (alpha_percent >= 30U && alpha_percent <= 100U) ref_sof.alpha_pc = (uint8_t)alpha_percent;
    if (step_cycles >= 2U && step_cycles <= 25U) ref_sof.step = (uint8_t)step_cycles;
}
uint8_t RADIO_RefFollowEnabled(void) { return ref_sof.follow; }

/* What the receive and transmit NCOs add to the plan, which already holds REF
 * CAL: the host's figure less REF CAL. None while a calibration or sweep
 * runs -- their test carrier comes from the same reference and must stay
 * where the plan puts it -- or with no host. */
int32_t RADIO_RefTrimPpb(void)
{
    if (!ref_sof.follow || RADIO_CalActive() || !ref_sof_have()) return 0;
    return ref_sof.ppb_nco - RADIO_Config()->ref_ppb;
}

void RADIO_RefSofStatus(radio_ref_sof_status_t *s)
{
    s->state = ref_sof.state;
    s->have = ref_sof_have();
    s->ppb = ref_sof.nominal_q14 ? ref_sof_round(ref_sof.y * (1.0e9f / ref_sof_nominal_f())) : 0;
    s->ppb_nco = ref_sof.ppb_nco;
    s->dropped = ref_sof.dropped;
    s->acquisitions = ref_sof.acquisitions;
    s->stepped = ref_sof.stepped;
    s->absorbed = ref_sof.absorbed;
    s->alpha_percent = ref_sof.alpha_pc;
    s->step_cycles = ref_sof.step;
    s->temp_mc = RADIO_RefTempMilliC();
}

/* The MCU's own temperature, from ADC5's internal sensor. The DSC1003 sits
 * beside the MCU and moves with the board's temperature by ppm (bench,
 * 2026-10-04: +6 ppm under a hair dryer), which the USB host cannot tell
 * apart from its own clock moving (the bench laptop's SOF ran 4.5 ppm slow
 * as its CPU warmed by 17 C) -- the board's temperature is the one figure
 * that belongs to this oscillator alone.
 *
 * Main loop only, a few register accesses a pass: a conversion every
 * REF_TEMP_EVERY_MS, REF_TEMP_SUM of them averaged into ref_temp_mc.
 * ADC5 shares ADC345_COMMON with the microphones' ADC3, and the sensor path
 * and clock mode can only be set with all three off: that happens once, and
 * microphone_start leaves a clock mode that is already right alone. */
#define REF_TEMP_EVERY_MS 10U
#define REF_TEMP_SUM      64U
#define REF_TS_CAL1 (*(const uint16_t *)0x1FFF75A8UL) /* at 30 C, VDDA 3.0 V */
#define REF_TS_CAL2 (*(const uint16_t *)0x1FFF75CAUL) /* at 130 C */
enum { TEMP_OFF, TEMP_REGULATOR, TEMP_CALIBRATING, TEMP_ENABLING, TEMP_READY, TEMP_CONVERTING, TEMP_FAILED };
static struct {
    uint32_t sum, at;
    uint8_t n, state;
} ref_temp;
volatile int32_t ref_temp_mc = INT32_MIN; /* milli-degrees C; INT32_MIN until the first average */

int32_t RADIO_RefTempMilliC(void) { return ref_temp_mc; }

void RADIO_RefTempService(uint32_t now)
{
    switch (ref_temp.state) {
    case TEMP_OFF:
        if ((ADC3->CR | ADC4->CR | ADC5->CR) & ADC_CR_ADEN) return; /* the common register is locked */
        __HAL_RCC_ADC345_CLK_ENABLE();
        ADC345_COMMON->CCR = (ADC345_COMMON->CCR & ~(ADC_CCR_CKMODE | ADC_CCR_PRESC)) |
                             ADC_CCR_CKMODE_0 | ADC_CCR_CKMODE_1 | ADC_CCR_VSENSESEL;
        ADC5->CR &= ~ADC_CR_DEEPPWD;
        ADC5->CR |= ADC_CR_ADVREGEN;
        ref_temp.at = now;
        ref_temp.state = TEMP_REGULATOR;
        return;
    case TEMP_REGULATOR:              /* 20 us needed; the tick gives 1-2 ms */
        if (now - ref_temp.at < 2U) return;
        ADC5->CR &= ~ADC_CR_ADCALDIF;
        ADC5->CR |= ADC_CR_ADCAL;
        ref_temp.at = now;
        ref_temp.state = TEMP_CALIBRATING;
        return;
    case TEMP_CALIBRATING:
        if (ADC5->CR & ADC_CR_ADCAL) {
            if (now - ref_temp.at > 20U) ref_temp.state = TEMP_FAILED;
            return;
        }
        ADC5->CFGR = ADC_CFGR_OVRMOD;  /* single software-started conversions, 12 bits */
        ADC5->CFGR2 = 0U;
        /* The sensor wants 5 us of sampling: 640.5 cycles of HCLK/4 is ~15. */
        ADC5->SMPR1 = (ADC5->SMPR1 & ~ADC_SMPR1_SMP4) | ADC_SMPR1_SMP4;
        ADC5->SQR1 = 4UL << ADC_SQR1_SQ1_Pos;
        ADC5->ISR = ADC_ISR_ADRDY | ADC_ISR_EOC | ADC_ISR_EOS | ADC_ISR_OVR;
        ADC5->CR |= ADC_CR_ADEN;
        ref_temp.at = now;
        ref_temp.state = TEMP_ENABLING;
        return;
    case TEMP_ENABLING:
        if (!(ADC5->ISR & ADC_ISR_ADRDY)) {
            if (now - ref_temp.at > 20U) ref_temp.state = TEMP_FAILED;
            return;
        }
        ADC5->ISR = ADC_ISR_ADRDY;
        ref_temp.state = TEMP_READY;
        return;
    case TEMP_READY:
        if (now - ref_temp.at < REF_TEMP_EVERY_MS) return;
        ref_temp.at = now;
        ADC5->CR |= ADC_CR_ADSTART;
        ref_temp.state = TEMP_CONVERTING;
        return;
    case TEMP_CONVERTING: {
        if (!(ADC5->ISR & ADC_ISR_EOC)) {
            if (now - ref_temp.at > 20U) ref_temp.state = TEMP_READY; /* lost: start another */
            return;
        }
        ref_temp.sum += ADC5->DR & 0xFFFU;
        ADC5->ISR = ADC_ISR_EOC | ADC_ISR_EOS;
        ref_temp.state = TEMP_READY;
        if (++ref_temp.n < REF_TEMP_SUM) return;
        const float vdda = MOD_GetVrefMillivolts() ? (float)MOD_GetVrefMillivolts() : 3300.0f;
        const float code = (float)ref_temp.sum * (1.0f / (float)REF_TEMP_SUM) * vdda * (1.0f / 3000.0f);
        const int32_t span = (int32_t)REF_TS_CAL2 - (int32_t)REF_TS_CAL1;
        ref_temp.sum = 0U;
        ref_temp.n = 0U;
        if (span > 0) ref_temp_mc = (int32_t)(30000.0f + 100000.0f * (code - (float)REF_TS_CAL1) / (float)span);
        return;
    }
    default:
        return;
    }
}
#else
/* Host builds: no USB, no TIM5 -- no host figure, nothing to follow. */
void RADIO_RefSofEntry(uint32_t stamp, uint16_t fn) { (void)stamp; (void)fn; }
void RADIO_RefSofAfterIsr(void) { }
void RADIO_RefSofIsrReturn(void) { }
void RADIO_RefSofTick(uint16_t fn) { (void)fn; }
uint32_t RADIO_RefSofFrame(void) { return 0U; }
uint8_t RADIO_RefUsbPpb(int32_t *ppb) { (void)ppb; return 0U; }
void RADIO_RefFollowEnable(uint8_t on) { (void)on; }
uint8_t RADIO_RefFollowEnabled(void) { return 0U; }
int32_t RADIO_RefTrimPpb(void) { return 0; }
void RADIO_RefSofStatus(radio_ref_sof_status_t *s) { memset(s, 0, sizeof(*s)); }
void RADIO_RefTempService(uint32_t now) { (void)now; }
int32_t RADIO_RefTempMilliC(void) { return INT32_MIN; }
#endif

uint8_t RADIO_RxClockQuality(void) { return rx_hz ? rx_quality : 0U; }
void RADIO_RxClockGeometry(radio_rx_geometry_t *g) { *g = rx_geometry; }

/* TEST_CLK on HRTIM timer D at period p. The caller holds the critical
 * section and has checked that nothing is transmitting. */
static uint8_t test_clock_program(uint32_t p)
{
    if (p < 192U || p > 0xFFDEU) return 0U;
    timer_init(HRTIM_TIMERINDEX_TIMER_D, p);
    HRTIM_Timerx_TypeDef *d = &HRTIM1->sTimerxRegs[HRTIM_TIMERINDEX_TIMER_D];
    d->SETx1R = HRTIM_OUTPUTSET_TIMPER;
    d->RSTx1R = HRTIM_OUTPUTRESET_TIMCMP1;
    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~(15UL << 24U)) | (13UL << 24U);
    GPIOB->OSPEEDR |= 3UL << 28U;
    HRTIM1->sCommonRegs.CR2 = HRTIM_CR2_TDRST;
    HRTIM1->sMasterRegs.MCR |= HRTIM_TIMERID_TIMER_D;
    HRTIM1->sCommonRegs.OENR = BOARD_TEST_OUTPUT;
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << 28U)) | (2UL << 28U);
    return 1U;
}

static uint8_t test_clock_allowed(void)
{
    return (uint8_t)!(BOARD_RfActive() || BOARD_EmergencyLatched() ||
                      RADIO_State() != RADIO_RECEIVE);
}

uint8_t RADIO_TestClockStart(uint32_t hz)
{
    RADIO_TestClockStop();
    uint32_t pm = clocks_enter_critical();
    uint8_t started = 0U;
    if (test_clock_allowed() && hz >= 1000000UL && hz <= RADIO_RX_MAX_HZ) {
        /* Even periods only: CMP1 at p/2 is then an exact half. */
        started = test_clock_program(
            (uint32_t)((hr_clock_hz() / 2U + hz / 2U) / hz) * 2U);
    }
    clocks_exit_critical(pm);
    return started;
}

/* The I/Q calibration needs the injected carrier a known, small offset from
 * the LO, which is a period a tick or two from the LO's own. One tick is about
 * 9 kHz at 7 MHz, so a frequency request cannot express it and the period goes
 * in directly. Odd periods are allowed here: a half-tick of duty asymmetry
 * only adds even harmonics of the injection, thousands of bins from the two
 * the correlator reads. */
uint8_t RADIO_TestClockStartPeriod(uint32_t p)
{
    RADIO_TestClockStop();
    uint32_t pm = clocks_enter_critical();
    uint8_t started = test_clock_allowed() ? test_clock_program(p) : 0U;
    clocks_exit_critical(pm);
    return started;
}
