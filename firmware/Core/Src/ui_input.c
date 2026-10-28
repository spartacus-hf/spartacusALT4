#pragma GCC optimize ("Os")

#include "ui_state.h"
#include "ui_app.h"
#include "board_rev1.h"
#include "main.h"

#define UI_DEB_MS          30U
#define UI_LONG_MS         1000U
/* A spun knob must not queue an unbounded scroll or tune. */
#define UI_ENC_PENDING_MAX 24

typedef struct {
    uint8_t  physical;
    uint8_t  stable;
    uint8_t  long_sent;
    uint32_t edge_tick;
    uint32_t press_tick;
} button_t;

ui_events_t ui_ev;
static button_t btn_up, btn_dn, btn_sel;

static void button_update(button_t *b, uint8_t raw, uint32_t now,
                          uint8_t *short_ev, uint8_t *long_ev)
{
    if (raw != b->physical) { b->physical = raw; b->edge_tick = now; }
    if ((now - b->edge_tick) < UI_DEB_MS) return;
    if (raw != b->stable) {
        b->stable = raw;
        if (raw) { b->press_tick = now; b->long_sent = 0U; }
        else if (!b->long_sent) *short_ev = 1U;
    }
    if (b->stable && !b->long_sent && (now - b->press_tick) >= UI_LONG_MS) {
        b->long_sent = 1U;
        *long_ev = 1U;
    }
}

static int8_t accumulate(int8_t pending, int8_t steps)
{
    int32_t sum = (int32_t)pending + (int32_t)steps;
    if (sum > UI_ENC_PENDING_MAX) sum = UI_ENC_PENDING_MAX;
    if (sum < -UI_ENC_PENDING_MAX) sum = -UI_ENC_PENDING_MAX;
    return (int8_t)sum;
}

void UI_AppButtonsPoll(uint32_t now)
{
    board_encoder_events_t enc;
    button_update(&btn_up,  BOARD_ButtonPressed(2U), now, &ui_ev.up,  &ui_ev.up_long);
    button_update(&btn_dn,  BOARD_ButtonPressed(0U), now, &ui_ev.dn,  &ui_ev.dn_long);
    button_update(&btn_sel, BOARD_ButtonPressed(1U), now, &ui_ev.sel, &ui_ev.sel_long);
    if (!BOARD_EncoderReadEvents(&enc)) return;
    /* The sampler counts the Gray sequence 00-01-11-10 as +1. With the REV1
     * encoder board on its default pins that is anticlockwise -- the sense in
     * which the v7 menus moved up -- while the UI wants clockwise = next,
     * more, tune up. ENCODER REVERSE still swaps both for other wiring. */
    ui_ev.turn = accumulate(ui_ev.turn, (int8_t)-enc.steps);
    ui_ev.turn_held = accumulate(ui_ev.turn_held, (int8_t)-enc.held_steps);
    if (enc.click) ui_ev.click = 1U;
    if (enc.long_click) ui_ev.hold = 1U;
    ui_ev.fkey_turn = accumulate(ui_ev.fkey_turn, (int8_t)-enc.fkey_steps);
    if (enc.fkey_click) ui_ev.fkey = 1U;
    if (enc.fkey_long) ui_ev.fkey_long = 1U;
}

uint8_t UI_EventsPending(void)
{
    return (uint8_t)((ui_ev.turn != 0) || (ui_ev.turn_held != 0) || ui_ev.click || ui_ev.hold ||
                     ui_ev.up || ui_ev.dn || ui_ev.sel ||
                     ui_ev.up_long || ui_ev.dn_long || ui_ev.sel_long ||
                     (ui_ev.fkey_turn != 0) || ui_ev.fkey || ui_ev.fkey_long);
}

void UI_AppConsumeEvents(void)
{
    ui_events_t none = {0};
    ui_ev = none;
}

uint8_t UI_AppBootChoice(uint32_t now)
{
    UI_AppButtonsPoll(now);
    const uint8_t choice = (ui_ev.hold || ui_ev.sel_long) ? 2U : (ui_ev.click || ui_ev.sel) ? 1U : 0U;
    UI_AppConsumeEvents();
    return choice;
}
