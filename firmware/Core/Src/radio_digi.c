#include "radio_digi.h"
#include "radio_slot_logic.h"
#include "radio_audio.h"
#include "radio_clocks.h"
#include "radio.h"
#include "main.h"

static radio_slot_t slot;
static uint8_t started;

/* The host is live while it pulls frames: an open capture interface alone
 * says nothing (PipeWire opens it at enumeration and may never poll, and the
 * ring then fills). Gaps count only once it has pulled for a second. */
#define LIVE_TIMEOUT_MS 200U
#define LIVE_SETTLE_MS  1000U
static uint32_t live_sent, live_at, live_since;
static uint8_t live;

/* Gaps: losses in the host stream (silence padded in, a full ring, a missed
 * receive half) that the host heard -- it went on pulling after them; a host
 * that stops leaves an overflowing ring behind, which is not a gap
 * (RADIO_GapStep). Counted in twenty 30 s buckets for the ten-minute figure,
 * and per slot. */
#define GAP_BUCKET_MS 30000U
#define GAP_BUCKETS   20U
static uint8_t gap_bucket[GAP_BUCKETS];   /* saturating: RAM is the budget */
static uint8_t gap_at;
static uint32_t gap_bucket_start, gaps_slot;
static radio_gap_judge_t gap_judge;
static int32_t gap_last_phase;

/* The reference tracker. Its trim follows the DSC1003's own wander (up to a
 * ppm in a few hundred ms, radio_clocks.c), which it cancels: the trim's
 * motion is not the dial's, and the host's clock, which the dial does
 * follow, cannot be seen from here. What can: whether it is locked, and the
 * re-acquisitions -- each one a frequency step -- in the last ten minutes,
 * not counting the first minute of a host or of a new PLL setting.
 * The ppm shown is the trim's mean over 10 s. */
#define REF_EVERY_MS 10000U
#define REF_SETTLE_MS 60000U  /* a host's first minute: its acquisitions are not steps */
static uint8_t reacq_bucket[GAP_BUCKETS];
static uint8_t reacq_last, host_seen;
static volatile uint8_t usb_reset;
static uint32_t host_since, ref_sysclk;
static float trim_sum, trim_mean;
static uint32_t trim_n, ref_at;

static uint32_t load_busy, load_cyc, load_at;
static uint8_t load_pct;

static uint32_t stream_bad(const radio_digi_stats_t *st)
{
    return st->cap_padded + st->cap_dropped + RADIO_AudioOverruns();
}

__attribute__((cold)) static void digi_start(uint32_t now)
{
    radio_digi_stats_t st;
    RADIO_DigiStats(&st);
    RADIO_SlotInit(&slot, now);
    gap_bucket_start = load_at = now;
    RADIO_GapInit(&gap_judge, stream_bad(&st));
    live_sent = st.cap_sent;
    radio_ref_sof_status_t r;
    RADIO_RefSofStatus(&r);
    reacq_last = (uint8_t)r.acquisitions;
    ref_at = now;
    gap_last_phase = -1;
    load_busy = RADIO_AudioBusyCycles();
    load_cyc = DWT->CYCCNT;
    started = 1U;
}

static void live_service(uint32_t now, const radio_digi_stats_t *st)
{
    if (st->cap_sent != live_sent) {
        live_sent = st->cap_sent;
        if (!live) live_since = now;
        live_at = now;
        live = 1U;
    } else if (!st->streaming || (uint32_t)(now - live_at) >= LIVE_TIMEOUT_MS) {
        live = 0U;
    }
}

static void gaps_service(uint32_t now, const radio_digi_stats_t *st)
{
    while ((uint32_t)(now - gap_bucket_start) >= GAP_BUCKET_MS) {
        gap_bucket_start += GAP_BUCKET_MS;
        gap_at = (uint8_t)((gap_at + 1U) % GAP_BUCKETS);
        gap_bucket[gap_at] = 0U;
        reacq_bucket[gap_at] = 0U;
    }
    const uint8_t settled = (uint8_t)(live && (uint32_t)(now - live_since) >= LIVE_SETTLE_MS);
    if (RADIO_GapStep(&gap_judge, settled, stream_bad(st), st->cap_sent)) {
        if (gap_bucket[gap_at] < 0xFFU) ++gap_bucket[gap_at];
        ++gaps_slot;
    }
    const int32_t phase = RADIO_SlotPhaseMs(&slot, now);
    if (phase >= 0 && gap_last_phase >= 0 && phase < gap_last_phase) gaps_slot = 0U; /* a new slot */
    gap_last_phase = phase;
}

static void ref_service(uint32_t now)
{
    trim_sum += (float)RADIO_RefTrimPpb();
    ++trim_n;
    if ((uint32_t)(now - ref_at) < REF_EVERY_MS) return;
    trim_mean = trim_sum / (float)trim_n;
    trim_sum = 0.0f;
    trim_n = 0U;
    ref_at = now;
    radio_ref_sof_status_t r;
    RADIO_RefSofStatus(&r);
    /* A bus reset is a new host, and a new PLL (a band change on AUTO) a new
     * clock tree the tracker rebuilds on: their acquisitions are not steps. */
    const uint32_t sysclk = RADIO_SysclkHz();
    if (!r.have || usb_reset || sysclk != ref_sysclk) host_seen = 0U;
    usb_reset = 0U;
    ref_sysclk = sysclk;
    if (r.have && !host_seen) { host_seen = 1U; host_since = now; }
    const uint8_t acq = (uint8_t)r.acquisitions;  /* the tracker's count wraps at 8 bits */
    const uint8_t n = (uint8_t)(acq - reacq_last);
    reacq_last = acq;
    if (n && host_seen && (uint32_t)(now - host_since) >= REF_SETTLE_MS && RADIO_RefFollowEnabled())
        reacq_bucket[gap_at] = (uint8_t)((reacq_bucket[gap_at] + n > 0xFFU) ? 0xFFU : reacq_bucket[gap_at] + n);
}

static void load_service(uint32_t now)
{
    if ((uint32_t)(now - load_at) < 1000U) return;
    const uint32_t busy = RADIO_AudioBusyCycles(), cyc = DWT->CYCCNT;
    const uint32_t db = busy - load_busy, dc = cyc - load_cyc;
    load_pct = dc ? (uint8_t)((db >= dc) ? 100U : (uint32_t)(((uint64_t)db * 100U) / dc)) : 0U;
    load_busy = busy;
    load_cyc = cyc;
    load_at = now;
}

void RADIO_DigiService(uint32_t now)
{
    if (!started) digi_start(now);
    /* The slot clock learns only from the DIGI passband: other modes' power
     * has no FT8 cadence, and a receiver that is off (TX) adds nothing. */
    (void)RADIO_SlotAdd(&slot, now, RADIO_DigiReceive() ? RADIO_AudioRxPower() : -1.0f);
    radio_digi_stats_t st;
    RADIO_DigiStats(&st);
    live_service(now, &st);
    gaps_service(now, &st);
    ref_service(now);
    load_service(now);
}

void RADIO_DigiStatus(radio_digi_status_t *out, uint32_t now)
{
    out->host_live = live;
    out->slot_ms = RADIO_SlotPhaseMs(&slot, now);
    out->slot_locked = slot.locked;
    out->slot_depth_db = slot.depth_db;
    uint32_t sum = 0U;
    for (unsigned i = 0U; i < GAP_BUCKETS; ++i) sum += gap_bucket[i];
    out->gaps_10min = sum;
    out->gaps_slot = gaps_slot;
    out->follow = RADIO_RefFollowEnabled();
    radio_ref_sof_status_t r;
    RADIO_RefSofStatus(&r);
    out->ref_state = r.have ? r.state : 0U;
    out->ref_ppm = trim_mean * 0.001f;
    uint32_t reacq = 0U;
    for (unsigned i = 0U; i < GAP_BUCKETS; ++i) reacq += reacq_bucket[i];
    out->reacq_10min = reacq;
    out->load_pct = load_pct;
}

void RADIO_DigiUsbReset(void) { usb_reset = 1U; }

void RADIO_DigiHostKey(uint32_t now)
{
    if (started && RADIO_DigiReceive()) (void)RADIO_SlotSync(&slot, now, 0U);
}

uint8_t RADIO_DigiQuiet(uint32_t now)
{
    if (!RADIO_DigiReceive() || !slot.locked) return 1U;
    return RADIO_SlotQuiet(&slot, now);
}
