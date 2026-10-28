#ifndef USB_DIAG_H
#define USB_DIAG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* REV1 USB diagnostic console.
 *
 * One CDC-ACM port carrying two interfaces to the same state:
 *
 *  - a line protocol meant for scripts and host tools: one request line in, data
 *    lines out, terminated by "OK" or "ERR <reason>". Every command is
 *    discoverable through `help`, values are integers in stated units, and
 *    nothing is redrawn or animated.
 *  - a QMX-style full-screen view for a person with a terminal, entered with
 *    `ui` and left with q or Ctrl-C.
 *
 * A person reaches it with any terminal program (PuTTY, minicom, picocom,
 * screen, `tools/rev1_diag.py term`). Like QMX, the port speaks the line
 * protocol until the host sends a CR: every terminal program sends one for
 * Enter and scripts end their lines with LF, so from that Enter on the console
 * echoes, edits the line (Backspace, Ctrl-U, Ctrl-C) and prompts. A terminal
 * that opens the port and sends nothing for 300 ms is told what it is
 * connected to, in one line starting with "# ".
 *
 * Real-time contract. USB_DiagService runs in the main loop only and does a
 * bounded amount of work per pass, and that bound is zero while
 * MOD_AudioRealtimeCritical() is true. Endpoint interrupts only move one
 * packet (see usbd_cdc_min.c), so with the console attached the radio does the
 * same work it does without it. */

void USB_DiagReset(void);
/* Main loop only. Parses at most one command and emits a bounded number of
 * packets per call. */
void USB_DiagService(uint32_t now);
/* Main loop, realtime branch only: while the virtual paddle page is open,
 * take the console's input (levers only, no output) so a key release is seen
 * while RF is keyed and USB_DiagService is standing aside. */
void USB_DiagPaddlePoll(uint32_t now);
/* The console is going away: drop the virtual paddle's levers and hand TX
 * mute back before the personality switches. */
void USB_DiagRelease(void);
uint8_t USB_DiagIsOpen(void);
/* Bytes sent and received since the console was opened, for the monitor page. */
uint32_t USB_DiagBytesOut(void);
uint32_t USB_DiagBytesIn(void);
/* The S-CTRL LINK's console lines (sctrl_link.c): may a line start now; one
 * byte of it; its end, committed (ok) or dropped. */
uint8_t USB_DiagLinkIdle(void);
void    USB_DiagLinkByte(uint8_t ch);
void    USB_DiagLinkEnd(uint8_t ok);

#ifdef __cplusplus
}
#endif

#endif
