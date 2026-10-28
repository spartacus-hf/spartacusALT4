#ifndef UI_WATERFALL_H
#define UI_WATERFALL_H

/* The WATERFALL page (ui_waterfall.c) over the engine in radio_spectrum.h.
 * Opened from the second cell of the home settings row only. The engine
 * runs while the page is open and not at all otherwise. */

#include <stdint.h>

void UI_WaterfallEnter(void);
void UI_WaterfallExit(void);
/* Main loop, page open: runs the engine; 1 when the frame should be rebuilt. */
uint8_t UI_WaterfallService(uint32_t now);
void UI_WaterfallRender(uint32_t now);

/* The settings list over the upper half of the page. */
uint8_t UI_WaterfallSetupOpen(void);
void UI_WaterfallSetupShow(void);
/* Knob and buttons while the list is open (ui_ev). */
void UI_WaterfallSetupEvents(void);
/* Hold-and-turn on the live view: the span, or in SCAN the scan range. */
void UI_WaterfallZoom(int8_t detents);
/* The home cell's value: what the page will show. */
void UI_WaterfallSummary(char *dst);

#endif
