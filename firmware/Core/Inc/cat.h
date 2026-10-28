#ifndef CAT_H
#define CAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* CAT and terminal on the audio personality's USB serial ports, as QMX does
 * it (usbd_cat.h for the USB side, cat_logic.h for the command set).
 *
 * Each port starts as a CAT port. A CR or LF turns it into the console's
 * (usb_diag.c) until the host closes it, which is how QMX enters its
 * terminal applications; one port at a time can be the console. Closing the
 * port that asked for transmit takes the radio out of it, and with CAT
 * timeout on (QB/QC, 120 s as QMX) so does a transmit no CAT command has
 * followed for that long.
 *
 * Real-time contract: CAT_Service runs in the main loop on every pass, the
 * realtime branch included (an RX; has to land while transmitting); it reads
 * at most a few packets and does no blocking I/O. Commands that would stall
 * the audio path (a mode change, a menu write) wait, or are refused, the way
 * the panel's are. */

void CAT_Service(uint32_t now);

/* The console's port when a CAT port is a terminal (usb_diag.c): open, may
 * send, send up to a packet, take what the host typed (the line that made it
 * a terminal first), and the arrival stamp of the packet being read. */
uint8_t  CAT_ConsoleOpen(void);
uint8_t  CAT_ConsoleTxReady(void);
uint16_t CAT_ConsoleTransmit(const uint8_t *data, uint16_t length);
uint16_t CAT_ConsoleReceive(uint8_t *out, uint16_t cap);
uint32_t CAT_ConsoleRxStamp(void);
/* Moves each time the console's port changes hands (a fresh session). */
uint8_t  CAT_ConsoleSession(void);

/* Ports open, CAT commands taken, and whether CAT transmit is on, for the
 * console's `cat` line and the DIAG page. */
void CAT_Status(uint8_t *open_mask, uint8_t *console_port, uint32_t *commands, uint8_t *ptt);

/* The S-CTRL LINK (sctrl_link.c). The port that asked for CAT transmit
 * (0xFF none); the time since port `port`'s last CAT command in 0.1 s
 * (0xFFFF never). */
uint8_t     CAT_PttPort(void);
uint16_t    CAT_PortAgeDs(uint8_t port, uint32_t now);
/* The LINK takes the console's port, and gives it back as a CAT port. */
void        CAT_LinkClaim(uint8_t port);
void        CAT_LinkRelease(uint8_t port);
/* The LINK's CAT tunnel: one command run as that port; its reply (possibly
 * empty) waits until taken. */
uint8_t     CAT_LinkRun(uint8_t port, const uint8_t *cmd, uint8_t len);
const char *CAT_LinkReply(uint8_t port, uint16_t *len);
void        CAT_LinkReplyDone(uint8_t port);

#ifdef __cplusplus
}
#endif

#endif
