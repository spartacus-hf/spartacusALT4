#ifndef UI_APP_H
#define UI_APP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void UI_AppInit(void);
void UI_AppButtonsPoll(uint32_t now);
void UI_AppConsumeEvents(void);
/* For a boot hold before the UI runs: 1 a click (knob or SELECT), 2 a hold
 * of either, 0 nothing. Every other panel event is dropped. */
uint8_t UI_AppBootChoice(uint32_t now);
void UI_AppHandle(void);
void UI_AppServiceDisplay(uint32_t now);
/* HELP.BIN on the SD card: 0 missing, 1 another build's, 2 this image's;
 * and the id this image wants in bytes 4..7 of the file (ui_help.c). */
uint8_t UI_HelpCardState(void);
/* A setting's help has waited out the scroll and is due to be read: the
 * next frame shows it. */
uint8_t UI_HelpWantsFrame(uint32_t now);
uint32_t UI_HelpCardId(void);
/* In place of UI_AppServiceDisplay while the RF path is realtime-critical:
 * the home screen's RX/TX badge follows the transmitter, nothing else. */
void UI_AppServiceTxCue(uint32_t now);
/* The last tune by the knob, for the console's `knob`: the carrier apply's
 * duration (and the longest), and the ticks of the detent and the apply. */
extern uint32_t ui_tune_apply_us, ui_tune_apply_max_us, ui_tune_turn_tick, ui_tune_apply_tick;

#ifdef __cplusplus
}
#endif

#endif
