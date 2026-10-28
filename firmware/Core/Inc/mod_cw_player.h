#ifndef MOD_CW_PLAYER_H
#define MOD_CW_PLAYER_H

/* The local Morse player: prompts, feedback beeps and the SPOT tone of the
 * trainer, played on the sidetone. It has no RF authority at all -- it is
 * stepped only from the headphone renderer (MOD_CwKeyerMonitorStep), never
 * from render_cw -- and reads nothing from the card: the main loop hands it
 * a whole program (CWP_Start), cached before it sounds.
 *
 * Hardware-free like the keyer core: tools/tests/cw_player_test.c runs
 * this code. */

#include <stdint.h>

#define CWP_TEXT_MAX 40U

typedef struct {
    uint8_t  text[CWP_TEXT_MAX];  /* codes (mod_morse_tables.h); 0 a word space */
    uint8_t  len;
    uint8_t  level;               /* 255 = the sidetone's level */
    uint32_t dit, dah;            /* mark lengths, samples */
    uint32_t elem_gap;            /* between elements of a symbol */
    uint32_t char_gap;            /* between symbols */
    uint32_t word_gap;            /* for a ' ' */
    uint32_t lead;                /* silence before the first mark */
    uint32_t ramp;                /* key-edge ramp, samples */
} cwp_prog_t;

/* Program timing at `char_wpm`, with the spacing of `eff_wpm` (Farnsworth,
 * ARRL formula; eff >= char is plain timing), or with `ics`/`iws` dits
 * between characters/words when nonzero (manual spacing). */
void CWP_Timing(cwp_prog_t *p, float char_wpm, float eff_wpm, uint8_t ics, uint8_t iws, uint32_t ramp);
/* Text into a program: symbols as given, unknown ones dropped. Returns the
 * symbols taken. */
uint8_t CWP_SetText(cwp_prog_t *p, const uint8_t *text, uint8_t len);

/* The ramp table: CW_RAMP_LUT_SIZE + 1 entries, 0..65535 (cw_ramp_lut). */
void CWP_Init(const uint16_t *ramp_lut, uint16_t lut_size);
/* Main loop: start a program (0 while one plays), stop one (it ramps down). */
uint8_t CWP_Start(const cwp_prog_t *p);
void CWP_Stop(void);
uint8_t CWP_Busy(void);
/* Symbols played to the end so far, of the program running or last run. */
uint8_t CWP_Done(void);
/* Total samples the program takes (for the trainer's timers). */
uint32_t CWP_Duration(const cwp_prog_t *p);

/* Headphone renderer: one 48 kHz sample, the envelope 0..1. */
float CWP_Step(void);
uint8_t CWP_Running(void);

#endif /* MOD_CW_PLAYER_H */
