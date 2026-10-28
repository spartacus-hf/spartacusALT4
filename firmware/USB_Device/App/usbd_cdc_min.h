#ifndef USBD_CDC_MIN_H
#define USBD_CDC_MIN_H

#include "usbd_ioreq.h"

/* Minimal CDC-ACM class for the REV1 diagnostic console.
 *
 * Written for this project rather than vendored: the only CDC source in the
 * tree belongs to a different ST library vintage, and project-owned code is
 * covered by strict_compile_check and the stack and complexity policies.
 *
 * Real-time contract. Both endpoint callbacks are O(one packet) memcpy and
 * nothing else: no parsing, no formatting, no allocation, no I/O. Everything
 * the console actually does happens in the foreground, in USB_DiagService.
 * Receive flow control is the USB one -- a packet that has not been consumed
 * yet simply is not re-armed, so the host NAKs instead of this code buffering.
 * That keeps the class at one packet of RAM in each direction. */

#define CDC_MIN_IN_EP      0x81U
#define CDC_MIN_OUT_EP     0x01U
#define CDC_MIN_CMD_EP     0x82U
#define CDC_MIN_PACKET     64U
#define CDC_MIN_CMD_PACKET 8U

/* The class handle comes from USBD_static_malloc, which hands out one fixed
 * buffer sized to the largest class this build registers. Declaring the size
 * here lets usbd_conf.c include it in that maximum; the .c static-asserts that
 * the real struct fits. */
#define CDC_MIN_HANDLE_SIZE 160U

extern const USBD_ClassTypeDef USBD_CDC_MIN;

/* 1 once the host has opened the port (DTR asserted). A console that writes
 * before this is talking to nobody, so the producer waits. */
uint8_t  USBD_CdcMinIsOpen(void);
/* 1 when the IN endpoint will accept another packet right now. */
uint8_t  USBD_CdcMinTxReady(void);
/* Queue up to CDC_MIN_PACKET bytes. Returns the number accepted (0 when the
 * endpoint is busy or the port is closed). */
uint16_t USBD_CdcMinTransmit(const uint8_t *data, uint16_t length);
/* SYSCLK count (TIM5) at which the packet now being read arrived. */
uint32_t USBD_CdcMinRxStamp(void);
/* Take whatever the host sent, up to `cap` bytes, and re-arm the endpoint.
 * Returns the number of bytes copied. */
uint16_t USBD_CdcMinReceive(uint8_t *out, uint16_t cap);

#endif
