#ifndef UI_STATE_H
#define UI_STATE_H

/* Front-panel state shared by input, navigation and the renderers of the
 * landscape UI. The settings model (ids, groups, help) is ui_priv.h. */

#include <stdint.h>
#include "ui_priv.h"
#include "ui_tune.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAGE_HOME = 0,
    PAGE_MENU,
    PAGE_FREQ,          /* frequency digit entry, works without the knob */
    PAGE_CAL,           /* SSB phase calibration bins */
    PAGE_DIAG,          /* live diagnostics, paged: receive .. system */
    PAGE_TESTS,         /* internal test signals (debug UI) */
    PAGE_TX_TEST,
    PAGE_CW_TRAINER,
    PAGE_SD,
    PAGE_PD,
    PAGE_BPF_SWEEP,
    PAGE_AF_SWEEP,
    PAGE_IQ_SCOPE,
    PAGE_SELFTEST,
    PAGE_RUNTIME,       /* raw transport and CPU counters (debug UI) */
    PAGE_WATERFALL,     /* spectrum and waterfall, from the home row only */
    PAGE_COUNT
} ui_page_t;

#define UI_CAL_SKEW_ITEM       0U
#define UI_CAL_COMMON_FIRST    1U
#define UI_CAL_THETA_FIRST     (UI_CAL_COMMON_FIRST + PHASE_CAL_BINS)
#define UI_CAL_STATUS_ITEM     (UI_CAL_THETA_FIRST + PHASE_CAL_BINS)
#define UI_CAL_ITEM_COUNT      (UI_CAL_STATUS_ITEM + 1U)

/* ---- Input ------------------------------------------------------------ */
/* Everything that happened since the last UI_AppHandle. The knob is the
 * primary control; UP/DN/SEL are the panel buttons. Clockwise is +. */
typedef struct {
    int8_t  turn;       /* knob detents, shaft released */
    int8_t  turn_held;  /* knob detents turned with the shaft pressed */
    uint8_t click;      /* knob press */
    uint8_t hold;       /* knob held 0.6 s */
    uint8_t up, dn, sel;
    uint8_t up_long, dn_long, sel_long;
    /* FUNC key (default PC14): the second key on the encoder board. */
    int8_t  fkey_turn;  /* knob detents turned with FUNC held */
    uint8_t fkey;       /* FUNC press */
    uint8_t fkey_long;  /* FUNC held 0.6 s */
} ui_events_t;

extern ui_events_t ui_ev;
uint8_t UI_EventsPending(void);

/* ---- Menu model (ui_menu.c) ------------------------------------------- */
typedef enum {
    UI_CAT_OPERATE = 0,
    UI_CAT_AUDIO,
    UI_CAT_CW,
    UI_CAT_VOICE,
    UI_CAT_AMFM,
    UI_CAT_TX,
    UI_CAT_DISPLAY,
    UI_CAT_BOARD,
    UI_CAT_SYSTEM,
    UI_CAT_DIAG,        /* debug UI only */
    UI_CAT_COUNT
} ui_cat_t;

typedef enum {
    UI_ROW_HEADER = 0,  /* section title, not selectable */
    UI_ROW_SETTING,     /* id = setting_id_t */
    UI_ROW_GROUP,       /* id = audio_group_t, opens as its own list */
    UI_ROW_PAGE,        /* id = ui_page_t */
    UI_ROW_ACTION       /* id = ui_action_t */
} ui_row_kind_t;

typedef enum {
    UI_ACT_SAVE = 0,
    UI_ACT_LOAD,
    UI_ACT_FACTORY,
    UI_ACT_AB_SWAP,
    UI_ACT_DEBUG_UI,
    /* The Morse trainer's list (CW > TRAINER): each starts a session. */
    UI_ACT_TRAIN_GUIDED,
    UI_ACT_TRAIN_ECHO,
    UI_ACT_TRAIN_LISTEN,
    UI_ACT_TRAIN_LEARN,
    UI_ACT_TRAIN_FREE,
    UI_ACT_TRAIN_PADDLE,
    UI_ACT_TRAIN_ADD,      /* the next character into the lesson */
    UI_ACT_TRAIN_DELETE,   /* the learner's record, after a second click */
    UI_ACT_COUNT
} ui_action_t;

typedef struct {
    uint8_t  kind;      /* ui_row_kind_t */
    uint16_t id;
} ui_row_t;

uint8_t UI_MenuCategoryCount(void);
uint8_t UI_MenuCategoryAt(uint8_t visible);
const char *UI_MenuCategoryLabel(uint8_t cat);
const char *UI_MenuCategoryHelp(uint8_t cat);
/* Rows of a category, or of an opened group link when group is not
 * AUDIO_GROUP_NONE. A category's inline sections expand to a header row
 * followed by the group's settings. */
uint8_t UI_MenuRowCount(uint8_t cat, uint8_t group);
uint8_t UI_MenuRowAt(uint8_t cat, uint8_t group, uint8_t index, ui_row_t *out);
/* First selectable row at or after (dir > 0) / before (dir < 0) index,
 * or index itself when there is none. */
uint8_t UI_MenuSelectable(uint8_t cat, uint8_t group, uint8_t index, int8_t dir);
const char *UI_MenuRowLabel(const ui_row_t *row);
const char *UI_MenuRowHelp(const ui_row_t *row);
const char *UI_PageTitle(ui_page_t page);
const char *UI_ActionLabel(ui_action_t action);

/* ---- Navigation state (ui_nav.c) -------------------------------------- */
/* Home is three zones stacked top to bottom. The knob tunes; holding it
 * lifts it to the zone picker, and holding again opens the menu. */
typedef enum { UI_ZONE_TUNE = 0, UI_ZONE_SETTINGS, UI_ZONE_DIAG, UI_ZONE_COUNT } ui_zone_t;
typedef enum { UI_HOME_TUNE = 0, UI_HOME_PICK, UI_HOME_SLIDE } ui_home_level_t;

typedef struct {
    ui_page_t page;
    uint8_t   dirty;
    /* home */
    uint8_t   home_level;       /* ui_home_level_t */
    uint8_t   zone;             /* ui_zone_t under the picker */
    uint8_t   slide_focus;      /* item of the settings row */
    uint8_t   slide_first;      /* first visible cell */
    uint8_t   tune_digit;
    uint8_t   band_index;       /* band chosen while its retune is pending */
    uint8_t   band_pending;     /* the pending tune is that band choice */
    uint8_t   tune_applied;     /* a tune went to the carrier this pass */
    uint8_t   mode_pending;     /* UI_UMODE_COUNT when none */
    uint32_t  home_until;       /* picker / row idle timeout */
    uint32_t  mode_due;
    uint32_t  last_turn_ms;
    ui_tune_state_t tune;
    /* the boot log stays readable until this, or a key */
    uint8_t   boot_hold;
    uint32_t  boot_until;
    /* menu */
    uint8_t   menu_level;       /* 0 categories, 1 rows, 2 rows of an opened group */
    uint8_t   menu_cat;         /* visible category index */
    uint8_t   menu_row;
    uint8_t   menu_group;       /* AUDIO_GROUP_NONE unless level 2 */
    uint8_t   menu_parent_row;  /* row of the group link, to return to */
    uint8_t   editing;          /* a setting is being edited: edit_id */
    uint8_t   edit_id;          /* setting_id_t, from the menu or the settings row */
    uint8_t   cat_row[UI_CAT_COUNT];
    float     edit_value;
    float     edit_orig;
    uint8_t   menu_first;       /* scroll position of the row pane */
    uint8_t   cat_first;        /* scroll position of the category pane */
    /* pages */
    uint8_t   cursor;
    uint8_t   list_first;
    uint8_t   page_edit;
    ui_page_t page_back;        /* where BACK leads from a bench page */
    /* feedback */
    const char *flash_text;
    uint32_t  flash_until;
} ui_state_t;

extern ui_state_t ui;

void UI_NavOpenPage(ui_page_t page);
/* DIAGNOSTICS on one of its pages (UI_DIAG_PAGE_*). */
void UI_NavOpenDiag(uint8_t diag_page);
void UI_NavFlash(const char *text);
/* "PROTECTION SWR - HOLD TO CLEAR": the latched faults and the way out. */
const char *UI_NavFaultMessage(void);
/* Start the Morse trainer: the activity in its preferences, or the one an
 * action names (UI_ACT_TRAIN_*), on its own page. */
void UI_EnterCwTrainerPage(void);
uint8_t UI_ModeApply(ui_umode_t mode);
/* DIGI on the given sideband (CAT MD9 = USB, MD6 = LSB), in one step. */
uint8_t UI_ModeApplyDigi(sideband_t sideband);
ui_umode_t UI_ModeCurrent(void);
/* For CAT (cat.c): the dial as the panel shows it (a tune just asked for,
 * else the carrier); a tune to `hz` taken the knob's way, paced by
 * tune_service -- 0 out of range or while a page owns the carrier; and the
 * mode the panel shows (one being dialled in counts). */
uint32_t UI_NavTuneHz(void);
uint8_t UI_NavTuneTo(uint32_t hz);
ui_umode_t UI_NavModeShown(void);
uint8_t UI_NavStepDigit(void);
void UI_NavSetStepDigit(uint8_t digit);
void UI_NavSelectBand(uint8_t index);
/* Run a menu action; returns the text to flash. */
const char *UI_NavAction(ui_action_t action);
/* Settings-row / menu value text: percent, dB and wpm where the settings
 * map would show a raw factor. */
void UI_SlideValue(uint8_t id, float value, char *dst);

/* ---- Diagnostics (ui_diag.c) ------------------------------------------ */
typedef enum { UI_TONE_TEXT = 0, UI_TONE_OK, UI_TONE_WARN, UI_TONE_ALERT, UI_TONE_DIM } ui_tone_t;
typedef struct {
    const char *label;
    char        value[20];
    uint8_t     tone;           /* ui_tone_t */
} ui_diag_cell_t;
#define UI_DIAG_PAGES      6U
#define UI_DIAG_PAGE_POWER  2U
#define UI_DIAG_PAGE_SYSTEM 4U
#define UI_DIAG_PAGE_DIGI   5U
#define UI_DIAG_CELLS      12U
#define UI_DIAG_SUMMARY    6U
/* The six cells under the settings row. */
void UI_DiagSummary(ui_diag_cell_t cells[UI_DIAG_SUMMARY], const modulator_status_t *st, uint32_t now);
/* The same six cells in DIGI: host level, gaps, slot, reference, drift, TX. */
void UI_DigiSummary(ui_diag_cell_t cells[UI_DIAG_SUMMARY], const modulator_status_t *st, uint32_t now);
/* One full-screen page; returns the cells written. */
uint8_t UI_DiagPage(uint8_t page, ui_diag_cell_t cells[UI_DIAG_CELLS], const modulator_status_t *st, uint32_t now);
const char *UI_DiagPageTitle(uint8_t page);
uint8_t UI_ToneColor(uint8_t tone);

/* ---- Live power and SWR (radio_rf_meter.h) ----------------------------- */
/* What the TX panel, the home meter row and the TX TEST page show of the
 * meter. While the display service is held off in TX (CW keying, voice) the
 * TX cue (UI_AppServiceTxCue) redraws only these, once per new reading, so
 * the panel stays live without a frame build. */
typedef struct {
    float    pep_w;      /* forward PEP */
    float    ref_w;      /* reflected */
    float    swr;        /* the bridge's leakage taken out */
    float    drain_v;    /* PA drain */
    float    vin_v;      /* supply while transmitting */
    uint32_t seq;        /* the reading's number (rf_meter.seq) */
    uint8_t  valid;      /* a reading of this over (or of the last, inside the hang) */
    uint8_t  swr_valid;  /* forward >= 0.1 W */
    uint8_t  swr_alert;  /* SWR at or over the protection limit */
    uint8_t  tune_pct;   /* TX TEST drain cap */
} ui_tx_meter_t;
/* Fills `m`; returns 1 while the home screen should show the TX panel:
 * transmitting, or within METER HANG MS of the last reading, with the panel
 * switched on. */
uint8_t UI_TxMeter(ui_tx_meter_t *m, uint32_t now);

/* ---- Home view (filled by ui_nav.c, drawn by ui_render.c) -------------- */
typedef enum { UI_TXS_RX = 0, UI_TXS_TX, UI_TXS_WAIT, UI_TXS_OFF } ui_txs_t;
typedef enum { UI_MSG_NONE = 0, UI_MSG_INFO, UI_MSG_WARN, UI_MSG_ALERT } ui_msg_level_t;

typedef struct {
    uint32_t    freq_hz;
    uint8_t     step_digit;
    uint8_t     tune_hold;      /* target waiting for the carrier */
    uint8_t     txs;            /* ui_txs_t */
    uint8_t     umode;          /* ui_umode_t */
    uint8_t     rx_mismatch;    /* receiver set to another mode */
    uint8_t     rx_mode;
    uint8_t     tx_muted;
    uint8_t     usb_audio;
    uint8_t     pc_link;        /* SCTRL_LinkBadge */
    uint8_t     meter_tx;
    uint8_t     clip;
    uint8_t     tx_rms;         /* 0..100 */
    uint8_t     tx_peak;        /* 0..100 */
    int16_t     rx_dbfs;
    const char *band;
    const char *message;
    uint8_t     message_level;  /* ui_msg_level_t */
    uint8_t     show_meter;
    uint8_t     show_hints;
    uint8_t     has_fkey;
    /* zones */
    uint8_t     home_level;     /* ui_home_level_t */
    uint8_t     zone;           /* picker highlight */
    uint8_t     editing;
    uint8_t     slide_count;
    uint8_t     slide_first;
    uint8_t     slide_focus;    /* 255 when the row is not in use */
    uint8_t     slide_id[UI_SLIDE_VISIBLE];
    char        slide_value[UI_SLIDE_VISIBLE][12];
    uint8_t     slide_level[UI_SLIDE_VISIBLE]; /* 0..100, 255 = no bar */
    const char *edit_name;      /* full name of the setting being edited */
    ui_diag_cell_t diag[UI_DIAG_SUMMARY];
    /* Receive CW decoder: in CW with the decoder on it takes the whole
     * diagnostics zone -- a status line over two lines of text. */
    uint8_t     cw_decode;
    uint8_t     cw_state;       /* UI_CW_* */
    uint8_t     cw_wpm;         /* while locked */
    uint8_t     cw_alert;       /* diag cell carried into the status line: index + 1, 0 none */
    int16_t     cw_offset_hz;   /* tracked offset from the pitch, while locked */
    const char *cw_line[2];     /* UI_CwTeletype: older, newer */
    /* The TX panel over the diagnostics zone, and power on the meter row. */
    uint8_t     tx_panel;
    ui_tx_meter_t txm;
} ui_home_view_t;
enum { UI_CW_PAUSED = 0, UI_CW_LISTENING, UI_CW_LOCKED };

void UI_HomeView(ui_home_view_t *view, const modulator_status_t *st, uint32_t now);
/* 1 in CW with the decoder switched on: the pages then show its text, the
 * same whether or not the receiver runs this moment (a key-down stops it). */
uint8_t UI_CwDecodeShown(void);
/* The decoded text as a two-line teleprinter: the newer line fills left to
 * right and moves up whole when the next word does not fit it, so words are
 * never cut and nothing moves until a line is full. Takes what the decoder
 * has added since the last call. */
#define UI_CW_LINE_CHARS 38U  /* (GFX_W - 2 x GFX_MARGIN) / 8, the body font */
void UI_CwTeletype(const char **older, const char **newer);

/* ---- Rendering (ui_render*.c) ----------------------------------------- */
/* Screen grid: title band 0..15, content 16..223, footer 224..239. Rows of
 * lists are 16 px (two tiles); no text is drawn nearer than 8 px to the left
 * or right edge. */
#define UI_TITLE_H    16U
#define UI_FOOTER_Y   224U
#define UI_ROW_H      16U
#define UI_LIST_Y     16U

void UI_AppBuildFrame(void *ctx, uint32_t now);
void UI_RenderHome(const ui_home_view_t *v);
/* The home screen's RX/TX badge alone, drawn over the last frame. */
void UI_RenderStateCue(uint8_t txs);
/* The TX CARRIER TEST page's carrier badge and RF lamp alone, likewise. */
void UI_RenderTxTestCue(uint8_t on);
/* The live meter alone, drawn over the last frame (and by the full frame):
 * the home TX panel (diagnostics zone, y 136-199), the home meter row in TX
 * (y 64-79), and the TX TEST page's meter lines (y 160-199). Each clears its
 * own rectangle first and keeps clear of the zone picker's bracket. */
void UI_RenderTxPanel(const ui_tx_meter_t *m);
void UI_RenderTxMeterRow(const ui_tx_meter_t *m, uint8_t clip);
void UI_RenderTxTestMeter(const ui_tx_meter_t *m);
void UI_RenderMenu(uint32_t now);
void UI_RenderPage(uint32_t now);
void UI_RenderFault(const modulator_status_t *st);
/* Boot log drawn by LCDUI_BootBegin/BootStep. */
void UI_DrawBootHeader(void);
void UI_DrawBootRow(uint8_t row, const char *name, uint8_t status, const char *detail);
/* Status fetched once per frame by UI_AppBuildFrame. */
const modulator_status_t *UI_FrameStatus(void);

/* Shared widgets. */
void UI_DrawTitle(const char *title, const char *right);
/* Hints in the footer; a live flash message replaces `left`. */
void UI_DrawFooter(const char *left, const char *right, uint32_t now);
typedef enum { UI_ROW_NORMAL = 0, UI_ROW_FOCUS, UI_ROW_MARKED, UI_ROW_EDIT, UI_ROW_DIM } ui_row_state_t;
/* One 16 px list row spanning [x, x + w): label left, value right-aligned. */
void UI_DrawRow(uint16_t x, uint16_t w, uint16_t y, ui_row_state_t state,
                const char *label, const char *value);
void UI_DrawSection(uint16_t x, uint16_t w, uint16_t y, const char *title);
void UI_DrawScrollbar(uint16_t x, uint16_t y, uint16_t h, uint8_t total, uint8_t first, uint8_t visible);
/* Scroll window of `visible` rows kept around `cursor`; *first persists. */
uint8_t UI_ListFirst(uint8_t *first, uint8_t cursor, uint8_t total, uint8_t visible);
/* Simple list page: rows of labels with a cursor. */
void UI_DrawList(const char *const *labels, uint8_t total, uint8_t cursor, uint8_t visible);
/* Word-wrapped text; '|' forces a break. Returns the lines drawn. */
uint8_t UI_DrawWrapped(uint16_t x, uint16_t y, uint16_t w, uint8_t max_lines,
                       uint8_t color, const char *text);
/* "KEY  value" pair in the small font, value in its own colour. */
void UI_DrawPair(uint16_t x, uint16_t y, const char *key, const char *value, uint8_t color);
/* Horizontal bar: outline and fill in one colour, fill = part/whole. */
void UI_DrawBar(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t part, uint32_t whole, uint8_t color);

#ifdef __cplusplus
}
#endif

#endif
