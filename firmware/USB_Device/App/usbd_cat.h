#ifndef USBD_CAT_H
#define USBD_CAT_H

#include "usbd_ioreq.h"

/* The audio personality as a composite device, the way QMX presents itself
 * (operating manual 1_02_000, "USB serial ports"): the USB sound card plus
 * virtual COM serial ports on the same cable.
 *
 *   interfaces 0-2  USB audio, usbd_audio.c unchanged (control, playback,
 *                   capture), so the sound card keeps its name on the host
 *   interfaces 3-4  CDC-ACM port 0 (Linux: ...-if03)
 *   interfaces 5-6  CDC-ACM port 1 (Linux: ...-if05)
 *
 * Both ports are equivalent, as QMX's are: each takes Kenwood TS-480 CAT
 * commands and turns into the console terminal on a line ending (cat.c), so
 * WSJT-X can hold one port for CAT while a terminal or a script uses the
 * other, with the sound card running throughout.
 *
 * Endpoints: the audio class owns 0x01, 0x82 and 0x83. Port p has its
 * notification IN at 0x84 + 2p (never sent: CAT has no line events), data OUT
 * at 0x05 + 2p and data IN at 0x85 + 2p -- endpoint numbers 4..7, all the FS
 * peripheral has left. Packet memory is as tight (usbd_conf.c): 8-byte OUT
 * packets (CAT commands and console lines are short) and 32-byte IN packets.
 *
 * Real-time contract, as usbd_cdc_min.h: the endpoint callbacks record one
 * packet and return; parsing and replies happen in the foreground (CAT_Service,
 * USB_DiagService). An OUT packet is not re-armed until the foreground has
 * read it, so the host NAKs instead of this code buffering. */

#define USBD_CAT_PORTS          2U
#define USBD_CAT_FIRST_IF       3U
#define USBD_CAT_OUT_PACKET     8U
#define USBD_CAT_IN_PACKET      32U
#define USBD_CAT_NOTIFY_PACKET  8U
#define USBD_CAT_NOTIFY_EP(p)   ((uint8_t)(0x84U + 2U * (uint32_t)(p)))
#define USBD_CAT_OUT_EP(p)      ((uint8_t)(0x05U + 2U * (uint32_t)(p)))
#define USBD_CAT_IN_EP(p)       ((uint8_t)(0x85U + 2U * (uint32_t)(p)))

/* USB audio plus the CAT ports; registered in place of USBD_AUDIO. */
extern const USBD_ClassTypeDef USBD_AUDIO_CAT;

/* 1 once the host has opened port `port` (DTR asserted). */
uint8_t  USBD_CatIsOpen(uint8_t port);
/* 1 when the port's IN endpoint takes another packet now. */
uint8_t  USBD_CatTxReady(uint8_t port);
/* Queue up to one packet of `data`; returns the bytes taken (0 when busy or
 * closed). Never ends on a full packet (see the .c), so the caller sends the
 * rest on a later call. */
uint16_t USBD_CatTransmit(uint8_t port, const uint8_t *data, uint16_t length);
/* Take the packet the host sent, up to `cap` bytes, and re-arm the endpoint.
 * Returns the number of bytes copied. */
uint16_t USBD_CatReceive(uint8_t port, uint8_t *out, uint16_t cap);
/* SYSCLK count (TIM5) at which the packet now waiting arrived. */
uint32_t USBD_CatRxStamp(uint8_t port);
/* Interrupt context only (the S-CTRL LINK): load one IN packet of at most
 * USBD_CAT_IN_PACKET bytes; 1 when it went into packet memory. */
uint8_t  USBD_CatInSend(uint8_t port, const uint8_t *data, uint16_t length);

#endif
