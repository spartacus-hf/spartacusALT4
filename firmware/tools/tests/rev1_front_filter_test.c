/* Response of the production REV1 receive front end at the offsets the
 * decimation table has to hold. The branch was 64 taps of Blackman (12 kHz
 * usable) and is 96 taps of Kaiser with the stopband edge at 25 kHz (19 kHz
 * usable), held as float rows. 96 taps is what fits the real-time budget: the
 * same 96 taps with Q15 rows and the older loop measured 78 % average and
 * 86 % peak of the 61 370-cycle I2S half on the board and missed its deadline
 * under load, 128 taps measured 89 % and 99 % and was rejected, and 96 float
 * taps with the rewritten loop measure 69 % and 78 %. Tone 0.1, dial at DC,
 * 48 kHz output. */
#include "radio_rx_front.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

/* The receiver's RAM copy of the decimation table (flash keeps half). */
static float coeff_ram[RADIO_RX_COEFF_FLOATS];

#define TAU 6.28318530717958647692

/* Mean magnitude of the 48 kHz complex output for a 0.1 tone at f_hz. */
static double response(float rate, float f_hz)
{
    static float ii[1024], qq[1024], oi[1024], oq[1024];
    radio_rx_front_t s;
    radio_config_t c;
    RADIO_ConfigDefaults(&c);
    RADIO_RxFrontInit(&s, rate, 0.0f, 0.0f); RADIO_RxFrontCoefficientsTo(&s, coeff_ram);
    double energy = 0.0;
    unsigned produced = 0U, measured = 0U;
    for (unsigned chunk = 0U; chunk < 150U; ++chunk) {
        for (unsigned n = 0U; n < 1024U; ++n) {
            const double a = TAU * (double)f_hz * (double)(chunk * 1024U + n) / (double)rate;
            ii[n] = (float)(0.1 * cos(a));
            qq[n] = (float)(0.1 * sin(a));
        }
        const unsigned got = RADIO_RxFrontBlock(&s, &c, ii, qq, 1024U, oi, oq);
        for (unsigned k = 0U; k < got; ++k, ++produced) {
            if (produced < 4000U) continue;      /* DC blockers, NCO, table */
            energy += (double)oi[k] * oi[k] + (double)oq[k] * oq[k];
            ++measured;
        }
    }
    assert(measured > 10000U);
    return sqrt(energy / (double)measured);
}

static double db(double h, double ref)
{
    return 20.0 * log10(fmax(h, 1e-12) / fmax(ref, 1e-12));
}

/* Seamless tuning: a tone 700 Hz above the dial, then the dial moved 500 Hz
 * by handing the running front end new IF terms (RADIO_RxFrontIfTerms, as
 * RADIO_AudioRetune does). The NCO keeps its phase, so the output must glide
 * from 700 Hz to 200 Hz with no amplitude step -- a restart would drop to
 * silence and a phase jump would put a spike in the FIR's output. */
static void seamless_retune_check(void)
{
    static float ii[256], qq[256], oi[256], oq[256];
    const float rate = 170833.333f, if1 = -13886.0f, if2 = if1 + 500.0f;
    const double f_in = (double)if1 + 700.0;
    radio_rx_front_t s;
    radio_config_t c;
    RADIO_ConfigDefaults(&c);
    RADIO_RxFrontInit(&s, rate, if1, 0.657f);
    RADIO_RxFrontCoefficientsTo(&s, coeff_ram);
    double lo = 1e9, hi = 0.0, steady = 0.0;
    unsigned produced = 0U, retuned_at = 0U;
    double last_phase = 0.0, freq_after = 0.0;
    unsigned freq_n = 0U;
    for (unsigned chunk = 0U; chunk < 400U; ++chunk) {
        for (unsigned n = 0U; n < 256U; ++n) {
            const double a = TAU * f_in * (double)(chunk * 256U + n) / (double)rate;
            ii[n] = (float)(0.1 * cos(a));
            qq[n] = (float)(0.1 * sin(a));
        }
        if (chunk == 200U) {
            RADIO_RxFrontIfTerms(&s.next, rate, if2, 0.657f);
            ++s.retune_seq;
            retuned_at = produced;
        }
        const unsigned got = RADIO_RxFrontBlock(&s, &c, ii, qq, 256U, oi, oq);
        for (unsigned k = 0U; k < got; ++k, ++produced) {
            const double m = sqrt((double)oi[k] * oi[k] + (double)oq[k] * oq[k]);
            const double ph = atan2((double)oq[k], (double)oi[k]);
            if (produced > 6000U && produced < 6000U + 1000U) steady += m / 1000.0;
            if (produced > 6000U + 1000U) {
                if (m < lo) lo = m;
                if (m > hi) hi = m;
            }
            if (retuned_at && produced > retuned_at + 200U) {
                double d = ph - last_phase;
                while (d > TAU / 2.0) d -= TAU;
                while (d < -TAU / 2.0) d += TAU;
                freq_after += d * 48000.0 / TAU;
                ++freq_n;
            }
            last_phase = ph;
        }
    }
    freq_after /= (double)(freq_n ? freq_n : 1U);
    printf("seamless retune: level %.3f..%.3f of %.3f (%.2f..%.2f dB), tone after %.1f Hz\n",
           lo, hi, steady, db(lo, steady), db(hi, steady), freq_after);
    assert(retuned_at > 7000U);
    assert(db(lo, steady) > -0.5 && db(hi, steady) < 0.5);   /* no dip, no spike */
    assert(fabs(freq_after - 200.0) < 0.5);                   /* on the new dial */

    /* The running LO is kept inside the 6-22 kHz IF window with the channel
     * clear of DC, and given up outside it. */
    assert(RADIO_RxKeepLo(-13886, 300, 3000));
    assert(RADIO_RxKeepLo(12000, -350, 350));
    assert(!RADIO_RxKeepLo(5000, -350, 350));        /* below the window */
    assert(!RADIO_RxKeepLo(23000, 300, 3000));       /* above it */
    assert(RADIO_RxKeepLo(-6500, 300, 3000));       /* USB channel at -6.2..-3.5 kHz: clear of DC */
    assert(!RADIO_RxKeepLo(44000, 300, 3000));       /* off the flat span */

    /* The ADC rate goes with the plan: ~96 kHz (an IF inside +-40 kHz)
     * wherever an LO fits that window -- every dial up to ~20 MHz -- and the
     * 170-190 kHz rate only where the LO grid is too coarse (radio_rx_front.c). */
    const uint64_t hr = 164000000ULL * 32ULL;
    for (uint32_t hz = 1800000U; hz <= 14350000U; hz += 37000U) {
        uint32_t p = 0U; int32_t o = 0; uint8_t a96 = 0U;
        assert(RADIO_RxPlanLoAdc(hr, hz, RADIO_RX_USB, &p, &o, &a96) == 2U);
        assert(a96 == 1U && o > -40000 && o < 40000);
    }
    unsigned wide = 0U, total = 0U;
    for (uint32_t hz = 28000000U; hz <= 29700000U; hz += 7000U, ++total) {
        uint32_t p = 0U; int32_t o = 0; uint8_t a96 = 1U;
        const uint8_t q = RADIO_RxPlanLoAdc(hr, hz, RADIO_RX_USB, &p, &o, &a96);
        if (q && !a96) { ++wide; assert(o >= -50000 && o <= 50000); }
    }
    printf("adc rate plan: 160-20 m all ~96 kHz at 164 MHz; 10 m %u of %u dials on 170-190 kHz\n", wide, total);
    RADIO_RxAdcPolicySet(RADIO_RX_ADC_HIGH);
    {
        uint8_t a96 = 1U;
        assert(RADIO_RxPlanLoAdc(hr, 14074000U, RADIO_RX_USB, NULL, NULL, &a96) == 2U && a96 == 0U);
    }
    RADIO_RxAdcPolicySet(RADIO_RX_ADC_AUTO);
}

/* Spurious floor of a clean tone through the resampler: fit the pure tone
 * to the 48 kHz output and measure what is left. Nearest-phase polyphase
 * alone leaves -47 dBc at 17 kHz and -51 dBc at 9 kHz (a strong signal off
 * the dial raising the floor under a weak one); the timing correction takes
 * it below -84 dBc across the passband. */
static double spurious_dbc(float rate, double f_hz)
{
    static float ii[512], qq[512], oi[512], oq[512];
    static double yi[40000], yq[40000];
    radio_rx_front_t s;
    radio_config_t c;
    RADIO_ConfigDefaults(&c);
    RADIO_RxFrontInit(&s, rate, 0.0f, 0.0f);
    RADIO_RxFrontCoefficientsTo(&s, coeff_ram);
    unsigned produced = 0U, kept = 0U;
    for (unsigned chunk = 0U; kept < 40000U; ++chunk) {
        for (unsigned n = 0U; n < 512U; ++n) {
            const double a = TAU * f_hz * (double)(chunk * 512U + n) / (double)rate;
            ii[n] = (float)(0.25 * cos(a));
            qq[n] = (float)(0.25 * sin(a));
        }
        const unsigned got = RADIO_RxFrontBlock(&s, &c, ii, qq, 512U, oi, oq);
        for (unsigned k = 0U; k < got && kept < 40000U; ++k, ++produced) {
            if (produced < 8000U) continue;   /* DC blockers settle */
            yi[kept] = oi[k]; yq[kept] = oq[k]; ++kept;
        }
    }
    /* Least-squares amplitude of exp(j w n) (the frequency is exact). */
    const double w = TAU * f_hz / 48000.0;
    double ar = 0.0, ai = 0.0;
    for (unsigned n = 0U; n < kept; ++n) {
        const double cr = cos(w * n), ci = -sin(w * n);
        ar += yi[n] * cr - yq[n] * ci;
        ai += yi[n] * ci + yq[n] * cr;
    }
    ar /= kept; ai /= kept;
    double resid = 0.0;
    for (unsigned n = 0U; n < kept; ++n) {
        const double cr = cos(w * n), ci = sin(w * n);
        const double er = yi[n] - (ar * cr - ai * ci), ei = yq[n] - (ar * ci + ai * cr);
        resid += er * er + ei * ei;
    }
    return 10.0 * log10((resid / kept) / (ar * ar + ai * ai));
}

int main(void)
{
    seamless_retune_check();
    {
        const float r = 170833.333f;
        const double s3 = spurious_dbc(r, 3000.0), s9 = spurious_dbc(r, 9000.0);
        const double s14 = spurious_dbc(r, 14000.0), s17 = spurious_dbc(r, 17000.0);
        printf("resampler spurious 3k %.1f  9k %.1f  14k %.1f  17k %.1f dBc\n", s3, s9, s14, s17);
        assert(s3 <= -84.0 && s9 <= -84.0 && s14 <= -84.0 && s17 <= -84.0);
    }
    const float rate = 177083.333f;                  /* primary I2S rate */
    const double ref  = response(rate,  1000.0f);    /* in-band reference */
    const double h16  = db(response(rate, 16000.0f), ref);
    const double h18  = db(response(rate, 18000.0f), ref);
    const double h20  = db(response(rate, 20000.0f), ref);
    const double h24  = db(response(rate, 24000.0f), ref);
    const double h25  = db(response(rate, 25000.0f), ref);
    const double h42  = db(response(rate, 42000.0f), ref);
    printf("front response 16k %.2f  18k %.2f  20k %.2f  24k %.2f  25k %.2f  42k %.2f dB\n",
           h16, h18, h20, h24, h25, h42);
    assert(h16  >= -0.5);    /* passband: 19 kHz is the -1 dB point */
    assert(h18  >= -1.0);
    assert(h20  >= -3.5);    /* 19 kHz usable, not 20.5: see the header */
    assert(h24  <= -25.0);   /* transition: where the 48 kHz output folds */
    assert(h25  <= -65.0);   /* stopband from the QSD image outwards */
    /* Far stopband, and the reason the rows are float: the designed
     * coefficients measure -84 dB here, their Q15 rounding -76 dB. An
     * int16 table, or a design with a shallower skirt, fails this. */
    assert(h42  <= -78.0);
    puts("REV1 receive front end passes to +-19 kHz and stops by 25 kHz: PASS");
    return 0;
}
