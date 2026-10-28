#include "radio_rx_front.h"
#include "radio_dsp.h"
#include "radio.h"
#include "audio_resample.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* The receiver's RAM copy of the decimation table (flash keeps half). */
static float coeff_ram[RADIO_RX_COEFF_FLOATS];

#define TAU 6.283185307179586
static double front_level(float rate, float offset, float tone)
{
    radio_rx_front_t s;
    radio_config_t c;
    RADIO_ConfigDefaults(&c);
    RADIO_RxFrontInit(&s, rate, offset, 0.0f); RADIO_RxFrontCoefficientsTo(&s, coeff_ram);
    double energy = 0.0, cross = 0.0, dot = 0.0;
    float last_i = 0, last_q = 0;
    unsigned count = 0U, measured = 0U;
    for (unsigned n = 0; n < (unsigned)rate; ++n) {
        double angle = TAU * (offset + tone) * n / rate;
        float i, q;
        if (!RADIO_RxFrontSample(&s, &c, 0.1f*cos(angle), 0.1f*sin(angle), &i, &q)) continue;
        if (++count > 4000U) {
            energy += i*i+q*q; ++measured;
            cross += q*last_i-i*last_q; dot += i*last_i+q*last_q;
        }
        last_i = i; last_q = q;
    }
    assert(count >= 47998 && count <= 48001);
    if (fabs(tone) <= 10000) assert(fabs(atan2(cross,dot)*48000/TAU-tone) < 2.0);
    return sqrt(energy/measured);
}

static double ssb_level_at(radio_rx_mode_t mode, double hz, uint8_t ssb_index)
{
    radio_dsp_t s;
    radio_config_t c;
    RADIO_ConfigDefaults(&c);
    RADIO_DspInit(&s,48000,0,mode);
    if (ssb_index != 0xFFU) RADIO_DspSetVoice(&s, ssb_index, 0xFFU, RADIO_RX_AGC_DEFAULT, 0U);
    double energy=0.0;
    for (unsigned n=0;n<24000;++n) {
        double angle=TAU*hz*n/48000;
        s.agc=1.0f; /* measure filter rejection independently of gain recovery */
        float out=RADIO_DspSample(&s,&c,0.1*cos(angle),0.1*sin(angle));
        assert(isfinite(out));
        if(n>=4000)energy+=out*out;
    }
    return sqrt(energy/20000);
}

static double ssb_level(radio_rx_mode_t mode, double hz) { return ssb_level_at(mode, hz, 0xFFU); }

/* FM: a 1 kHz tone at `dev_hz` deviation through the discriminator; the
 * output's 1 kHz component against everything else after 0.25 s (SINAD, dB). */
static double fm_sinad(double dev_hz, double carrier_hz)
{
    radio_dsp_t s;
    RADIO_DspInit(&s, 48000, 0, RADIO_RX_FM);
    double phase = 0.0, ci = 0.0, cq = 0.0, total = 0.0;
    unsigned measured = 0U;
    float out[24000];
    for (unsigned n = 0U; n < 24000U; ++n) {
        phase += TAU * (carrier_hz + dev_hz * sin(TAU * 1000.0 * n / 48000.0)) / 48000.0;
        out[n] = RADIO_DspDemodulate(&s, 0.1f * (float)cos(phase), 0.1f * (float)sin(phase));
        assert(isfinite(out[n]));
    }
    double mean = 0.0; /* an offset carrier is DC out of the discriminator */
    for (unsigned n = 12000U; n < 24000U; ++n) mean += out[n];
    mean /= 12000.0;
    for (unsigned n = 12000U; n < 24000U; ++n) {
        const double a = TAU * 1000.0 * n / 48000.0, v = out[n] - mean;
        ci += v * cos(a); cq += v * sin(a); total += v * v;
        ++measured;
    }
    ci *= 2.0 / measured; cq *= 2.0 / measured;
    const double tone = 0.5 * (ci * ci + cq * cq), rest = total / measured - tone;
    return 10.0 * log10(tone / (rest > 1e-30 ? rest : 1e-30));
}

static double front_image_rejection(float if_hz, float audio_hz,
                                    float physical_skew, float correction_skew)
{
    const float rate = 177083.0f;
    const float tone = if_hz + audio_hz;
    radio_rx_front_t s;
    radio_config_t c;
    RADIO_ConfigDefaults(&c);
    c.rx_skew_samples = correction_skew;
    RADIO_RxFrontInit(&s, rate, if_hz, correction_skew); RADIO_RxFrontCoefficientsTo(&s, coeff_ram);
    double pi = 0.0, pq = 0.0, ni = 0.0, nq = 0.0;
    unsigned outputs = 0U, measured = 0U;
    for (unsigned n = 0U; n < 120000U && measured < 24000U; ++n) {
        const double w = TAU * tone / rate;
        const double a = w * (double)n;
        float i, q;
        if (!RADIO_RxFrontSample(&s, &c, 0.1f * cos(a),
                                 0.1f * sin(a + w * physical_skew), &i, &q)) continue;
        const double b = TAU * audio_hz * (double)outputs / 48000.0;
        const double ib = TAU * (-2.0 * if_hz - audio_hz) *
                          (double)outputs / 48000.0;
        ++outputs;
        if (outputs <= 4000U) continue;
        const double cb = cos(b), sb = sin(b);
        pi += i * cb + q * sb;
        pq += q * cb - i * sb;
        const double cib = cos(ib), sib = sin(ib);
        ni += i * cib + q * sib;
        nq += q * cib - i * sib;
        ++measured;
    }
    assert(measured == 24000U);
    const double wanted = hypot(pi, pq), image = hypot(ni, nq);
    return 20.0 * log10(wanted / fmax(image, 1e-20));
}

/* The headphone renderer: 48 kHz levels in, one TIM2 code per PWM period
 * out, as phone_process drives it -- RADIO_PhoneNeeds levels per run of
 * periods, each from RADIO_PhoneLevel. */
static void phone_render_run(radio_phone_t *p, const radio_shape_t *k, double hz, double amp,
                             unsigned periods, uint32_t *codes, double *ideal)
{
    float levels[64];
    double t48 = k->mid, last = k->mid;
    unsigned done = 0U, sample = 0U;
    while (done < periods) {
        const unsigned run = periods - done < 256U ? periods - done : 256U;
        const unsigned need = RADIO_PhoneNeeds(p, run);
        assert(need <= 64U);
        for (unsigned n = 0U; n < need; ++n, ++sample)
            levels[n] = RADIO_PhoneLevel((float)(amp * sin(TAU * hz * sample / 48000.0)), k);
        const uint32_t phase0 = p->phase;
        RADIO_PhoneRender(p, levels, &codes[done], run);
        /* The same linear interpolation in double, from the phase alone. */
        uint64_t phase = phase0;
        unsigned used = 0U;
        for (unsigned n = 0U; n < run; ++n) {
            phase += p->step;
            if (phase >> 32) {
                phase &= 0xFFFFFFFFULL;
                t48 = last; last = levels[used++];
            }
            if (ideal) ideal[done + n] = t48 + (last - t48) * (double)phase / 4294967296.0;
        }
        assert(used == need);
        done += run;
    }
}

static void phone_test(void)
{
    static uint32_t codes[1U << 18];
    static double ideal[1U << 18];
    double worst = 1e9;
    for (unsigned top = 448U; top <= 576U; top += 16U) {
        const radio_shape_t k = RADIO_NoiseShapeScale(top);
        radio_phone_t p;
        RADIO_PhoneReset(&p, &k);
        p.phase = 0U;
        p.step = RADIO_PhoneStep(top, 170000000U);
        assert(fabs(p.step / 4294967296.0 - 48000.0 * top / 170e6) < 1e-9);
        /* Both channels carry the code; silence sits exactly at mid-scale
         * (an odd period alternates the two codes either side of it). */
        phone_render_run(&p, &k, 1000.0, 0.0, 4096U, codes, NULL);
        double mean = 0.0;
        for (unsigned n = 0U; n < 4096U; ++n) {
            assert((codes[n] & 0xFFFFU) == (codes[n] >> 16));
            mean += (double)(codes[n] & 0xFFFFU);
        }
        assert(fabs(mean / 4096.0 - 0.5 * top) < 0.002);
        /* A 1 kHz tone at 0.9 of full scale: every code stays off both rails
         * (the renderer has no clamp), the mean is the level's mean, and what
         * the shaper adds to the interpolated level is all high-frequency. */
        const unsigned count = 1U << 18;
        phone_render_run(&p, &k, 1000.0, 0.9, count, codes, ideal);
        double drift = 0.0, err_i = 0.0, err_q = 0.0, sig_i = 0.0, sig_q = 0.0;
        const double pwm = 170e6 / top;
        for (unsigned n = 0U; n < count; ++n) {
            const uint32_t c = codes[n] & 0xFFFFU;
            assert(c >= 1U && c <= top - 2U);
            const double e = (double)c - ideal[n];
            drift += e;
            /* In-band error at the 1 kHz tone and a 3 kHz probe (the
             * shaped noise would show there first). */
            const double a1 = TAU * 1000.0 * n / pwm, a3 = TAU * 3000.0 * n / pwm;
            sig_i += (ideal[n] - 0.5 * top) * cos(a1); sig_q += (ideal[n] - 0.5 * top) * sin(a1);
            err_i += e * cos(a3); err_q += e * sin(a3);
        }
        assert(fabs(drift / count) < 0.001); /* no DC error: the truncation bias is shaped away */
        const double tone_db = 20.0 * log10(hypot(sig_i, sig_q));
        const double err_db = 20.0 * log10(hypot(err_i, err_q) + 1e-12);
        /* ~156 dB for (1 - z^-1)^2 shaping at this rate; an unshaped
         * quantizer would sit near 105 dB. */
        assert(tone_db - err_db > 130.0);
        if (tone_db - err_db < worst) worst = tone_db - err_db;
        /* NaN is silence; out-of-range levels clamp to +-0.90 of full scale. */
        assert(RADIO_PhoneLevel(NAN, &k) == k.mid);
        assert(RADIO_PhoneLevel(10.0f, &k) == k.mid + 0.90f * k.scale);
        assert(RADIO_PhoneLevel(-1e30f, &k) == k.mid - 0.90f * k.scale);
        /* Consumption is exact: one level per phase carry. */
        RADIO_PhoneReset(&p, &k);
        p.phase = 0xFFFFFFF0U;
        assert(RADIO_PhoneNeeds(&p, 0U) == 0U);
        assert(RADIO_PhoneNeeds(&p, 1U) == 1U);
        assert(RADIO_PhoneNeeds(&p, 256U) ==
               (unsigned)((0xFFFFFFF0ULL + 256ULL * p.step) >> 32));
    }
    assert(RADIO_PhoneStep(464U, 0U) == 0U);
    {   /* A zero step never carries: no level is read, and it holds mid-scale. */
        const radio_shape_t k = RADIO_NoiseShapeScale(464U);
        radio_phone_t p;
        uint32_t still[8];
        RADIO_PhoneReset(&p, &k);
        p.phase = 0U; p.step = 0U;
        assert(RADIO_PhoneNeeds(&p, 8U) == 0U);
        RADIO_PhoneRender(&p, NULL, still, 8U);
        for (unsigned n = 0U; n < 8U; ++n) assert(still[n] == 232U * 0x00010001UL);
    }
    printf("PASS  headphone renderer: 3 kHz shaped error %.1f dB under a 0.9 FS tone, no DC error\n", worst);
}

int main(void)
{
    assert(!RADIO_CalUsesReceiver(RADIO_CAL_NONE));
    assert(!RADIO_CalUsesReceiver(RADIO_CAL_BIAS));
    assert(!RADIO_CalUsesReceiver(RADIO_CAL_ENV_A));
    assert(!RADIO_CalUsesReceiver(RADIO_CAL_ENV_B));
    assert(RADIO_CalUsesReceiver(RADIO_CAL_RX_TEST));
    assert(RADIO_CalUsesReceiver(RADIO_CAL_BPF_SWEEP));
    assert(RADIO_CalUsesReceiver(RADIO_CAL_SELFTEST));
    assert(RADIO_CalUsesReceiver(RADIO_CAL_RX_IQ));
    assert(RADIO_CalUsesReceiver(RADIO_CAL_AF_SWEEP));
    for (unsigned n = 0U; n < 10000U; ++n) {
        float turn = (float)n / 10000.0f;
        float sine = RADIO_SineTurn(turn);
        assert(fabs((double)sine - sin(TAU * turn)) < 0.000004);
        assert(sine >= -1.0f && sine <= 1.0f);
    }
    const float rates[]={171875.0f,177083.328125f,184151.78125f,189732.140625f};
    for(unsigned n=0;n<4;++n) {
        double wanted=front_level(rates[n],42000,1000);
        assert(wanted>0.095 && wanted<0.105);
        assert(front_level(rates[n],-42000,-9000)>0.09);
        double alias=front_level(rates[n],0,42000);
        assert(20*log10(wanted/fmax(alias,1e-12))>60);
    }
    /* The production front end applies the calibrated time skew before its
     * NCO and resampler. Exercise the real per-sample path, including the
     * low-cost interpolation and its exact centre-frequency cleanup.
     *
     * These four numbers all share the mirror bin at -(2*IF + 1 kHz) = -19 kHz,
     * so widening the decimation table to 128 taps (which passes 19 kHz at
     * ~0 dB, where the old 64-tap table was 21 dB down) moved them:
     *   before (64 taps):  raw 40.4 dB, 9 kHz 78.9/128.4 dB, 18 kHz 134.7/136.8 dB
     *   after  (128 taps): raw 18.7 dB, 9 kHz 57.3/125.6 dB, 18 kHz 133.1/135.6 dB
     * The raw figure is now the honest QSD image at the IF instead of the old
     * table's attenuation there; the 30 dB improvement the correction buys is
     * unchanged, and the absolute image rejection of the front end is what
     * `rxcal` measures on the raw 177 kHz stream, before decimation. */
    const double image_raw = front_image_rejection(9000.0f, 1000.0f, 0.65f, 0.0f);
    const double image_9k = front_image_rejection(9000.0f, 1000.0f, 0.65f, 0.65f);
    const double image_18k = front_image_rejection(18000.0f, 1000.0f, 0.65f, 0.65f);
    const double ideal_9k = front_image_rejection(9000.0f, 1000.0f, 0.0f, 0.0f);
    const double ideal_18k = front_image_rejection(18000.0f, 1000.0f, 0.0f, 0.0f);
    printf("fractional skew: raw %.1f dB, corrected/ideal 9 kHz %.1f/%.1f dB, 18 kHz %.1f/%.1f dB\n",
           image_raw, image_9k, ideal_9k, image_18k, ideal_18k);
    assert(image_raw < 30.0);          /* measured 18.7 (was 40.4 at 64 taps): the
                                        * skew really does spoil the raw image */
    assert(image_9k > image_raw + 30.0);
    assert(image_18k > ideal_18k - 3.0);
    /* I2S divider: 177.08 kHz (+0.39 % of 176.4 kHz) at 170 MHz, 192 kHz
     * family as the fallback, never above 192 kHz, for the whole PLL range. */
    assert(RADIO_I2sDivider(170000000U,0U)==15U && RADIO_I2sDivider(170000000U,1U)==14U);
    for(unsigned clock=165000000U;clock<=170000000U;clock+=250000U) {
        for(unsigned alt=0;alt<2;++alt) {
            unsigned d=RADIO_I2sDivider(clock,(uint8_t)alt); double fs=clock/(64.0*d);
            assert(d>=4U && fs<=192000.0 && fs>=165000.0);
        }
        double f0=clock/(64.0*RADIO_I2sDivider(clock,0U)), f1=clock/(64.0*RADIO_I2sDivider(clock,1U));
        double e0=fmin(fabs(f0/176400-1),fabs(f0/192000-1)), e1=fmin(fabs(f1/176400-1),fabs(f1/192000-1));
        assert(e0<=e1 && e0<0.03);
    }
    {
        radio_rx_front_t s; radio_config_t c; RADIO_ConfigDefaults(&c); c.rx_iq_invert=1U;
        RADIO_RxFrontInit(&s,177083.0f,0.0f,0.0f); RADIO_RxFrontCoefficientsTo(&s, coeff_ram);
        double cross=0,dot=0; float li=0,lq=0; unsigned count=0;
        for(unsigned n=0;n<177083U;++n){
            double a=TAU*1000.0*n/177083.0; float i,q;
            if(!RADIO_RxFrontSample(&s,&c,0.1f*cos(a),0.1f*sin(a),&i,&q)) continue;
            if(++count>4000U){cross+=q*li-i*lq; dot+=i*li+q*lq;}
            li=i; lq=q;
        }
        assert(fabs(atan2(cross,dot)*48000/TAU+1000.0)<2.0); /* inverted spectrum */
    }
    double usb=ssb_level(RADIO_RX_USB,1000), image=ssb_level(RADIO_RX_USB,-1000);
    assert(usb>0.06 && 20*log10(usb/image)>75);
    /* QMX-class selectivity: opposite sideband from 300 Hz audio up is >= 70 dB
     * down, passband 300..3000 Hz flat within 1 dB, CW single-signal 700 Hz. */
    {
        double ref=ssb_level(RADIO_RX_USB,1500);
        for(int hz=300;hz<=3000;hz+=150) assert(fabs(20*log10(ssb_level(RADIO_RX_USB,hz)/ref))<1.0);
        for(int hz=300;hz<=3000;hz+=150) assert(20*log10(ref/fmax(ssb_level(RADIO_RX_USB,-hz),1e-12))>70);
        assert(20*log10(ssb_level(RADIO_RX_LSB,-1000)/fmax(ssb_level(RADIO_RX_LSB,1000),1e-12))>75);
        double cw=ssb_level(RADIO_RX_CW,150); /* DC itself is blocked */
        assert(cw>0.05 && 20*log10(cw/fmax(ssb_level(RADIO_RX_CW,1000),1e-12))>60);
        assert(20*log10(cw/fmax(ssb_level(RADIO_RX_CW,-1000),1e-12))>60);
    }
    assert(ssb_level(RADIO_RX_LSB,-1000)>0.06);
    /* DIGI: QMX's 150-3200 Hz, flat within 1 dB, the opposite sideband from
     * 450 Hz up as far down as SSB's. Below that the low edge is 150 Hz from
     * DC and the 300 Hz transition to -150 Hz gives 34 dB at 150 Hz rising to
     * 52 at 300 and 66 at 400, which any 150 Hz passband shares. The default SSB filter is ~19 dB down on FT8 at
     * 3150 Hz; DIGI keeps it. */
    {
        const uint8_t digi = (uint8_t)RADIO_SSB_BW_DIGI;
        double ref=ssb_level_at(RADIO_RX_USB,1500,digi), worst=0.0, rej=200.0, rej_low=200.0;
        for(int hz=150;hz<=3200;hz+=50) {
            double d=fabs(20*log10(ssb_level_at(RADIO_RX_USB,hz,digi)/ref));
            if(d>worst) worst=d;
            double r=20*log10(ref/fmax(ssb_level_at(RADIO_RX_USB,-hz,digi),1e-12));
            if(hz>=450) { if(r<rej) rej=r; } else if(r<rej_low) rej_low=r;
            assert(fabs(20*log10(ssb_level_at(RADIO_RX_LSB,-hz,digi)/ref))<1.0);
        }
        double edge_ssb=20*log10(ssb_level(RADIO_RX_USB,3150)/ssb_level(RADIO_RX_USB,1500));
        double out=20*log10(ssb_level_at(RADIO_RX_USB,3700,digi)/ref);
        printf("digi passband: 150-3200 Hz within %.2f dB, opposite sideband >= %.1f dB down "
               "(150-250 Hz: %.1f), 3700 Hz %.1f dB; SSB 2700 at 3150 Hz %.1f dB\n",
               worst, rej, rej_low, out, edge_ssb);
        assert(worst<1.0 && rej>70.0 && rej_low>30.0 && out<-40.0 && edge_ssb<-15.0);
    }
    /* DIGI's USB tap (RADIO_DSP_DIGI_USB_GAIN): the pre-AGC sample, linear
     * across 40 dB while the headphone output, AGC MED, holds level; SSB's
     * passbands leave the flag clear (the capture keeps the AGC output). */
    {
        double tap[2], phone[2];
        const double amp[2] = {0.001, 0.1};
        for(int k=0;k<2;++k) {
            radio_dsp_t s; RADIO_DspInit(&s,48000,0,RADIO_RX_USB);
            RADIO_DspSetVoice(&s,(uint8_t)RADIO_SSB_BW_DIGI,0xFFU,RADIO_RX_AGC_MED,0U);
            assert(s.digi==1U);
            double et=0.0, ep=0.0;
            for(unsigned n=0;n<48000;++n) {
                double a=TAU*1500.0*n/48000;
                float o=RADIO_DspDemodulate(&s,(float)(amp[k]*cos(a)),(float)(amp[k]*sin(a)));
                if(n>=24000) { et+=(double)s.raw*s.raw; ep+=(double)o*o; }
            }
            tap[k]=sqrt(et/24000)*RADIO_DSP_DIGI_USB_GAIN; phone[k]=sqrt(ep/24000);
        }
        double tap_step=20*log10(tap[1]/tap[0]), phone_step=20*log10(phone[1]/phone[0]);
        const double tap_gain=20*log10(tap[1]*sqrt(2.0)/amp[1]);
        printf("digi usb tap: 40 dB input step -> tap %.2f dB (fixed gain %.1f dB), headphones %.1f dB (AGC)\n",
               tap_step, tap_gain, phone_step);
        assert(fabs(tap_step-40.0)<0.1 && fabs(tap_gain-20*log10(RADIO_DSP_DIGI_USB_GAIN))<0.2 && phone_step<10.0);
        for(uint8_t i=0;i<RADIO_SSB_BW_COUNT;++i) {
            radio_dsp_t s; RADIO_DspInit(&s,48000,0,RADIO_RX_USB);
            RADIO_DspSetVoice(&s,i,0xFFU,RADIO_RX_AGC_MED,0U);
            assert(s.digi==0U);
        }
    }
    for(int sign=-1;sign<=1;sign+=2) { /* S-meter: in-channel -20 dBFS tone, image rejected */
        radio_dsp_t s; RADIO_DspInit(&s,48000,0,RADIO_RX_USB);
        for(unsigned n=0;n<24000;++n){ double a=TAU*sign*1000.0*n/48000; (void)RADIO_DspDemodulate(&s,0.1f*cos(a),0.1f*sin(a)); }
        float db=RADIO_DspLevelDbfs(&s);
        if(sign>0) assert(db>-21.5f && db<-18.5f); else assert(db<-45.0f);
    }
    { radio_dsp_t s; RADIO_DspInit(&s,48000,0,RADIO_RX_AM); assert(RADIO_DspLevelDbfs(&s)==-150.0f); }
    phone_test();
    /* FM discriminator: 1 kHz at 1, 2.5 and 4 kHz deviation, on frequency
     * and 1.5 kHz off. With libm's atan2f (v8.08) these read 63.78/45.88,
     * 48.47/43.09 and 39.04/33.22 dB (the channel filter sets them); the
     * polynomial must not lose more than 0.5 dB of any. */
    {
        static const double dev[3] = {1000.0, 2500.0, 4000.0};
        static const double floor_db[3][2] = {{63.28, 45.38}, {47.97, 42.59}, {38.54, 32.72}};
        for (unsigned k = 0U; k < 3U; ++k) {
            const double on = fm_sinad(dev[k], 0.0), off = fm_sinad(dev[k], 1500.0);
            printf("fm: %4.0f Hz deviation, SINAD %.2f dB on frequency, %.2f dB 1.5 kHz off\n", dev[k], on, off);
            fflush(stdout);
            assert(on > floor_db[k][0] && off > floor_db[k][1]);
        }
    }
    /* Headphone PWM has no board reconstruction filter: every HF receive
     * frequency keeps the nearest PWM harmonic outside the PCM1821 I/Q span. */
    for(unsigned clock=165000000U;clock<=175000000U;clock+=5000000U) {
        for(unsigned rf=1000000U;rf<=30000000U;rf+=997U) {
            unsigned arr=RADIO_PhonePwmReload(clock,rf);
            assert(arr>=448U && arr<=575U);
            unsigned pwm=clock/(arr+1U), h=(rf+pwm/2U)/pwm*pwm;
            assert((h>rf?h-rf:rf-h)>=100000U);
        }
    }
    assert(RADIO_PhonePwmReload(170000000U,0U)==511U);
    /* Bias buck TIM4 period during TX, around QMX's 116 kHz: harmonics
     * >= 50 kHz from the carrier (QMX "dynamic noise relocation"). */
    for(unsigned clock=165000000U;clock<=170000000U;clock+=5000000U)
        for(unsigned rf=1000000U;rf<=30000000U;rf+=4999U) {
            unsigned arr=RADIO_PwmReloadAway(clock,rf,1307U,1699U,1465U);
            assert(arr>=1307U && arr<=1699U);
            unsigned pwm=clock/(arr+1U), h=(rf+pwm/2U)/pwm*pwm;
            assert(pwm>=97000U && pwm<=131000U);
            assert((h>rf?h-rf:rf-h)>=50000U);
        }
    assert(RADIO_Saturate16(NAN)==0 && RADIO_Saturate16(10)==32767 && RADIO_Saturate16(-10)==-32768);
    audio_resample_t resample={0};
    float in[3]={0.1f,-0.2f,0.3f},out[3]; unsigned count=0;
    for(unsigned n=0;n<48500;++n)count+=AUDIO_Resample(&resample,in,48500,out);
    assert(count>=47999 && count<=48000);
    assert(fabsf(out[0]-0.1f)<0.0001f && fabsf(out[1]+0.2f)<0.0001f);
    assert(!AUDIO_Resample(&resample,in,NAN,out));
    puts("REV1 receiver offset, anti-alias, sideband, resampling, S-meter, headphone quantizer and PWM harmonic placement: PASS");
    return 0;
}
