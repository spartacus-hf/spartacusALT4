#ifndef RADIO_SPECTRUM_H
#define RADIO_SPECTRUM_H

/* Spectrum and waterfall engine behind the WATERFALL page.
 *
 * Nothing here runs unless the page is open: the receive worker then copies
 * one 512-sample I/Q block per request into the receiver arena
 * (RADIO_AudioSpecArm), and SPEC_Service -- main loop, thread context, only
 * while RECEIVING -- windows it, runs a 512-point radix-2^2 FFT in place and
 * folds it into one pixel row of dB values. Rows are averaged into a line; a
 * line updates the spectrum trace and becomes one row of a 4-bit waterfall
 * history. The engine owns no buffer of its own: the page lends it the
 * history, trace and peak storage (SPEC_Attach) out of the framebuffer rows
 * the colour raster covers, and the per-line scratch lives in the arena.
 *
 * Views (x = 0 is panel column 8, SPEC_CENTER the centre):
 *  - WIDE: the raw PCM1821 stream around the LO, one bin (fS/512, ~346 Hz)
 *    per pixel: +-152 bins, the ADC's flat +-0.3 fS (TI SBASA61 table 8-14).
 *  - 38/19/9.5 kHz: the front end's 48 kHz output around the dial, through
 *    0, 1 or 2 half-band stages, so each zoom halves the bin as well.
 *  - SCAN (experimental): the LO hops across a range (default the band the
 *    dial is in, never outside the dial's filter slot) and each capture's
 *    part of the range is pasted into a panorama, one waterfall line per
 *    sweep. The receiver is muted during a sweep and listens at the dial
 *    between them.
 *
 * TX: the engine does nothing outside RECEIVE, so transmit always has the
 * whole core; a key-down simply ends a sweep. SPEC_TxAllowed adds a measured
 * guard: with the page open, a missed receive deadline or a receive worker
 * plus waterfall load of SPEC_GUARD_PCT or more holds TX off for two
 * seconds. */

#include <stdint.h>

#define SPEC_W          304U  /* pixels across (panel x 8..311) */
#define SPEC_X0         8U
#define SPEC_CENTER     152U  /* pixel of the centre frequency */
#define SPEC_LINE_BYTES (SPEC_W / 2U) /* one history line, 4 bits a pixel */
#define SPEC_GUARD_PCT  95U
#define SPEC_GUARD_MS   2000U

typedef enum { SPEC_MODE_VIEW = 0, SPEC_MODE_SCAN, SPEC_MODE_COUNT } spec_mode_t;
typedef enum { SPEC_SPAN_WIDE = 0, SPEC_SPAN_38K, SPEC_SPAN_19K, SPEC_SPAN_9K5, SPEC_SPAN_COUNT } spec_span_t;
typedef enum { SPEC_LAYOUT_WF = 0, SPEC_LAYOUT_BOTH, SPEC_LAYOUT_SPECTRUM, SPEC_LAYOUT_COUNT } spec_layout_t;
typedef enum { SPEC_WIN_HANN = 0, SPEC_WIN_BLACKMAN, SPEC_WIN_FLATTOP, SPEC_WIN_RECT, SPEC_WIN_COUNT } spec_window_t;
/* How the FFTs of one line combine: log-power mean, or the maximum. */
typedef enum { SPEC_DET_AVG = 0, SPEC_DET_PEAK, SPEC_DET_COUNT } spec_detector_t;
typedef enum { SPEC_SCALE_AUTO = 0, SPEC_SCALE_FIXED, SPEC_SCALE_COUNT } spec_scale_t;
typedef enum { SPEC_PEAK_OFF = 0, SPEC_PEAK_FAST, SPEC_PEAK_SLOW, SPEC_PEAK_HOLD, SPEC_PEAK_COUNT } spec_peak_t;
typedef enum { SPEC_MARK_OFF = 0, SPEC_MARK_DIAL, SPEC_MARK_PASS, SPEC_MARK_COUNT } spec_markers_t;
typedef enum { SPEC_SCAN_BAND = 0, SPEC_SCAN_250K, SPEC_SCAN_500K, SPEC_SCAN_1M, SPEC_SCAN_2M,
               SPEC_SCAN_FILTER, SPEC_SCAN_RANGE_COUNT } spec_scan_range_t;
/* LISTEN: sweep, then listen at the dial for the LISTEN time. CONT: sweep
 * back to back, receiver muted. SINGLE: one sweep per SPEC_ScanTrigger. */
typedef enum { SPEC_SCANA_LISTEN = 0, SPEC_SCANA_CONT, SPEC_SCANA_SINGLE, SPEC_SCANA_COUNT } spec_scan_audio_t;

/* Every field is an index into its choice list, except the two levels,
 * which are dB. Kept in RAM: the defaults return at every boot. */
typedef struct {
    uint8_t mode, span, layout, rate, average, window, detector, scale;
    uint8_t range, palette, fill, smooth, peak_hold, dc_notch, iq_correct, markers;
    uint8_t scan_range, scan_audio, listen, scan_avg;
    int8_t  floor_offset; /* AUTO: bottom of the colour scale, dB from the tracked floor */
    int8_t  ref_dbfs;     /* FIXED: top of the colour scale */
} spec_config_t;

#define SPEC_RATES        {2U, 5U, 10U, 15U, 20U, 25U}   /* lines per second */
#define SPEC_RATE_COUNT   6U
#define SPEC_AVERAGES     {1U, 2U, 4U, 8U, 16U}          /* FFTs per line */
#define SPEC_AVG_COUNT    5U
#define SPEC_RANGES       {30U, 40U, 50U, 60U, 70U, 80U, 90U} /* dB of colour scale */
#define SPEC_RANGE_COUNT  7U
#define SPEC_LISTEN_MS    {200U, 500U, 1000U, 2000U, 5000U, 10000U}
#define SPEC_LISTEN_COUNT 6U
#define SPEC_SCAN_AVGS    {1U, 2U, 4U}                   /* FFTs per scan step */
#define SPEC_SCAN_AVG_COUNT 3U
#define SPEC_SMOOTH_COUNT 4U
#define SPEC_PALETTE_COUNT 4U
#define SPEC_FLOOR_MIN    (-20)
#define SPEC_FLOOR_MAX    20
#define SPEC_REF_MIN      (-120)
#define SPEC_REF_MAX      0
/* The choice lists behind the index fields above, for the page's labels. */
extern const uint8_t spec_rates[SPEC_RATE_COUNT];
extern const uint8_t spec_averages[SPEC_AVG_COUNT];
extern const uint8_t spec_ranges[SPEC_RANGE_COUNT];
extern const uint16_t spec_listens[SPEC_LISTEN_COUNT];
extern const uint8_t spec_scan_avgs[SPEC_SCAN_AVG_COUNT];

spec_config_t *SPEC_Config(void);
void SPEC_ConfigDefaults(void);
/* A setting changed: the engine picks up the new axis, window and pacing at
 * its next service (the history clears only when the frequency axis moves
 * in a way a shift cannot follow). */
void SPEC_ConfigChanged(void);

void SPEC_Open(void);
void SPEC_Close(void);
uint8_t SPEC_IsOpen(void);
/* Storage from the page: a ring of `slots` history lines of SPEC_LINE_BYTES
 * (it shows slots - 1, the last one takes the line being made while the
 * panel is still sending the previous frame) and, for the layouts with a
 * spectrum, SPEC_W trace values and SPEC_W peak bytes; either may be NULL.
 * Everything restarts empty. Detach before the memory goes away. */
void SPEC_Attach(uint8_t *hist, uint8_t slots, int16_t *trace, uint8_t *peak);
void SPEC_Detach(void);
/* Main loop, page open. Returns 1 when there is something new to draw. */
uint8_t SPEC_Service(uint32_t now);
/* Scanner, SINGLE: run one sweep now (other scan modes: sweep at once). */
void SPEC_ScanTrigger(void);
/* RADIO_Service's TX permission term: 0 only while the guard holds TX. */
uint8_t SPEC_TxAllowed(void);

/* What the page draws. Frequencies in Hz; x in pixels from SPEC_X0. */
typedef struct {
    uint32_t centre_hz;   /* frequency at SPEC_CENTER */
    float    hz_per_px;
    uint32_t dial_hz;
    int16_t  dial_x;      /* may lie off screen */
    int16_t  pass_lo_x, pass_hi_x; /* receive channel edges */
    uint8_t  valid;       /* the axis describes live data */
    uint8_t  receiving;
    uint8_t  scanning;    /* a sweep is running now */
    uint8_t  sweep_pct;   /* its progress */
    uint8_t  lines;       /* history lines filled */
    uint8_t  load_rx, load_wf; /* percent of the core */
    uint8_t  guard;       /* TX held */
    int16_t  bottom_db, top_db; /* colour scale, dBFS */
    uint16_t rate10;      /* lines per second actually produced, x10 */
} spec_view_t;
void SPEC_GetView(spec_view_t *v);
/* Fix the history lines the raster shows, once per frame build: a line made
 * while the panel is still being sent goes to the ring's spare slot and
 * appears with the next frame. */
void SPEC_Latch(void);
/* Latched history line `age` (0 = newest) as SPEC_LINE_BYTES of 4-bit
 * levels, pixel x in the low nibble of byte x/2 when x is even; NULL past
 * the lines made. */
const uint8_t *SPEC_HistoryLine(uint8_t age);
/* Spectrum trace in dBFS x 8 per pixel and peak hold in dBFS + 160 (0 is
 * empty); NULL while there is none. */
const int16_t *SPEC_Trace(void);
const uint8_t *SPEC_Peak(void);
/* Changes whenever the history, trace or scale do: the raster's version. */
uint32_t SPEC_Version(void);

#endif
