/* Host suite for Core/Src/ui_tune.c: unified mode, tuning arithmetic, knob
 * acceleration, tune coalescing, the settings row, the click step cycle and
 * frequency text. */
#include "ui_tune.h"
#include "ui_priv.h"
#include "radio_config.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_umode(void)
{
    ui_umode_plan_t p;

    /* USB from CW: production SSB, sideband set, receiver follows. */
    UI_UModePlan(UI_UMODE_USB, MOD_CFG_CW_14MHZ, MOD_TYPE_CW, SIDEBAND_LSB, &p);
    assert(p.cfg == MOD_CFG_PROD_SSB_14MHZ && p.change_cfg && p.change_sb &&
           p.sideband == SIDEBAND_USB && p.rx_mode == RADIO_RX_USB);

    /* LSB from an active (diagnostic) SSB configuration keeps it and only
     * flips the sideband, which is what reloads calibration. */
    UI_UModePlan(UI_UMODE_LSB, MOD_CFG_ENHANCED_SSB_14MHZ, MOD_TYPE_SSB, SIDEBAND_USB, &p);
    assert(p.cfg == MOD_CFG_ENHANCED_SSB_14MHZ && !p.change_cfg && p.change_sb &&
           p.sideband == SIDEBAND_LSB && p.rx_mode == RADIO_RX_LSB);

    /* Same mode again changes nothing. */
    UI_UModePlan(UI_UMODE_USB, MOD_CFG_PROD_SSB_14MHZ, MOD_TYPE_SSB, SIDEBAND_USB, &p);
    assert(!p.change_cfg && !p.change_sb);

    /* CW, AM and FM leave the sideband alone. */
    UI_UModePlan(UI_UMODE_CW, MOD_CFG_PROD_SSB_14MHZ, MOD_TYPE_SSB, SIDEBAND_LSB, &p);
    assert(p.cfg == MOD_CFG_CW_14MHZ && p.change_cfg && !p.change_sb && p.rx_mode == RADIO_RX_CW);
    UI_UModePlan(UI_UMODE_AM, MOD_CFG_PROD_SSB_14MHZ, MOD_TYPE_SSB, SIDEBAND_USB, &p);
    assert(p.cfg == MOD_CFG_AM_14MHZ && !p.change_sb && p.rx_mode == RADIO_RX_AM);
    UI_UModePlan(UI_UMODE_FM, MOD_CFG_AM_14MHZ, MOD_TYPE_AM, SIDEBAND_USB, &p);
    assert(p.cfg == MOD_CFG_FM_NARROW_14MHZ && p.rx_mode == RADIO_RX_FM);

    /* DIGI: SSB, upper sideband from the panel, the DIGI policy set; USB
     * and LSB clear it, CW/AM/FM leave it alone. */
    UI_UModePlan(UI_UMODE_DIGI, MOD_CFG_CW_14MHZ, MOD_TYPE_CW, SIDEBAND_LSB, &p);
    assert(p.cfg == MOD_CFG_PROD_SSB_14MHZ && p.sideband == SIDEBAND_USB && p.change_sb &&
           p.rx_mode == RADIO_RX_USB && p.digi == 1U);
    UI_UModePlan(UI_UMODE_USB, MOD_CFG_PROD_SSB_14MHZ, MOD_TYPE_SSB, SIDEBAND_USB, &p);
    assert(p.digi == 0U && !p.change_cfg && !p.change_sb);
    UI_UModePlan(UI_UMODE_CW, MOD_CFG_PROD_SSB_14MHZ, MOD_TYPE_SSB, SIDEBAND_USB, &p);
    assert(p.digi == 0xFFU);

    /* Round trip between the transmitter's state and the unified mode. */
    for (int m = 0; m < (int)UI_UMODE_COUNT; ++m) {
        UI_UModePlan((ui_umode_t)m, MOD_CFG_PROD_SSB_14MHZ, MOD_TYPE_SSB, SIDEBAND_USB, &p);
        mod_type_t type = (m == UI_UMODE_CW) ? MOD_TYPE_CW : (m == UI_UMODE_AM) ? MOD_TYPE_AM
                        : (m == UI_UMODE_FM) ? MOD_TYPE_FM : MOD_TYPE_SSB;
        assert(UI_UModeFromTx(type, p.sideband, (uint8_t)(m == UI_UMODE_DIGI)) == (ui_umode_t)m);
        assert(UI_UModeRxMode((ui_umode_t)m) == p.rx_mode);
        if (m != UI_UMODE_DIGI) assert(!strcmp(UI_RxModeLabel(p.rx_mode), UI_UModeLabel((ui_umode_t)m)));
    }
    assert(!strcmp(UI_UModeLabel(UI_UMODE_DIGI), "DIGI"));
    /* DIGI reads as DIGI on either sideband, and only for SSB. */
    assert(UI_UModeFromTx(MOD_TYPE_SSB, SIDEBAND_LSB, 1U) == UI_UMODE_DIGI);
    assert(UI_UModeFromTx(MOD_TYPE_CW, SIDEBAND_USB, 1U) == UI_UMODE_CW);
    assert(UI_UModeFromTx(MOD_TYPE_TEST, SIDEBAND_USB, 0U) == UI_UMODE_USB);
    assert(UI_UModeFromTx(MOD_TYPE_TEST, SIDEBAND_USB, 1U) == UI_UMODE_USB);
    assert(UI_UModeNext(UI_UMODE_FM, 1) == UI_UMODE_DIGI);
    assert(UI_UModeNext(UI_UMODE_DIGI, 1) == UI_UMODE_USB);
    assert(UI_UModeNext(UI_UMODE_USB, -1) == UI_UMODE_DIGI);
    assert(UI_UModeNext(UI_UMODE_LSB, 1) == UI_UMODE_CW);
}

static void test_tuning(void)
{
    assert(UI_TuneStepHz(0) == 1U && UI_TuneStepHz(2) == 100U && UI_TuneStepHz(3) == 500U);
    assert(UI_TuneStepHz(4) == 1000U && UI_TuneStepHz(7) == 1000000U && UI_TuneStepHz(9) == 1000000U);
    /* 500 Hz steps snap to the 500 Hz grid, as the other steps do to theirs. */
    assert(UI_TuneMove(14074000U, 1, UI_TUNE_STEP_500) == 14074500U);
    assert(UI_TuneMove(14074123U, 1, UI_TUNE_STEP_500) == 14074500U);
    assert(UI_TuneMove(14074123U, -1, UI_TUNE_STEP_500) == 14074000U);

    /* On-grid moves. */
    assert(UI_TuneMove(14074000U, 1, 2) == 14074100U);
    assert(UI_TuneMove(14074000U, -3, 4) == 14071000U);
    /* Off-grid: the first detent lands on the grid in its direction. */
    assert(UI_TuneMove(14074123U, 1, 2) == 14074200U);
    assert(UI_TuneMove(14074123U, -1, 2) == 14074100U);
    assert(UI_TuneMove(14074123U, -2, 2) == 14074000U);
    assert(UI_TuneMove(14074123U, 0, 2) == 14074100U);
    /* Clamped to the receive range at both ends. */
    assert(UI_TuneMove(1000050U, -5, 2) == UI_TUNE_MIN_HZ);
    assert(UI_TuneMove(28333000U, 10, 4) == UI_TUNE_MAX_HZ);
    assert(UI_TuneMove(28000000U, 5, 6) == UI_TUNE_MAX_HZ);
    assert(UI_TuneClamp(0U) == UI_TUNE_MIN_HZ && UI_TuneClamp(30000000U) == UI_TUNE_MAX_HZ);

    /* Band presets round to whole kHz rather than truncating the float. */
    assert(UI_BandPresetHz(28.074f) == 28074000U);
    assert(UI_BandPresetHz(1.840f) == 1840000U);
    assert(UI_BandPresetHz(14.225f) == 14225000U);
    assert(UI_BandPresetHz(0.0f) == UI_TUNE_MIN_HZ);

    /* Acceleration never falls as the knob turns faster. */
    uint8_t last = 1U;
    for (uint32_t interval = 400U; interval >= 5U; interval -= 5U) {
        uint8_t f = UI_TuneAccel(interval, 2U);
        assert(f >= last && f <= 10U);
        last = f;
    }
    assert(UI_TuneAccel(200U, 1U) == 1U);   /* 5 per second: hand speed */
    assert(UI_TuneAccel(10U, 1U) == 10U);   /* 100 per second: a spin */
    assert(UI_TuneAccel(0U, 0U) == 1U);
}

static void test_coalescer(void)
{
    ui_tune_state_t s = {0};
    s.applied_ms = 0U;
    /* First detent after a pause applies at once. */
    UI_TuneTarget(&s, 14074100U, 1000U);
    assert(UI_TuneDue(&s, 1000U, 1U));
    UI_TuneApplied(&s, 1000U);
    assert(!UI_TuneDue(&s, 1001U, 1U));
    /* While turning, no faster than the apply period (25 ms). */
    _Static_assert(UI_TUNE_APPLY_PERIOD_MS == 25U && UI_TUNE_SETTLE_MS == 20U, "test timings");
    UI_TuneTarget(&s, 14074200U, 1005U);
    UI_TuneTarget(&s, 14074300U, 1010U);
    assert(!UI_TuneDue(&s, 1012U, 1U));
    UI_TuneTarget(&s, 14074400U, 1015U);
    assert(!UI_TuneDue(&s, 1020U, 1U));
    assert(UI_TuneDue(&s, 1025U, 1U));
    UI_TuneApplied(&s, 1025U);
    /* The trailing detent is applied once the knob settles. */
    UI_TuneTarget(&s, 14074500U, 1030U);
    assert(!UI_TuneDue(&s, 1045U, 1U));
    assert(UI_TuneDue(&s, 1050U, 1U));
    /* Never while background I/O is refused; the target waits. */
    assert(!UI_TuneDue(&s, 5000U, 0U));
    assert(s.pending && s.target_hz == 14074500U);
    UI_TuneTarget(&s, 40000000U, 5000U);
    assert(s.target_hz == UI_TUNE_MAX_HZ);
}

static void test_format(void)
{
    char t[16];
    assert(UI_FormatFreq(14074000U, t, sizeof(t)) == 10U && !strcmp(t, "14.074.000"));
    assert(UI_FormatFreq(7074250U, t, sizeof(t)) == 9U && !strcmp(t, "7.074.250"));
    assert(UI_FormatFreq(1000001U, t, sizeof(t)) == 9U && !strcmp(t, "1.000.001"));
    assert(UI_FormatFreq(28333333U, t, 5U) == 4U && !strcmp(t, "28.3"));
    assert(UI_FormatFreq(28333333U, NULL, 0U) == 10U);
}

static void test_mic_gain(void)
{
    float g = 1.0f;
    assert(UI_MicGainDb(g) == 0);
    g = UI_MicGainStep(g, 1);
    assert(UI_MicGainDb(g) == 1);
    g = UI_MicGainStep(g, -1);
    assert(fabsf(g - 1.0f) < 1e-4f);
    assert(UI_MicGainStep(1.0f, 100) <= 100.0f && UI_MicGainDb(UI_MicGainStep(1.0f, 100)) == 40);
    assert(UI_MicGainStep(1.0f, -100) >= 0.1f && UI_MicGainDb(UI_MicGainStep(1.0f, -100)) == -20);
    assert(UI_MicGainDb(UI_MicGainStep(3.3f, 0)) == 10);
    assert(UI_MicGainStep(NAN, 0) >= 0.1f);
}

static uint8_t has(const uint8_t *items, uint8_t n, uint8_t id)
{
    for (uint8_t i = 0U; i < n; ++i) if (items[i] == id) return 1U;
    return 0U;
}

static void test_slide(void)
{
    uint8_t it[UI_SLIDE_MAX];
    /* Voice: volume, the waterfall (always the second cell), mode and band,
     * then the receiver as SSB uses it -- filter and AGC -- and power. */
    uint8_t n = UI_SlideItems(UI_UMODE_USB, 0U, it);
    const uint8_t ssb_head[7] = {SET_REV1_VOLUME, UI_SLIDE_WATERFALL, SET_UI_MODE, SET_UI_BAND,
                                 SET_SSB_RX_BW, SET_RX_AGC, SET_DRIVE};
    assert(n >= 10U && !memcmp(it, ssb_head, sizeof(ssb_head)));
    assert(!has(it, n, SET_CW_RX_BW) && !has(it, n, SET_AM_RX_BW) && !has(it, n, SET_FM_SQUELCH));
    assert(has(it, n, SET_REV1_MIC_GAIN) && !has(it, n, SET_USB_GAIN) && has(it, n, SET_TX_BW));
    assert(!has(it, n, SET_CW_WPM) && !has(it, n, SET_CW_DRIVE));
    /* The on-air settings a voice operator reaches for sit on the row. */
    assert(has(it, n, SET_MICAGC) && has(it, n, SET_NG) && has(it, n, SET_EQ) &&
           has(it, n, SET_EQ_BASS) && has(it, n, SET_EQ_TREBLE) && has(it, n, SET_PROFILE) &&
           has(it, n, SET_REV1_USB_IQ) && !has(it, n, SET_CW_PRACTICE));
    /* USB audio as the input swaps the gain. */
    n = UI_SlideItems(UI_UMODE_LSB, 1U, it);
    assert(has(it, n, SET_USB_GAIN) && !has(it, n, SET_REV1_MIC_GAIN));
    /* CW: its receive filter, pitch, AGC and decoder, then speed instead of
     * the microphone and CW drive for power. */
    n = UI_SlideItems(UI_UMODE_CW, 0U, it);
    const uint8_t cw_head[10] = {SET_REV1_VOLUME, UI_SLIDE_WATERFALL, SET_UI_MODE, SET_UI_BAND,
                                 SET_CW_RX_BW, SET_CW_PITCH, SET_CW_AGC, SET_CW_DECODE, SET_CW_WPM, SET_CW_DRIVE};
    assert(!memcmp(it, cw_head, sizeof(cw_head)));
    assert(!has(it, n, SET_SSB_RX_BW) && !has(it, n, SET_RX_AGC));
    assert(!has(it, n, SET_DRIVE) && !has(it, n, SET_REV1_MIC_GAIN) && !has(it, n, SET_TX_BW));
    /* Then the memories, the keyer, the trainer; weight, hang and break-in
     * are ADVANCED CW's (v8.10). */
    assert(it[10] == UI_SLIDE_MEMORIES && has(it, n, SET_CW_MODE) && has(it, n, SET_TRAIN_OPEN));
    assert(!has(it, n, SET_CW_TAIL) && !has(it, n, SET_CW_WEIGHT) && !has(it, n, SET_CW_BREAKIN));
    assert(has(it, n, SET_CW_SWAP) && has(it, n, SET_CW_PRACTICE) && has(it, n, SET_REV1_USB_IQ) &&
           !has(it, n, SET_MICAGC));
    for (unsigned m = 0U; m < UI_UMODE_COUNT; ++m) {
        const uint8_t k = UI_SlideItems((ui_umode_t)m, 0U, it);
        assert((m == UI_UMODE_CW) == (has(it, k, UI_SLIDE_MEMORIES) && has(it, k, SET_TRAIN_OPEN)));
    }
    n = UI_SlideItems(UI_UMODE_CW, 0U, it);
    /* AM and FM each get their own modulation control. */
    n = UI_SlideItems(UI_UMODE_AM, 0U, it);
    assert(has(it, n, SET_AM_DEPTH) && !has(it, n, SET_FM_PRESET));
    assert(it[4] == SET_AM_RX_BW && it[5] == SET_RX_AGC && !has(it, n, SET_FM_SQUELCH));
    n = UI_SlideItems(UI_UMODE_FM, 0U, it);
    assert(has(it, n, SET_FM_PRESET) && !has(it, n, SET_AM_DEPTH));
    assert(it[4] == SET_FM_SQUELCH && !has(it, n, SET_RX_AGC) && !has(it, n, SET_AM_RX_BW));
    /* DIGI: no microphone chain, no AGC, no SSB width or compressor; the
     * transmit power and how the tone goes out (DIGI TONE, USB VOX). */
    n = UI_SlideItems(UI_UMODE_DIGI, 1U, it);
    assert(has(it, n, SET_DRIVE) && has(it, n, SET_DIGI_TONE) && has(it, n, SET_USB_VOX));
    assert(!has(it, n, SET_REV1_VOLUME)); /* the headphones are off in DIGI */
    assert(!has(it, n, SET_SSB_RX_BW) && !has(it, n, SET_RX_AGC) && !has(it, n, SET_COMP) &&
           !has(it, n, SET_USB_GAIN) && !has(it, n, SET_REV1_MIC_GAIN) && !has(it, n, SET_TX_BW));
    n = UI_SlideItems(UI_UMODE_USB, 1U, it);
    assert(!has(it, n, SET_DIGI_TONE) && !has(it, n, SET_USB_VOX));
    /* AM/FM: no SSB width. Everything ends with SAVE, and every item has a
     * caption that fits a 64 px cell in the 6 px font. */
    for (uint8_t m = 0U; m < (uint8_t)UI_UMODE_COUNT; ++m) {
        for (uint8_t src = 0U; src < 2U; ++src) {
            n = UI_SlideItems((ui_umode_t)m, src, it);
            assert(n > UI_SLIDE_VISIBLE && n <= UI_SLIDE_MAX && it[n - 1U] == SET_SAVE);
            assert(UI_SlideFormat(SET_SAVE) == UI_SLIDE_FMT_ACTION);
            /* After VOLUME, which DIGI (headphones off) has not. */
            assert(it[(m == (uint8_t)UI_UMODE_DIGI) ? 0U : 1U] == UI_SLIDE_WATERFALL &&
                   UI_SlideFormat(UI_SLIDE_WATERFALL) == UI_SLIDE_FMT_PAGE);
            for (uint8_t i = 0U; i < n; ++i) {
                const size_t len = strlen(UI_SlideCaption(it[i]));
                assert(len > 0U && len <= 10U);
            }
            if (m >= (uint8_t)UI_UMODE_AM) assert(!has(it, n, SET_TX_BW));
        }
    }
    /* The window keeps a neighbour beside the focus and never runs past the end. */
    assert(UI_SlideFirst(0U, 0U, 12U) == 0U);
    assert(UI_SlideFirst(0U, 3U, 12U) == 1U);
    assert(UI_SlideFirst(5U, 5U, 12U) == 4U);
    assert(UI_SlideFirst(0U, 11U, 12U) == 8U);
    assert(UI_SlideFirst(3U, 2U, 4U) == 0U);
    /* Click on the tuning screen: 10 Hz, 100 Hz, 1 kHz, 10 kHz and round. */
    /* 10 -> 100 -> 500 Hz -> 1 kHz -> 10 kHz -> 10 Hz */
    assert(UI_StepCycle(1U) == 2U && UI_StepCycle(2U) == 3U && UI_StepCycle(3U) == 4U);
    assert(UI_StepCycle(4U) == 5U && UI_StepCycle(5U) == 1U && UI_StepCycle(0U) == 1U && UI_StepCycle(7U) == 1U);
}

int main(void)
{
    test_umode();
    test_tuning();
    test_coalescer();
    test_format();
    test_mic_gain();
    test_slide();
    puts("UI tuning: unified mode map, step/snap/clamp, acceleration, coalesced apply, "
         "frequency text, mic dB steps, settings row and step cycle: PASS");
    return 0;
}
