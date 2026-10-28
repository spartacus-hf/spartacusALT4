#ifndef UI_TRAINER_H
#define UI_TRAINER_H

/* The Morse trainer's page (ui_trainer.c): a session -- echo, listen, learn,
 * free keying, the paddle check or the guided first session -- with its
 * action panel and summary. What to do is chosen in the CW menu's TRAINER
 * list. While the page shows, the radio is in CW, the keyer drives the
 * sidetone only and the receiver is quiet in the headphones; leaving gives
 * all three back. */

#include <stdint.h>

#define UI_TRAINER_GUIDED 0xFFU

/* Take the radio and start an activity (mtrain_act_t) or the guided
 * session. 0: refused, and UI_NavFlash says why (transmitting,
 * calibrating, USB disk, the SD player, a USB TX stream). */
uint8_t UI_TrainerBegin(uint8_t act, uint32_t now);
/* The radio back as it was: the session stopped, the record saved. */
void UI_TrainerLeave(void);
uint8_t UI_TrainerOwns(void);
/* The TRAINER list's ADD row: the next character into the lesson; what to
 * flash in the footer. */
const char *UI_TrainerAddCharacter(void);

/* The page's events: UI_TRAINER_EXIT once the session is over. */
enum { UI_TRAINER_STAY = 0U, UI_TRAINER_EXIT };
uint8_t UI_TrainerEvents(uint32_t now);
void UI_TrainerRender(uint32_t now);

#endif /* UI_TRAINER_H */
