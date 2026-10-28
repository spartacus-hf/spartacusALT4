"""mod_pipeline_health_t over SWD (8 stages, 30 reasons): health(r) reads it,
hdelta(a, b) is what changed between two reads."""
REASONS = ["none", "usb_missing", "usb_bad_size", "usb_zero", "usb_out_incomplete", "usb_rearm_failed",
           "usb_feedback_tx_failed", "usb_hw_error", "usb_frame_seq", "pcm_overflow", "pcm_reset_discard",
           "render_underrun", "render_overflow", "render_reset_discard", "render_seq_gap", "render_deadline",
           "dma_error", "dma_start_failed", "dma_seq_gap", "dma_cndtr", "dma_flags_coalesced", "dma_refill_late",
           "dma_channel_skew", "hrtim_compare_range", "hrtim_fault", "rate_plan_invalid", "sched_io_deferred",
           "sched_ui_deferred", "controlled_stream_reset", "mode_not_available"]
SEV = ["NOTE", "WARN", "CONC", "HARD"]
NST, NRE = 8, 30


def health(r):
    base = r.A("pipeline_health")
    w = r.o.read(base, 32, 4 + NST + NRE + 4)
    b = r.o.read(base + 4 * (4 + NST + NRE + 4), 8, 10)
    return {"total": w[0], "soft": w[1], "reasons": w[4 + NST:4 + NST + NRE], "last_tick": w[4 + NST + NRE],
            "last_reason": b[2], "last_severity": b[3], "hard_lockout": b[8]}


def hdelta(a, b, ignore=("sched_io_deferred", "sched_ui_deferred")):
    d = {REASONS[i]: b["reasons"][i] - a["reasons"][i] for i in range(NRE)
         if b["reasons"][i] != a["reasons"][i] and REASONS[i] not in ignore}
    d["soft+"] = b["soft"] - a["soft"]
    if b["last_tick"] != a["last_tick"]:
        d["last"] = (REASONS[b["last_reason"]], SEV[b["last_severity"] & 3])
    d["hard"] = b["hard_lockout"]
    return d
