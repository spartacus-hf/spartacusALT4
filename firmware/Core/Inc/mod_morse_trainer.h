#ifndef MOD_MORSE_TRAINER_H
#define MOD_MORSE_TRAINER_H

#ifdef __cplusplus
extern "C" {
#endif

/* The Morse trainer: Morserino-32's Koch method with a guided start for
 * someone new to an iambic paddle. It never transmits: while it runs the
 * keyer only drives the sidetone (MOD_CwSetTrainInhibit), and what it plays
 * goes through the sidetone player (mod_cw_player.h). Its material is the
 * CW pack on the SD card (cw_pack.h); without the card and the pack no
 * activity starts.
 *
 * Main loop only (MTRAIN_Service), except MTRAIN_KeyEvent, which the keyer
 * calls from the audio renderers. Hardware-free like the keyer core:
 * tools/tests/morse_trainer_test.c runs this code against the real pack. */

#include <stdint.h>
#include "mod_morse_tables.h"

#define MTRAIN_PROMPT_MAX   12U  /* symbols in a prompt */
#define MTRAIN_ANSWER_MAX   16U
#define MTRAIN_LINE_MAX     24U  /* free keying's decoded line */
#define MTRAIN_GROUP_MAX    8U
#define MTRAIN_COURSES      5U   /* M32 LCWO CWA LICW, and CUSTOM (CW/CUSTOM.TXT) */
#define MTRAIN_CAROUSEL_MAX 13U  /* LICW entry points, as Morserino-32 numbers them */
#define MTRAIN_RETRY_ALWAYS 7U   /* retries: 7 repeats a prompt until it is right */

/* Activities. */
typedef enum {
    MTRAIN_ECHO = 0,   /* hear a prompt, key it back */
    MTRAIN_LISTEN,     /* hear groups, see each one after it */
    MTRAIN_LEARN,      /* hear and see the newest character, then echo it */
    MTRAIN_FREE,       /* key anything; the trainer decodes it */
    MTRAIN_PADDLE,     /* find the DIT lever, swap, squeeze, release */
    MTRAIN_ACT_COUNT
} mtrain_act_t;

/* What a prompt is made of. */
typedef enum {
    MTRAIN_CHARS = 0,  /* a group of the lesson's characters */
    MTRAIN_WORDS,      /* an English word made of them */
    MTRAIN_ABBR,       /* a CW abbreviation made of them */
    MTRAIN_CALLS,      /* a call sign */
    MTRAIN_MIXED,
    MTRAIN_SRC_COUNT
} mtrain_src_t;

typedef enum { MTRAIN_COURSE_M32 = 0, MTRAIN_COURSE_LCWO, MTRAIN_COURSE_CWA, MTRAIN_COURSE_LICW,
               MTRAIN_COURSE_CUSTOM } mtrain_course_t;

/* How an echo prompt is given. */
typedef enum { MTRAIN_SHOW_SOUND = 0, MTRAIN_SHOW_TEXT, MTRAIN_SHOW_BOTH, MTRAIN_SHOW_COUNT } mtrain_show_t;

/* The preferences (saved with the settings). */
typedef struct {
    uint8_t act, src, course, carousel;
    uint8_t group;         /* characters in a group, 1..8 */
    uint8_t listen_wpm;    /* character speed of what you hear */
    uint8_t pace_wpm;      /* its overall speed (Farnsworth spacing), <= listen_wpm */
    uint8_t key_wpm;       /* your keyer while training */
    uint8_t manual;        /* spacing in dits (ics, iws) instead of Farnsworth */
    uint8_t ics, iws;      /* dits between characters (3..15) and words (7..30) */
    uint8_t retries;       /* 0..6 repeats of a wrong prompt, MTRAIN_RETRY_ALWAYS */
    uint8_t wait_ds;       /* answer wait, tenths of a second */
    uint8_t think_s;       /* time to start an answer, seconds */
    uint8_t show;          /* mtrain_show_t */
    uint8_t tone;          /* feedback tones */
    uint8_t boost;         /* weak characters come up more often (Morserino adaptive) */
    uint8_t adapt;         /* listen speed follows the answers, +-1 WPM */
    uint8_t minutes;       /* session length */
    uint8_t nosave;        /* SAVE PROGRESS off: the record on the card is left as it is */
} mtrain_prefs_t;

/* The learner's record (store slot, not the settings). Characters by
 * MORSE_Index. Version 1 (v8.10-v8.11) kept in `once` how many steps of the
 * guided session were done (MTRAIN_GUIDED_DONE: all), and had padding
 * where `stage` is; the store reads it as version 2 (mod_storage.c). */
#define MTRAIN_PROGRESS_VERSION 2U
typedef struct {
    uint8_t  version;
    uint8_t  once;                     /* course steps done for good: MTRAIN_ONCE_* */
    uint8_t  lesson[MTRAIN_COURSES];   /* characters taught, per course */
    uint8_t  newest_n;                 /* outcomes in newest_win */
    uint8_t  win_n;                    /* outcomes in win */
    uint8_t  tries[MORSE_SYMBOLS];     /* first attempts, halved with right[] at 255 */
    uint8_t  right[MORSE_SYMBOLS];     /* of them right */
    uint8_t  stage;                    /* the course's steps before this one are passed, once
                                        * the whole alphabet is in */
    uint16_t heard[MORSE_SYMBOLS];     /* times heard, saturating */
    uint32_t newest_win;               /* the newest character's last first attempts, bit 0 the latest */
    uint64_t win;                      /* this lesson's last 50 first attempts */
} mtrain_progress_t;
#define MTRAIN_ONCE_PADDLE  1U  /* the course's paddle check */
#define MTRAIN_ONCE_EXPLORE 2U  /* and its first try of the keyer */
#define MTRAIN_GUIDED_DONE  0xFFU  /* version 1's `once`: the guided session finished */

/* Controller states. */
typedef enum {
    MTRAIN_OFF = 0,
    MTRAIN_ERROR,      /* no card or no pack: why in view.error */
    MTRAIN_LOAD,       /* the next prompt from the pack */
    MTRAIN_PLAY,       /* the prompt sounds; the levers are held off */
    MTRAIN_THINK,      /* your turn: nothing keyed yet */
    MTRAIN_ANSWER,     /* keying an answer */
    MTRAIN_FEEDBACK,   /* the verdict */
    MTRAIN_SHOW,       /* listen: the group shown before the next */
    MTRAIN_KEYING,     /* free keying */
    MTRAIN_CHECK,      /* the paddle check */
    MTRAIN_PAUSE,
    MTRAIN_SUMMARY,    /* the session is over */
    MTRAIN_STEP_DONE   /* a step is done: MTRAIN_Next goes on, by itself after a moment */
} mtrain_state_t;

typedef enum {
    MTRAIN_ERR_NONE = 0,
    MTRAIN_ERR_CARD,     /* no SD card */
    MTRAIN_ERR_PACK,     /* CWPACK.BIN missing, another build's or damaged */
    MTRAIN_ERR_CUSTOM,   /* the CUSTOM course has no CW/CUSTOM.TXT */
} mtrain_error_t;

/* Verdicts. */
enum { MTRAIN_V_NONE = 0, MTRAIN_V_RIGHT, MTRAIN_V_WRONG, MTRAIN_V_TIMEOUT, MTRAIN_V_SKIPPED, MTRAIN_V_LOST };

/* Paddle check steps. */
enum { MTRAIN_CHECK_DIT = 0, MTRAIN_CHECK_SWAP, MTRAIN_CHECK_SQUEEZE, MTRAIN_CHECK_RELEASE, MTRAIN_CHECK_OK };

/* What the trainer page shows. Symbols as mod_morse_tables.h has them. */
typedef struct {
    uint8_t state, act, error, verdict;
    uint8_t guided;        /* the course (START LEARNING) runs: its step + 1; 0 not */
    uint8_t text;          /* the step's card text, TXT_TR_PADDLE_DIT + text - 1; 0 none */
    uint8_t reveal;        /* the prompt may be shown */
    uint8_t prompt[MTRAIN_PROMPT_MAX + 1U];
    uint8_t prompt_len;
    uint8_t answer[MTRAIN_ANSWER_MAX + 1U];  /* '*' an unknown symbol */
    uint8_t answer_len;
    uint16_t wrong;        /* bit i: prompt symbol i was missed */
    uint8_t expected, heard;  /* the first difference: 0 for none (missing or extra) */
    uint8_t joined;        /* right once letters split by too long a gap were joined */
    uint8_t attempt;       /* 0 the first try */
    uint8_t line[MTRAIN_LINE_MAX + 1U];      /* free keying, newest at the end */
    uint8_t line_len;
    uint8_t check;         /* paddle check step */
    uint8_t levers;        /* bit 0 DIT, bit 1 DAH, as the keyer reads them */
    uint8_t lesson, lesson_max;
    uint8_t newest;        /* the lesson's newest character */
    uint8_t weakest;       /* the lesson's weakest character, 0 none yet */
    uint8_t recommend;     /* ready for the next character: add it with MTRAIN_AddCharacter */
    uint8_t window;        /* of the lesson's last first tries (progress win_n), the right ones */
    uint8_t listen_wpm, pace_wpm;
    uint16_t done, right;  /* this session: first attempts and right ones */
    uint16_t timeouts, lost;
    uint32_t left_ms;      /* of the session */
} mtrain_view_t;

/* Element events from the keyer, timestamped in ms. */
enum { MTRAIN_EV_DIT = 0U, MTRAIN_EV_DAH, MTRAIN_EV_UP, MTRAIN_EV_DOWN /* straight key, its kind unknown */ };
void MTRAIN_KeyEvent(uint8_t ev, uint32_t ms);
/* The keyer should report its elements. */
uint8_t MTRAIN_Capturing(void);

void MTRAIN_Init(void);
/* Start the activity in prefs, or (guided) the course in the pack: from its
 * first step, where what the record has done skips itself. Returns 0 and
 * sets view.error when it cannot. */
uint8_t MTRAIN_Start(uint8_t guided, uint32_t now);
/* Stop: the player, the levers released, the record kept. */
void MTRAIN_Stop(void);
uint8_t MTRAIN_Active(void);
void MTRAIN_Service(uint32_t now);

/* Actions (the trainer page's panel). */
void MTRAIN_Submit(void);     /* judge the answer now */
void MTRAIN_Repeat(void);     /* play the prompt again (not a retry) */
void MTRAIN_Skip(void);       /* the next prompt */
void MTRAIN_Pause(uint8_t on);
void MTRAIN_Hold(uint8_t on); /* the panel is open: deadlines wait */
void MTRAIN_Next(void);       /* after a step or the summary */
void MTRAIN_Swap(void);       /* the paddle check: swap DIT and DAH */
uint8_t MTRAIN_AddCharacter(void);
void MTRAIN_SetLesson(uint8_t lesson);

const mtrain_view_t *MTRAIN_View(void);
mtrain_prefs_t *MTRAIN_Prefs(void);       /* the UI edits, MTRAIN_PrefsChanged applies */
void MTRAIN_PrefsChanged(void);
mtrain_progress_t *MTRAIN_Progress(void); /* the store saves it */
/* A record read from the card is one this image takes (version 1 too). */
uint8_t MTRAIN_ProgressValid(const mtrain_progress_t *p);
/* It becomes the learner's record, as version 2. */
void MTRAIN_ProgressSet(const mtrain_progress_t *p);
/* The record changed since it was last saved (MTRAIN_ProgressSaved clears). */
uint8_t MTRAIN_ProgressDirty(void);
void MTRAIN_ProgressSaved(void);
void MTRAIN_DefaultPrefs(mtrain_prefs_t *p);
void MTRAIN_ResetProgress(void);
uint8_t MTRAIN_LessonMax(void);
/* The course of prefs from the card, for the page before a session (view
 * lesson, newest, lesson_max); 1 once loaded. Main loop, blocking I/O. */
uint8_t MTRAIN_LoadCourse(void);
/* Its characters in teaching order, NULL until loaded. */
const uint8_t *MTRAIN_Course(void);

/* Silence after a key-up that ends a character and a word, in dits:
 * standard spacing is 1 inside a character, 3 between characters and 7
 * between words; 2.5 forgives reaching for the other lever a little late,
 * 5 splits 3 from 7. */
/* The character and word gaps' floors; the trainer raises both as it learns
 * how the learner spaces (mod_morse_trainer.c gap_ms). */
#define MTRAIN_CHAR_GAP_TENTHS 25U
#define MTRAIN_WORD_GAP_TENTHS 50U

#ifdef __cplusplus
}
#endif

#endif /* MOD_MORSE_TRAINER_H */
