#include "radio_audio.h"
#include "radio_mic_front.h"
#include "radio_mic_monitor.h"
#include "radio.h"
#include "radio_clocks.h"
#include "radio_dsp.h"
#include "radio_rx_front.h"
#include "radio_cw_decoder.h"
#include "radio_bpf_sweep.h"
#include "radio_iq_fit.h"
#include "radio_spectrum_dsp.h"
#include "board_rev1.h"
#include "sd_file_browser.h"
#include "modulator.h"
#include "mod_cw_player.h"
#include "stm32g4xx_ll_adc.h"
#include "stm32g4xx_ll_spi.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

/* One I2S half is RX_FRAMES / fS: 64 frames is 361 us at 177 kHz, and the DSP
 * needs about 183 us of that. 128 frames was tried and is worse: it puts the
 * I2S half (722 us) and the headphone half (776 us) almost on top of each
 * other, so the worker has to do both back to back and the pair no longer
 * fits. The deadline is one half period either way, because the DMA starts
 * overwriting this half as soon as the next one completes. */
#define RX_FRAMES 64U
#define RX_PART (RX_FRAMES / 2U) /* frames per front-end call */
#define RX_FRAMES_PER_MS ((uint32_t)(i2s_rate * 0.001f + 0.5f)) /* the decoder's clock */
#define PHONE_FRAMES 256U
/* 48 kHz levels one phone half can consume: 256 * 48000 * 576 / 165 MHz is
 * 42.9 at the lowest SYSCLK and longest reload, so 48 with margin. */
#define PHONE_LEVELS 48U
/* 256 frames: the host reader is regulated around 128 of them (47/48/49
 * frames a USB millisecond, RADIO_CapturePacket), and the receive ring
 * below took the other 1 KB this ring used to have. */
#define CAPTURE_FRAMES 256U
/* Completed receive halves waiting for the worker. The DMA interrupt copies
 * each one out of the double buffer, so a half is no longer lost the moment
 * the next one completes: the worker has RX_RING half periods to reach it
 * instead of one. See rx_block_in. */
#define RX_RING 2U
#define MIC_DMA_SAMPLES 2048U
#define MIC_DMA_HALF (MIC_DMA_SAMPLES / 2U)
/* RX Q COMB CANCEL. REV1's receive Q channel (PCM1821 channel 2) carries a
 * comb locked to the I2S frame: lines at k x fS/312 and fS/64 +- k x fS/312,
 * about 40 dB over the floor near DC, at every SYSCLK and I2S divider tried
 * (measured 2026-09-26; stopping the QSD LO drops it ~29 dB, so it rides on
 * the LO's own DC). Everything in it repeats every lcm(64, 312) = 2496 frames,
 * so a per-position running mean over that period learns it and subtracts it.
 * A real signal averages away; only one within ~0.1 Hz of a multiple of
 * fS/2496 (70.9 Hz) and steady for seconds would be notched. */
#define QCOMB_PERIOD 2496U
#define QCOMB_ALPHA  (1.0f / 128.0f) /* ~128 periods: about 1.8 s */
#define CWDEC_RING 128U /* 1 ms CW decoder blocks in flight, a power of two */

typedef struct {
    uint16_t i2s[RX_FRAMES * 8U];
    uint16_t phone[PHONE_FRAMES * 4U];
    float mono[256];
    uint16_t rx_ring[RX_RING][RX_FRAMES * 4U];
    int16_t capture[CAPTURE_FRAMES * 2U];
    /* One I2S half unpacked, and what the front end makes of it. These live
     * in the shared 38400-byte audio workspace rather than on the stack,
     * because RADIO_AudioWorker runs from PendSV inside a 1 KiB realtime
     * stack budget. */
    float block_i[RX_FRAMES], block_q[RX_FRAMES];
    float block_out_i[RADIO_RX_FRONT_MAX_OUT(RX_PART)];
    float block_out_q[RADIO_RX_FRONT_MAX_OUT(RX_PART)];
    radio_dsp_t dsp;
    radio_rx_front_t front;
    radio_phone_t phone_state;
    radio_shape_t phone_shape;
    float phone_levels[PHONE_LEVELS];
    float tone_i, tone_q, gain;
    uint32_t phone_top, mono_read, mono_write;
    /* DIGI: the headphones are off -- TIM2 parked, its outputs Hi-Z -- and
     * the 48 kHz ring is drained unread: WSJT-X has the audio, the operator
     * the LCD, and the renderer's ~8 % of the core goes to the receiver. A
     * DIGI change restarts the receiver (cw_signature), which decides again. */
    uint8_t phones_off;
    volatile uint32_t cap_read, cap_write;
    /* The receive decimation rows, copied out of flash at every start: the
     * FIR reads 96 of them per 48 kHz output. See RADIO_RxFrontCoefficientsTo. */
    float rx_coeff[RADIO_RX_COEFF_FLOATS];
    /* RX Q COMB CANCEL: the learned frame-locked pattern and its position. */
    float qcomb[QCOMB_PERIOD];
    uint32_t qcomb_index;
    /* Waterfall capture block, the zoom decimator and the engine's scratch:
     * the arena's slack, untouched unless the WATERFALL page is open. The
     * block shares its bytes with QMX 8.6.7's raw I/Q snapshot (one half as
     * the ADC delivered it): while the page holds the capture (spec_owned) a
     * snapshot is refused, and arming the capture abandons a pending one. */
    union {
        float spec[2U * RADIO_SPEC_N];
        int32_t snapshot[RX_FRAMES][2];
    };
    spec_hb_t spec_hb[2];
    uint32_t spec_scratch[RADIO_SPEC_SCRATCH_BYTES / 4U];
    /* The CW decoder's block sum in progress, and the finished 1 ms blocks
     * waiting for the main loop (cwdec_push, cwdec_service). The ring is
     * only in flight: blocks not yet decoded when the receiver stops are
     * dropped with it. 128 ms covers the longest main-loop pass (an LCD
     * frame, ~30 ms) with room to spare. Each block is I and Q as bfloat16
     * in one word (cwdec_pack): half the bytes, one store per block, and a
     * rounding error 48 dB under the block itself -- host suite cw_decoder
     * scores the same through it. */
    float cw_i, cw_q;
    uint32_t cw_n;
    uint32_t cw_ring[CWDEC_RING];
    int32_t trim_ppb;   /* rx_trim_apply: what the front end's trim holds */
    uint32_t trim_dial, dial_hz;
    /* The receive ring (rx_ring above): rx_block_in writes, the worker reads,
     * each half's chop gate and arrival time (for `lat`) travel with it. */
    volatile uint32_t ring_wr, ring_rd;
    volatile uint32_t ring_stamp[RX_RING];
    volatile uint8_t ring_chop[RX_RING];
} receiver_workspace_t;
_Static_assert(sizeof(receiver_workspace_t) <= SD_AUDIO_WORKSPACE_SIZE,
               "the receiver workspace must fit the shared SD/audio arena");

/* While the receiver is off, the QMX-style microphone DMA uses the first
 * 4096 bytes of its shared arena. The USB microphone capture ring starts
 * immediately afterwards, so DMA and the host-facing ring cannot overlap. */
_Static_assert(offsetof(receiver_workspace_t, capture) >=
               MIC_DMA_SAMPLES * sizeof(uint16_t),
               "microphone DMA must not overlap USB capture ring");

static receiver_workspace_t *receiver;
static volatile uint8_t phone_pending, mic_pending;
static volatile uint8_t audio_fault, working_channel, working_half;
static volatile uint32_t overruns;
/* Set while the audio class asks for capture frames, i.e. a host has
 * interface 2 (the receiver's stream, I/Q or audio) open at alt 1. With it
 * clear, the per-sample ring fill in receiver_process is skipped and an
 * idle receiver pays nothing for a stream nobody reads. Written in the USB
 * SOF callback, read once per I2S half in the audio worker. */
static volatile uint8_t capture_streaming;
/* What the host's capture stream got (RADIO_DigiStats): frames sent, frames
 * the ring could not supply while a receiver ran (silence padded in -- a
 * click in WSJT-X's audio), frames the worker produced into a full ring
 * (dropped), and frames sent while no receiver ran (TX, a restart). The
 * level and peak windows cover ~1 s of the stream; the ADC's own peak and
 * full-scale count come from the raw words. Counted, never reset. */
static volatile uint32_t cap_sent, cap_padded, cap_dropped, cap_silent;
static float cap_acc_sq, cap_acc_peak;
static uint32_t cap_acc_n;
static volatile float cap_level, cap_peak;
static volatile uint32_t cap_clips, adc_clips, adc_peak_word;
static uint32_t adc_acc_peak, adc_acc_halves;
static uint8_t storage_mode, microphone_source, microphone_armed;
/* Diagnostic microphone monitor: the microphone ADC into the USB capture
 * endpoint so an operator can hear, or record, what the microphone sounds
 * like. It never asks for TX and never claims the modulator's audio source,
 * which is what separates it from the PTT path below. The receiver is
 * suspended, then its workspace is borrowed for the capture ring. Bounded by
 * a deadline so a console session cannot leave the microphone ADC running. */
static uint8_t mic_monitor;
/* Console diagnostic: put the uncancelled microphone and its same-ADC
 * reference in the two capture channels rather than one cancelled mono
 * signal. That turns QRP Labs' 0.68 from an inherited constant into
 * something a host can solve for on this board. */
static uint8_t mic_monitor_split;
static uint32_t mic_monitor_until;
static uint16_t *microphone;
static uint8_t microphone_stride, microphone_workspace_owned;
/* QMX's published example subtracts 0.68 of a second channel on the same ADC
 * peripheral, which cancels its converter noise by about 24 dB. That noise is
 * not correlated across channels on REV1: a split capture of MIC2 against
 * MIC1 measures a coherence of 0.10 to 0.31 across the speech band, so the
 * best any coefficient can remove is 0.5 to 1.6 dB, and 0.68 is 0.2 dB worse
 * than none at all. The default is therefore off; `mic ref` keeps the knob,
 * and `mic split` is the measurement that decides it on a given board. */
static uint8_t microphone_reference_percent;
/* CPU-only decimator state; raw DMA samples remain in the shared SRAM arena. */
static radio_mic_front_t mic_front __attribute__((section(".ccmram_bss")));
static ADC_TypeDef *microphone_adc;
static float i2s_rate;
static uint32_t microphone_rate;
static uint32_t audio_clock, phone_top = 512U, i2s_divider;
/* AGC gain and S-meter level handed from one receiver to the next; see
 * receiver_start(). */
static float carry_agc = RADIO_DSP_AGC_MAX_GAIN, carry_level;
static float tone_cs = 1.0f, tone_sn;
static uint8_t audio_mode, rx_enabled;
/* PCM1821 powers its channels down (all-zero words) when it rejects the
 * FSYNC/BCLK rate. Watch for real data and try the other divider family. */
static volatile uint8_t rx_data_seen;
/* Receive CW decoder. The worker sums the CW channel output over
 * RADIO_CWDEC_BLOCK samples into the workspace's ring (two adds a sample,
 * one push a millisecond); RADIO_AudioService decodes it in the main loop.
 * The decoder itself lives outside the workspace so it keeps its text,
 * speed and levels across the receiver restart that every retune causes. */
static volatile uint32_t cwdec_wr, cwdec_rd, cwdec_dropped;
static uint32_t cwdec_dropped_seen;
static radio_cwdec_t cwdec;
static uint8_t cwdec_ready, cwdec_active;
static volatile uint8_t cwdec_retune;
static uint32_t cwdec_dial;
/* Seamless tuning's duck (RADIO_AudioDuckWait): the headphone gain slews to
 * zero around an in-place LO move, whose QSD step would otherwise click. */
static volatile uint8_t phone_duck;
/* The Morse trainer is open: the headphones carry its sidetone and prompts
 * only, not the receiver (RADIO_AudioRxQuiet). */
static volatile uint8_t phone_rx_quiet;
/* The PC monitor (RADIO_AudioPcMonitor, the S-CTRL LINK's MONITOR): bit 0
 * the headphone mix -- receive audio, sidetone, the trainer's prompts, SPOT,
 * before the volume -- into the USB capture in place of the receiver's
 * write, so a PC hears what the phones hear, keyed CW included; bit 1 the
 * radio's own phones silent meanwhile. Gain in 1/128ths. Never in DIGI,
 * with USB RAW IQ or the microphone monitor: WSJT-X and gqrx get their
 * streams as they are. */
static volatile uint8_t pc_monitor, pc_monitor_gain = 128U;
/* The CW receive settings the running receiver was started with: a change
 * restarts it, like a new mode. */
static uint32_t audio_cw;

/* A CW decoder block in the ring: I and Q as bfloat16, the float's top
 * half rounded to nearest even, I in the high half. */
static inline uint32_t cwdec_bf16(float x)
{
    uint32_t u;
    memcpy(&u, &x, 4U);
    return (u + 0x7FFFU + ((u >> 16) & 1U)) >> 16;
}
static inline float cwdec_float(uint32_t half)
{
    const uint32_t u = half << 16;
    float x;
    memcpy(&x, &u, 4U);
    return x;
}

static uint32_t cw_signature(const radio_config_t *c)
{
    const uint32_t cw = (uint32_t)c->cw_pitch_10hz ^ ((uint32_t)c->cw_bw_10hz << 8) ^
                        ((uint32_t)(c->cw_flags & RADIO_CW_AGC_MASK) << 16);
    const uint32_t voice = (uint32_t)c->ssb_bw_100hz ^ ((uint32_t)c->am_bw_khz << 6) ^
                           ((uint32_t)c->rx_agc << 12) ^ ((uint32_t)c->fm_squelch << 18);
    /* DIGI's passband follows the TX AUDIO INPUT and USB AUDIO POLICY. */
    return (cw ^ (voice << 7) ^ (voice >> 25)) ^ ((uint32_t)RADIO_DigiReceive() << 31);
}

/* The receive DSP as configured: mode, CW and voice receive settings. */
static void dsp_configure(radio_dsp_t *d, float offset_hz)
{
    const radio_config_t *c = RADIO_Config();
    RADIO_DspInit(d, 48000.0f, offset_hz, (radio_rx_mode_t)c->rx_mode);
    RADIO_DspSetCw(d, RADIO_ConfigCwPitchHz(c), RADIO_ConfigCwBandwidthIndex(c), RADIO_ConfigCwAgc(c));
    RADIO_DspSetVoice(d, RADIO_DigiReceive() ? (uint8_t)RADIO_SSB_BW_DIGI : RADIO_ConfigSsbBandwidthIndex(c),
                      RADIO_ConfigAmBandwidthIndex(c),
                      RADIO_ConfigRxAgc(c), c->fm_squelch);
}
static uint8_t i2s_alternate;
static uint32_t receiver_started_at, i2s_fallbacks;
/* Waterfall capture: 0 idle, 1 armed (skipping, then filling), 2 full. */
static volatile uint8_t spec_state, spec_source, spec_scan, spec_halvings;
/* The page holds the capture block, from its first arm to a stop: the I/Q
 * snapshot, which shares the block's bytes, is refused meanwhile. */
static volatile uint8_t spec_owned;
static volatile uint16_t spec_skip, spec_fill, spec_prime;
/* Until then a waterfall capture skips ahead: a receiver start (the PCM1821
 * re-locking, RX_EN, filters from zero) or an LO move is not a spectrum.
 * Arming straight after a restart painted the whole width bright. */
static uint32_t spec_settle_until;
#define SPEC_SETTLE_START_MS 30U
static float spec_out_phase; /* scan mute: 48 kHz samples owed to the ring */
static volatile uint32_t receiver_generation;
/* BPF sweep synchronous detector. The DMA IRQ records the injection state of
 * each completed 64-frame half and toggles TEST_CLK for the next one; PendSV
 * integrates the settled tail of each half. */
#define CHOP_FIRST_FRAME 40U /* > PCM1821 group delay (17/fS) + IRQ latency */
static volatile uint8_t chop_run, chop_inject, chop_on, chop_half_on[2], chop_done;
static volatile uint16_t chop_skip, chop_halves, chop_total;
static radio_chop_t chop;
#if !MOD_DEEP_DIAG_ENABLE
static volatile uint8_t snapshot_state; /* 0 idle, 1 armed, 2 ready */
static volatile uint16_t phone_stats_halves;
static volatile uint32_t phone_stats_sum, phone_stats_count;
static volatile uint32_t phone_stats_min, phone_stats_max;
/* Bounded mean power of the channel output, for the AF and image sweeps. */
static volatile uint8_t level_done;
static volatile uint16_t level_skip, level_halves;
static volatile uint32_t level_count;
static float level_sum, level_peak;
#endif
/* QSD I/Q calibration correlator (radio_rx_cal.c). Foreground arms it, PendSV
 * feeds it the raw I/Q of every frame. CPU-only state, so it goes in CCM and
 * leaves SRAM for things DMA has to reach. Absent from the deep-diagnostic
 * flavour along with the rest of the receive bench, so that image does not
 * carry a branch in its inner loop for a measurement it cannot run. */
#if !MOD_DEEP_DIAG_ENABLE
static radio_iq_corr_t iq_corr __attribute__((section(".ccmram_bss")));
static volatile uint8_t iq_run, iq_done;
static volatile uint16_t iq_skip;
#endif

/* Why the audio path gave up, split by cause. The receive DMA deadline had
 * never been exercised on hardware before REV1 came up, and `overruns` alone
 * cannot tell a missed PendSV deadline from an I2S overrun from a DMA error.
 * These are counted, never latched, and cost one increment on a path that has
 * already failed. `worker_calls` is the control: PendSV should run about
 * 2 x fS / RX_FRAMES times a second while receiving. CPU-only, so CCM: the
 * deep-diagnostic flavour has four bytes of SRAM to spare and plenty of CCM. */
static volatile uint32_t fail_pending __attribute__((section(".ccmram_bss")));
static volatile uint32_t fail_working __attribute__((section(".ccmram_bss")));
static volatile uint32_t fail_overrun __attribute__((section(".ccmram_bss")));
static volatile uint32_t fail_dma __attribute__((section(".ccmram_bss")));
static volatile uint32_t worker_calls __attribute__((section(".ccmram_bss")));

#if !MOD_DEEP_DIAG_ENABLE
/* Cycle cost of the receive path. Nothing in a shipping image measures it --
 * the deep-diagnostic profiler is not built here -- and two numbers place a
 * missed I2S deadline exactly: how long the worker runs, and how long a
 * completed half waits for it. One DWT read per half, CPU-only state in CCM. */
static volatile uint32_t worker_max_cyc __attribute__((section(".ccmram_bss")));
static volatile uint32_t latency_max_cyc __attribute__((section(".ccmram_bss")));
/* Per section, max and a 64-sample exponential mean. The mean is what decides
 * whether the receiver keeps up; a max can be one preempted outlier. */
static volatile uint32_t rx_cyc_max __attribute__((section(".ccmram_bss")));
static volatile uint32_t rx_cyc_avg __attribute__((section(".ccmram_bss")));
static volatile uint32_t ph_cyc_max __attribute__((section(".ccmram_bss")));
static volatile uint32_t ph_cyc_avg __attribute__((section(".ccmram_bss")));
/* Splitting a receive half three ways costs about 160 DWT reads per half, so
 * it is off unless someone is chasing the receive budget again. The coarse
 * per-section costs above are always on and are what operation needs. */
#ifndef RADIO_AUDIO_FINE_PROFILE
#define RADIO_AUDIO_FINE_PROFILE 0
#endif
/* Inside a receive half, split three ways. A front-end call that produces no
 * 48 kHz output is the per-sample cost alone; one that does also runs the
 * 64-tap polyphase FIR, so the difference is the filter. Summed over a half
 * and averaged, so all three are directly comparable with rx_cyc_avg. */
#if RADIO_AUDIO_FINE_PROFILE
static volatile uint32_t fe_avg __attribute__((section(".ccmram_bss")));
static volatile uint32_t demod_avg __attribute__((section(".ccmram_bss")));
#endif
static volatile uint32_t pend_cyc[3] __attribute__((section(".ccmram_bss")));
static volatile uint8_t cycles_ready __attribute__((section(".ccmram_bss")));
/* Off unless someone is measuring. The remaining cost is then one read of
 * this flag per DMA half -- not per sample -- so a shipping receiver pays
 * nothing measurable for the instrument, and that claim is testable by
 * toggling it and watching the miss rate. */
static volatile uint8_t cycles_enabled __attribute__((section(".ccmram_bss")));

static void cycles_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    cycles_ready = (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) ? 1U : 0U;
}

void RADIO_AudioProfileEnable(uint8_t on) { cycles_enabled = on ? 1U : 0U; }
uint8_t RADIO_AudioProfileEnabled(void) { return cycles_enabled; }
static void cycles_peak(volatile uint32_t *peak, uint32_t value)
{
    if (value > *peak && value < 0x10000000UL) *peak = value;
}
static void cycles_mean(volatile uint32_t *avg, uint32_t value)
{
    if (value < 0x10000000UL) *avg = *avg - (*avg >> 6) + (value >> 6);
}
#endif

/* Reprogram and start the headphone PWM in one short run of register writes.
 * With CC1E/CC2E set and the counter stopped, PA0/PA1 hold a static level;
 * receiver_start() used to enable the outputs and then spend the front-end,
 * coefficient and DSP setup (libm calls included) before setting CEN, and
 * that held level was a click in the headphones on every restart -- every
 * encoder detent. Here the window is a handful of bus writes. */
static void phone_pwm_start(uint32_t top)
{
    phone_top = top;
    TIM2->DIER = 0U;
    TIM2->CR1 = 0U;
    TIM2->PSC = 0U; TIM2->ARR = top - 1U;
    TIM2->CCR1 = TIM2->CCR2 = top / 2U;
    TIM2->CCMR1 = TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1PE |
                  TIM_CCMR1_OC2M_1 | TIM_CCMR1_OC2M_2 | TIM_CCMR1_OC2PE;
    TIM2->DCR = 13U | TIM_DCR_DBL_0; /* CCR1, CCR2: one ordered stereo burst */
    TIM2->EGR = TIM_EGR_UG;
    TIM2->CCER = TIM_CCER_CC1E | TIM_CCER_CC2E;
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~255UL) | 0x11UL;
    GPIOA->MODER = (GPIOA->MODER & ~15UL) | 10UL;
    DMA2_Channel3->CCR |= DMA_CCR_EN;
    TIM2->DIER = TIM_DIER_UDE;
    TIM2->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
}

/* Stop the headphone PWM outright, for the microphone: a 50 % carrier still
 * switches PA0/PA1 next to MIC1/MIC2. CCxE = 0 makes the outputs Hi-Z
 * (RM0440, "Output control bit for standard tim_ocx channels"), not low, so
 * the coupling capacitors keep their mid-rail charge and restarting at 50 %
 * puts no step into the headphones either way. */
static void phone_pwm_park(void)
{
    TIM2->DIER = 0U;
    DMA2_Channel3->CCR = 0U;
    TIM2->CCER = 0U;
    TIM2->CR1 &= ~TIM_CR1_CEN;
}

static void audio_stand_down(void)
{
    audio_fault = 1U;
    TIM2->DIER = 0U;
    DMA2_Channel3->CCR = 0U;
    TIM2->CCR1 = TIM2->CCR2 = phone_top / 2U;
    __DSB();
}

static void audio_fail(void)
{
    ++overruns;
    audio_stand_down();
}

static void audio_irq_pending(volatile uint8_t *pending, uint8_t half, uint8_t channel)
{
    /* A completed half is writable/readable until the NEXT boundary only. */
    if (*pending || (working_channel == channel && working_half != half)) {
        /* The transmitter has asked for the radio (RADIO_RequestTx, from the
         * first rendered RF frame) and its renderer now holds PendSV, which
         * the receive worker shares: receive and USB-audio SSB together want
         * about 80 % + 57 % of the CPU, and the main loop, starved, took
         * ~100 ms to leave RECEIVE. Every USB-audio over began with ~35
         * receive "overruns" that showed as AUDIO GLITCHES on the panel for
         * ten seconds. The receiver is stopping for TX anyway: stand it down
         * without counting a glitch. The microphone (channel 2) feeds the
         * transmitter and still counts. */
        if (channel != 2U && RADIO_TxHandover()) { audio_stand_down(); return; }
        if (*pending) ++fail_pending; else ++fail_working;
        if (channel == 2U) { audio_fail(); RADIO_Trip(RADIO_FAULT_DMA); return; }
        /* The headphones: the half the DMA has started reading again was
         * not rendered in time and plays stale -- one click. Count it and go
         * on with the half that just completed. Standing the audio path
         * down here restarted the receiver as well, a ~13 ms hole in the
         * USB capture for one late headphone half. */
        ++overruns;
    }
    *pending = (uint8_t)(1U << half);
#if !MOD_DEEP_DIAG_ENABLE
    if (cycles_enabled && cycles_ready && channel >= 2U && channel <= 4U) pend_cyc[channel - 2U] = DWT->CYCCNT;
#endif
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}

static uint8_t audio_take(volatile uint8_t *pending, unsigned half, uint8_t channel)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    uint8_t take = !audio_fault && ((*pending & (1U << half)) != 0U);
    if (take) {
        *pending &= (uint8_t)~(1U << half);
        working_half = (uint8_t)half;
        working_channel = channel;
#if !MOD_DEEP_DIAG_ENABLE
        if (cycles_enabled && cycles_ready && channel >= 2U && channel <= 4U)
            cycles_peak(&latency_max_cyc, DWT->CYCCNT - pend_cyc[channel - 2U]);
#endif
    }
    __set_PRIMASK(pm);
    return take;
}

void RADIO_AudioSuspend(void)
{
    uint8_t release_workspace = 0U;
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    if (receiver && !microphone_workspace_owned) {
        SPI3->CR2 &= ~SPI_CR2_RXDMAEN;
        SPI3->I2SCFGR &= ~SPI_I2SCFGR_I2SE;
        TIM2->DIER = 0U;
        DMA2_Channel3->CCR = 0U;
        DMA2_Channel4->CCR = 0U;
        __DSB(); /* complete DMA disables before returning its arena */
        DMA2->IFCR = DMA_IFCR_CGIF3 | DMA_IFCR_CGIF4;
        /* Idle the headphones at 50 % rather than disconnecting them: the
         * coupling capacitors stay at their operating point, so a retune, a
         * TX turn-around or a recovered fault is a silent gap, not a pop.
         * The microphone, which must not see the carrier, parks TIM2 itself
         * (phone_pwm_park). */
        TIM2->CCR1 = TIM2->CCR2 = phone_top / 2U;
        if (rx_enabled && rx_data_seen) {
            const float agc = receiver->dsp.agc, level = receiver->dsp.level;
            carry_agc = (agc >= 1.0f && agc <= RADIO_DSP_AGC_MAX_GAIN) ? agc : RADIO_DSP_AGC_MAX_GAIN;
            carry_level = (level >= 0.0f && level <= 4.0f) ? level : 0.0f;
        }
        phone_pending = 0U;
        receiver = NULL;
        rx_enabled = 0U;
        release_workspace = 1U;
        chop_on = 0U;
        chop_halves = 0U; /* a pending measurement never completes */
        spec_state = 0U;
        spec_scan = 0U;
#if !MOD_DEEP_DIAG_ENABLE
        iq_run = 0U;
        level_halves = 0U;
        snapshot_state = 0U;
        phone_stats_halves = 0U;
#endif
        RADIO_TestOutput(0U);
    }
    __set_PRIMASK(pm);
    /* No DMA or USB callback can retain a pointer when ownership is returned. */
    if (release_workspace) SD_SharedReceiverWorkspaceRelease();
}

void RADIO_AudioStorageMode(uint8_t on)
{
    storage_mode = on;
    if (on) RADIO_AudioSuspend();
}

/* Another station: the dial has moved more than the decoder's +-100 Hz
 * tracking from where it was last reset. Measured from that point, not from
 * the previous retune, so tuning across the band in 10 Hz steps still resets
 * it. */
static void cwdec_note_dial(uint32_t dial)
{
    if (dial > cwdec_dial + 100U || dial + 100U < cwdec_dial) {
        cwdec_retune = 1U;
        cwdec_dial = dial;
    }
}

/* The plan's offset for a dial, which the workspace keeps (w->dial_hz) for
 * the trim: its size in hertz is the dial times the reference's error. */
static float rx_offset_plan(receiver_workspace_t *w, uint32_t dial_hz)
{
    w->dial_hz = dial_hz;
    return RADIO_RxOffsetHz(dial_hz);
}

/* The reference's error against the USB host, followed in the receive NCO
 * (radio_clocks.c, RADIO_RefTrimPpb). The LO and the PCM1821 both run from
 * the MEMS reference, so a signal on the dial arrives dial x error away from
 * where the plan put it; turning the NCO by the same amount puts it back,
 * with no LO or receiver restart. Taken per block by the worker, so the NCO
 * follows the tracker's 10 ms updates within a block. */
static void rx_trim_apply(receiver_workspace_t *w)
{
    const int32_t trim = RADIO_RefTrimPpb();
    const uint32_t dial = w->dial_hz;
    if (trim == w->trim_ppb && dial == w->trim_dial) return;
    w->trim_ppb = trim;
    w->trim_dial = dial;
    const float d = 6.28318530718f * (float)dial * ((float)trim * 1.0e-9f) / w->front.rate;
    w->front.trim_i = cosf(d);
    w->front.trim_q = sinf(d);
}

static uint8_t receiver_start(uint8_t receive)
{
    receiver_workspace_t *w = SD_SharedReceiverWorkspaceAcquire(sizeof(*w));
    if (!w) return 0U;
    memset(w, 0, sizeof(*w));
    /* TIM2 runs from PCLK1 = SYSCLK. The reload is chosen per RF frequency so
     * PWM harmonics avoid the receive (or transmit) channel. */
    uint32_t rf_hz = RADIO_RxClockHz() ? RADIO_RxClockHz() : MOD_GetCarrierRequestedHz();
    const uint32_t top = RADIO_PhonePwmReload(SystemCoreClock, rf_hz) + 1U;
    w->phone_top = top;
    w->phones_off = RADIO_DigiReceive();
    w->phone_shape = RADIO_NoiseShapeScale(top);
    RADIO_PhoneReset(&w->phone_state, &w->phone_shape);
    /* The headphones consume 48 kHz on the same clock figure the front end
     * produces it on (i2s_rate below, from the reference-corrected SYSCLK).
     * With SystemCoreClock here, a REF CAL of +18 ppm made the consumer run
     * 0.86 samples a second slower than the producer: the 256-sample ring
     * filled in about 2.5 minutes of listening, then dropped a sample every
     * ~1.2 s -- a tick in a steady CW note. */
    w->phone_state.step = RADIO_PhoneStep(top, RADIO_SysclkHz());
    w->tone_i = 1.0f;
    /* The sidetone is the CW pitch, so a signal matched to it by ear is
     * zero-beat: the carrier goes out on the dial, and the receiver puts a
     * signal on the dial at the same pitch (RADIO_DspSetCw). */
    const float pitch = (float)RADIO_ConfigCwPitchHz(RADIO_Config());
    tone_cs = RADIO_SineTurn(pitch / 48000.0f + 0.25f);
    tone_sn = RADIO_SineTurn(pitch / 48000.0f);
    for (unsigned n = 0U; n < PHONE_FRAMES * 4U; ++n) w->phone[n] = (uint16_t)(top / 2U);
    w->mono_write = 128U; /* silence prefill gives DMA scheduling headroom */
    /* The USB capture likewise, inside RADIO_CapturePacket's 80..176 band:
     * the host reads on while the first half is taken. From empty, the first
     * packets after every start -- a mode change, the end of an over -- were
     * padding, counted as gaps (bench, v8.11). */
    w->cap_write = 160U;
#if !MOD_DEEP_DIAG_ENABLE
    if (!cycles_ready) cycles_init();
#endif
    __HAL_RCC_SPI3_CLK_ENABLE();
    __HAL_RCC_TIM2_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();
    __HAL_RCC_DMAMUX1_CLK_ENABLE();
    __HAL_RCC_I2S_CONFIG(RCC_I2SCLKSOURCE_SYSCLK);
    uint32_t divisor = RADIO_RxAdc96() ? RADIO_I2sDivider96(SystemCoreClock, i2s_alternate)
                                       : RADIO_I2sDivider(SystemCoreClock, i2s_alternate);
    i2s_divider = divisor;
    /* The rate as it really runs: the corrected reference, not the nominal
     * SystemCoreClock, so the receive NCO below lands on the true IF. */
    i2s_rate = (float)RADIO_SysclkHz() / (float)(divisor * 64UL);
    SPI3->I2SCFGR = 0U;
    __HAL_RCC_SPI3_FORCE_RESET();
    __HAL_RCC_SPI3_RELEASE_RESET();
    SPI3->CR1 = 0U;
    SPI3->I2SPR = (divisor / 2U) | ((divisor & 1U) ? SPI_I2SPR_ODD : 0U);
    SPI3->I2SCFGR = SPI_I2SCFGR_I2SMOD | LL_I2S_MODE_MASTER_RX | LL_I2S_DATAFORMAT_32B;
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~(15UL << 28U)) | (6UL << 28U);
    GPIOA->MODER = (GPIOA->MODER & ~(3UL << 30U)) | (2UL << 30U);
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~((15UL << 12U) | (15UL << 20U))) |
                    (6UL << 12U) | (6UL << 20U);
    GPIOB->MODER = (GPIOB->MODER & ~((3UL << 6U) | (3UL << 10U))) |
                   (2UL << 6U) | (2UL << 10U);
    GPIOA->OSPEEDR |= 3UL << 30U;
    GPIOB->OSPEEDR |= (3UL << 6U) | (3UL << 10U);
    DMA2_Channel4->CCR = 0U;
    DMA2_Channel4->CPAR = (uint32_t)&SPI3->DR;
    DMA2_Channel4->CMAR = (uint32_t)w->i2s;
    DMA2_Channel4->CNDTR = RX_FRAMES * 8U;
    DMAMUX1_Channel11->CCR = DMA_REQUEST_SPI3_RX;
    DMA2->IFCR = DMA_IFCR_CGIF4;
    DMA2_Channel4->CCR = DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0 |
                         DMA_CCR_PL_1 | DMA_CCR_HTIE | DMA_CCR_TCIE | DMA_CCR_TEIE;
    /* TIM2 may still be running here: RADIO_AudioSuspend leaves it idling at
     * 50 % so the headphone coupling capacitors keep their charge. Only its
     * DMA is set up now; the timer itself is reprogrammed at the very end. */
    DMA2_Channel3->CCR = 0U;
    DMA2_Channel3->CPAR = (uint32_t)&TIM2->DMAR;
    DMA2_Channel3->CMAR = (uint32_t)w->phone;
    DMA2_Channel3->CNDTR = PHONE_FRAMES * 4U;
    DMAMUX1_Channel10->CCR = DMA_REQUEST_TIM2_UP;
    DMA2->IFCR = DMA_IFCR_CGIF3;
    /* Halfword reads, word writes. TIM2 is a 32-bit timer on APB, which
     * ignores the byte lanes: a 16-bit DMA write arrives duplicated on both
     * halves of the bus, so a code of 232 became a compare of 0x00E800E8,
     * far past ARR, and held PA0/PA1 high -- DC into the NE5532 and silence
     * at any volume. A 32-bit peripheral size zero-extends each code
     * (RM0440, DMA data width). */
    DMA2_Channel3->CCR = DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_CIRC |
                         DMA_CCR_PSIZE_1 | DMA_CCR_MSIZE_0 | DMA_CCR_PL_0 |
                         DMA_CCR_HTIE | DMA_CCR_TCIE | DMA_CCR_TEIE;
    /* Exact integer residual between the tuned frequency and the running LO
     * (the planned IF); float MHz round trips would add several hertz. */
    /* dial - LO to a micro-hertz: the old whole-hertz LO left up to 0.5 Hz of
     * receive tuning error, and SystemCoreClock's truncated 8 MHz / 3 another
     * 4-6 Hz on the M = 3 PLL settings. */
    const float offset = rx_offset_plan(w, MOD_GetCarrierRequestedHz());
    RADIO_RxFrontInit(&w->front, i2s_rate, offset, RADIO_Config()->rx_skew_samples);
    RADIO_RxFrontCoefficientsTo(&w->front, w->rx_coeff);
    dsp_configure(&w->dsp, 0.0f);
    audio_cw = cw_signature(RADIO_Config());
    cwdec_wr = cwdec_rd = 0U; /* the ring was cleared with the workspace */
    cwdec_note_dial(MOD_GetCarrierRequestedHz());
    /* Every retune -- each encoder detent -- restarts the receiver. A fresh
     * DSP starts at unity gain and the AGC recovers at its slow release, so
     * the audio used to come back ~40 dB down and swell for half a second
     * after every step, and the S-meter fell to its floor. Resume from what
     * the last receiver had settled on (full gain the first time); the AGC's
     * ~1 ms attack corrects a gain that is now too high well inside the
     * headphones' 5 ms fade-in (PHONE_GAIN_SLEW from a zeroed workspace). */
    w->dsp.agc = carry_agc;
    w->dsp.level = carry_level;
    spec_state = 0U;
    spec_scan = 0U;
    spec_settle_until = HAL_GetTick() + SPEC_SETTLE_START_MS;
    phone_duck = 0U; /* a fresh start fades in from zero gain anyway */
    ++receiver_generation;
    receiver = w;
    audio_clock = SystemCoreClock; audio_mode = RADIO_Config()->rx_mode;
    phone_pending = 0U; /* the receive ring starts empty with the workspace */
    HAL_NVIC_SetPriority(DMA2_Channel3_IRQn, 1U, 0U);
    HAL_NVIC_SetPriority(DMA2_Channel4_IRQn, 1U, 0U);
    HAL_NVIC_EnableIRQ(DMA2_Channel3_IRQn);
    HAL_NVIC_EnableIRQ(DMA2_Channel4_IRQn);
    rx_enabled = receive;
    rx_data_seen = 0U;
    receiver_started_at = HAL_GetTick();
    if (receive) DMA2_Channel4->CCR |= DMA_CCR_EN;
    SPI3->CR2 = SPI_CR2_RXDMAEN;
    if (receive) SPI3->I2SCFGR |= SPI_I2SCFGR_I2SE;
    if (w->phones_off) phone_pwm_park();
    else phone_pwm_start(top);
    return 1U;
}

static void microphone_stop(void)
{
    TIM3->CR1 = 0U;
    DMA2_Channel2->CCR = 0U;
    if (microphone_adc) {
        if (microphone_adc->CR & ADC_CR_ADSTART) microphone_adc->CR |= ADC_CR_ADSTP;
        for (unsigned n = 0U; n < 100000U && (microphone_adc->CR & ADC_CR_ADSTART); ++n) __NOP();
        if (microphone_adc->CR & ADC_CR_ADEN) microphone_adc->CR |= ADC_CR_ADDIS;
        for (unsigned n = 0U; n < 100000U && (microphone_adc->CR & ADC_CR_ADEN); ++n) __NOP();
    }
    microphone_source = 0U; mic_pending = 0U; microphone_armed = 0U;
    microphone_adc = NULL;
    microphone = NULL;
    __DSB();
    if (microphone_workspace_owned) {
        uint32_t pm = __get_PRIMASK();
        __disable_irq();
        receiver = NULL;
        microphone_workspace_owned = 0U;
        __set_PRIMASK(pm);
        SD_SharedReceiverWorkspaceRelease();
    }
    if (!mic_monitor && MOD_GetAudioSource() == MOD_AUDIO_SOURCE_MIC)
        MOD_LocalAudioRelease();
}

static uint8_t microphone_workspace_acquire(void)
{
    if (receiver != NULL) return microphone_workspace_owned;
    receiver_workspace_t *w = SD_SharedReceiverWorkspaceAcquire(sizeof(*w));
    if (w == NULL) return 0U;
    /* The microphone and headphone paths are mutually exclusive on this
     * radio; the suspended receiver left TIM2 idling, so stop it now. */
    phone_pwm_park();
    memset(w, 0, sizeof(*w));
    receiver = w;
    microphone = (uint16_t *)(void *)w;
    microphone_workspace_owned = 1U;
    return 1U;
}

/* `monitor` runs the same ADC/TIM3/DMA path for the console's microphone
 * monitor: no claim on the modulator's audio source, so RADIO_AudioService
 * never follows it with RADIO_RequestTx(). */
__attribute__((cold)) static uint8_t microphone_start(uint8_t source, uint8_t monitor)
{
    if (!monitor && !MOD_LocalAudioAcquireSource(MOD_AUDIO_SOURCE_MIC)) return 0U;
    if (!microphone_workspace_acquire()) { microphone_stop(); return 0U; }
    __HAL_RCC_TIM3_CLK_ENABLE();
    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_ADC345_CLK_ENABLE();
    ADC_TypeDef *a = source == RADIO_SOURCE_MIC3 ? ADC3 : ADC1;
    microphone_adc = a;
    const unsigned channel = source == RADIO_SOURCE_MIC1 ? 3U : source == RADIO_SOURCE_MIC2 ? 4U : 2U;
    const unsigned reference = source == RADIO_SOURCE_MIC1 ? 4U : 3U;
    /* Only when it is not set already: the reference's temperature reading
     * (RADIO_RefTempService) keeps ADC5 running on the same clock, and the
     * register is locked while any of ADC3-5 is on. */
    if (a == ADC3 && (ADC345_COMMON->CCR & ADC_CCR_CKMODE) != ADC_CCR_CKMODE)
        ADC345_COMMON->CCR = ADC_CCR_CKMODE_0 | ADC_CCR_CKMODE_1;
    a->CR &= ~ADC_CR_DEEPPWD;
    a->CR |= ADC_CR_ADVREGEN;
    HAL_Delay(1U);
    a->CR &= ~ADC_CR_ADCALDIF;
    a->CR |= ADC_CR_ADCAL;
    for (unsigned n = 0U; n < 1000000U && (a->CR & ADC_CR_ADCAL); ++n) __NOP();
    if (a->CR & ADC_CR_ADCAL) { microphone_stop(); return 0U; }
    a->CFGR = ADC_CFGR_DMAEN | ADC_CFGR_DMACFG | ADC_CFGR_EXTEN_0 |
               (LL_ADC_REG_TRIG_EXT_TIM3_TRGO & ADC_CFGR_EXTSEL);
    /* QMX runs the microphone at about 700 ksps. With ADC12 synchronous /4,
     * two 12.5 + 12.5 cycle conversions take 50 ADC clocks and fit inside the
     * 243-cycle (about 59 ADC clock) trigger period; a third would not. So
     * the trigger buys exactly two conversions, and what they are spent on
     * depends on whether reference cancellation is wanted.
     *
     * With it on, they are the microphone and its reference, as QMX does it.
     * With it off -- REV1's measured default -- both go to the microphone
     * through the hardware oversampler instead. That halves the noise power
     * the converter contributes and moves the first alias band from 700 kHz
     * to 1.4 MHz, at no CPU cost, and REV1's microphone noise is measurably
     * high-frequency-heavy. Shift 1 keeps the plain 12-bit scale. ADC3 has no
     * paired microphone channel and always takes this path. */
    a->SMPR1 = 2UL << (channel * 3U);
    if (a == ADC1 && microphone_reference_percent) {
        a->CFGR2 = 0U;
        a->SMPR1 |= 2UL << (reference * 3U);
        a->SQR1 = ADC_SQR1_L_0 |
                  ((uint32_t)channel << ADC_SQR1_SQ1_Pos) |
                  ((uint32_t)reference << ADC_SQR1_SQ2_Pos);
        microphone_stride = 2U;
    } else {
        a->CFGR2 = ADC_CFGR2_ROVSE | (1UL << ADC_CFGR2_OVSS_Pos);
        a->SQR1 = (uint32_t)channel << ADC_SQR1_SQ1_Pos;
        microphone_stride = 1U;
    }
    DMA2_Channel2->CCR = 0U;
    DMA2_Channel2->CPAR = (uint32_t)&a->DR;
    DMA2_Channel2->CMAR = (uint32_t)microphone;
    DMA2_Channel2->CNDTR = MIC_DMA_SAMPLES;
    DMAMUX1_Channel9->CCR = a == ADC3 ? DMA_REQUEST_ADC3 : DMA_REQUEST_ADC1;
    DMA2->IFCR = DMA_IFCR_CGIF2;
    DMA2_Channel2->CCR = DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0 |
                         DMA_CCR_PL_1 | DMA_CCR_HTIE | DMA_CCR_TCIE | DMA_CCR_TEIE | DMA_CCR_EN;
    a->ISR = ADC_ISR_ADRDY | ADC_ISR_OVR;
    a->CR |= ADC_CR_ADEN;
    for (unsigned n = 0U; n < 1000000U && !(a->ISR & ADC_ISR_ADRDY); ++n) __NOP();
    if (!(a->ISR & ADC_ISR_ADRDY)) { microphone_stop(); return 0U; }
    microphone_adc = a; microphone_source = source; mic_pending = 0U;
    TIM3->PSC = 0U;
    TIM3->ARR = (SystemCoreClock + 350000UL) / 700000UL - 1UL;
    microphone_rate = SystemCoreClock / (TIM3->ARR + 1UL);
    RADIO_MicFrontInit(&mic_front, microphone_rate,
                       microphone_stride == 2U
                           ? (float)microphone_reference_percent * 0.01f : 0.0f);
    TIM3->CR2 = TIM_CR2_MMS_1;
    TIM3->EGR = TIM_EGR_UG;
    a->CR |= ADC_CR_ADSTART;
    HAL_NVIC_SetPriority(DMA2_Channel2_IRQn, 1U, 0U);
    HAL_NVIC_EnableIRQ(DMA2_Channel2_IRQn);
    TIM3->CR1 = TIM_CR1_CEN;
    if (!monitor) MOD_LocalAudioSetClean(0U);
    return 1U;
}

__attribute__((cold)) uint8_t RADIO_MicrophoneReference(uint8_t percent)
{
    if (percent > 100U) return 0U;
    microphone_reference_percent = percent;
    /* Restart at a DMA boundary so one decimation interval never contains two
     * coefficients. The requested monitor/PTT path is restarted by service. */
    if (microphone_adc) microphone_stop();
    return 1U;
}

uint8_t RADIO_MicrophoneReferencePercent(void)
{
    return microphone_reference_percent;
}

__attribute__((cold)) uint8_t RADIO_MicMonitorSplit(uint8_t on)
{
    /* ADC3's microphone has no paired channel on its own peripheral, so the
     * split has nothing to put in the right channel there. */
    mic_monitor_split = on ? 1U : 0U;
    return 1U;
}

uint8_t RADIO_MicMonitorSplitEnabled(void)
{
    return mic_monitor_split;
}

/* Monitored microphone frames into that capture ring. `stereo` says whether
 * `pcm` is one interleaved left/right pair per frame -- the split diagnostic
 * below -- or one mono sample that both channels carry. The receiver is
 * suspended while this runs, so there is one writer. */
static void mic_capture_push(const int16_t *pcm, unsigned frames, uint8_t stereo)
{
    receiver_workspace_t *w = receiver;
    if (w == NULL || !capture_streaming) return;
    for (unsigned n = 0U; n < frames; ++n) {
        uint32_t wr = w->cap_write;
        if (wr - w->cap_read >= CAPTURE_FRAMES) break;
        unsigned idx = (wr & (CAPTURE_FRAMES - 1U)) * 2U;
        w->capture[idx] = stereo ? pcm[n * 2U] : pcm[n];
        w->capture[idx + 1U] = stereo ? pcm[n * 2U + 1U] : pcm[n];
        __DMB(); w->cap_write = wr + 1U;
    }
}

__attribute__((cold)) uint8_t RADIO_MicMonitorStart(uint8_t source, uint32_t seconds)
{
    if (source < RADIO_SOURCE_MIC1 || source > RADIO_SOURCE_MIC3) return 0U;
    if (!seconds || seconds > 600U) return 0U;
    /* The ring belongs to a running receiver, and the PTT path must not be
     * mid-transmission holding the same ADC. */
    if (RADIO_State() != RADIO_RECEIVE || storage_mode || audio_fault) return 0U;
    if (microphone_source && !mic_monitor) return 0U;
    mic_monitor = source;
    mic_monitor_until = HAL_GetTick() + seconds * 1000U;
    return 1U;
}

__attribute__((cold)) void RADIO_MicMonitorStop(void)
{
    if (!mic_monitor) return;
    if (microphone_source) microphone_stop();   /* still "monitoring": owns nothing */
    mic_monitor = 0U;
}

/* One-shot polled microphone level, the QMX "Microphone test" (operating
 * manual 8.6.9) reduced to a measurement. Deliberately independent of the
 * audio pipeline: it neither acquires the local audio source nor starts the
 * TIM3/DMA path, so the self test can read a microphone without PTT and
 * without disturbing whatever is streaming. Returns 0 when the DMA mic path
 * owns the ADC. Main loop only -- it polls for about 12 ms.
 *
 * ADC12_COMMON is left alone: ADC2 is mid-scan on the telemetry channels and
 * shares that clock configuration. */
__attribute__((cold)) uint8_t RADIO_MicProbe(uint8_t source, radio_mic_probe_t *out)
{
    if (out == NULL) return 0U;
    if (microphone_adc || source < RADIO_SOURCE_MIC1 || source > RADIO_SOURCE_MIC3) return 0U;
    ADC_TypeDef *a = source == RADIO_SOURCE_MIC3 ? ADC3 : ADC1;
    const unsigned channel = source == RADIO_SOURCE_MIC1 ? 3U : source == RADIO_SOURCE_MIC2 ? 4U : 2U;
    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_ADC345_CLK_ENABLE();
    if (a == ADC3 && (ADC345_COMMON->CCR & ADC_CCR_CKMODE) != ADC_CCR_CKMODE)
        ADC345_COMMON->CCR = ADC_CCR_CKMODE_0 | ADC_CCR_CKMODE_1; /* as microphone_start */
    /* CFGR2 carries the hardware oversampler. Leaving it alone would let a
     * setting from another user of this ADC scale the result past 12 bits and
     * silently change what every number below means, so it is cleared here
     * and put back with the rest. */
    const uint32_t cfgr = a->CFGR, cfgr2 = a->CFGR2, smpr1 = a->SMPR1, sqr1 = a->SQR1;
    uint8_t ok = 0U;
    uint16_t lo = 4095U, hi = 0U;
    /* Deviations from the first sample, not the codes themselves: the mean of
     * a biased input is around 2300 and its RMS deviation is a few codes, so
     * accumulating the raw squares would lose the answer in the rounding of a
     * 32-bit float. Referred to a base, both sums stay small and exact. */
    float base = 0.0f, sum_dev = 0.0f, sum_dev_squares = 0.0f;
    unsigned taken = 0U;
    a->CR &= ~ADC_CR_DEEPPWD;
    a->CR |= ADC_CR_ADVREGEN;
    HAL_Delay(1U);
    if (!(a->CR & ADC_CR_ADEN)) {
        a->CR &= ~ADC_CR_ADCALDIF;
        a->CR |= ADC_CR_ADCAL;
        for (unsigned n = 0U; n < 1000000U && (a->CR & ADC_CR_ADCAL); ++n) __NOP();
    }
    if (!(a->CR & ADC_CR_ADCAL)) {
        a->CFGR = 0U;                       /* single conversion, no DMA, no trigger */
        a->CFGR2 = 0U;                      /* plain 12-bit results */
        a->SMPR1 = 6UL << (channel * 3U);   /* 247.5 cycles: settles 5 kohm */
        a->SQR1 = channel << ADC_SQR1_SQ1_Pos;
        a->ISR = ADC_ISR_ADRDY | ADC_ISR_EOC | ADC_ISR_EOS | ADC_ISR_OVR;
        a->CR |= ADC_CR_ADEN;
        for (unsigned n = 0U; n < 1000000U && !(a->ISR & ADC_ISR_ADRDY); ++n) __NOP();
        if (a->ISR & ADC_ISR_ADRDY) {
            ok = 1U;
            for (unsigned i = 0U; i < 2048U; ++i) {
                a->CR |= ADC_CR_ADSTART;
                unsigned guard = 0U;
                while (!(a->ISR & ADC_ISR_EOC) && ++guard < 100000U) __NOP();
                if (!(a->ISR & ADC_ISR_EOC)) { ok = 0U; break; }
                const uint16_t v = (uint16_t)(a->DR & 0x0FFFU);
                a->ISR = ADC_ISR_EOC | ADC_ISR_EOS;
                if (v < lo) lo = v;
                if (v > hi) hi = v;
                if (taken == 0U) base = (float)v;
                const float d = (float)v - base;
                sum_dev += d;
                sum_dev_squares += d * d;
                ++taken;
            }
        }
    }
    if (a->CR & ADC_CR_ADSTART) {
        a->CR |= ADC_CR_ADSTP;
        for (unsigned n = 0U; n < 1000000U && (a->CR & ADC_CR_ADSTART); ++n) __NOP();
    }
    if (a->CR & ADC_CR_ADEN) {
        a->CR |= ADC_CR_ADDIS;
        for (unsigned n = 0U; n < 1000000U && (a->CR & ADC_CR_ADEN); ++n) __NOP();
    }
    a->CFGR = cfgr; a->CFGR2 = cfgr2; a->SMPR1 = smpr1; a->SQR1 = sqr1;
    a->CR &= ~ADC_CR_ADVREGEN;
    a->CR |= ADC_CR_DEEPPWD;
    if (!ok || taken == 0U) return 0U;
    const float inverse_count = 1.0f / (float)taken;
    const float offset = sum_dev * inverse_count;
    /* E[d^2] - E[d]^2, clamped: the two terms are close for a quiet input and
     * float rounding can put the difference just below zero. */
    float variance = sum_dev_squares * inverse_count - offset * offset;
    if (!(variance > 0.0f)) variance = 0.0f;
    const float rms_x16 = sqrtf(variance) * 16.0f;
    out->min = lo;
    out->max = hi;
    out->mean_x16 = (uint16_t)((base + offset) * 16.0f + 0.5f);
    out->rms_x16 = rms_x16 > 65535.0f ? 65535U : (uint16_t)(rms_x16 + 0.5f);
    return 1U;
}

void RADIO_AudioStop(void)
{
    if (microphone_source || microphone_adc) microphone_stop();
    RADIO_AudioSuspend();
    phone_pwm_park();
    mic_monitor = 0U;
    audio_fault = 0U;
}

/* The decoder's main-loop half: drain the worker's 1 ms blocks. */
static void cwdec_service(void)
{
    if (!cwdec_ready) { RADIO_CwDecoderInit(&cwdec); cwdec_dropped_seen = cwdec_dropped; cwdec_ready = 1U; }
    if (cwdec_retune) { cwdec_retune = 0U; RADIO_CwDecoderRetune(&cwdec); }
    /* The search spans the CW filter (a change retunes the decoder). */
    RADIO_CwDecoderSetSpan(&cwdec, RADIO_ConfigCwBandwidthHz(RADIO_Config()));
    const receiver_workspace_t *w = receiver;
    const uint32_t wr = cwdec_wr;
    uint32_t rd = cwdec_rd;
    while (w && rd != wr) {
        const uint32_t z = w->cw_ring[rd & (CWDEC_RING - 1U)];
        (void)RADIO_CwDecoderStep(&cwdec, cwdec_float(z >> 16), cwdec_float(z & 0xFFFFU));
        ++rd;
    }
    /* Blocks the worker had to drop came after every one just read (the
     * ring was full until now): the decoder takes the hole in its place. */
    const uint32_t dropped = cwdec_dropped;
    if (dropped != cwdec_dropped_seen) {
        RADIO_CwDecoderLost(&cwdec, dropped - cwdec_dropped_seen);
        cwdec_dropped_seen = dropped;
    }
    __DMB();
    cwdec_rd = wr;
    cwdec_active = (uint8_t)(receiver && rx_enabled && RADIO_Config()->rx_mode == RADIO_RX_CW &&
                             RADIO_ConfigCwDecode(RADIO_Config()));
}

const char *RADIO_CwDecodeText(void) { return cwdec_ready ? cwdec.text : ""; }
uint32_t RADIO_CwDecodeCount(void) { return cwdec_ready ? cwdec.emitted : 0U; }
uint8_t RADIO_CwDecodeActive(void) { return cwdec_active; }
uint32_t RADIO_CwDecodeDropped(void) { return cwdec_dropped; }
void RADIO_CwDecodeStatus(uint8_t *wpm, float *offset_hz, float *snr_db, uint8_t *locked)
{
    if (wpm) *wpm = cwdec_ready ? RADIO_CwDecoderWpm(&cwdec) : 0U;
    if (offset_hz) *offset_hz = cwdec_ready ? RADIO_CwDecoderOffsetHz(&cwdec) : 0.0f;
    if (snr_db) *snr_db = cwdec_ready ? RADIO_CwDecoderSnrDb(&cwdec) : 0.0f;
    if (locked) *locked = cwdec_ready ? RADIO_CwDecoderLocked(&cwdec) : 0U;
}
void RADIO_CwDecodeClear(void)
{
    cwdec.text[0] = '\0';
    cwdec.text_len = 0U;
    cwdec.emitted = 0U; /* a reader that had seen more starts over */
}

__attribute__((cold)) void RADIO_AudioService(void)
{
    cwdec_service();
    if (RADIO_State() == RADIO_OFF) { RADIO_AudioStop(); return; }
    if (audio_fault) { RADIO_AudioStop(); return; }
    /* The receive-side bench routines need the receiver running: the RX test
     * clock, the BPF sweep's chop detector, and the self test's receive and
     * filter groups all measure through it. Everything else (bias, envelope)
     * gets a quiet receiver. */
    if (RADIO_CalActive() && !RADIO_CalUsesReceiver(RADIO_CalActive())) {
        RADIO_AudioSuspend();
        phone_pwm_park(); /* the bias/envelope bench measures with TIM2 quiet */
        return;
    }
    uint8_t mic = RADIO_Config()->mic_source;
    if (mic_monitor) {
        /* The console monitor owns the microphone and the receiver's shared
         * arena for a bounded window, but it never requests transmit. */
        const radio_mic_monitor_state_t ms = {
            .requested = mic_monitor, .running = microphone_source,
            .receiving = (uint8_t)(RADIO_State() == RADIO_RECEIVE),
            .ptt = (uint8_t)(RADIO_Ptt() ? 1U : 0U), .storage = storage_mode,
            .now_ms = HAL_GetTick(), .until_ms = mic_monitor_until,
        };
        const radio_mic_monitor_action_t act = RADIO_MicMonitorDecide(&ms);
        if (act == RADIO_MIC_MONITOR_END) {
            RADIO_MicMonitorStop();
        } else if (act == RADIO_MIC_MONITOR_START) {
            if (microphone_source) microphone_stop();
            RADIO_AudioSuspend();
            if (!microphone_start(mic_monitor, 1U))
                RADIO_MicMonitorStop();
        }
        /* Still monitoring: the receiver stays off and its restart below
         * would take the arena back out from under the ring. */
        if (mic_monitor) return;
    } else {
        if (microphone_source && microphone_source != mic) microphone_stop();
        if (!RADIO_Ptt() || !mic || RADIO_State() == RADIO_FAULT || storage_mode) {
            if (microphone_source) microphone_stop();
        } else if (!microphone_source) {
            (void)MOD_TxClockHop();   /* stops the receiver itself if it moves */
            RADIO_AudioSuspend();
            (void)microphone_start(mic, 0U);
        }
        if (microphone_source) {
            RADIO_RequestTx();
            if (!microphone_armed) microphone_armed = MOD_LocalAudioArm();
            return;
        }
    }
    /* Not while a transmission is being handed the radio (see
     * audio_irq_pending): restarting the receiver there only fails again. */
    uint8_t receive = RADIO_State() == RADIO_RECEIVE && !RADIO_TxHandover();
    uint8_t monitor = MOD_CwMonitorActive() && RADIO_State() != RADIO_FAULT && RADIO_State() != RADIO_OFF;
    if (storage_mode || (!receive && !monitor)) { RADIO_AudioSuspend(); return; }
    if (receiver && (audio_clock != SystemCoreClock || audio_mode != RADIO_Config()->rx_mode || rx_enabled != receive ||
                     audio_cw != cw_signature(RADIO_Config())))
        RADIO_AudioSuspend();
    if (receiver && rx_enabled && !rx_data_seen && HAL_GetTick() - receiver_started_at > 750U) {
        i2s_alternate ^= 1U;
        ++i2s_fallbacks;
        RADIO_AudioSuspend();
    }
    if (!receiver) (void)receiver_start(receive);
}

#if !MOD_DEEP_DIAG_ENABLE
/* One sample of channel power into the AF/image sweep window. Out of line so
 * the accumulator's own branch does not live in RADIO_AudioWorker. */
static void level_add(float power)
{
    level_sum += power;
    ++level_count;
    if (power > level_peak) level_peak = power;
}

/* QMX 8.6.7: keep one half of raw PCM1821 words. Re-reading the DMA buffer
 * here rather than storing them in the unpack loop keeps a branch that fires
 * once in a session out of a loop that runs 177,000 times a second. */
__attribute__((noinline)) static void snapshot_take(receiver_workspace_t *w, const uint16_t *raw)
{
    for (unsigned n = 0U; n < RX_FRAMES; ++n) {
        w->snapshot[n][0] = (int32_t)(((uint32_t)raw[4U*n] << 16U) | raw[4U*n+1U]);
        w->snapshot[n][1] = (int32_t)(((uint32_t)raw[4U*n+2U] << 16U) | raw[4U*n+3U]);
    }
}
#endif

/* The measurement windows that count whole I2S halves rather than samples.
 * Out of line and in one place so the worker itself stays readable: an
 * audio fault abandons every one of them by simply not advancing it. Once per
 * half, so keeping it out of line costs nothing and keeps the worker's own
 * complexity to the audio path. */
__attribute__((noinline)) static void receiver_half_end(void)
{
    if (audio_fault) return;
    if (chop_run && chop_halves) {
        if (chop_skip) --chop_skip;
        else if (--chop_halves == 0U) chop_done = 1U;
    }
    if (spec_state == 1U && spec_skip) --spec_skip;
#if !MOD_DEEP_DIAG_ENABLE
    if (snapshot_state == 1U) snapshot_state = 2U;
    if (level_halves && !level_done) {
        if (level_skip) --level_skip;
        else if (--level_halves == 0U) level_done = 1U;
    }
#endif
}

/* One CW decoder block complete: into the ring, or counted if the main loop
 * has fallen 128 ms behind. Out of line: it runs once a millisecond. */
__attribute__((noinline)) static void cwdec_push(receiver_workspace_t *w)
{
    const uint32_t wr = cwdec_wr;
    if (wr - cwdec_rd < CWDEC_RING) {
        w->cw_ring[wr & (CWDEC_RING - 1U)] = (cwdec_bf16(w->cw_i) << 16) | cwdec_bf16(w->cw_q);
        __DMB();
        cwdec_wr = wr + 1U;
    } else {
        ++cwdec_dropped;
    }
    w->cw_i = w->cw_q = 0.0f;
    w->cw_n = 0U;
}

/* A half's channel-filter outputs (receiver_process leaves them in
 * block_out) into the decoder's 1 ms sums. Out of line, like cwdec_push:
 * the worker pays one test a half for it. */
__attribute__((noinline)) static void cwdec_feed(receiver_workspace_t *w, unsigned outputs)
{
    for (unsigned k = 0U; k < outputs; ++k) {
        w->cw_i += w->block_out_i[k];
        w->cw_q += w->block_out_q[k];
        if (++w->cw_n == RADIO_CWDEC_BLOCK) cwdec_push(w);
    }
}

/* RX Q COMB CANCEL on one unpacked half, in place. Out of line so the
 * worker's own loop does not grow. */
__attribute__((noinline)) static void qcomb_cancel(receiver_workspace_t *w)
{
    unsigned k = w->qcomb_index;
    for (unsigned n = 0U; n < RX_FRAMES; ++n) {
        float *mean = &w->qcomb[k];
        *mean += QCOMB_ALPHA * (w->block_q[n] - *mean);
        w->block_q[n] -= *mean;
        if (++k == QCOMB_PERIOD) k = 0U;
    }
    w->qcomb_index = k;
}

/* Waterfall capture: append up to n samples of one stream to the block,
 * through the zoom's half-band stages if it has any. Out of line, and reached
 * only while armed, so a closed page costs the worker one flag test per half.
 * The decimators only run while a block fills, so each fill first discards
 * the outputs their stale history shapes. */
__attribute__((noinline)) static void spec_capture(receiver_workspace_t *w, const float *in_i,
                                                   const float *in_q, unsigned n)
{
    unsigned fill = spec_fill, prime = spec_prime;
    const unsigned halvings = spec_halvings;
    float *dst = w->spec;
    for (unsigned k = 0U; (k < n) && (fill < RADIO_SPEC_N); ++k) {
        float i = in_i[k], q = in_q[k];
        if (halvings) {
            if (!spec_hb_push(&w->spec_hb[0], i, q, &i, &q)) continue;
            if ((halvings > 1U) && !spec_hb_push(&w->spec_hb[1], i, q, &i, &q)) continue;
            if (prime) { --prime; continue; }
        }
        dst[2U * fill] = i;
        dst[2U * fill + 1U] = q;
        ++fill;
    }
    spec_prime = (uint16_t)prime;
    spec_fill = (uint16_t)fill;
    if (fill == RADIO_SPEC_N) spec_state = 2U;
}

/* Scanner half: what the front end would have produced, as silence, so the
 * headphone ring, a listening USB host and the CW decoder's clock keep their
 * rate (the decoder hears a fade, not a splice of before and after). A
 * paused receiver (receiver_half) keeps the headphone ring's rate with it. */
__attribute__((noinline)) static void spec_scan_silence(receiver_workspace_t *w, uint8_t capture, uint8_t cw_decode)
{
    spec_out_phase += (float)RX_FRAMES * 48000.0f / w->front.rate;
    unsigned n = (unsigned)spec_out_phase;
    spec_out_phase -= (float)n;
    for (; n; --n) {
        if (w->mono_write - w->mono_read < 256U) {
            w->mono[w->mono_write & 255U] = 0.0f; ++w->mono_write;
        }
        if (cw_decode && ++w->cw_n == RADIO_CWDEC_BLOCK) cwdec_push(w);
        if (capture) {
            uint32_t wr = w->cap_write;
            if (wr - w->cap_read < CAPTURE_FRAMES) {
                unsigned idx = (wr & (CAPTURE_FRAMES - 1U)) * 2U;
                w->capture[idx] = w->capture[idx + 1U] = 0;
                __DMB(); w->cap_write = wr + 1U;
            }
        }
    }
}

/* The waterfall's part of a receive half, once the raw block is in: a raw
 * capture takes it now; a channel capture is asked for (SPEC_HALF_OUT); and
 * a scanner half skips the front end (SPEC_HALF_SCAN) and feeds silence
 * instead. Out of line, so a closed page costs the worker one test. */
#define SPEC_HALF_OUT  1U
#define SPEC_HALF_SCAN 2U
__attribute__((noinline)) static uint8_t spec_half(receiver_workspace_t *w, uint8_t capture, uint8_t cw_decode)
{
    uint8_t out = 0U;
    if (spec_state == 1U && !spec_skip) {
        if (spec_source == RADIO_SPEC_SRC_RAW) spec_capture(w, w->block_i, w->block_q, RX_FRAMES);
        else out = SPEC_HALF_OUT;
    }
    if (spec_scan) {
        if (!audio_fault) spec_scan_silence(w, capture, cw_decode);
        return SPEC_HALF_SCAN;
    }
    return out;
}

/* The host stream's level and peak (full scale 1.0), published once about
 * a second of it has gone by; a block with a sample at full scale counts as
 * one clip. Once per front-end part, not per sample. */
static void capture_level_add(float sq, float peak, uint32_t n)
{
    cap_acc_sq += sq;
    if (peak > cap_acc_peak) cap_acc_peak = peak;
    if (peak >= 1.0f) ++cap_clips;
    cap_acc_n += n;
    if (cap_acc_n >= 48000U) {
        cap_level = cap_acc_sq / (float)cap_acc_n;
        cap_peak = cap_acc_peak;
        cap_acc_sq = cap_acc_peak = 0.0f;
        cap_acc_n = 0U;
    }
}

/* One front-end part into the USB capture ring, `l` and `r` times `gain`;
 * `level` measures the left channel for the host stream's level. The free
 * space read once (the reader only ever adds to it), one barrier and one index
 * store after the block. Out of the demodulator's loop: inside it, per sample,
 * the loop outgrew the flash cache and a recording host cost the receiver
 * ~10k cycles a half, enough for the WATERFALL guard to hold TX off; here
 * ~2.3k (bench, v8.11). */
static __attribute__((noinline)) void capture_block(receiver_workspace_t *w, const float *l, const float *r,
                                                    unsigned n, float gain, uint8_t level)
{
    uint32_t wr = w->cap_write;
    const uint32_t end = w->cap_read + CAPTURE_FRAMES;
    float sq = 0.0f, peak = 0.0f;
    unsigned k = 0U;
    for (; k < n && wr != end; ++k, ++wr) {
        const unsigned idx = (wr & (CAPTURE_FRAMES - 1U)) * 2U;
        const float v = l[k] * gain, m = fabsf(v);
        sq += v * v;
        if (m > peak) peak = m;
        const int16_t s = RADIO_Saturate16(v);
        w->capture[idx] = s;
        w->capture[idx+1U] = (r == l) ? s : RADIO_Saturate16(r[k] * gain);
    }
    __DMB();
    w->cap_write = wr;
    if (k < n) cap_dropped += n - k;
    if (level) capture_level_add(sq, peak, k);
}

/* One receive half. Out of line from RADIO_AudioWorker: one call a half
 * (~0.36 ms), and each keeps its own complexity budget. */
/* The headphone mix is the capture now (pc_monitor): the host streaming, a
 * headphone path, not DIGI, not I/Q, not the microphone monitor. Out of
 * line, as are the two below: the monitor's code stays out of the receive
 * and headphone loops, which only test for it. */
static __attribute__((noinline)) uint8_t pc_monitor_live(const receiver_workspace_t *w)
{
    return (uint8_t)((pc_monitor & 1U) && capture_streaming && !w->phones_off && !w->dsp.digi &&
                     !RADIO_Config()->usb_iq && !mic_monitor);
}

/* One monitor sample into the capture ring at pc_cap_wr; committed by
 * pc_monitor_commit after the half. */
static uint32_t pc_cap_wr;
static __attribute__((noinline)) void pc_monitor_put(receiver_workspace_t *w, float mix)
{
    if (pc_cap_wr == w->cap_read + CAPTURE_FRAMES) { ++cap_dropped; return; }
    const unsigned idx = (pc_cap_wr & (CAPTURE_FRAMES - 1U)) * 2U;
    /* VCVT saturates and takes NaN to 0; SSAT clamps to int16. */
    const int16_t v = (int16_t)__SSAT((int32_t)(mix * (float)pc_monitor_gain * 256.0f), 16);
    w->capture[idx] = v;
    w->capture[idx + 1U] = v;
    ++pc_cap_wr;
}

static __attribute__((noinline)) void pc_monitor_commit(receiver_workspace_t *w)
{
    __DMB();
    w->cap_write = pc_cap_wr;
}

/* The receiver writes the capture: the host streaming, and neither monitor
 * writing it instead. */
static __attribute__((noinline)) uint8_t capture_from_receiver(const receiver_workspace_t *w)
{
    return (uint8_t)(capture_streaming && !mic_monitor && !pc_monitor_live(w));
}

__attribute__((noinline)) static void receiver_process(receiver_workspace_t *w, unsigned slot)
{
    const uint16_t *raw = w->rx_ring[slot];
    const radio_config_t *c = RADIO_Config();
    const uint8_t collect = chop_run && chop_halves && !chop_skip, on = w->ring_chop[slot];
    const float weight = collect ? RADIO_ChopWeight((unsigned)(chop_total - chop_halves), chop_total) : 0.0f;
    const uint8_t capture = capture_from_receiver(w), capture_iq = c->usb_iq;
    const uint8_t cw_decode = (uint8_t)(w->dsp.mode == RADIO_RX_CW && RADIO_ConfigCwDecode(c));
#if !MOD_DEEP_DIAG_ENABLE
    const uint8_t correlate = iq_run && !iq_skip && !iq_done;
    const uint8_t measure_level = level_halves && !level_skip && !level_done;
#if RADIO_AUDIO_FINE_PROFILE
    const uint8_t measure = cycles_ready;
    uint32_t fe_sum = 0U, demod_sum = 0U;
#endif
#endif
    /* Unpack the half and feed the two detectors that work on raw input
     * samples. The fault check moves out of the loop: one half is under a
     * millisecond, so testing it here and again before the DSP is the same
     * granularity the per-sample test gave. */
    if (audio_fault) return;
    uint32_t adc_peak = 0U;
    for (unsigned n = 0U; n < RX_FRAMES; ++n) {
        int32_t li = (int32_t)(((uint32_t)raw[4U*n] << 16U) | raw[4U*n+1U]);
        int32_t rq = (int32_t)(((uint32_t)raw[4U*n+2U] << 16U) | raw[4U*n+3U]);
        if ((li | rq) && !rx_data_seen) rx_data_seen = 1U;
        /* |x| without the INT32_MIN overflow: ~x is -x - 1 for x < 0. */
        const uint32_t ai = (uint32_t)(li < 0 ? ~li : li), aq = (uint32_t)(rq < 0 ? ~rq : rq);
        if (ai > adc_peak) adc_peak = ai;
        if (aq > adc_peak) adc_peak = aq;
        const float fi = (float)li / 2147483648.0f, fq = (float)rq / 2147483648.0f;
        w->block_i[n] = fi;
        w->block_q[n] = fq;
        if (collect && n >= CHOP_FIRST_FRAME) RADIO_ChopAdd(&chop, on, weight, fi, fq);
#if !MOD_DEEP_DIAG_ENABLE
        if (correlate) RADIO_IqCorrAdd(&iq_corr, fi, fq);
#endif
    }
    /* Within 0.07 dB of full scale counts as a clip. About a second of
     * halves per published peak. */
    if (adc_peak >= 0x7F000000UL) ++adc_clips;
    if (adc_peak > adc_acc_peak) adc_acc_peak = adc_peak;
    if (++adc_acc_halves >= 1500U) {
        adc_peak_word = adc_acc_peak;
        adc_acc_peak = 0U;
        adc_acc_halves = 0U;
    }
#if !MOD_DEEP_DIAG_ENABLE
    if (snapshot_state == 1U) snapshot_take(w, raw);
#endif
    if (c->clock_flags & RADIO_CLOCK_FLAG_QCOMB) qcomb_cancel(w);
    uint8_t spec = 0U;
    if (spec_state | spec_scan) spec = spec_half(w, capture, cw_decode);
    if (spec & SPEC_HALF_SCAN) {
        /* The scanner's LO is elsewhere: spec_half fed the rings silence. */
    } else if (!audio_fault) {
#if !MOD_DEEP_DIAG_ENABLE && RADIO_AUDIO_FINE_PROFILE
        const uint32_t f0 = measure ? DWT->CYCCNT : 0U;
#endif
        rx_trim_apply(w);
        /* The half in two parts, so the output arrays stay sized for one
         * part at the lowest PCM1821 rate (RADIO_RX_FRONT_MAX_OUT). */
        for (unsigned part = 0U; part < RX_FRAMES; part += RX_PART) {
        const unsigned outputs = RADIO_RxFrontBlock(&w->front, c, w->block_i + part, w->block_q + part,
                                                    RX_PART, w->block_out_i, w->block_out_q);
#if !MOD_DEEP_DIAG_ENABLE && RADIO_AUDIO_FINE_PROFILE
        if (measure) fe_sum += DWT->CYCCNT - f0;
        const uint32_t d0 = measure ? DWT->CYCCNT : 0U;
#endif
        /* The waterfall takes the front end's output before the loop below
         * writes the channel filter's over it. */
        if (spec & SPEC_HALF_OUT) spec_capture(w, w->block_out_i, w->block_out_q, outputs);
        /* The USB capture: the front end's output for USB RAW IQ, before the
         * loop below writes the channel filter's over it; otherwise the audio,
         * kept in the part of block_i the front end has consumed. DIGI: fixed
         * gain, the AGC's for the headphones only. */
        const uint8_t cap_audio = (uint8_t)(capture && !capture_iq), digi = w->dsp.digi;
        float *const cap_v = w->block_i + part;
        if (capture && capture_iq) capture_block(w, w->block_out_i, w->block_out_q, outputs, 1.0f, 0U);
        if (w->phones_off) w->mono_read = w->mono_write; /* no reader: never full */
        for (unsigned k = 0U; k < outputs; ++k) {
            const float audio = RADIO_DspDemodulate(&w->dsp, w->block_out_i[k], w->block_out_q[k]);
#if !MOD_DEEP_DIAG_ENABLE
            if (measure_level) level_add(RADIO_DspLastPower(&w->dsp));
#endif
            /* The channel filter's output, for the CW decoder (cwdec_feed):
             * two stores, not a branch a sample in this loop. */
            w->block_out_i[k] = w->dsp.last_i;
            w->block_out_q[k] = w->dsp.last_q;
            if (w->mono_write - w->mono_read < 256U) {
                w->mono[w->mono_write & 255U] = audio; ++w->mono_write;
            } else ++overruns;
            if (cap_audio) cap_v[k] = digi ? w->dsp.raw : audio;
        }
        if (cap_audio) capture_block(w, cap_v, cap_v, outputs, digi ? RADIO_DSP_DIGI_USB_GAIN : 1.0f, 1U);
        if (cw_decode) cwdec_feed(w, outputs);
#if !MOD_DEEP_DIAG_ENABLE && RADIO_AUDIO_FINE_PROFILE
        if (measure) demod_sum += DWT->CYCCNT - d0;
#endif
        }
    }
    receiver_half_end();
#if !MOD_DEEP_DIAG_ENABLE
#if RADIO_AUDIO_FINE_PROFILE
    if (measure) {
        cycles_mean(&fe_avg, fe_sum);
        cycles_mean(&demod_avg, demod_sum);
    }
#endif
    if (iq_run && !audio_fault) {
        if (iq_skip) --iq_skip;
        else if (RADIO_IqCorrDone(&iq_corr)) iq_done = 1U;
    }
#endif
}

#if !MOD_DEEP_DIAG_ENABLE
/* What actually reached the PWM comparators, for HEADPHONE PWM PATH in the
 * self test. Counting DMA overruns only proves the path kept its deadline;
 * this proves the interpolator and the noise shaper produced a signal, which
 * is the other half of "the headphone path works". Once per half, not per
 * sample, so it stays out of line. */
__attribute__((noinline)) static void phone_stats_add(const uint16_t *out)
{
    uint32_t sum = 0U, low = 0xFFFFFFFFUL, high = 0U;
    for (unsigned n = 0U; n < PHONE_FRAMES; ++n) {
        const uint32_t code = out[n * 2U];
        sum += code;
        if (code < low) low = code;
        if (code > high) high = code;
    }
    phone_stats_sum += sum;
    phone_stats_count += PHONE_FRAMES;
    if (low < phone_stats_min) phone_stats_min = low;
    if (high > phone_stats_max) phone_stats_max = high;
    --phone_stats_halves;
}
#endif

/* Headphone gain at 100 % volume. The AGC holds a signal at 0.35 of full
 * scale, and unity gain left the top 8 dB of the PWM range unused, so full
 * volume was quieter than the hardware allows. Two puts an AGC-held signal at
 * 0.70, 2.2 dB under the renderer's +-0.90 clamp: room for the AGC's ~1 ms
 * attack on a signal that appears suddenly. */
#define PHONE_FULL_GAIN 2.0f
/* The applied gain slews to the volume setting at most this much per 48 kHz
 * sample: silence to full volume in 5 ms. That is the fade-in after every
 * receiver start (the workspace starts at zero gain), and it makes volume
 * steps and muting free of the zipper clicks a per-half gain step made. */
#define PHONE_GAIN_SLEW (PHONE_FULL_GAIN / 240.0f)

/* A keyer that cannot radiate (practice, TX muted, TX not permitted) is
 * clocked by the headphones: one step per 48 kHz sample they consume, at any
 * volume, since the Morse trainer reads the keyer too. The shaped sidetone
 * level for each sample is left in `levels` for phone_process. Out of line so
 * its loop does not live in RADIO_AudioWorker. */
__attribute__((noinline)) static uint8_t phone_keyer(float *levels, unsigned needed)
{
    if (!MOD_CwKeyerMonitorActive()) {
        MOD_CwMonitorIdle();
        if (!CWP_Busy()) return 0U;
        /* SPOT while the keyer is the transmitter's: the player alone, the
         * transmit sidetone kept under it. */
        const float tx = MOD_CwMonitorEnvelope();
        for (unsigned k = 0U; k < needed; ++k) {
            const float env = CWP_Step();
            if (k < PHONE_LEVELS) levels[k] = 0.2f * (env > tx ? env : tx);
        }
        return 1U;
    }
    for (unsigned k = 0U; k < needed; ++k) {
        const float env = MOD_CwKeyerMonitorStep();
        if (k < PHONE_LEVELS) levels[k] = 0.2f * env;
    }
    return 1U;
}

static void phone_process(receiver_workspace_t *w, unsigned half)
{
    /* 256 TIM2 periods (~330 kHz) per half. The per-period work is
     * RADIO_PhoneRender: linear interpolation from 48 kHz and one quantizer
     * for the identical left/right drive, with no float compare in it. This
     * function does the per-48 kHz-sample work -- ring, sidetone, gain,
     * clamp -- once for the few dozen samples the half consumes. Receive
     * audio is at most 10 kHz wide, so the interpolation images near 48 kHz
     * stay more than 40 dB down. */
    uint32_t *out = (uint32_t *)(void *)&w->phone[half * PHONE_FRAMES * 2U];
    const uint8_t mon = pc_monitor_live(w);
    /* Silent through the BPF sweep's buzz and the scanner's foreign LOs, and
     * for a PC monitor that asked for the phones off. */
    float volume = (chop_run | spec_scan | phone_duck | (mon && (pc_monitor & 2U))) ? 0.0f
                                                                                  : (float)RADIO_Config()->volume * 0.01f;
    volume *= volume * PHONE_FULL_GAIN;
    float tone = 0.2f * MOD_CwMonitorEnvelope();
    float norm = 1.5f - 0.5f * (w->tone_i * w->tone_i + w->tone_q * w->tone_q);
    w->tone_i *= norm; w->tone_q *= norm;
    if (audio_fault) return;
    radio_phone_t *p = &w->phone_state;
    /* The 48 kHz FIFO drains at the same rate whatever the headphones do, or
     * receiver_process fills it and counts overruns; the count is exact, one
     * sample per phase carry. */
    const unsigned needed = RADIO_PhoneNeeds(p, PHONE_FRAMES);
    const uint32_t read = w->mono_read;
    const uint32_t queued = w->mono_write - read;
    const unsigned take = needed < queued ? needed : queued;
    w->mono_read = read + take;
    if (rx_enabled && needed > take && !RADIO_TxHandover() && !MOD_CwMonitorKeying()) overruns += needed - take;
    float *levels = w->phone_levels;
    const uint8_t keyed = phone_keyer(levels, needed);
    float gain = w->gain;
    /* Nothing is listening -- volume and sidetone at zero and the gain slewed
     * all the way down -- so emit mid-scale directly rather than render
     * silence (a clock outside what PHONE_LEVELS was sized for lands here
     * too). The renderer is reset so audio resumes from a clean state. */
    if ((volume == 0.0f && tone == 0.0f && gain == 0.0f && !mon) || needed > PHONE_LEVELS) {
        p->phase += PHONE_FRAMES * p->step;
        RADIO_PhoneReset(p, &w->phone_shape);
        w->gain = 0.0f;
        const uint32_t pair = (w->phone_top / 2U) * 0x00010001UL;
        for (unsigned n = 0U; n < PHONE_FRAMES; ++n) out[n] = pair;
        return;
    }
    float ti_s = w->tone_i, tq_s = w->tone_q;
    /* The FIFO still drains (above) while the trainer quiets the receiver. */
    const unsigned heard = phone_rx_quiet ? 0U : take;
    pc_cap_wr = w->cap_write;
    for (unsigned k = 0U; k < needed; ++k) {
        const float next = k < heard ? w->mono[(read + k) & 255U] : 0.0f;
        const float side = keyed ? levels[k] : tone;
        const float ti = ti_s * tone_cs - tq_s * tone_sn;
        tq_s = tq_s * tone_cs + ti_s * tone_sn;
        ti_s = ti;
        const float mix = next + side * tq_s;
        if (mon) pc_monitor_put(w, mix);
        if (gain < volume) {
            gain += PHONE_GAIN_SLEW;
            if (gain > volume) gain = volume;
        } else if (gain > volume) {
            gain -= PHONE_GAIN_SLEW;
            if (gain < volume) gain = volume;
        }
        levels[k] = RADIO_PhoneLevel(mix * gain, &w->phone_shape);
    }
    if (mon) pc_monitor_commit(w);
    w->tone_i = ti_s; w->tone_q = tq_s;
    w->gain = gain;
    RADIO_PhoneRender(p, levels, out, PHONE_FRAMES);
#if !MOD_DEEP_DIAG_ENABLE
    if (phone_stats_halves) phone_stats_add((const uint16_t *)(const void *)out);
#endif
}

/* One completed microphone DMA half, PendSV only. An ADC1 frame is
 * {microphone, reference} when cancellation is on and one oversampled
 * microphone code otherwise; ADC3 is always the latter. QMX's speech DSP
 * runs at 12 kHz while REV1's transport runs at 48 kHz, so each decimated
 * sample is held for four transport samples and the TX speech filter removes
 * the images that hold puts above the voice band. */
__attribute__((noinline)) static void mic_service_half(unsigned half)
{
    /* A half of 512 microphone frames yields at most 9 decimated samples and
     * so 36 transport samples; ADC3's unpaired 1024 frames at most 18 and 72.
     * `block` holds either 72 mono samples or 36 interleaved pairs. */
    int16_t block[80];
    unsigned count = 0U;
    const uint16_t *raw = &microphone[half * MIC_DMA_HALF];
    const unsigned frames = MIC_DMA_HALF / microphone_stride;
    const uint8_t split = mic_monitor && mic_monitor_split && microphone_stride == 2U;
    const unsigned limit = split ? 40U : 80U;
    const float gain = RADIO_Config()->mic_gain;
    for (unsigned n = 0U; n < frames && count + 4U <= limit; ++n) {
        const unsigned i = n * microphone_stride;
        float sample, pair[2];
        if (!RADIO_MicFrontPush(&mic_front, raw[i],
                                microphone_stride == 2U ? raw[i + 1U] : 0U,
                                gain, &sample, split ? pair : NULL))
            continue;
        const int16_t left = RADIO_Saturate16(split ? pair[0] : sample);
        const int16_t right = split ? RADIO_Saturate16(pair[1]) : left;
        for (unsigned k = 0U; k < 4U; ++k) {
            if (split) {
                block[count * 2U] = left;
                block[count * 2U + 1U] = right;
                ++count;
            } else {
                block[count++] = left;
            }
        }
    }
    if (!count || audio_fault) return;
    if (mic_monitor) mic_capture_push(block, count, split);
    else if (!MOD_LocalAudioSubmit(block, count)) ++overruns;
}

/* One receive half from ring slot `slot`, or none while a transmission is
 * starting (see RADIO_AudioWorker). Out of line to keep the worker's
 * complexity. */
static __attribute__((noinline)) void receiver_half(receiver_workspace_t *w, unsigned slot)
{
    /* Paused for a transmission starting, or for a sidetone-only keyer that
     * is sending (MOD_CwMonitorKeying). The CW decoder is told the time
     * passed (cwdec_dropped, in its 1 ms blocks), so the other station's
     * timing on either side of the pause stays true. */
    if (RADIO_TxHandover() || MOD_CwMonitorKeying()) {
        /* The headphones read on through the pause. With nothing written the
         * ring ran dry and the first halves after it came up short: 6 to 23
         * samples counted for every sidetone-only dit, memory or SPOT, AUDIO
         * GLITCHES on the panel for 10 s (bench, v8.10). So does a recording
         * host: its ring ran dry too, and the padding counted as gaps it
         * heard, hundreds in ten minutes of practice (bench, 2026-10-06). */
        spec_scan_silence(w, capture_from_receiver(w), 0U);
        static uint32_t skipped_frames;
        skipped_frames += RX_FRAMES;
        if (skipped_frames >= RX_FRAMES_PER_MS) {
            cwdec_dropped += skipped_frames / RX_FRAMES_PER_MS;
            skipped_frames %= RX_FRAMES_PER_MS;
        }
        return;
    }
#if !MOD_DEEP_DIAG_ENABLE
    const uint8_t profile = cycles_enabled && cycles_ready;
    const uint32_t t0 = profile ? DWT->CYCCNT : 0U;
#endif
    receiver_process(w, slot);
#if !MOD_DEEP_DIAG_ENABLE
    if (profile) {
        const uint32_t dt = DWT->CYCCNT - t0;
        cycles_peak(&rx_cyc_max, dt);
        cycles_mean(&rx_cyc_avg, dt);
    }
#endif
}

/* One headphone half, with its cycle cost. Out of line, like receiver_half. */
static __attribute__((noinline)) void phone_half(receiver_workspace_t *w, unsigned half)
{
#if !MOD_DEEP_DIAG_ENABLE
    const uint8_t profile = cycles_enabled && cycles_ready;
    const uint32_t t0 = profile ? DWT->CYCCNT : 0U;
#endif
    phone_process(w, half);
#if !MOD_DEEP_DIAG_ENABLE
    if (profile) {
        const uint32_t dt = DWT->CYCCNT - t0;
        cycles_peak(&ph_cyc_max, dt);
        cycles_mean(&ph_cyc_avg, dt);
    }
#endif
}

/* Cycles the worker has spent, interrupts that preempted it included: the
 * receive path's share of the core (RADIO_AudioBusyCycles), two DWT reads a
 * call. */
static volatile uint32_t worker_busy_cyc __attribute__((section(".ccmram_bss")));
static void worker_body(void);
void RADIO_AudioWorker(void)
{
#if !MOD_DEEP_DIAG_ENABLE
    const uint8_t timed = cycles_ready;
    const uint32_t t0 = timed ? DWT->CYCCNT : 0U;
    worker_body();
    if (timed) worker_busy_cyc += DWT->CYCCNT - t0;
#else
    worker_body();
#endif
}

uint32_t RADIO_AudioBusyCycles(void) { return worker_busy_cyc; }

static __attribute__((noinline)) void worker_body(void)
{
    /* PendSV only: bounded DSP and memory copies, no storage or UI. */
    ++worker_calls;
#if !MOD_DEEP_DIAG_ENABLE
    const uint8_t profile = cycles_enabled && cycles_ready;
    const uint32_t entry_cyc = profile ? DWT->CYCCNT : 0U;
#endif
    for (unsigned half = 0U; half < 2U; ++half) {
        if (audio_take(&mic_pending, half, 2U)) {
            mic_service_half(half);
            working_channel = 0U;
        }
    }
    receiver_workspace_t *w = receiver;
    if (!w) return;
    /* A transmission is starting (RADIO_TxHandover): the receive DSP gives
     * its ~80 % of the CPU to the TX renderer at once instead of when the
     * main loop, starved by both, gets round to leaving RECEIVE. This worker
     * runs first in PendSV, so with CW's short frames it kept its deadlines
     * while the CW renderer behind it and the main loop were squeezed, and a
     * few receive halves were still lost at each key-down. The halves are
     * taken and dropped (receiver_half); the headphones keep running for the
     * CW sidetone.
     *
     * The headphones go first and are looked at again after every receive
     * half: their deadline is one headphone half (~0.78 ms) and their work
     * ~10k cycles, while a receive half waits in the ring with RX_RING half
     * periods to go. Before the ring a receive half had to finish inside its
     * own 61k-cycle period behind whatever headphone half and USB interrupt
     * landed first, and with a host recording that missed about 2 % of
     * halves. */
    for (;;) {
        for (unsigned half = 0U; half < 2U; ++half) {
            if (audio_take(&phone_pending, half, 3U)) {
                phone_half(w, half);
                working_channel = 0U;
            }
        }
        const uint32_t rd = w->ring_rd;
        if (rd == w->ring_wr || audio_fault) break;
#if !MOD_DEEP_DIAG_ENABLE
        if (profile) cycles_peak(&latency_max_cyc, DWT->CYCCNT - w->ring_stamp[rd & (RX_RING - 1U)]);
#endif
        receiver_half(w, rd & (RX_RING - 1U));
        __DMB();
        w->ring_rd = rd + 1U;
    }
#if !MOD_DEEP_DIAG_ENABLE
    if (profile) cycles_peak(&worker_max_cyc, DWT->CYCCNT - entry_cyc);
#endif
}

void RADIO_CaptureRead(int16_t *stereo, uint32_t frames)
{
    receiver_workspace_t *w = receiver;
    uint32_t padded = 0U;
    for (uint32_t i = 0U; i < frames; ++i) {
        int16_t l = 0, r = 0;
        if (w && w->cap_read != w->cap_write) {
            unsigned pos = (w->cap_read & (CAPTURE_FRAMES - 1U)) * 2U;
            l = w->capture[pos]; r = w->capture[pos+1U]; ++w->cap_read;
        } else {
            ++padded;
        }
        stereo[2U*i] = l; stereo[2U*i+1U] = r;
    }
    /* Silence while a receiver runs is a gap in what the host records; with
     * none running (TX, storage, a restart) the stream is meant to be quiet. */
    if (padded) {
        if (w && rx_enabled) cap_padded += padded;
        else cap_silent += padded;
    }
    cap_sent += frames;
}

#if REF_SOF_BENCH
static uint8_t capture_mark;
static uint32_t capture_mark_block;
void RADIO_CaptureMark(uint8_t on) { capture_mark = on; }
#endif

uint16_t RADIO_CapturePacket(int16_t *stereo, uint16_t max_frames)
{
    /* The PC monitor reads on while transmitting (CW sidetone); never from a
     * workspace the microphone has borrowed. */
    receiver_workspace_t *w =
        (rx_enabled || mic_monitor || ((pc_monitor & 1U) && !microphone_workspace_owned)) ? receiver : NULL;
    if (!max_frames) {
        /* Called with no buffer when the host selects interface 2, either
         * alternative: the stream is closed, so stop filling. */
        capture_streaming = 0U;
        if (w) w->cap_read = w->cap_write;
        return 0U;
    }
    capture_streaming = 1U;
    uint32_t queued = w ? w->cap_write - w->cap_read : 0U;
    /* Asynchronous source: regulate around 128 frames with 47/48/49 frames
     * per USB millisecond. Silence remains 48 kHz during TX/storage pauses. */
    uint16_t frames = w ? (queued > 176U ? 49U : queued < 80U ? 47U : 48U) : 48U;
    if (frames > max_frames) frames = max_frames;
    RADIO_CaptureRead(stereo, frames);
#if REF_SOF_BENCH
    /* BENCH: once every 1024 host frames, the first three stereo frames of
     * the packet carry a marker and the frame count, so a recording can be
     * lined up with the SOF stamps (radio_clocks.c, ref_sof_raw). */
    const uint32_t frame = RADIO_RefSofFrame();
    if (capture_mark && (frame >> 10) != capture_mark_block && frames >= 3U) {
        capture_mark_block = frame >> 10;
        stereo[0] = 32767; stereo[1] = -32768; stereo[2] = -32768; stereo[3] = 32767;
        stereo[4] = (int16_t)(frame & 0xFFFFU); stereo[5] = (int16_t)(frame >> 16);
    }
#endif
    return frames;
}

uint32_t RADIO_AudioOverruns(void) { return overruns; }

void RADIO_DigiStats(radio_digi_stats_t *out)
{
    out->cap_sent = cap_sent;
    out->cap_padded = cap_padded;
    out->cap_dropped = cap_dropped;
    out->cap_silent = cap_silent;
    out->cap_clips = cap_clips;
    out->adc_clips = adc_clips;
    out->restarts = receiver_generation;
    out->streaming = capture_streaming;
    const float level = cap_level, peak = cap_peak;
    out->level_dbfs = level > 1e-15f ? 10.0f * log10f(level) : -150.0f;
    out->peak_dbfs = peak > 1e-8f ? 20.0f * log10f(peak) : -150.0f;
    const uint32_t word = adc_peak_word;
    out->adc_peak_dbfs = word ? 20.0f * log10f((float)word * (1.0f / 2147483648.0f)) : -150.0f;
}
uint8_t RADIO_CaptureStreaming(void) { return capture_streaming; }
void RADIO_AudioCycleCosts(radio_audio_cycles_t *out)
{
#if !MOD_DEEP_DIAG_ENABLE
    out->worker_max = worker_max_cyc;
    out->latency_max = latency_max_cyc;
    out->rx_max = rx_cyc_max;
    out->rx_avg = rx_cyc_avg;
    out->phone_max = ph_cyc_max;
    out->phone_avg = ph_cyc_avg;
#if RADIO_AUDIO_FINE_PROFILE
    out->fe_avg = fe_avg;
    out->demod_avg = demod_avg;
#else
    out->fe_avg = out->demod_avg = 0U;
#endif
#else
    memset(out, 0, sizeof(*out));
#endif
}

void RADIO_AudioFailCounts(uint32_t *pending, uint32_t *working, uint32_t *overrun,
                           uint32_t *dma, uint32_t *workers)
{
    if (pending) *pending = fail_pending;
    if (working) *working = fail_working;
    if (overrun) *overrun = fail_overrun;
    if (dma) *dma = fail_dma;
    if (workers) *workers = worker_calls;
}
uint32_t RADIO_AudioI2sFallbacks(void) { return i2s_fallbacks; }
float RADIO_AudioRxPower(void)
{
    const receiver_workspace_t *w = receiver;
    return (w && rx_enabled) ? w->dsp.level : -1.0f;
}

float RADIO_AudioRxLevelDbfs(void)
{
    receiver_workspace_t *w = receiver; /* single float read of PendSV state */
    return (w && rx_enabled) ? RADIO_DspLevelDbfs(&w->dsp) : -150.0f;
}
float RADIO_AudioRate(void) { return i2s_rate; }

void RADIO_AudioSpecArm(uint8_t source, uint8_t halvings, uint16_t skip_halves)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    spec_source = source ? (uint8_t)RADIO_SPEC_SRC_CHANNEL : (uint8_t)RADIO_SPEC_SRC_RAW;
    /* The half-bands work on the 48 kHz channel only. A stage settles in 16
     * outputs, the second 8 of its own after the first has. */
    spec_halvings = (uint8_t)(spec_source ? (halvings > 2U ? 2U : halvings) : 0U);
    spec_prime = (uint16_t)(spec_halvings ? 8U + 8U * spec_halvings : 0U);
    /* Halves still owed to a start or LO move: about 2.7 per millisecond. */
    const int32_t owe_ms = (int32_t)(spec_settle_until - HAL_GetTick());
    if (owe_ms > 0) {
        const uint32_t owe = (uint32_t)owe_ms * 3U;
        if (owe > skip_halves) skip_halves = (uint16_t)(owe > 0xFFFFU ? 0xFFFFU : owe);
    }
    spec_skip = skip_halves;
    spec_fill = 0U;
    spec_state = (receiver && rx_enabled) ? 1U : 0U;
    spec_owned = 1U;
#if !MOD_DEEP_DIAG_ENABLE
    snapshot_state = 0U; /* the block overwrites a snapshot's bytes */
#endif
    __set_PRIMASK(pm);
}

float *RADIO_AudioSpecTake(void)
{
    receiver_workspace_t *w = receiver;
    return (w && spec_state == 2U) ? w->spec : NULL;
}

void RADIO_AudioSpecStop(void)
{
    spec_state = 0U;
    spec_owned = 0U;
    RADIO_AudioScanMute(0U);
}

void *RADIO_AudioSpecScratch(void)
{
    receiver_workspace_t *w = receiver;
    return (w && rx_enabled) ? (void *)w->spec_scratch : NULL;
}

uint32_t RADIO_AudioGeneration(void) { return receiver_generation; }

void RADIO_AudioScanMute(uint8_t on)
{
    if (on && !spec_scan) spec_out_phase = 0.0f;
    spec_scan = on ? 1U : 0U;
}

/* ---- Seamless tuning ---------------------------------------------------------
 * Every applied tune used to restart the whole receiver: RX_EN and the QSD LO
 * dropped, I2S stopped (the PCM1821 lost its clocks), the DSP started from
 * zero and faded back in over 5 ms -- a click on every encoder step, up to ten
 * a second, and a full-width flash in the waterfall. Now a tune that the
 * running LO can serve only moves the receive NCO (phase-continuous), and one
 * that needs another LO period moves the HRTIM in place under a short duck. */

uint8_t RADIO_AudioRunning(void)
{
    return (uint8_t)(receiver != NULL && rx_enabled && rx_data_seen && !audio_fault &&
                     !microphone_workspace_owned && !spec_scan && !chop_run);
}

/* 1 while the headphone PWM carrier (TIM2, reload chosen at receiver start
 * for the dial then) keeps every harmonic RADIO_PHONE_PWM_GAP_HZ from rf_hz;
 * a tune that breaks that takes the full restart, which re-picks it. */
#define RADIO_PHONE_PWM_GAP_HZ 60000U
uint8_t RADIO_AudioPwmClear(uint32_t rf_hz)
{
    const uint32_t top = phone_top ? phone_top : 512U;
    const uint32_t pwm = SystemCoreClock / top;
    if (!pwm) return 0U;
    const uint32_t harmonic = (rf_hz + pwm / 2U) / pwm * pwm;
    const uint32_t gap = harmonic > rf_hz ? harmonic - rf_hz : rf_hz - harmonic;
    return (uint8_t)(gap >= RADIO_PHONE_PWM_GAP_HZ);
}

/* Slew the headphones to silence and wait for it (at most ~8 ms): the gain
 * falls at PHONE_GAIN_SLEW, full volume to zero in 5 ms. */
void RADIO_AudioDuckWait(void)
{
    phone_duck = 1U;
    const uint32_t start = HAL_GetTick();
    for (;;) {
        receiver_workspace_t *w = receiver;
        if (w == NULL || *(volatile float *)&w->gain <= 0.0f || (HAL_GetTick() - start) >= 8U) break;
    }
}

void RADIO_AudioDuckRelease(void) { phone_duck = 0U; }
void RADIO_AudioRxQuiet(uint8_t on) { phone_rx_quiet = on ? 1U : 0U; }
void RADIO_AudioPcMonitor(uint8_t flags, uint8_t gain)
{
    pc_monitor_gain = gain;
    pc_monitor = (uint8_t)(flags & 3U);
}
uint8_t RADIO_AudioPcMonitorFlags(void) { return pc_monitor; }

__attribute__((cold)) void RADIO_AudioRetune(uint32_t dial_hz, uint8_t lo_moved)
{
    receiver_workspace_t *w = receiver;
    if (w == NULL) return;
    radio_rx_if_terms_t t;
    RADIO_RxFrontIfTerms(&t, w->front.rate, rx_offset_plan(w, dial_hz), RADIO_Config()->rx_skew_samples);
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    w->front.next = t;
    ++w->front.retune_seq;
    /* A waterfall block being filled straddles the move: start it again,
     * past the LO step's transient if the LO moved. */
    if (spec_state == 1U) {
        spec_fill = 0U;
        spec_prime = (uint16_t)(spec_halvings ? 8U + 8U * spec_halvings : 0U);
        if (lo_moved && spec_skip < 8U) spec_skip = 8U;
    }
    if (lo_moved) spec_settle_until = HAL_GetTick() + 10U;
    __set_PRIMASK(pm);
    cwdec_note_dial(dial_hz);
    phone_duck = 0U;
}

__attribute__((cold)) uint8_t RADIO_AudioLoadPct(void)
{
#if !MOD_DEEP_DIAG_ENABLE
    receiver_workspace_t *w = receiver;
    if (!w || !cycles_enabled || !cycles_ready || !(i2s_rate > 1000.0f)) return 0U;
    /* A receive half is RX_FRAMES / fS; a headphone half PHONE_FRAMES TIM2
     * periods of phone_top SYSCLK cycles each. */
    const float rx_half = (float)RX_FRAMES * (float)SystemCoreClock / i2s_rate;
    const float ph_half = (float)PHONE_FRAMES * (float)w->phone_top;
    float pct = 100.0f * ((float)rx_cyc_avg / rx_half + (float)ph_cyc_avg / ph_half);
    return pct >= 100.0f ? 100U : (uint8_t)pct;
#else
    return 0U;
#endif
}

void RADIO_AudioChopStart(uint8_t inject)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    chop_inject = inject ? 1U : 0U;
    chop_on = 0U;
    chop_halves = 0U;
    chop_done = 0U;
    RADIO_TestOutput(0U);
    chop_run = 1U;
    __set_PRIMASK(pm);
}

void RADIO_AudioChopMeasure(uint16_t settle_halves, uint16_t halves)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    memset(&chop, 0, sizeof(chop));
    chop_done = 0U;
    chop_skip = settle_halves;
    chop_halves = chop_total = (uint16_t)(halves & ~1U); /* equal injected and idle halves */
    __set_PRIMASK(pm);
}

uint8_t RADIO_AudioChopResult(float *dbfs, uint8_t *clipped)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    uint8_t done = chop_done;
    radio_chop_t copy = chop;
    __set_PRIMASK(pm);
    if (!done) return 0U;
    *dbfs = RADIO_ChopDbfs(&copy);
    *clipped = copy.peak > 0.98f;
    return 1U;
}

void RADIO_AudioChopStop(void)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    chop_run = 0U;
    chop_on = 0U;
    chop_halves = 0U;
    RADIO_TestOutput(0U);
    __set_PRIMASK(pm);
}

uint8_t RADIO_AudioPhoneCompareOk(void)
{
    if (!(TIM2->CR1 & TIM_CR1_CEN)) return 1U;
    for (unsigned n = 0U; n < 256U; ++n) {
        const uint32_t reload = TIM2->ARR;
        if (TIM2->CCR1 > reload || TIM2->CCR2 > reload) return 0U;
    }
    return 1U;
}

#if !MOD_DEEP_DIAG_ENABLE
/* Demodulated channel level over a bounded window, for the AF and image
 * sweeps. RADIO_AudioRxLevelDbfs() is an exponential average built for an
 * S-meter: it needs a long settle per point and it holds its last value when
 * the receiver stops, which reads as a measurement that never happened. A
 * sweep of 152 points can afford neither, so this integrates a fixed number
 * of I2S halves and says plainly when it is done. settle_halves covers the
 * channel filters ringing after a retune, the same way the chop detector's
 * settling covers the PCM1821 group delay. */
void RADIO_AudioLevelMeasure(uint16_t settle_halves, uint16_t halves)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    level_done = 0U;
    level_sum = 0.0f;
    level_peak = 0.0f;
    level_count = 0U;
    level_skip = settle_halves;
    level_halves = halves ? halves : 1U;
    __set_PRIMASK(pm);
}

uint8_t RADIO_AudioLevelResult(float *dbfs, uint8_t *clipped)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    const uint8_t done = level_done;
    const float sum = level_sum, peak = level_peak;
    const uint32_t count = level_count;
    __set_PRIMASK(pm);
    if (!done) return 0U;
    /* Same reference as RADIO_DspLevelDbfs, so a sweep point and the S-meter
     * read the same number for the same signal. */
    const float mean_power = count ? sum / (float)count : 0.0f;
    *dbfs = (isfinite(mean_power) && mean_power > 1e-15f) ? 10.0f * log10f(mean_power) : -150.0f;
    *clipped = peak > 0.96f; /* i*i + q*q for a full-scale complex envelope */
    return 1U;
}

/* QMX 8.6.7. Arm takes the next whole I2S half; Read copies it out once that
 * half has landed. The samples are the raw PCM1821 words, before the DC
 * blockers, the I/Q correction and the anti-alias filter, so what comes back
 * is what the converter saw. Refused (Read stays at 0) while the WATERFALL
 * page holds the capture block the snapshot shares. */
void RADIO_AudioIqSnapshotArm(void)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    snapshot_state = (receiver && !spec_owned) ? 1U : 0U;
    __set_PRIMASK(pm);
}

unsigned RADIO_AudioIqSnapshotRead(int32_t *pairs, unsigned first, unsigned n)
{
    if (first >= RX_FRAMES) return 0U;
    if (n > RX_FRAMES - first) n = RX_FRAMES - first;
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    const uint8_t ready = snapshot_state == 2U && receiver != NULL;
    if (ready) memcpy(pairs, receiver->snapshot[first], (size_t)n * 2U * sizeof(int32_t));
    __set_PRIMASK(pm);
    return ready ? n : 0U;
}

unsigned RADIO_AudioIqSnapshotFrames(void) { return RX_FRAMES; }

/* Drive the headphone path with the CW sidetone at a known level and read
 * back what the PWM comparators were given. `halves` phone halves are
 * integrated (one is about 776 us). Returns 0 until the window is complete. */
void RADIO_AudioPhoneStatsBegin(uint16_t halves)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    phone_stats_sum = 0U;
    phone_stats_count = 0U;
    phone_stats_min = 0xFFFFFFFFUL;
    phone_stats_max = 0U;
    phone_stats_halves = halves ? halves : 1U;
    __set_PRIMASK(pm);
}

uint8_t RADIO_AudioPhoneStats(uint32_t *mean, uint32_t *swing, uint32_t *top)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    const uint8_t done = phone_stats_halves == 0U && phone_stats_count != 0U;
    const uint32_t sum = phone_stats_sum, count = phone_stats_count;
    const uint32_t low = phone_stats_min, high = phone_stats_max;
    const uint32_t reload = phone_top;
    __set_PRIMASK(pm);
    if (!done) return 0U;
    if (mean) *mean = count ? sum / count : 0U;
    if (swing) *swing = high >= low ? high - low : 0U;
    if (top) *top = reload;
    return 1U;
}

void RADIO_AudioLevelStop(void)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    level_halves = 0U;
    level_done = 0U;
    __set_PRIMASK(pm);
}

/* QSD I/Q calibration. settle_halves covers the PCM1821 group delay after the
 * injection is gated, as the sweep's chop settling does; the correction is the
 * one to apply to the stream before correlating, which is how the verification
 * pass measures a residual instead of predicting it. */
void RADIO_AudioIqCalStart(float cycles_per_sample, uint32_t samples, uint16_t settle_halves,
                           uint8_t invert, float q_gain, float skew_samples)
{
    /* Built outside the critical section: the oscillator setup calls libm. */
    radio_iq_corr_t fresh;
    RADIO_IqCorrInit(&fresh, cycles_per_sample, samples);
    RADIO_IqCorrCorrection(&fresh, invert, q_gain, skew_samples);
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    iq_run = 0U;
    iq_corr = fresh;
    iq_done = 0U;
    iq_skip = settle_halves;
    iq_run = 1U;
    __set_PRIMASK(pm);
}

uint8_t RADIO_AudioIqCalResult(radio_iq_corr_t *out)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    const uint8_t done = iq_done;
    if (done) *out = iq_corr;
    __set_PRIMASK(pm);
    return done;
}

void RADIO_AudioIqCalStop(void)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    iq_run = 0U;
    iq_done = 0U;
    iq_skip = 0U;
    __set_PRIMASK(pm);
}

__attribute__((cold)) void RADIO_AudioIqCalProgress(uint32_t *count, uint32_t *samples, uint16_t *skip)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    const uint8_t run = iq_run;
    if (count) *count = run ? iq_corr.count : 0U;
    if (samples) *samples = run ? iq_corr.samples : 0U;
    if (skip) *skip = run ? iq_skip : 0U;
    __set_PRIMASK(pm);
}

/* Move the demodulator's tuning inside the running receiver, so a carrier at a
 * fixed frequency can be placed in the wanted and then the opposite sideband
 * of the audio channel. The new state is built on the stack and swapped in, so
 * the DMA path is held off for a copy rather than for the libm setup. */
void RADIO_AudioRetuneDsp(float offset_hz)
{
    if (!receiver) return;
    radio_dsp_t fresh;
    dsp_configure(&fresh, offset_hz);
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    if (receiver) receiver->dsp = fresh;
    __set_PRIMASK(pm);
}

#endif /* !MOD_DEEP_DIAG_ENABLE */

#if MOD_DEEP_DIAG_ENABLE
/* The receive bench is not built into this flavour, but the LCD page and the
 * settings action are shared, so the capture API answers "nothing here"
 * rather than failing to link -- the same shape as the AF sweep's stubs. */
void     RADIO_AudioIqSnapshotArm(void) { }
unsigned RADIO_AudioIqSnapshotRead(int32_t *pairs, unsigned first, unsigned n)
{ (void)pairs; (void)first; (void)n; return 0U; }
unsigned RADIO_AudioIqSnapshotFrames(void) { return 0U; }
#endif

uint32_t RADIO_AudioI2sDivider(void) { return i2s_divider; }

/* DMA IRQ: the state that was live during the half that just completed, then
 * the gate for the next half. */
static void chop_edge(unsigned half)
{
    chop_half_on[half] = chop_on;
    if (!chop_run) return;
    chop_on ^= 1U;
    RADIO_TestOutput(chop_inject && chop_on);
}

/* DMA IRQ: half `half` of the receive double buffer is complete and the DMA
 * is filling the other one, so this half stays intact for one more half
 * period -- ample time to copy its 512 bytes into the ring here, a few
 * hundred cycles. The worker then has RX_RING half periods to reach it
 * instead of one. A full ring means the worker is that far behind: the half
 * is lost and counted (AUDIO GLITCHES), and the receiver runs on with
 * 0.36 ms missing from the stream. Through v8.09 every late half stood the
 * audio path down and restarted the receiver, a ~13 ms hole each time;
 * with a host recording that blanked about 15 % of the audio, ten holes a
 * second, and WSJT-X decoded nothing. A transmission starting still stands
 * it down, uncounted (audio_irq_pending). */
static void rx_block_in(unsigned half)
{
    receiver_workspace_t *w = receiver;
    if (!w || !rx_enabled || audio_fault) return;
    const uint32_t wr = w->ring_wr;
    if (wr - w->ring_rd >= RX_RING) {
        if (RADIO_TxHandover()) { audio_stand_down(); return; }
        ++fail_pending;
        ++overruns;
        return;
    }
    const unsigned slot = wr & (RX_RING - 1U);
    const uint32_t *src = (const uint32_t *)(const void *)&w->i2s[half * RX_FRAMES * 4U];
    uint32_t *dst = (uint32_t *)(void *)w->rx_ring[slot];
    for (unsigned n = 0U; n < RX_FRAMES * 2U; ++n) dst[n] = src[n];
    w->ring_chop[slot] = chop_half_on[half];
#if !MOD_DEEP_DIAG_ENABLE
    if (cycles_enabled && cycles_ready) w->ring_stamp[slot] = DWT->CYCCNT;
#endif
    __DMB();
    w->ring_wr = wr + 1U;
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}

static inline __attribute__((always_inline)) void mic_dma_irq(void)
{
    uint32_t f = DMA2->ISR;
    DMA2->IFCR = DMA_IFCR_CGIF2;
    if ((f & DMA_ISR_TEIF2) || (microphone_adc && (microphone_adc->ISR & ADC_ISR_OVR))) { RADIO_Trip(RADIO_FAULT_ADC); return; }
    if (f & DMA_ISR_HTIF2) audio_irq_pending(&mic_pending, 0U, 2U);
    if (f & DMA_ISR_TCIF2) audio_irq_pending(&mic_pending, 1U, 2U);
}

/* Every exit marks a SOF this interrupt held up (radio_clocks.c). */
void DMA2_Channel2_IRQHandler(void)
{
    mic_dma_irq();
    RADIO_RefSofIsrReturn();
}

static inline __attribute__((always_inline)) void phone_dma_irq(void)
{
    uint32_t f = DMA2->ISR;
    DMA2->IFCR = DMA_IFCR_CGIF3;
    if (f & DMA_ISR_TEIF3) { ++fail_dma; audio_fail(); return; }
    if (f & DMA_ISR_HTIF3) audio_irq_pending(&phone_pending, 0U, 3U);
    if (f & DMA_ISR_TCIF3) audio_irq_pending(&phone_pending, 1U, 3U);
}

/* Every exit marks a SOF this interrupt held up (radio_clocks.c). */
void DMA2_Channel3_IRQHandler(void)
{
    phone_dma_irq();
    RADIO_RefSofIsrReturn();
}

static inline __attribute__((always_inline)) void i2s_rx_dma_irq(void)
{
    uint32_t f = DMA2->ISR;
    DMA2->IFCR = DMA_IFCR_CGIF4;
    if ((f & DMA_ISR_TEIF4) || (SPI3->SR & SPI_SR_OVR)) {
        if (f & DMA_ISR_TEIF4) ++fail_dma; else ++fail_overrun;
        audio_fail();
        return;
    }
    if (f & DMA_ISR_HTIF4) { chop_edge(0U); rx_block_in(0U); }
    if (f & DMA_ISR_TCIF4) { chop_edge(1U); rx_block_in(1U); }
}

/* Every exit marks a SOF this interrupt held up (radio_clocks.c). */
void DMA2_Channel4_IRQHandler(void)
{
    i2s_rx_dma_irq();
    RADIO_RefSofIsrReturn();
}
