"""Band/mode changes, settling, the system clock, and panel input over SWD.

Panel events: write ui_ev bytes (+0 turn, +1 turn_held, +2 click, +3 hold,
+11 function key). The ui struct: +0 page, +2 home level, +48 menu level,
+49 menu category, +50 menu row.
"""
import time

RCC = 0x40021000
MODES = {"USB": 0, "LSB": 1, "CW": 2, "AM": 3, "FM": 4}
UI_EV = {"turn": 0, "turn_held": 1, "click": 2, "hold": 3, "fkey": 11}


def settle(r, timeout=6.0):
    """Seconds until the configuration and carrier retune are applied (None: not settled)."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        s = [r.u8(n) for n in ("carrier_retune_state", "carrier_frequency_apply_pending",
                               "carrier_clock_retune_in_progress")]
        if r.u32("requested_config") == r.u32("active_config") and not any(s):
            return round(time.monotonic() - t0, 2)
        time.sleep(0.05)
    return None


def settle_real(r, t_min=0.8, timeout=6.0):
    time.sleep(t_min)
    return settle(r, timeout)


def set_band_mode(r, band=None, mode=None):
    """band: index (set 201); mode: 0 USB 1 LSB 2 CW 3 AM 4 FM (set 200)."""
    if band is not None and r.u8("active_band_idx") != band:
        r.sput(201, band)
        t0 = time.monotonic()
        while r.u8("active_band_idx") != band and time.monotonic() - t0 < 5:
            time.sleep(0.05)
    if mode is not None:
        for _ in range(20):
            r.sput(200, mode)
            if int(float(r.sget(200))) == mode:
                break
            time.sleep(0.25)
    return settle(r)


def go(r, band, mode, via=()):
    """Tune band/mode, through the bands in via first (the clock plan
    depends on the way there), and settle. Returns SYSCLK in MHz."""
    if via:
        set_band_mode(r, mode=mode)
        settle_real(r)
        for vb in via:
            set_band_mode(r, band=vb)
            settle_real(r)
    set_band_mode(r, band=band, mode=mode)
    settle_real(r)
    return sysclk(r)


def sysclk(r):
    """SYSCLK in MHz from RCC (PLL on the 8 MHz reference)."""
    cfgr, pll = r.o.u32(RCC + 0x08), r.o.u32(RCC + 0x0C)
    assert (cfgr >> 2) & 3 == 3, hex(cfgr)          # SWS = PLL
    m = ((pll >> 4) & 0xF) + 1
    n = (pll >> 8) & 0x7F
    rr = (((pll >> 25) & 3) + 1) * 2
    return round(8e6 / m * n / rr / 1e6, 3)


def ui_fields(r):
    u = r.o.rbytes(r.A("ui"), 64)
    return {"page": u[0], "home_level": u[2], "menu_level": u[48], "menu_cat": u[49],
            "menu_row": u[50], "editing": u[53]}


def ui_event(r, ev, value=1, settle_s=0.35):
    r.o.write(r.A("ui_ev") + UI_EV[ev], 8, value & 0xFF)
    time.sleep(settle_s)
    return ui_fields(r)


def ui_home(r):
    """Back to the home page, tuning level (function key)."""
    for _ in range(6):
        f = ui_fields(r)
        if f["page"] == 0 and f["home_level"] == 0:
            return f
        ui_event(r, "fkey", settle_s=0.5)
    return ui_fields(r)
