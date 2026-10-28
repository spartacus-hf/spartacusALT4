#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
#include "usbd_cdc_min.h"
#include "usbd_ctlreq.h"
#include <string.h>

/* CDC class-specific requests (USB CDC 1.2, PSTN subclass 1.2 table 13). */
#define CDC_SET_LINE_CODING        0x20U
#define CDC_GET_LINE_CODING        0x21U
#define CDC_SET_CONTROL_LINE_STATE 0x22U
#define CDC_SEND_BREAK             0x23U

typedef struct {
    uint8_t  rx[CDC_MIN_PACKET];
    uint8_t  tx[CDC_MIN_PACKET];
    uint8_t  line_coding[7];   /* dwDTERate, bCharFormat, bParityType, bDataBits */
    uint8_t  command;          /* pending class request opcode, 0xFF when none */
    uint8_t  command_length;
    uint16_t rx_length;        /* bytes waiting in rx; the OUT EP stays un-armed */
    uint8_t  tx_busy;
    uint8_t  open;             /* DTR from SET_CONTROL_LINE_STATE */
    uint32_t rx_stamp;         /* TIM5 (SYSCLK) count when rx arrived */
} cdc_min_handle_t;

_Static_assert(sizeof(cdc_min_handle_t) <= CDC_MIN_HANDLE_SIZE,
               "USBD_static_malloc reserves CDC_MIN_HANDLE_SIZE for this handle");

static cdc_min_handle_t *handle;

/* A tiny helper so the descriptor arrays below read as the spec tables do. */
#define LE16(v) (uint8_t)((v) & 0xFFU), (uint8_t)(((v) >> 8) & 0xFFU)

__ALIGN_BEGIN static const uint8_t cdc_min_config_desc[] __ALIGN_END = {
    /* Configuration */
    0x09, USB_DESC_TYPE_CONFIGURATION, LE16(75), 0x02, 0x01, 0x00,
    0x80 | (USBD_SELF_POWERED ? 0x40 : 0x00), USBD_MAX_POWER,
    /* Interface association: one function made of the two interfaces below.
     * Without it Windows binds the data interface as a separate device. */
    0x08, 0x0B, 0x00, 0x02, 0x02, 0x02, 0x01, 0x00,
    /* Communication interface */
    0x09, USB_DESC_TYPE_INTERFACE, 0x00, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,
    0x05, 0x24, 0x00, LE16(0x0110),               /* header functional */
    0x05, 0x24, 0x01, 0x00, 0x01,                 /* call management, no data class */
    0x04, 0x24, 0x02, 0x02,                       /* ACM: line coding supported */
    0x05, 0x24, 0x06, 0x00, 0x01,                 /* union: control 0, sub 1 */
    0x07, USB_DESC_TYPE_ENDPOINT, CDC_MIN_CMD_EP, 0x03, LE16(CDC_MIN_CMD_PACKET), 0x10,
    /* Data interface */
    0x09, USB_DESC_TYPE_INTERFACE, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,
    0x07, USB_DESC_TYPE_ENDPOINT, CDC_MIN_OUT_EP, 0x02, LE16(CDC_MIN_PACKET), 0x00,
    0x07, USB_DESC_TYPE_ENDPOINT, CDC_MIN_IN_EP, 0x02, LE16(CDC_MIN_PACKET), 0x00,
};
_Static_assert(sizeof(cdc_min_config_desc) == 75U, "CDC config descriptor length field");

__ALIGN_BEGIN static const uint8_t cdc_min_device_qualifier[USB_LEN_DEV_QUALIFIER_DESC] __ALIGN_END = {
    USB_LEN_DEV_QUALIFIER_DESC, USB_DESC_TYPE_DEVICE_QUALIFIER,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x01, 0x00,
};

static uint8_t cdc_min_init(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
    (void)cfgidx;
    handle = (cdc_min_handle_t *)USBD_malloc(sizeof(cdc_min_handle_t));
    if (!handle) return (uint8_t)USBD_EMEM;
    pdev->pClassData = handle;
    memset(handle, 0, sizeof(*handle));
    handle->command = 0xFFU;
    /* 115200 8N1: a value the host can read back, though the rate is ignored. */
    handle->line_coding[0] = 0x00U; handle->line_coding[1] = 0xC2U;
    handle->line_coding[2] = 0x01U; handle->line_coding[3] = 0x00U;
    handle->line_coding[6] = 8U;
    (void)USBD_LL_OpenEP(pdev, CDC_MIN_IN_EP, USBD_EP_TYPE_BULK, CDC_MIN_PACKET);
    (void)USBD_LL_OpenEP(pdev, CDC_MIN_OUT_EP, USBD_EP_TYPE_BULK, CDC_MIN_PACKET);
    (void)USBD_LL_OpenEP(pdev, CDC_MIN_CMD_EP, USBD_EP_TYPE_INTR, CDC_MIN_CMD_PACKET);
    pdev->ep_in[CDC_MIN_IN_EP & 0xFU].is_used = 1U;
    pdev->ep_out[CDC_MIN_OUT_EP & 0xFU].is_used = 1U;
    pdev->ep_in[CDC_MIN_CMD_EP & 0xFU].is_used = 1U;
    return (uint8_t)USBD_LL_PrepareReceive(pdev, CDC_MIN_OUT_EP, handle->rx, CDC_MIN_PACKET);
}

static uint8_t cdc_min_deinit(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
    (void)cfgidx;
    (void)USBD_LL_CloseEP(pdev, CDC_MIN_IN_EP);
    (void)USBD_LL_CloseEP(pdev, CDC_MIN_OUT_EP);
    (void)USBD_LL_CloseEP(pdev, CDC_MIN_CMD_EP);
    pdev->ep_in[CDC_MIN_IN_EP & 0xFU].is_used = 0U;
    pdev->ep_out[CDC_MIN_OUT_EP & 0xFU].is_used = 0U;
    pdev->ep_in[CDC_MIN_CMD_EP & 0xFU].is_used = 0U;
    pdev->pClassData = NULL;
    handle = NULL;
    return (uint8_t)USBD_OK;
}

static uint8_t cdc_min_setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
    cdc_min_handle_t *h = handle;
    static __ALIGN_BEGIN uint16_t status __ALIGN_END = 0U;
    static uint8_t alternate = 0U;
    if (!h) return (uint8_t)USBD_FAIL;
    switch (req->bmRequest & USB_REQ_TYPE_MASK) {
        case USB_REQ_TYPE_CLASS:
            if (!req->wLength) {
                /* SET_CONTROL_LINE_STATE carries DTR/RTS in wValue, no data. */
                if (req->bRequest == CDC_SET_CONTROL_LINE_STATE) h->open = (req->wValue & 1U) ? 1U : 0U;
                return (uint8_t)USBD_OK;
            }
            if (req->bmRequest & 0x80U) {
                if (req->bRequest == CDC_GET_LINE_CODING)
                    (void)USBD_CtlSendData(pdev, h->line_coding,
                                           req->wLength < 7U ? req->wLength : 7U);
                else return (uint8_t)USBD_FAIL;
            } else {
                h->command = (uint8_t)req->bRequest;
                h->command_length = (uint8_t)(req->wLength < 7U ? req->wLength : 7U);
                (void)USBD_CtlPrepareRx(pdev, h->line_coding, h->command_length);
            }
            return (uint8_t)USBD_OK;
        case USB_REQ_TYPE_STANDARD:
            switch (req->bRequest) {
                case USB_REQ_GET_STATUS:
                    if (pdev->dev_state != USBD_STATE_CONFIGURED) break;
                    (void)USBD_CtlSendData(pdev, (uint8_t *)&status, 2U);
                    return (uint8_t)USBD_OK;
                case USB_REQ_GET_INTERFACE:
                    if (pdev->dev_state != USBD_STATE_CONFIGURED) break;
                    (void)USBD_CtlSendData(pdev, &alternate, 1U);
                    return (uint8_t)USBD_OK;
                case USB_REQ_SET_INTERFACE:
                    if (pdev->dev_state == USBD_STATE_CONFIGURED) return (uint8_t)USBD_OK;
                    break;
                case USB_REQ_CLEAR_FEATURE:
                    return (uint8_t)USBD_OK;
                default: break;
            }
            break;
        default: break;
    }
    USBD_CtlError(pdev, req);
    return (uint8_t)USBD_FAIL;
}

static uint8_t cdc_min_ep0_rx_ready(USBD_HandleTypeDef *pdev)
{
    (void)pdev;
    if (handle) handle->command = 0xFFU; /* line coding already landed in place */
    return (uint8_t)USBD_OK;
}

/* Endpoint callbacks. These run in USB_LP_IRQ; they must stay this short. */
static uint8_t cdc_min_data_in(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    (void)pdev; (void)epnum;
    if (handle) handle->tx_busy = 0U;
    return (uint8_t)USBD_OK;
}

static uint8_t cdc_min_data_out(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    /* The arrival time of the packet, for `clock stamp`: the endpoint is not
     * re-armed until the foreground has read this packet, so the stamp always
     * belongs to the command being parsed. One register read. */
    if (handle) handle->rx_stamp = TIM5->CNT;
    if (handle) handle->rx_length = (uint16_t)USBD_LL_GetRxDataSize(pdev, epnum);
    return (uint8_t)USBD_OK; /* not re-armed until the foreground drains it */
}

static uint8_t *cdc_min_config(uint16_t *length)
{
    *length = (uint16_t)sizeof(cdc_min_config_desc);
    return (uint8_t *)cdc_min_config_desc;
}

static uint8_t *cdc_min_qualifier(uint16_t *length)
{
    *length = (uint16_t)sizeof(cdc_min_device_qualifier);
    return (uint8_t *)cdc_min_device_qualifier;
}

const USBD_ClassTypeDef USBD_CDC_MIN = {
    cdc_min_init, cdc_min_deinit, cdc_min_setup,
    NULL, cdc_min_ep0_rx_ready,
    cdc_min_data_in, cdc_min_data_out,
    NULL, NULL, NULL,
    cdc_min_config, cdc_min_config, cdc_min_config, cdc_min_qualifier,
};

/* ---- foreground side ---- */

extern USBD_HandleTypeDef hUsbDeviceFS;

uint8_t USBD_CdcMinIsOpen(void)
{
    return (handle && handle->open && hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED) ? 1U : 0U;
}

uint8_t USBD_CdcMinTxReady(void)
{
    return (USBD_CdcMinIsOpen() && !handle->tx_busy) ? 1U : 0U;
}

uint16_t USBD_CdcMinTransmit(const uint8_t *data, uint16_t length)
{
    if (!USBD_CdcMinTxReady() || !data || !length) return 0U;
    if (length > CDC_MIN_PACKET) length = CDC_MIN_PACKET;
    memcpy(handle->tx, data, length);
    handle->tx_busy = 1U;
    if (USBD_LL_Transmit(&hUsbDeviceFS, CDC_MIN_IN_EP, handle->tx, length) != USBD_OK) {
        handle->tx_busy = 0U;
        return 0U;
    }
    return length;
}

uint32_t USBD_CdcMinRxStamp(void)
{
    return handle ? handle->rx_stamp : 0U;
}

uint16_t USBD_CdcMinReceive(uint8_t *out, uint16_t cap)
{
    if (!handle || !handle->rx_length || !out || !cap) return 0U;
    uint16_t n = handle->rx_length;
    if (n > cap) n = cap; /* the rest of the packet is dropped, never silently split */
    memcpy(out, handle->rx, n);
    handle->rx_length = 0U;
    (void)USBD_LL_PrepareReceive(&hUsbDeviceFS, CDC_MIN_OUT_EP, handle->rx, CDC_MIN_PACKET);
    return n;
}
