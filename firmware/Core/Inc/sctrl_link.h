#ifndef SCTRL_LINK_H
#define SCTRL_LINK_H

/* The S-CTRL LINK on the radio (docs/SCTRL_LINK.md): once the console
 * command `link 1` has been answered on a CAT port, that port carries frames
 * only -- the screen as it changes, STATUS ten times a second, the waterfall
 * line by line, events, the CW decoder, console replies, CAT replies -- and
 * takes the PC's panel input, console lines, CAT commands and its keying,
 * under the transmit guard (sctrl_link_logic.h).
 *
 * Cost: one flag test per main-loop pass while no LINK is up. While one is,
 * the USB interrupt sends at most `packets per frame` 32-byte packets per
 * millisecond, armed at SOF (so the SOF the reference discipline stamps never
 * meets a burst), and the main loop codes at most one screen page a pass,
 * only when the panel itself may be redrawn. The LINK's RAM is its 256-byte
 * ring and its decoder. */

#include <stdint.h>

/* Console side (usb_diag.c). */
/* Take CAT port `port`, the console's, for the LINK; HELLO goes first. */
void     SCTRL_LinkStart(uint8_t port);
uint8_t  SCTRL_LinkActive(void);
/* HOME's PC badge: 0 no LINK, 1 a PC is linked, 2 it holds the transmitter. */
uint8_t  SCTRL_LinkBadge(void);
uint8_t  SCTRL_LinkConsoleReady(void);
uint16_t SCTRL_LinkConsoleWrite(const uint8_t *data, uint16_t length);

/* cat.c: what the host sent on the LINK's port, and the port closing
 * (DTR, USB reset or suspend). */
void     SCTRL_LinkRx(const uint8_t *data, uint16_t length);
void     SCTRL_LinkClosed(void);

/* Main loop, every pass, the realtime branch included. */
void     SCTRL_LinkService(uint32_t now);

/* ui_lcd.c: pages whose pixels or colours changed in this frame build. */
void     SCTRL_LinkScreenChanged(uint32_t pages);

/* usbd_cat.c, USB interrupt: a start of frame, and the LINK port's IN
 * packet sent. */
void     SCTRL_LinkUsbSof(void);
void     SCTRL_LinkUsbInDone(uint8_t port);

#endif
