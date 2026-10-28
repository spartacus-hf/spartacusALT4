#pragma GCC optimize ("Os") /* the trainer's page: built for size */

/* The Morse trainer's page (ui_trainer.h).
 *
 * A band across the top says whose turn it is; the prompt (once it may be
 * seen) and the answer are drawn in large letters, the correction under
 * them, the first-try score under that. The knob keys: turning sets the
 * keying speed, turning while held the volume, a press opens the action
 * panel (the deadlines wait while it is open), holding ends the session.
 * The panel buttons do the same: UP/DN the speed, SEL the panel, SEL held
 * ends. Longer texts are the card's (card_text.def): the trainer needs the
 * card anyway. */

#include "ui_trainer.h"
#include "ui_state.h"
#include "ui_gfx.h"
#include "ui_labels_priv.h"
#include "ui_format.h"
#include "card_text.h"
#include "mod_morse_trainer.h"
#include "mod_morse_tables.h"
#include "radio.h"
#include "radio_audio.h"
#include "ui_sd.h"
#include "usb_disk_mode.h"
#include "sd_audio_player.h"
#include <stdio.h>
#include <string.h>

/* Panel rows. */
enum { PNL_NEXT = 0U, PNL_SUBMIT, PNL_REPEAT, PNL_SKIP, PNL_PAUSE, PNL_SWAP, PNL_ADD, PNL_LISTEN, PNL_PACE,
       PNL_END, PNL_ROWS };

static struct {
    uint8_t owned;
    uint8_t panel, prow, pedit;  /* action panel, its cursor, a speed being set */
    uint8_t prev_valid;
    mod_config_t prev_mode;
} tr;

uint8_t UI_TrainerOwns(void) { return tr.owned; }

/* ---- Owning the radio ------------------------------------------------------ */

static const char *refusal(void)
{
    const modulator_status_t *st = UI_FrameStatus();
    if (st->tx_state == TX_STATE_TX) return "TRAINER: STOP TRANSMITTING FIRST";
    if (RADIO_CalActive()) return "TRAINER: CALIBRATION RUNS";
    if (USB_DiskModeIsActive()) return "TRAINER: USB DISK MODE";
    if (SD_AudioPlayerIsOpen() || UI_SDPlayerPageActive()) return "TRAINER: CLOSE THE SD PLAYER";
    if (st->usb_stream_active) return "TRAINER: USB AUDIO IS TRANSMITTING";
    return NULL;
}

uint8_t UI_TrainerBegin(uint8_t act, uint32_t now)
{
    const char *why = tr.owned ? NULL : refusal();
    if (why != NULL) {
        UI_NavFlash(why);
        return 0U;
    }
    /* The record before the switch to CW: that starts the transport, and the
     * card waits while it runs. Read after it, from any other mode, the
     * record was skipped and the trainer ran on a blank one it never saved
     * (bench, 2026-10-07). */
    (void)MOD_TrainerRecordLoad();
    if (!tr.owned) {
        /* The keyer runs in CW only; the mode the radio was in comes back. */
        tr.prev_mode = MOD_GetTestConfig();
        tr.prev_valid = (uint8_t)(tr.prev_mode != MOD_CFG_CW_14MHZ);
        if (tr.prev_valid) {
            MOD_SetTestConfig(MOD_CFG_CW_14MHZ);
            MOD_ApplyRequestedConfigNow();
        }
        RADIO_AudioRxQuiet(1U);
        tr.owned = 1U;
    }
    if (act != UI_TRAINER_GUIDED) {
        MTRAIN_Prefs()->act = act;
        MTRAIN_PrefsChanged();
    }
    tr.panel = 0U;
    return MTRAIN_Start((uint8_t)(act == UI_TRAINER_GUIDED), now);
}

void UI_TrainerLeave(void)
{
    if (!tr.owned) return;
    MTRAIN_Stop();
    (void)MOD_TrainerRecordSave();
    RADIO_AudioRxQuiet(0U);
    if (tr.prev_valid) {
        MOD_SetTestConfig(tr.prev_mode);
        MOD_ApplyRequestedConfigNow();
        tr.prev_valid = 0U;
    }
    tr.owned = 0U;
    tr.panel = 0U;
}

/* A symbol as the page shows it: "K", or "<SK>". */
static void sym_label(uint8_t sym, char *out)
{
    char l[5];
    strcat(out, sym == '*' ? "*" : MORSE_Label(sym, l));
}

const char *UI_TrainerAddCharacter(void)
{
    static char said[24];
    (void)MOD_TrainerRecordLoad();
    if (!MTRAIN_LoadCourse()) return "THE COURSE IS ON THE SD CARD";
    if (!MTRAIN_AddCharacter()) return "THE WHOLE COURSE IS IN";
    (void)MOD_TrainerRecordSave();
    const mtrain_view_t *v = MTRAIN_View();
    (void)snprintf(said, sizeof(said), "LESSON %u: ", (unsigned)v->lesson);
    sym_label(v->newest, said);
    return said;
}

/* ---- Panel ------------------------------------------------------------------ */

static const mtrain_view_t *view(void) { return MTRAIN_View(); }

static uint8_t panel_has(uint8_t row)
{
    const mtrain_view_t *v = view();
    const uint8_t s = v->state, answering = (uint8_t)(s == MTRAIN_THINK || s == MTRAIN_ANSWER);
    switch (row) {
        case PNL_NEXT:   return (uint8_t)(s == MTRAIN_STEP_DONE || s == MTRAIN_SUMMARY ||
                                          (v->guided && (s == MTRAIN_KEYING || s == MTRAIN_CHECK)));
        case PNL_SUBMIT: return answering;
        case PNL_REPEAT: return (uint8_t)(v->prompt_len && (answering || s == MTRAIN_FEEDBACK || s == MTRAIN_SHOW));
        case PNL_SKIP:   return (uint8_t)(v->prompt_len && s != MTRAIN_PAUSE && s != MTRAIN_STEP_DONE && s != MTRAIN_SUMMARY);
        case PNL_PAUSE:  return (uint8_t)(s != MTRAIN_SUMMARY && s != MTRAIN_ERROR);
        case PNL_SWAP:   return (uint8_t)(s == MTRAIN_CHECK);
        case PNL_ADD:    return (uint8_t)(v->recommend && v->lesson < v->lesson_max);
        case PNL_LISTEN:
        case PNL_PACE:   return (uint8_t)(v->act == MTRAIN_ECHO || v->act == MTRAIN_LISTEN);
        default:         return 1U;
    }
}

static uint8_t panel_rows(uint8_t *rows)
{
    uint8_t n = 0U;
    for (uint8_t r = 0U; r < PNL_ROWS; ++r)
        if (panel_has(r)) rows[n++] = r;
    return n;
}

static void panel_open(uint8_t on)
{
    tr.panel = on;
    tr.prow = 0U;
    tr.pedit = 0U;
    MTRAIN_Hold(on);
}

static void panel_do(uint8_t row)
{
    if (row == PNL_LISTEN || row == PNL_PACE) {
        tr.pedit = (uint8_t)!tr.pedit;
        return;
    }
    panel_open(0U);
    switch (row) {
        case PNL_NEXT:   MTRAIN_Next(); break;
        case PNL_SUBMIT: MTRAIN_Submit(); break;
        case PNL_REPEAT: MTRAIN_Repeat(); break;
        case PNL_SKIP:   MTRAIN_Skip(); break;
        case PNL_PAUSE:  MTRAIN_Pause((uint8_t)(view()->state != MTRAIN_PAUSE)); break;
        case PNL_SWAP:   MTRAIN_Swap(); break;
        case PNL_ADD:    if (MTRAIN_AddCharacter()) (void)MOD_TrainerRecordSave(); break;
        default:         MTRAIN_Stop(); break; /* END: back to the list */
    }
}

static uint8_t clamp_wpm(int32_t w, int32_t lo) { return (uint8_t)(w < lo ? lo : (w > 60 ? 60 : w)); }

static void panel_events(void)
{
    uint8_t rows[PNL_ROWS];
    const uint8_t n = panel_rows(rows);
    if (tr.prow >= n) tr.prow = 0U;
    const int32_t move = ui_ev.turn + ui_ev.dn - ui_ev.up;
    if (ui_ev.hold || ui_ev.sel_long || view()->levers) {
        panel_open(0U); /* a lever closes it: the learner is keying */
        return;
    }
    if (tr.pedit && move != 0) {
        mtrain_prefs_t *p = MTRAIN_Prefs();
        uint8_t *w = (rows[tr.prow] == PNL_LISTEN) ? &p->listen_wpm : &p->pace_wpm;
        *w = clamp_wpm((int32_t)*w + move, 2);
        MTRAIN_PrefsChanged();
        return;
    }
    if (move != 0) tr.prow = (uint8_t)(((int32_t)tr.prow + move % (int32_t)n + (int32_t)n) % (int32_t)n);
    if (ui_ev.click || ui_ev.sel) panel_do(rows[tr.prow]);
}

/* ---- Session ---------------------------------------------------------------- */

uint8_t UI_TrainerEvents(uint32_t now)
{
    (void)now;
    const mtrain_view_t *v = view();
    if (!MTRAIN_Active()) return UI_TRAINER_EXIT;
    ui.dirty = 1U;
    if (tr.panel) {
        panel_events();
    } else if (ui_ev.hold || ui_ev.sel_long || ui_ev.fkey) {
        MTRAIN_Stop();
    } else {
        const int32_t speed = ui_ev.turn + ui_ev.up - ui_ev.dn;
        if (speed != 0) {
            mtrain_prefs_t *p = MTRAIN_Prefs();
            p->key_wpm = clamp_wpm((int32_t)p->key_wpm + speed, 5);
            MTRAIN_PrefsChanged();
        }
        if (ui_ev.turn_held != 0) {
            const setting_item_t *it = UI_GetSetting(SET_REV1_VOLUME);
            if (it && it->get && it->set) it->set(UI_ClampSettingValue(it, it->get() + 2.0f * (float)ui_ev.turn_held));
        }
        const uint8_t s = v->state;
        if (ui_ev.click && s == MTRAIN_CHECK && v->check == MTRAIN_CHECK_SWAP) MTRAIN_Swap();
        else if (ui_ev.click && (s == MTRAIN_STEP_DONE || s == MTRAIN_SUMMARY ||
                                 (s == MTRAIN_CHECK && v->check == MTRAIN_CHECK_OK) ||
                                 (v->guided && s == MTRAIN_KEYING))) MTRAIN_Next();
        else if (ui_ev.click || ui_ev.sel) panel_open(1U);
    }
    /* The record is written at the quiet moments only, never while keying. */
    const uint8_t s = v->state;
    if (MTRAIN_ProgressDirty() && (s == MTRAIN_OFF || s == MTRAIN_PAUSE || s == MTRAIN_SUMMARY ||
                                   s == MTRAIN_STEP_DONE || s == MTRAIN_ERROR))
        (void)MOD_TrainerRecordSave();
    return MTRAIN_Active() ? UI_TRAINER_STAY : UI_TRAINER_EXIT;
}

/* ---- Drawing ------------------------------------------------------------------ */

#define BAND_Y   34U
#define PROMPT_Y 62U
#define ANSWER_Y 112U

static void card_line(card_text_t id, uint16_t y, uint8_t lines)
{
    (void)UI_DrawWrapped(GFX_MARGIN, y, (uint16_t)(GFX_W - 2U * GFX_MARGIN), lines, GFX_TEXT, UI_CardTextCached(id));
}

/* A symbol's dots and dashes as blocks: a dot a square, a dash three. */
static void draw_pattern(uint16_t x, uint16_t y, uint8_t sym)
{
    char p[8];
    for (const char *e = MORSE_Pattern(sym, p); *e; ++e) {
        const uint16_t w = (*e == '-') ? 24U : 8U;
        GFX_Rect(x, y, w, 8U, GFX_ACCENT);
        x = (uint16_t)(x + w + 8U);
    }
}

/* A row of symbols in large letters, as large as the row allows. */
static void draw_big(uint16_t y, const uint8_t *s, uint8_t n, uint8_t fg)
{
    char text[64];
    text[0] = 0;
    for (uint8_t i = 0U; i < n && strlen(text) + 5U < sizeof(text); ++i) sym_label(s[i], text);
    const size_t len = strlen(text);
    const uint8_t scale = (len <= 6U) ? 3U : ((len <= 18U) ? 2U : 1U);
    (void)GFX_TextBig(GFX_MARGIN, y, scale, fg, text + (len > 36U ? len - 36U : 0U));
}

/* The band: whose turn it is, in the verdict's colour. */
static const char *band_text(const mtrain_view_t *v, uint8_t *bg)
{
    *bg = GFX_PANEL_HI;
    switch (v->state) {
        case MTRAIN_LOAD:
        case MTRAIN_PLAY:     return "LISTEN";
        case MTRAIN_THINK:
        case MTRAIN_ANSWER:   return "YOUR TURN";
        case MTRAIN_SHOW:     return "THAT WAS";
        case MTRAIN_PAUSE:    return "PAUSED";
        case MTRAIN_SUMMARY:  return "SESSION OVER";
        case MTRAIN_STEP_DONE:*bg = GFX_OK_BG; return "WELL DONE";
        case MTRAIN_KEYING:   return "KEY ANYTHING";
        case MTRAIN_CHECK:    return "PADDLE CHECK";
        case MTRAIN_ERROR:    *bg = GFX_ALERT_BG; return "STOPPED";
        default:              break;
    }
    if (v->verdict == MTRAIN_V_RIGHT) { *bg = GFX_OK_BG; return "RIGHT"; }
    if (v->verdict == MTRAIN_V_LOST) return "MISSED SOME KEYING: AGAIN";
    if (v->verdict == MTRAIN_V_SKIPPED) return "SKIPPED";
    *bg = GFX_ALERT_BG;
    return (v->verdict == MTRAIN_V_TIMEOUT) ? "TIME UP" : "NOT QUITE";
}

/* What the card says to do now, for the screens that are mostly words. */
static card_text_t screen_text(const mtrain_view_t *v)
{
    static const card_text_t check[] = {TXT_TR_PADDLE_DIT, TXT_TR_PADDLE_SWAP, TXT_TR_PADDLE_SQUEEZE,
                                        TXT_TR_PADDLE_RELEASE, TXT_TR_PADDLE_OK};
    switch (v->state) {
        case MTRAIN_CHECK:     return check[v->check < 5U ? v->check : 4U];
        case MTRAIN_KEYING:    return v->guided ? TXT_TR_EXPLORE : TXT_TR_FREE;
        case MTRAIN_STEP_DONE: return TXT_TR_STEP_DONE;
        case MTRAIN_SUMMARY:   return v->recommend ? TXT_TR_ADD : TXT_TR_SUMMARY;
        default:               break;
    }
    if (v->reveal) return TXT_NONE;
    if (v->guided && v->text) return (card_text_t)(TXT_TR_PADDLE_DIT - 1 + v->text); /* the course's step */
    return (v->act == MTRAIN_LISTEN) ? TXT_TR_LISTEN : (v->act == MTRAIN_LEARN ? TXT_TR_LEARN : TXT_TR_ECHO);
}

static void draw_session(const mtrain_view_t *v)
{
    char line[64];
    uint8_t bg;
    const char *band = band_text(v, &bg);
    GFX_Fill(0U, BAND_Y, GFX_W, 24U, bg);
    (void)GFX_TextCenter(0U, GFX_W, (uint16_t)(BAND_Y + 4U), GFX_FONT_BODY, GFX_WHITE, band);
    if (v->state == MTRAIN_ERROR) {
        (void)GFX_Text(GFX_MARGIN, PROMPT_Y, GFX_FONT_BODY, GFX_WARN,
                       v->error == MTRAIN_ERR_CARD ? "NO SD CARD: PROGRESS IS NOT SAVED"
                       : (v->error == MTRAIN_ERR_CUSTOM ? "NO CW/CUSTOM.TXT ON THE CARD"
                                                        : "CWPACK.BIN MISSING OR OLD ON THE CARD"),
                       (uint16_t)(GFX_W - GFX_MARGIN));
        return;
    }
    const card_text_t say = screen_text(v);
    if (say != TXT_NONE) card_line(say, PROMPT_Y, 3U);
    if (v->state == MTRAIN_CHECK) {
        (void)GFX_Badge(64U, 130U, GFX_FONT_BODY, (v->levers & 1U) ? GFX_OK : GFX_MUTED, "  DIT  ");
        (void)GFX_Badge(184U, 130U, GFX_FONT_BODY, (v->levers & 2U) ? GFX_OK : GFX_MUTED, "  DAH  ");
        return;
    }
    if (v->state == MTRAIN_KEYING) { draw_big(110U, v->line, v->line_len, GFX_WHITE); return; }
    if (v->state == MTRAIN_STEP_DONE || v->state == MTRAIN_SUMMARY) return;
    /* The prompt large once it may be seen, a single character's pattern beside it. */
    if (v->reveal && v->prompt_len) {
        draw_big(PROMPT_Y, v->prompt, v->prompt_len, GFX_TEXT);
        if (v->prompt_len == 1U) draw_pattern(96U, (uint16_t)(PROMPT_Y + 20U), v->prompt[0]);
    }
    if (v->act != MTRAIN_LISTEN && v->answer_len)
        draw_big(ANSWER_Y, v->answer, v->answer_len, v->verdict == MTRAIN_V_RIGHT ? GFX_OK : GFX_WHITE);
    if (v->state == MTRAIN_FEEDBACK && v->verdict == MTRAIN_V_WRONG && v->expected) {
        char p[8];
        strcpy(line, "EXPECTED ");
        sym_label(v->expected, line);
        strcat(line, " ");
        strcat(line, MORSE_Pattern(v->expected, p));
        if (v->heard) {
            strcat(line, "   HEARD ");
            sym_label(v->heard, line);
            if (v->heard != '*') { strcat(line, " "); strcat(line, MORSE_Pattern(v->heard, p)); }
        }
        (void)GFX_Text(GFX_MARGIN, 164U, GFX_FONT_BODY, GFX_WARN, line, (uint16_t)(GFX_W - GFX_MARGIN));
    }
    if (v->state == MTRAIN_FEEDBACK && v->verdict == MTRAIN_V_RIGHT && v->joined) card_line(TXT_TR_JOINED, 164U, 1U);
}

/* First tries, the weakest character, the time or how the lesson goes. */
static void draw_score(const mtrain_view_t *v)
{
    char line[64], w[6] = "";
    line[0] = 0;
    if (v->done) (void)snprintf(line, sizeof(line), "FIRST TRY %u%%  ", (unsigned)(100U * v->right / v->done));
    if (v->weakest) {
        sym_label(v->weakest, w);
        strcat(line, "WEAKEST ");
        strcat(line, w);
        strcat(line, "  ");
    }
    if (v->guided && v->done)
        (void)snprintf(line + strlen(line), sizeof(line) - strlen(line), "LAST %u: %u RIGHT",
                       (unsigned)MTRAIN_Progress()->win_n, (unsigned)v->window);
    else if (v->left_ms)
        (void)snprintf(line + strlen(line), sizeof(line) - strlen(line), "%lu:%02lu LEFT",
                       (unsigned long)(v->left_ms / 60000UL), (unsigned long)(v->left_ms / 1000UL % 60UL));
    (void)GFX_Text(GFX_MARGIN, 184U, GFX_FONT_SMALL, GFX_DIM, line, (uint16_t)(GFX_W - GFX_MARGIN));
    uint16_t x = (uint16_t)(GFX_Badge(GFX_MARGIN, 200U, GFX_FONT_BODY, (v->levers & 1U) ? GFX_OK : GFX_MUTED, "DIT") + 8U);
    x = (uint16_t)(GFX_Badge(x, 200U, GFX_FONT_BODY, (v->levers & 2U) ? GFX_OK : GFX_MUTED, "DAH") + 8U);
    if (v->recommend && v->lesson < v->lesson_max) (void)GFX_Badge(x, 200U, GFX_FONT_BODY, GFX_ACCENT, "READY FOR MORE");
}

static void draw_panel(void)
{
    static const char *const names[PNL_ROWS] = {"NEXT", "SUBMIT ANSWER", "REPEAT", "SKIP", "PAUSE", "SWAP LEVERS",
                                                "ADD NEXT CHARACTER", "LISTEN WPM", "SPACING WPM", "END SESSION"};
    uint8_t rows[PNL_ROWS];
    char value[8];
    const uint8_t n = panel_rows(rows);
    /* Whole 8-pixel tiles, pixels cleared: a fill only colours the tiles, so
     * the page under the panel showed through its rows, and the panel's
     * colour spilled onto the half tile below it. The frame keeps tiles of
     * its own; sharing one, it took the title's or a row's colour there. */
    const uint16_t y0 = 40U, h = (uint16_t)(n * UI_ROW_H + 32U);
    const mtrain_prefs_t *p = MTRAIN_Prefs();
    GFX_ClearPixels(32U, y0, (uint16_t)(GFX_W - 64U), h);
    GFX_Fill(32U, y0, (uint16_t)(GFX_W - 64U), h, GFX_PANEL);
    GFX_Frame(32U, y0, (uint16_t)(GFX_W - 64U), h, GFX_LINE);
    (void)GFX_Text(40U, (uint16_t)(y0 + 8U), GFX_FONT_SMALL, GFX_ACCENT, "ACTIONS", (uint16_t)(GFX_W - 40U));
    for (uint8_t i = 0U; i < n; ++i) {
        const uint8_t r = rows[i];
        value[0] = 0;
        if (r == PNL_LISTEN || r == PNL_PACE)
            (void)UI_AppendU(value, r == PNL_LISTEN ? p->listen_wpm : p->pace_wpm, 1U);
        UI_DrawRow(40U, (uint16_t)(GFX_W - 80U), (uint16_t)(y0 + 24U + i * UI_ROW_H),
                   (i == tr.prow) ? (tr.pedit ? UI_ROW_EDIT : UI_ROW_FOCUS) : UI_ROW_NORMAL,
                   (r == PNL_PAUSE && view()->state == MTRAIN_PAUSE) ? "RESUME" : names[r], value);
    }
}

void UI_TrainerRender(uint32_t now)
{
    const mtrain_view_t *v = view();
    const mtrain_prefs_t *p = MTRAIN_Prefs();
    char line[64];
    strcpy(line, mtrain_act_label(v->act));
    if (v->act != MTRAIN_PADDLE && v->act != MTRAIN_FREE && v->lesson)
        (void)snprintf(line + strlen(line), sizeof(line) - strlen(line), "  LESSON %u", (unsigned)v->lesson);
    UI_DrawTitle(line, "RF OFF");
    (void)snprintf(line, sizeof(line), "LISTEN %u/%u WPM   KEY %u WPM   VOLUME %u", (unsigned)v->listen_wpm,
                   (unsigned)(p->pace_wpm < v->listen_wpm ? p->pace_wpm : v->listen_wpm), (unsigned)p->key_wpm,
                   (unsigned)RADIO_Config()->volume);
    (void)GFX_Text(GFX_MARGIN, 19U, GFX_FONT_SMALL, GFX_DIM, line, (uint16_t)(GFX_W - GFX_MARGIN));
    if (MTRAIN_Active()) {
        draw_session(v);
        draw_score(v);
    }
    if (tr.panel) draw_panel();
    UI_DrawFooter(tr.panel ? GFX_SYM_TURN " CHOOSE   " GFX_SYM_PRESS " DO   HOLD CLOSE"
                           : GFX_SYM_PRESS " ACTIONS   " GFX_SYM_TURN " KEY WPM   HOLD END",
                  tr.panel ? NULL : "HOLD+" GFX_SYM_TURN " VOL", now);
}
