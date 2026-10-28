#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "usbd_cat.h"
#include "usbd_audio.h"
#include "usbd_ctlreq.h"
#include "sctrl_link.h"
#include <string.h>

/* CDC class-specific requests (USB CDC 1.2, PSTN subclass 1.2 table 13). */
#define CDC_SET_LINE_CODING        0x20U
#define CDC_GET_LINE_CODING        0x21U
#define CDC_SET_CONTROL_LINE_STATE 0x22U

typedef struct {
    uint8_t           rx[USBD_CAT_OUT_PACKET];
    uint8_t           line_coding[7]; /* dwDTERate, bCharFormat, bParityType, bDataBits */
    volatile uint8_t  rx_length;      /* bytes waiting in rx; the OUT EP stays un-armed */
    volatile uint8_t  tx_busy;
    volatile uint8_t  open;           /* DTR from SET_CONTROL_LINE_STATE */
    volatile uint32_t rx_stamp;       /* TIM5 (SYSCLK) count when rx arrived */
} cat_port_t;

/* Static, not USBD_malloc: that hands out one buffer, the audio class's. */
static cat_port_t ports[USBD_CAT_PORTS];
/* The port whose SET_LINE_CODING data stage EP0 is receiving, 0xFF when the
 * pending EP0 OUT data (if any) is the audio class's. */
static volatile uint8_t ep0_port = 0xFFU;
static volatile uint8_t configured;

#define LE16(v) (uint8_t)((v) & 0xFFU), (uint8_t)(((v) >> 8) & 0xFFU)
#define CAT_IF(p)   ((uint8_t)(USBD_CAT_FIRST_IF + 2U * (p)))

/* One port: interface association (so Windows binds the pair as one COM
 * port), communication interface with its notification endpoint, data
 * interface with the bulk pair. 66 bytes. */
#define CAT_PORT_DESC(p) \
    0x08, 0x0B, CAT_IF(p), 0x02, 0x02, 0x02, 0x01, 0x00, \
    0x09, USB_DESC_TYPE_INTERFACE, CAT_IF(p), 0x00, 0x01, 0x02, 0x02, 0x01, 0x00, \
    0x05, 0x24, 0x00, LE16(0x0110), \
    0x05, 0x24, 0x01, 0x00, (uint8_t)(CAT_IF(p) + 1U), \
    0x04, 0x24, 0x02, 0x02, \
    0x05, 0x24, 0x06, CAT_IF(p), (uint8_t)(CAT_IF(p) + 1U), \
    0x07, USB_DESC_TYPE_ENDPOINT, USBD_CAT_NOTIFY_EP(p), 0x03, LE16(USBD_CAT_NOTIFY_PACKET), 0xFF, \
    0x09, USB_DESC_TYPE_INTERFACE, (uint8_t)(CAT_IF(p) + 1U), 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00, \
    0x07, USB_DESC_TYPE_ENDPOINT, USBD_CAT_OUT_EP(p), 0x02, LE16(USBD_CAT_OUT_PACKET), 0x00, \
    0x07, USB_DESC_TYPE_ENDPOINT, USBD_CAT_IN_EP(p), 0x02, LE16(USBD_CAT_IN_PACKET), 0x00
#define CAT_PORT_DESC_SIZE 66U

/* The configuration: the audio class's own interfaces (USBD_AUDIO_FUNCTION_DESC,
 * the same bytes its bare descriptor has) behind an association that keeps
 * control and streaming together for a composite host driver (Linux binds
 * UAC1 by the control interface's header and ignores it), then the ports.
 * Constant, in flash: the core only reads it. */
#define COMPOSITE_CFG_SIZE (USB_AUDIO_CONFIG_DESC_SIZ + 8U + USBD_CAT_PORTS * CAT_PORT_DESC_SIZE)
static const uint8_t composite_cfg[] = {
    0x09, USB_DESC_TYPE_CONFIGURATION, LE16(COMPOSITE_CFG_SIZE),
    (uint8_t)(3U + 2U * USBD_CAT_PORTS), 0x01, 0x00,
    (uint8_t)(0x80U | (USBD_SELF_POWERED ? 0x40U : 0x00U)), USBD_MAX_POWER,
    0x08, 0x0B, 0x00, 0x03, 0x01, 0x01, 0x00, 0x00,
    USBD_AUDIO_FUNCTION_DESC,
    CAT_PORT_DESC(0U), CAT_PORT_DESC(1U)
};
_Static_assert(sizeof(composite_cfg) == COMPOSITE_CFG_SIZE, "composite configuration descriptor size");

static uint8_t *cat_class_config(uint16_t *length)
{
    *length = (uint16_t)sizeof(composite_cfg);
    return (uint8_t *)composite_cfg;
}

static int8_t port_of_interface(uint8_t itf)
{
    if ((itf < USBD_CAT_FIRST_IF) || (itf >= USBD_CAT_FIRST_IF + 2U * USBD_CAT_PORTS)) return -1;
    return (int8_t)((itf - USBD_CAT_FIRST_IF) / 2U);
}

static int8_t port_of_endpoint(uint8_t ep)
{
    for (uint8_t p = 0U; p < USBD_CAT_PORTS; ++p) {
        if ((ep == USBD_CAT_NOTIFY_EP(p)) || (ep == USBD_CAT_OUT_EP(p)) || (ep == USBD_CAT_IN_EP(p))) return (int8_t)p;
    }
    return -1;
}

static uint8_t cat_class_init(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
    const uint8_t ret = USBD_AUDIO_Init(pdev, cfgidx);
    memset(ports, 0, sizeof(ports));
    ep0_port = 0xFFU;
    for (uint8_t p = 0U; p < USBD_CAT_PORTS; ++p) {
        cat_port_t *c = &ports[p];
        /* 115200 8N1: a value the host can read back; the rate is ignored. */
        c->line_coding[0] = 0x00U; c->line_coding[1] = 0xC2U; c->line_coding[2] = 0x01U;
        c->line_coding[6] = 8U;
        (void)USBD_LL_OpenEP(pdev, USBD_CAT_NOTIFY_EP(p), USBD_EP_TYPE_INTR, USBD_CAT_NOTIFY_PACKET);
        (void)USBD_LL_OpenEP(pdev, USBD_CAT_OUT_EP(p), USBD_EP_TYPE_BULK, USBD_CAT_OUT_PACKET);
        (void)USBD_LL_OpenEP(pdev, USBD_CAT_IN_EP(p), USBD_EP_TYPE_BULK, USBD_CAT_IN_PACKET);
        pdev->ep_in[USBD_CAT_NOTIFY_EP(p) & 0xFU].is_used = 1U;
        pdev->ep_out[USBD_CAT_OUT_EP(p) & 0xFU].is_used = 1U;
        pdev->ep_in[USBD_CAT_IN_EP(p) & 0xFU].is_used = 1U;
        (void)USBD_LL_PrepareReceive(pdev, USBD_CAT_OUT_EP(p), c->rx, USBD_CAT_OUT_PACKET);
    }
    configured = 1U;
    return ret;
}

static uint8_t cat_class_deinit(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
    configured = 0U;
    for (uint8_t p = 0U; p < USBD_CAT_PORTS; ++p) {
        (void)USBD_LL_CloseEP(pdev, USBD_CAT_NOTIFY_EP(p));
        (void)USBD_LL_CloseEP(pdev, USBD_CAT_OUT_EP(p));
        (void)USBD_LL_CloseEP(pdev, USBD_CAT_IN_EP(p));
        pdev->ep_in[USBD_CAT_NOTIFY_EP(p) & 0xFU].is_used = 0U;
        pdev->ep_out[USBD_CAT_OUT_EP(p) & 0xFU].is_used = 0U;
        pdev->ep_in[USBD_CAT_IN_EP(p) & 0xFU].is_used = 0U;
        ports[p].open = 0U;
        ports[p].tx_busy = 0U;
        ports[p].rx_length = 0U;
    }
    ep0_port = 0xFFU;
    return USBD_AUDIO_DeInit(pdev, cfgidx);
}

static uint8_t cat_setup(USBD_HandleTypeDef *pdev, uint8_t p, USBD_SetupReqTypedef *req)
{
    static __ALIGN_BEGIN uint8_t zero[2] __ALIGN_END = {0U, 0U};
    cat_port_t *c = &ports[p];
    switch (req->bmRequest & USB_REQ_TYPE_MASK) {
        case USB_REQ_TYPE_CLASS:
            if (req->bRequest == CDC_SET_CONTROL_LINE_STATE) {
                /* DTR: the host program has the port open. A packet it sent
                 * that nobody took yet goes with it: the next program's
                 * first line must not start with it. */
                const uint8_t open = (uint8_t)(req->wValue & 1U);
                if (!open && c->rx_length) {
                    c->rx_length = 0U;
                    (void)USBD_LL_PrepareReceive(pdev, USBD_CAT_OUT_EP(p), c->rx, USBD_CAT_OUT_PACKET);
                }
                c->open = open;
                return (uint8_t)USBD_OK;
            }
            if (!req->wLength) return (uint8_t)USBD_OK; /* SEND_BREAK and the like */
            if (req->bmRequest & 0x80U) {
                if (req->bRequest != CDC_GET_LINE_CODING) break;
                (void)USBD_CtlSendData(pdev, c->line_coding, (uint16_t)(req->wLength < 7U ? req->wLength : 7U));
                return (uint8_t)USBD_OK;
            }
            if (req->bRequest != CDC_SET_LINE_CODING) break;
            ep0_port = p;
            (void)USBD_CtlPrepareRx(pdev, c->line_coding, (uint16_t)(req->wLength < 7U ? req->wLength : 7U));
            return (uint8_t)USBD_OK;
        case USB_REQ_TYPE_STANDARD:
            switch (req->bRequest) {
                case USB_REQ_GET_STATUS:
                    if (pdev->dev_state != USBD_STATE_CONFIGURED) break;
                    (void)USBD_CtlSendData(pdev, zero, 2U);
                    return (uint8_t)USBD_OK;
                case USB_REQ_GET_INTERFACE:
                    if (pdev->dev_state != USBD_STATE_CONFIGURED) break;
                    (void)USBD_CtlSendData(pdev, zero, 1U);
                    return (uint8_t)USBD_OK;
                case USB_REQ_SET_INTERFACE:
                    if ((pdev->dev_state == USBD_STATE_CONFIGURED) && (req->wValue == 0U)) return (uint8_t)USBD_OK;
                    break;
                case USB_REQ_CLEAR_FEATURE:
                    return (uint8_t)USBD_OK;
                default:
                    break;
            }
            break;
        default:
            break;
    }
    USBD_CtlError(pdev, req);
    return (uint8_t)USBD_FAIL;
}

/* Requests for the ports' interfaces and endpoints go to them, everything
 * else to the audio class exactly as before. */
static uint8_t cat_class_setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
    int8_t port = -1;
    switch (req->bmRequest & USB_REQ_RECIPIENT_MASK) {
        case USB_REQ_RECIPIENT_INTERFACE: port = port_of_interface(LOBYTE(req->wIndex)); break;
        case USB_REQ_RECIPIENT_ENDPOINT:  port = port_of_endpoint(LOBYTE(req->wIndex)); break;
        default: break;
    }
    if (port < 0) return USBD_AUDIO_Setup(pdev, req);
    return cat_setup(pdev, (uint8_t)port, req);
}

static uint8_t cat_class_ep0_tx_sent(USBD_HandleTypeDef *pdev)
{
    return USBD_AUDIO_EP0_TxReady(pdev);
}

static uint8_t cat_class_ep0_rx_ready(USBD_HandleTypeDef *pdev)
{
    if (ep0_port != 0xFFU) {
        ep0_port = 0xFFU; /* the line coding already landed in place */
        return (uint8_t)USBD_OK;
    }
    return USBD_AUDIO_EP0_RxReady(pdev);
}

/* Endpoint callbacks. These run in USB_LP_IRQ; they must stay this short. */
static uint8_t cat_class_data_in(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    for (uint8_t p = 0U; p < USBD_CAT_PORTS; ++p) {
        if (epnum == (USBD_CAT_IN_EP(p) & 0x7FU)) {
            ports[p].tx_busy = 0U;
            SCTRL_LinkUsbInDone(p); /* the LINK's next packet, if its port */
            return (uint8_t)USBD_OK;
        }
        if (epnum == (USBD_CAT_NOTIFY_EP(p) & 0x7FU)) return (uint8_t)USBD_OK;
    }
    return USBD_AUDIO_DataIn(pdev, epnum);
}

static uint8_t cat_class_data_out(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    for (uint8_t p = 0U; p < USBD_CAT_PORTS; ++p) {
        if (epnum != USBD_CAT_OUT_EP(p)) continue;
        cat_port_t *c = &ports[p];
        const uint32_t n = USBD_LL_GetRxDataSize(pdev, epnum);
        if (n == 0U) {
            /* A zero-length packet carries nothing for the foreground to
             * take, so nothing would ever re-arm the endpoint: do it here. */
            (void)USBD_LL_PrepareReceive(pdev, epnum, c->rx, USBD_CAT_OUT_PACKET);
            return (uint8_t)USBD_OK;
        }
        c->rx_stamp = TIM5->CNT;
        c->rx_length = (uint8_t)(n > USBD_CAT_OUT_PACKET ? USBD_CAT_OUT_PACKET : n);
        return (uint8_t)USBD_OK; /* not re-armed until the foreground drains it */
    }
    return USBD_AUDIO_DataOut(pdev, epnum);
}

static uint8_t cat_class_sof(USBD_HandleTypeDef *pdev)
{
    const uint8_t ret = USBD_AUDIO_SOF(pdev);
    SCTRL_LinkUsbSof(); /* after the audio class: its SOF work is the timed one */
    return ret;
}

static uint8_t cat_class_iso_in_incomplete(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    return USBD_AUDIO_IsoINIncomplete(pdev, epnum);
}

static uint8_t cat_class_iso_out_incomplete(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    return USBD_AUDIO_IsoOutIncomplete(pdev, epnum);
}

static uint8_t *cat_class_qualifier(uint16_t *length)
{
    return USBD_AUDIO_GetDeviceQualifierDesc(length);
}

const USBD_ClassTypeDef USBD_AUDIO_CAT = {
    cat_class_init, cat_class_deinit, cat_class_setup,
    cat_class_ep0_tx_sent, cat_class_ep0_rx_ready,
    cat_class_data_in, cat_class_data_out,
    cat_class_sof, cat_class_iso_in_incomplete, cat_class_iso_out_incomplete,
    cat_class_config, cat_class_config, cat_class_config, cat_class_qualifier,
};

/* ---- foreground side ---- */

extern USBD_HandleTypeDef hUsbDeviceFS;

static uint8_t live(void)
{
    return (uint8_t)(configured && (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED) &&
                     (hUsbDeviceFS.pClass == &USBD_AUDIO_CAT));
}

uint8_t USBD_CatIsOpen(uint8_t port)
{
    return (uint8_t)((port < USBD_CAT_PORTS) && live() && ports[port].open);
}

uint8_t USBD_CatTxReady(uint8_t port)
{
    return (uint8_t)(USBD_CatIsOpen(port) && !ports[port].tx_busy);
}

uint16_t USBD_CatTransmit(uint8_t port, const uint8_t *data, uint16_t length)
{
    if (!USBD_CatTxReady(port) || !data || !length) return 0U;
    cat_port_t *c = &ports[port];
    /* Never end on a full packet. Linux cdc-acm reads in URBs of two packets
     * and completes one only when it is full or a short packet arrives, so a
     * reply that exactly filled a packet waited in the host for whatever came
     * next. With exactly one packet left the caller gets one byte less now
     * and sends the last byte next time: the last packet is always short. */
    uint16_t n = length;
    if (n > USBD_CAT_IN_PACKET) n = USBD_CAT_IN_PACKET;
    else if (n == USBD_CAT_IN_PACKET) n = USBD_CAT_IN_PACKET - 1U;
    /* No copy: a single-buffered IN endpoint takes the packet into packet
     * memory inside the transmit call (USB_EPStartXfer), so `data` may change
     * as soon as this returns. */
    c->tx_busy = 1U;
    if (USBD_LL_Transmit(&hUsbDeviceFS, USBD_CAT_IN_EP(port), (uint8_t *)data, n) != USBD_OK) {
        c->tx_busy = 0U;
        return 0U;
    }
    return n;
}

uint16_t USBD_CatReceive(uint8_t port, uint8_t *out, uint16_t cap)
{
    if ((port >= USBD_CAT_PORTS) || !live() || !out || !cap) return 0U;
    cat_port_t *c = &ports[port];
    uint16_t n = c->rx_length;
    if (!n) return 0U;
    if (n > cap) n = cap; /* the rest of the packet is dropped, never silently split */
    memcpy(out, c->rx, n);
    c->rx_length = 0U;
    (void)USBD_LL_PrepareReceive(&hUsbDeviceFS, USBD_CAT_OUT_EP(port), c->rx, USBD_CAT_OUT_PACKET);
    return n;
}

uint32_t USBD_CatRxStamp(uint8_t port)
{
    return (port < USBD_CAT_PORTS) ? ports[port].rx_stamp : 0U;
}

/* The S-CTRL LINK's packets, from the USB interrupt only (sctrl_link.c): the
 * same endpoint USBD_CatTransmit drives, never both -- a LINK port sends
 * nothing from the foreground. */
uint8_t USBD_CatInSend(uint8_t port, const uint8_t *data, uint16_t length)
{
    if ((port >= USBD_CAT_PORTS) || !live() || !ports[port].open || ports[port].tx_busy || !length) return 0U;
    ports[port].tx_busy = 1U;
    if (USBD_LL_Transmit(&hUsbDeviceFS, USBD_CAT_IN_EP(port), (uint8_t *)data, length) != USBD_OK) {
        ports[port].tx_busy = 0U;
        return 0U;
    }
    return 1U;
}
