/**
  ******************************************************************************
  * @file    usbd_audio.c
  * @author  MCD Application Team
  * @brief   This file provides the Audio core functions.
  *
  * @verbatim
  *
  *          ===================================================================
  *                                AUDIO Class  Description
  *          ===================================================================
  *           This driver manages the Audio Class 1.0 following the "USB Device Class Definition for
  *           Audio Devices V1.0 Mar 18, 98".
  *           This driver implements the following aspects of the specification:
  *             - Device descriptor management
  *             - Configuration descriptor management
  *             - Standard AC Interface Descriptor management
  *             - 1 Audio Streaming Interface (with single channel, PCM, Stereo mode)
  *             - 1 Audio Streaming Endpoint
  *             - 1 Audio Terminal Input (1 channel)
  *             - Audio Class-Specific AC Interfaces
  *             - Audio Class-Specific AS Interfaces
  *             - AudioControl Requests: only SET_CUR and GET_CUR requests are supported (for Mute)
  *             - Audio Feature Unit (limited to Mute control)
  *             - Audio Synchronization type: Asynchronous
  *             - Single fixed audio sampling rate (configurable in usbd_conf.h file)
  *          The current audio class version supports the following audio features:
  *             - Pulse Coded Modulation (PCM) format
  *             - sampling rate: 48KHz.
  *             - Bit resolution: 16
  *             - Number of channels: 2
  *             - No volume control
  *             - Mute/Unmute capability
  *             - Asynchronous Endpoints
  *
  * @note     In HS mode and when the DMA is used, all variables and data structures
  *           dealing with the DMA during the transaction process should be 32-bit aligned.
  *
  *
  *  @endverbatim
  *
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2015 STMicroelectronics.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by ST under Ultimate Liberty license
  * SLA0044, the "License"; You may not use this file except in compliance with
  * the License. You may obtain a copy of the License at:
  *                      www.st.com/SLA0044
  *
  ******************************************************************************
  */

/* BSPDependencies
- "stm32xxxxx_{eval}{discovery}.c"
- "stm32xxxxx_{eval}{discovery}_io.c"
- "stm32xxxxx_{eval}{discovery}_audio.c"
EndBSPDependencies */

/* Includes ------------------------------------------------------------------*/
#include "usbd_audio.h"
#include "usbd_ctlreq.h"


/** @addtogroup STM32_USB_DEVICE_LIBRARY
  * @{
  */


/** @defgroup USBD_AUDIO
  * @brief usbd core module
  * @{
  */

/** @defgroup USBD_AUDIO_Private_TypesDefinitions
  * @{
  */
/**
  * @}
  */


/** @defgroup USBD_AUDIO_Private_Defines
  * @{
  */
/**
  * @}
  */


/** @defgroup USBD_AUDIO_Private_Macros
  * @{
  */
/* AUDIO_SAMPLE_FREQ and AUDIO_PACKET_SZE: usbd_audio.h, with the descriptor. */
#define AUDIO_FEEDBACK_10_14(frq)      ((uint32_t)((((uint64_t)(frq) << 14) + 500ULL) / 1000ULL))
#define AUDIO_REARM_NOTIFY_PERIOD      32U

_Static_assert(AUDIO_FEEDBACK_10_14(48000U) == 0x0C0000U,
               "48 kHz USB feedback fallback must be 48 samples/frame in Q10.14");
_Static_assert((AUDIO_FEEDBACK_REFRESH >= 1U) && (AUDIO_FEEDBACK_REFRESH <= 9U),
               "USB audio feedback bRefresh must be in the UAC1 full-speed range");
_Static_assert(AUDIO_FEEDBACK_REFRESH_SOF_PERIOD == (1U << AUDIO_FEEDBACK_REFRESH),
               "feedback cadence must be the 2^bRefresh ms the descriptor advertises");
_Static_assert(AUDIO_FEEDBACK_REFRESH_SOF_PERIOD == 2U,
               "this firmware recomputes feedback every other SOF: bRefresh must be 1");
_Static_assert(AUDIO_FEEDBACK_BUSY_CLEAR_SOF > AUDIO_FEEDBACK_REFRESH_SOF_PERIOD,
               "the stale-packet watchdog must outlast one refresh period");
_Static_assert(AUDIO_OUT_PACKET >= 196U,
               "48 kHz asynchronous stereo S16 OUT endpoint must accept 49-frame packets");
_Static_assert(AUDIO_RX_BUFFER_SIZE >= AUDIO_OUT_PACKET,
               "USB audio RX buffer must hold the largest OUT packet");

/**
  * @}
  */


/** @defgroup USBD_AUDIO_Private_FunctionPrototypes
  * @{
  */
uint8_t USBD_AUDIO_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx);
uint8_t USBD_AUDIO_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx);

uint8_t USBD_AUDIO_Setup(USBD_HandleTypeDef *pdev,
                                USBD_SetupReqTypedef *req);

uint8_t *USBD_AUDIO_GetCfgDesc(uint16_t *length);
uint8_t *USBD_AUDIO_GetDeviceQualifierDesc(uint16_t *length);
uint8_t USBD_AUDIO_DataIn(USBD_HandleTypeDef *pdev, uint8_t epnum);
uint8_t USBD_AUDIO_DataOut(USBD_HandleTypeDef *pdev, uint8_t epnum);
uint8_t USBD_AUDIO_EP0_RxReady(USBD_HandleTypeDef *pdev);
uint8_t USBD_AUDIO_EP0_TxReady(USBD_HandleTypeDef *pdev);
uint8_t USBD_AUDIO_SOF(USBD_HandleTypeDef *pdev);

uint8_t USBD_AUDIO_IsoINIncomplete(USBD_HandleTypeDef *pdev, uint8_t epnum);
uint8_t USBD_AUDIO_IsoOutIncomplete(USBD_HandleTypeDef *pdev, uint8_t epnum);
static void AUDIO_REQ_GetCurrent(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req);
static void AUDIO_REQ_SetCurrent(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req);
static void USBD_AUDIO_SendFeedback(USBD_HandleTypeDef *pdev, USBD_AUDIO_HandleTypeDef *haudio);
static void USBD_AUDIO_FeedbackBusyTick(USBD_HandleTypeDef *pdev, USBD_AUDIO_HandleTypeDef *haudio);
static void USBD_AUDIO_Notify(USBD_HandleTypeDef *pdev, uint8_t event, uint32_t value);
static USBD_StatusTypeDef USBD_AUDIO_PrepareOut(USBD_HandleTypeDef *pdev,
                                                USBD_AUDIO_HandleTypeDef *haudio);
static void USBD_AUDIO_RetryOutRearm(USBD_HandleTypeDef *pdev,
                                     USBD_AUDIO_HandleTypeDef *haudio);
static void USBD_AUDIO_PrimeCapture(USBD_HandleTypeDef *pdev,
                                    USBD_AUDIO_HandleTypeDef *haudio);
static void USBD_AUDIO_RefillCapture(USBD_HandleTypeDef *pdev,
                                     USBD_AUDIO_HandleTypeDef *haudio);

/**
  * @}
  */

/** @defgroup USBD_AUDIO_Private_Variables
  * @{
  */

USBD_ClassTypeDef USBD_AUDIO =
{
  USBD_AUDIO_Init,
  USBD_AUDIO_DeInit,
  USBD_AUDIO_Setup,
  USBD_AUDIO_EP0_TxReady,
  USBD_AUDIO_EP0_RxReady,
  USBD_AUDIO_DataIn,
  USBD_AUDIO_DataOut,
  USBD_AUDIO_SOF,
  USBD_AUDIO_IsoINIncomplete,
  USBD_AUDIO_IsoOutIncomplete,
  USBD_AUDIO_GetCfgDesc,
  USBD_AUDIO_GetCfgDesc,
  USBD_AUDIO_GetCfgDesc,
  USBD_AUDIO_GetDeviceQualifierDesc,
};

/* Retained across USB personality changes so the diagnostic console can read
 * the capture session that just ended. Each field is a naturally atomic
 * Cortex-M word; the snapshot is diagnostic rather than control state. */
static volatile USBD_AUDIO_CaptureStatsTypeDef capture_stats;

/* USB AUDIO device Configuration Descriptor (flash; GetCfgDesc hands out a
 * pointer the core only reads) */
static const uint8_t USBD_AUDIO_CfgDesc[USB_AUDIO_CONFIG_DESC_SIZ] =
{
  /* Configuration 1 */
  0x09,                                 /* bLength */
  USB_DESC_TYPE_CONFIGURATION,          /* bDescriptorType */
  LOBYTE(USB_AUDIO_CONFIG_DESC_SIZ),    /* wTotalLength */
  HIBYTE(USB_AUDIO_CONFIG_DESC_SIZ),
  0x03,                                 /* bNumInterfaces */
  0x01,                                 /* bConfigurationValue */
  0x00,                                 /* iConfiguration */
#if (USBD_SELF_POWERED == 1U)
  0xC0,                                 /* bmAttributes: Bus Powered according to user configuration */
#else
  0x80,                                 /* bmAttributes: Bus Powered according to user configuration */
#endif
  USBD_MAX_POWER,                       /* bMaxPower = 100 mA */
  /* 09 byte*/
  USBD_AUDIO_FUNCTION_DESC
};
_Static_assert(sizeof((const uint8_t[]){ USBD_AUDIO_FUNCTION_DESC }) + 9U == USB_AUDIO_CONFIG_DESC_SIZ,
               "USB_AUDIO_CONFIG_DESC_SIZ must match the audio descriptor");

/* USB Standard Device Descriptor */
__ALIGN_BEGIN static const uint8_t USBD_AUDIO_DeviceQualifierDesc[USB_LEN_DEV_QUALIFIER_DESC] __ALIGN_END =
{
  USB_LEN_DEV_QUALIFIER_DESC,
  USB_DESC_TYPE_DEVICE_QUALIFIER,
  0x00,
  0x02,
  0x00,
  0x00,
  0x00,
  0x40,
  0x01,
  0x00,
};

/**
  * @}
  */

/** @defgroup USBD_AUDIO_Private_Functions
  * @{
  */

/**
  * @brief  USBD_AUDIO_Init
  *         Initialize the AUDIO interface
  * @param  pdev: device instance
  * @param  cfgidx: Configuration index
  * @retval status
  */
uint8_t USBD_AUDIO_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
  UNUSED(cfgidx);
  USBD_AUDIO_HandleTypeDef *haudio;

  /* Allocate Audio structure */
  haudio = USBD_malloc(sizeof(USBD_AUDIO_HandleTypeDef));

  if (haudio == NULL)
  {
    pdev->pClassData = NULL;
    return (uint8_t)USBD_EMEM;
  }

  pdev->pClassData = (void *)haudio;

  if (pdev->dev_speed == USBD_SPEED_HIGH)
  {
    pdev->ep_out[AUDIO_OUT_EP & 0xFU].bInterval = AUDIO_HS_BINTERVAL;
    pdev->ep_in[AUDIO_FEEDBACK_EP & 0xFU].bInterval = AUDIO_HS_BINTERVAL;
  }
  else   /* LOW and FULL-speed endpoints */
  {
    pdev->ep_out[AUDIO_OUT_EP & 0xFU].bInterval = AUDIO_FS_BINTERVAL;
    pdev->ep_in[AUDIO_FEEDBACK_EP & 0xFU].bInterval = AUDIO_FS_BINTERVAL;
  }

  /* Open EP OUT and explicit feedback IN endpoint */
  (void)USBD_LL_OpenEP(pdev, AUDIO_OUT_EP, USBD_EP_TYPE_ISOC, AUDIO_OUT_PACKET);
  pdev->ep_out[AUDIO_OUT_EP & 0xFU].is_used = 1U;
  (void)USBD_LL_OpenEP(pdev, AUDIO_FEEDBACK_EP, USBD_EP_TYPE_ISOC, AUDIO_FEEDBACK_PACKET);
  pdev->ep_in[AUDIO_FEEDBACK_EP & 0xFU].is_used = 1U;

  (void)USBD_LL_OpenEP(pdev, AUDIO_CAPTURE_EP, USBD_EP_TYPE_ISOC, AUDIO_CAPTURE_PACKET);
  pdev->ep_in[AUDIO_CAPTURE_EP & 0xFU].is_used = 1U;
  pdev->ep_in[AUDIO_CAPTURE_EP & 0xFU].bInterval = AUDIO_FS_BINTERVAL;
  haudio->capture_alt = haudio->capture_busy = haudio->capture_age = 0U;
  haudio->alt_setting = 0U;
  haudio->offset = AUDIO_OFFSET_NONE;
  haudio->wr_ptr = 0U;
  haudio->rd_ptr = 0U;
  haudio->rd_enable = 0U;
  haudio->feedback_busy = 0U;
  haudio->feedback_busy_sofs = 0U;
  haudio->feedback_refresh_sofs = 0U;
  haudio->rx_buffer_idx = 0U;
  haudio->rx_rearm_pending = 0U;
  haudio->rx_rearm_attempts = 0U;
  (void)USBD_memset(haudio->buffer, 0, sizeof(haudio->buffer));
  (void)USBD_memset(haudio->feedback_data, 0, sizeof(haudio->feedback_data));

  /* Initialize the Audio output Hardware layer */
  if (((USBD_AUDIO_ItfTypeDef *)pdev->pUserData)->Init(USBD_AUDIO_FREQ,
                                                       AUDIO_DEFAULT_VOLUME,
                                                       0U) != 0U)
  {
    return (uint8_t)USBD_FAIL;
  }

  /* Prepare Out endpoint to receive 1st packet */
  (void)USBD_AUDIO_PrepareOut(pdev, haudio);

  return (uint8_t)USBD_OK;
}

/**
  * @brief  USBD_AUDIO_Init
  *         DeInitialize the AUDIO layer
  * @param  pdev: device instance
  * @param  cfgidx: Configuration index
  * @retval status
  */
uint8_t USBD_AUDIO_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
  UNUSED(cfgidx);

  /* Close audio OUT EP and feedback IN EP */
  (void)USBD_LL_CloseEP(pdev, AUDIO_OUT_EP);
  pdev->ep_out[AUDIO_OUT_EP & 0xFU].is_used = 0U;
  pdev->ep_out[AUDIO_OUT_EP & 0xFU].bInterval = 0U;
  (void)USBD_LL_CloseEP(pdev, AUDIO_FEEDBACK_EP);
  pdev->ep_in[AUDIO_FEEDBACK_EP & 0xFU].is_used = 0U;
  pdev->ep_in[AUDIO_FEEDBACK_EP & 0xFU].bInterval = 0U;

  (void)USBD_LL_CloseEP(pdev, AUDIO_CAPTURE_EP);
  pdev->ep_in[AUDIO_CAPTURE_EP & 0xFU].is_used = 0U;
  pdev->ep_in[AUDIO_CAPTURE_EP & 0xFU].bInterval = 0U;

  /* DeInit  physical Interface components */
  if (pdev->pClassData != NULL)
  {
    ((USBD_AUDIO_ItfTypeDef *)pdev->pUserData)->DeInit(0U);
    (void)USBD_free(pdev->pClassData);
    pdev->pClassData = NULL;
  }

  return (uint8_t)USBD_OK;
}

/**
  * @brief  USBD_AUDIO_Setup
  *         Handle the AUDIO specific requests
  * @param  pdev: instance
  * @param  req: usb requests
  * @retval status
  */
uint8_t USBD_AUDIO_Setup(USBD_HandleTypeDef *pdev,
                                USBD_SetupReqTypedef *req)
{
  USBD_AUDIO_HandleTypeDef *haudio;
  uint16_t len;
  uint8_t *pbuf;
  uint16_t status_info = 0U;
  USBD_StatusTypeDef ret = USBD_OK;

  haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;

  if (haudio == NULL)
  {
    return (uint8_t)USBD_FAIL;
  }

  switch (req->bmRequest & USB_REQ_TYPE_MASK)
  {
    case USB_REQ_TYPE_CLASS:
      switch (req->bRequest)
      {
        case AUDIO_REQ_GET_CUR:
          AUDIO_REQ_GetCurrent(pdev, req);
          break;

        case AUDIO_REQ_SET_CUR:
          AUDIO_REQ_SetCurrent(pdev, req);
          break;

        default:
          USBD_CtlError(pdev, req);
          ret = USBD_FAIL;
          break;
      }
      break;

    case USB_REQ_TYPE_STANDARD:
      switch (req->bRequest)
      {
        case USB_REQ_GET_STATUS:
          if (pdev->dev_state == USBD_STATE_CONFIGURED)
          {
            (void)USBD_CtlSendData(pdev, (uint8_t *)&status_info, 2U);
          }
          else
          {
            USBD_CtlError(pdev, req);
            ret = USBD_FAIL;
          }
          break;

        case USB_REQ_GET_DESCRIPTOR:
          if ((req->wValue >> 8) == AUDIO_DESCRIPTOR_TYPE)
          {
            pbuf = (uint8_t *)USBD_AUDIO_CfgDesc + 18;
            len = MIN(USB_AUDIO_DESC_SIZ, req->wLength);

            (void)USBD_CtlSendData(pdev, pbuf, len);
          }
          break;

        case USB_REQ_GET_INTERFACE:
          if (pdev->dev_state == USBD_STATE_CONFIGURED && req->wIndex <= 2U)
          {
            haudio->control.data[0] = req->wIndex == 2U ? haudio->capture_alt : req->wIndex == 1U ? (uint8_t)haudio->alt_setting : 0U;
            (void)USBD_CtlSendData(pdev, haudio->control.data, 1U);
          }
          else
          {
            USBD_CtlError(pdev, req);
            ret = USBD_FAIL;
          }
          break;

        case USB_REQ_SET_INTERFACE:
          if (pdev->dev_state == USBD_STATE_CONFIGURED)
          {
            if (req->wIndex <= 2U && req->wValue <= 1U && !(req->wIndex == 0U && req->wValue != 0U))
            {
              uint8_t new_alt = (uint8_t)(req->wValue);
              if (req->wIndex == 0U) break;
              if (req->wIndex == 2U)
              {
                (void)USBD_LL_FlushEP(pdev, AUDIO_CAPTURE_EP);
                haudio->capture_alt = new_alt;
                haudio->capture_busy = haudio->capture_age = 0U;
                if (new_alt != 0U)
                {
                  (void)USBD_memset((void *)&capture_stats, 0, sizeof(capture_stats));
                }
                USBD_AUDIO_ItfTypeDef *itf = (USBD_AUDIO_ItfTypeDef *)pdev->pUserData;
                if (itf->Capture != NULL) (void)itf->Capture(NULL, 0U);
                USBD_AUDIO_PrimeCapture(pdev, haudio);
                USBD_AUDIO_RefillCapture(pdev, haudio);
                break;
              }
              if (new_alt != haudio->alt_setting)
              {
                haudio->alt_setting = new_alt;
                if (new_alt == 0U)
                {
	                  ((USBD_AUDIO_ItfTypeDef *)pdev->pUserData)->DeInit(0U);
	                  haudio->feedback_busy = 0U;
	                  haudio->feedback_busy_sofs = 0U;
	                  haudio->feedback_refresh_sofs = 0U;
	                  haudio->rx_rearm_pending = 0U;
                  haudio->rx_rearm_attempts = 0U;
                  USBD_AUDIO_Notify(pdev, USBD_AUDIO_EVENT_STREAM_INACTIVE, 0U);
                }
                else
                {
	                  ((USBD_AUDIO_ItfTypeDef *)pdev->pUserData)->Init(USBD_AUDIO_FREQ,
	                                                                   AUDIO_DEFAULT_VOLUME,
	                                                                   0U);
	                  haudio->feedback_busy = 0U;
	                  haudio->feedback_busy_sofs = 0U;
	                  haudio->feedback_refresh_sofs = 0U;
	                  haudio->rx_buffer_idx = 0U;
                  haudio->rx_rearm_pending = 0U;
                  haudio->rx_rearm_attempts = 0U;
                  (void)USBD_AUDIO_PrepareOut(pdev, haudio);
                  USBD_AUDIO_SendFeedback(pdev, haudio);
                  USBD_AUDIO_Notify(pdev, USBD_AUDIO_EVENT_STREAM_ACTIVE, 1U);
                }
              }
              haudio->offset = AUDIO_OFFSET_NONE;
              haudio->wr_ptr = 0U;
              haudio->rd_ptr = 0U;
              haudio->rd_enable = 0U;
            }
            else
            {
              /* Call the error management function (command will be NAKed */
              USBD_CtlError(pdev, req);
              ret = USBD_FAIL;
            }
          }
          else
          {
            USBD_CtlError(pdev, req);
            ret = USBD_FAIL;
          }
          break;

        case USB_REQ_CLEAR_FEATURE:
          break;

        default:
          USBD_CtlError(pdev, req);
          ret = USBD_FAIL;
          break;
      }
      break;
    default:
      USBD_CtlError(pdev, req);
      ret = USBD_FAIL;
      break;
  }

  return (uint8_t)ret;
}


/**
  * @brief  USBD_AUDIO_GetCfgDesc
  *         return configuration descriptor
  * @param  speed : current device speed
  * @param  length : pointer data length
  * @retval pointer to descriptor buffer
  */
uint8_t *USBD_AUDIO_GetCfgDesc(uint16_t *length)
{
  *length = (uint16_t)sizeof(USBD_AUDIO_CfgDesc);

  return (uint8_t *)USBD_AUDIO_CfgDesc;
}

/**
  * @brief  USBD_AUDIO_DataIn
  *         handle data IN Stage
  * @param  pdev: device instance
  * @param  epnum: endpoint index
  * @retval status
  */
uint8_t USBD_AUDIO_DataIn(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
  USBD_AUDIO_HandleTypeDef *haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;

  if ((haudio != NULL) && (epnum == (AUDIO_FEEDBACK_EP & 0x7FU)))
  {
    /* Collected. The refresh cadence stays the SOF's: the host polls this
     * endpoint every frame, and restarting the count here kept it from ever
     * reaching the refresh -- the host had the stream's first value (the
     * nominal 48.0) and nothing after it, except when an SOF came late and
     * two passed between polls (bench, 2026-10-03). */
    haudio->feedback_busy = 0U;
    haudio->feedback_busy_sofs = 0U;
  }

  if (haudio != NULL && epnum == (AUDIO_CAPTURE_EP & 0x7FU))
  {
    ++capture_stats.completions;
    haudio->capture_busy = haudio->capture_age = 0U;
    USBD_AUDIO_RefillCapture(pdev, haudio);
  }

  return (uint8_t)USBD_OK;
}

/**
  * @brief  USBD_AUDIO_EP0_RxReady
  *         handle EP0 Rx Ready event
  * @param  pdev: device instance
  * @retval status
  */
uint8_t USBD_AUDIO_EP0_RxReady(USBD_HandleTypeDef *pdev)
{
  USBD_AUDIO_HandleTypeDef *haudio;
  haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;

  if (haudio == NULL)
  {
    return (uint8_t)USBD_FAIL;
  }

  if (haudio->control.cmd == AUDIO_REQ_SET_CUR)
  {
    /* In this driver, to simplify code, only SET_CUR request is managed */

    if (haudio->control.unit == AUDIO_OUT_STREAMING_CTRL)
    {
      ((USBD_AUDIO_ItfTypeDef *)pdev->pUserData)->MuteCtl(haudio->control.data[0]);
      haudio->control.cmd = 0U;
      haudio->control.len = 0U;
    }
  }

  return (uint8_t)USBD_OK;
}
/**
  * @brief  USBD_AUDIO_EP0_TxReady
  *         handle EP0 TRx Ready event
  * @param  pdev: device instance
  * @retval status
  */
uint8_t USBD_AUDIO_EP0_TxReady(USBD_HandleTypeDef *pdev)
{
  UNUSED(pdev);

  /* Only OUT control data are processed */
  return (uint8_t)USBD_OK;
}
/**
  * @brief  USBD_AUDIO_SOF
  *         handle SOF event
  * @param  pdev: device instance
  * @retval status
  */
uint8_t USBD_AUDIO_SOF(USBD_HandleTypeDef *pdev)
{
  USBD_AUDIO_HandleTypeDef *haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;

  if (haudio != NULL)
  {
    if (pdev->dev_state == USBD_STATE_CONFIGURED && haudio->capture_alt)
    {
      ++capture_stats.sofs;
      /* Both PMA banks stay one packet ahead. DataIn refills the bank released
       * by the last transaction; SOF is only the independent cadence meter. */
    }
    USBD_AUDIO_RetryOutRearm(pdev, haudio);
    USBD_AUDIO_FeedbackBusyTick(pdev, haudio);
    if ((pdev->dev_state == USBD_STATE_CONFIGURED) && (haudio->alt_setting != 0U))
    {
      if (++haudio->feedback_refresh_sofs >= AUDIO_FEEDBACK_REFRESH_SOF_PERIOD)
      {
        haudio->feedback_refresh_sofs = 0U;
        USBD_AUDIO_SendFeedback(pdev, haudio);
      }
    }
    else
    {
      haudio->feedback_refresh_sofs = 0U;
    }
  }

  return (uint8_t)USBD_OK;
}

static void USBD_AUDIO_PrimeCapture(USBD_HandleTypeDef *pdev,
                                    USBD_AUDIO_HandleTypeDef *haudio)
{
  if ((pdev == NULL) || (haudio == NULL) || haudio->capture_busy ||
      !haudio->capture_alt || (pdev->dev_state != USBD_STATE_CONFIGURED))
  {
    return;
  }

  USBD_AUDIO_ItfTypeDef *itf = (USBD_AUDIO_ItfTypeDef *)pdev->pUserData;
  uint16_t frames = (itf != NULL) && (itf->Capture != NULL)
                  ? itf->Capture(haudio->capture_data, 49U) : 0U;
  if (frames > 49U) frames = 49U;
  if (!frames)
  {
    frames = 48U;
    (void)USBD_memset(haudio->capture_data, 0, 192U);
  }
  haudio->capture_age = 0U;
  ++capture_stats.submits;
  capture_stats.submitted_frames += frames;
  haudio->capture_busy = USBD_LL_Transmit(pdev, AUDIO_CAPTURE_EP,
      (uint8_t *)haudio->capture_data, frames * 4U) == USBD_OK;
  if (!haudio->capture_busy) ++capture_stats.submit_failures;
}

static void USBD_AUDIO_RefillCapture(USBD_HandleTypeDef *pdev,
                                     USBD_AUDIO_HandleTypeDef *haudio)
{
  if ((pdev == NULL) || (haudio == NULL) || !haudio->capture_alt ||
      (pdev->dev_state != USBD_STATE_CONFIGURED))
  {
    return;
  }

  USBD_AUDIO_ItfTypeDef *itf = (USBD_AUDIO_ItfTypeDef *)pdev->pUserData;
  uint16_t frames = (itf != NULL) && (itf->Capture != NULL)
                  ? itf->Capture(haudio->capture_data, 49U) : 0U;
  if (frames > 49U) frames = 49U;
  if (!frames)
  {
    frames = 48U;
    (void)USBD_memset(haudio->capture_data, 0, 192U);
  }
  haudio->capture_age = 0U;
  ++capture_stats.submits;
  capture_stats.submitted_frames += frames;
  haudio->capture_busy = USBD_LL_RefillIsoIn(pdev->pData, AUDIO_CAPTURE_EP,
      (uint8_t *)haudio->capture_data, (uint16_t)(frames * 4U));
  if (!haudio->capture_busy) ++capture_stats.submit_failures;
}

/**
  * @brief  USBD_AUDIO_SOF
  *         handle SOF event
  * @param  pdev: device instance
  * @retval status
  */
static void USBD_AUDIO_SendFeedback(USBD_HandleTypeDef *pdev, USBD_AUDIO_HandleTypeDef *haudio)
{
  uint32_t fb;
  USBD_StatusTypeDef tx_status;

  if ((pdev == NULL) || (haudio == NULL))
  {
    return;
  }

  if ((pdev->dev_state != USBD_STATE_CONFIGURED) || (haudio->alt_setting == 0U))
  {
    return;
  }
  if (haudio->feedback_busy != 0U)
  {
    return;
  }

  fb = AUDIO_FEEDBACK_10_14(USBD_AUDIO_FREQ);
  if ((((USBD_AUDIO_ItfTypeDef *)pdev->pUserData) != NULL) && (((USBD_AUDIO_ItfTypeDef *)pdev->pUserData)->GetFeedback10_14 != NULL))
  {
    fb = ((USBD_AUDIO_ItfTypeDef *)pdev->pUserData)->GetFeedback10_14();
  }

  haudio->feedback_data[0] = (uint8_t)(fb & 0xFFU);
  haudio->feedback_data[1] = (uint8_t)((fb >> 8) & 0xFFU);
  haudio->feedback_data[2] = (uint8_t)((fb >> 16) & 0xFFU);
  /* Both banks (usbd_conf.c): the host reads this value whichever bank the
   * peripheral sends. */
  tx_status = USBD_LL_LoadIsoInBoth(pdev->pData, AUDIO_FEEDBACK_EP, haudio->feedback_data,
                                    AUDIO_FEEDBACK_PACKET) ? USBD_OK : USBD_FAIL;
  haudio->feedback_busy = (tx_status == USBD_OK) ? 1U : 0U;
  haudio->feedback_busy_sofs = 0U;
  if (tx_status != USBD_OK)
  {
    USBD_AUDIO_Notify(pdev, USBD_AUDIO_EVENT_FEEDBACK_TX_FAILED, (uint32_t)tx_status);
  }
}

static void USBD_AUDIO_FeedbackBusyTick(USBD_HandleTypeDef *pdev, USBD_AUDIO_HandleTypeDef *haudio)
{
  if ((pdev == NULL) || (haudio == NULL) || (haudio->feedback_busy == 0U))
  {
    return;
  }

  if (haudio->feedback_busy_sofs < 0xFFU)
  {
    haudio->feedback_busy_sofs++;
  }
  if (haudio->feedback_busy_sofs < AUDIO_FEEDBACK_BUSY_CLEAR_SOF)
  {
    return;
  }

  haudio->feedback_busy = 0U;
  haudio->feedback_busy_sofs = 0U;
  USBD_AUDIO_Notify(pdev, USBD_AUDIO_EVENT_FEEDBACK_TX_FAILED, 0xFEED0001UL);
}

static void USBD_AUDIO_Notify(USBD_HandleTypeDef *pdev, uint8_t event, uint32_t value)
{
  if ((pdev != NULL) &&
      (((USBD_AUDIO_ItfTypeDef *)pdev->pUserData) != NULL) &&
      (((USBD_AUDIO_ItfTypeDef *)pdev->pUserData)->Notify != NULL))
  {
    ((USBD_AUDIO_ItfTypeDef *)pdev->pUserData)->Notify(event, value);
  }
}

static void USBD_AUDIO_MarkRearmFailed(USBD_HandleTypeDef *pdev,
                                       USBD_AUDIO_HandleTypeDef *haudio,
                                       USBD_StatusTypeDef status)
{
  if (haudio == NULL)
  {
    return;
  }

  haudio->rx_rearm_pending = 1U;
  if (haudio->rx_rearm_attempts < 0xFFU)
  {
    haudio->rx_rearm_attempts++;
  }
  if ((haudio->rx_rearm_attempts == 1U) ||
      ((haudio->rx_rearm_attempts % AUDIO_REARM_NOTIFY_PERIOD) == 0U))
  {
    USBD_AUDIO_Notify(pdev, USBD_AUDIO_EVENT_REARM_FAILED,
                      ((uint32_t)haudio->rx_rearm_attempts << 16) | (uint32_t)status);
  }
}

static USBD_StatusTypeDef USBD_AUDIO_PrepareOut(USBD_HandleTypeDef *pdev,
                                                USBD_AUDIO_HandleTypeDef *haudio)
{
  USBD_StatusTypeDef status;

  if ((pdev == NULL) || (haudio == NULL))
  {
    return USBD_FAIL;
  }

  status = USBD_LL_PrepareReceive(pdev, AUDIO_OUT_EP,
                                  &haudio->buffer[haudio->rx_buffer_idx & 1U][0],
                                  AUDIO_OUT_PACKET);
  if (status == USBD_OK)
  {
    haudio->rx_rearm_pending = 0U;
    haudio->rx_rearm_attempts = 0U;
  }
  else
  {
    USBD_AUDIO_MarkRearmFailed(pdev, haudio, status);
  }
  return status;
}

static void USBD_AUDIO_RetryOutRearm(USBD_HandleTypeDef *pdev,
                                     USBD_AUDIO_HandleTypeDef *haudio)
{
  if ((pdev == NULL) || (haudio == NULL))
  {
    return;
  }
  if ((haudio->rx_rearm_pending == 0U) ||
      (pdev->dev_state != USBD_STATE_CONFIGURED) ||
      (haudio->alt_setting == 0U))
  {
    return;
  }
  (void)USBD_AUDIO_PrepareOut(pdev, haudio);
}

void USBD_AUDIO_Sync(USBD_HandleTypeDef *pdev, AUDIO_OffsetTypeDef offset)
{
  UNUSED(pdev);
  UNUSED(offset);
}

void USBD_AUDIO_GetCaptureStats(USBD_AUDIO_CaptureStatsTypeDef *out)
{
  if (out == NULL) return;
  out->sofs = capture_stats.sofs;
  out->submits = capture_stats.submits;
  out->completions = capture_stats.completions;
  out->incomplete = capture_stats.incomplete;
  out->submit_failures = capture_stats.submit_failures;
  out->submitted_frames = capture_stats.submitted_frames;
}

/**
  * @brief  USBD_AUDIO_IsoINIncomplete
  *         handle data ISO IN Incomplete event
  * @param  pdev: device instance
  * @param  epnum: endpoint index
  * @retval status
  */
uint8_t USBD_AUDIO_IsoINIncomplete(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
  USBD_AUDIO_HandleTypeDef *haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;

  if ((haudio != NULL) && (epnum == (AUDIO_FEEDBACK_EP & 0x7FU)))
  {
    /* Not collected this frame: the next SOF refresh loads it again (the
     * count is the SOF's, as in USBD_AUDIO_DataIn). */
    haudio->feedback_busy = 0U;
    haudio->feedback_busy_sofs = 0U;
  }

  if (haudio != NULL && epnum == (AUDIO_CAPTURE_EP & 0x7FU))
  {
    ++capture_stats.incomplete;
    haudio->capture_busy = haudio->capture_age = 0U;
  }

  return (uint8_t)USBD_OK;
}
/**
  * @brief  USBD_AUDIO_IsoOutIncomplete
  *         handle data ISO OUT Incomplete event
  * @param  pdev: device instance
  * @param  epnum: endpoint index
  * @retval status
  */
uint8_t USBD_AUDIO_IsoOutIncomplete(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
  if (epnum == AUDIO_OUT_EP)
  {
    USBD_AUDIO_HandleTypeDef *haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;
    if (haudio != NULL)
    {
      haudio->rx_rearm_pending = 1U;
    }
    USBD_AUDIO_Notify(pdev, USBD_AUDIO_EVENT_OUT_INCOMPLETE, epnum);
  }

  return (uint8_t)USBD_OK;
}
/**
  * @brief  USBD_AUDIO_DataOut
  *         handle data OUT Stage
  * @param  pdev: device instance
  * @param  epnum: endpoint index
  * @retval status
  */
uint8_t USBD_AUDIO_DataOut(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
  uint16_t PacketSize;
  USBD_AUDIO_HandleTypeDef *haudio;
  uint8_t rx_idx;
  uint8_t next_idx;
  uint8_t *packet;

  haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;

  if (haudio == NULL)
  {
    return (uint8_t)USBD_FAIL;
  }

  if (epnum == AUDIO_OUT_EP)
  {
    /* Get received data packet length */
    PacketSize = (uint16_t)USBD_LL_GetRxDataSize(pdev, epnum);
    rx_idx = haudio->rx_buffer_idx & 1U;
    next_idx = rx_idx ^ 1U;
    packet = &haudio->buffer[rx_idx][0];

    haudio->rx_buffer_idx = next_idx;
    (void)USBD_AUDIO_PrepareOut(pdev, haudio);

    /* v42: use immediate packet delivery instead of ST's large internal
     * circular staging buffer. The modulation transport already has its own
     * PCM/render queues and clock servo, so forwarding the exact USB packet
     * here removes one redundant buffering layer and makes packet timing
     * deterministic. */
    if ((haudio->alt_setting != 0U) && (PacketSize > 0U))
    {
      ((USBD_AUDIO_ItfTypeDef *)pdev->pUserData)->PeriodicTC(packet,
                                                             PacketSize, AUDIO_OUT_TC);
    }
    else if ((haudio->alt_setting != 0U) && (PacketSize == 0U))
    {
      USBD_AUDIO_Notify(pdev, USBD_AUDIO_EVENT_ZERO_PACKET, 0U);
    }

    haudio->offset = AUDIO_OFFSET_NONE;
    haudio->wr_ptr = 0U;
    haudio->rd_ptr = 0U;
    haudio->rd_enable = 0U;
  }

  return (uint8_t)USBD_OK;
}

/**
  * @brief  AUDIO_Req_GetCurrent
  *         Handles the GET_CUR Audio control request.
  * @param  pdev: instance
  * @param  req: setup class request
  * @retval status
  */
static void AUDIO_REQ_GetCurrent(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
  USBD_AUDIO_HandleTypeDef *haudio;
  uint16_t len;
  haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;

  if (haudio == NULL)
  {
    return;
  }

  (void)USBD_memset(haudio->control.data, 0, sizeof(haudio->control.data)); /* USB_MAX_EP0_SIZE, 32 since v8.10 */

  /* Send the current mute state */
  len = MIN(req->wLength, (uint16_t)sizeof(haudio->control.data));
  (void)USBD_CtlSendData(pdev, haudio->control.data, len);
}

/**
  * @brief  AUDIO_Req_SetCurrent
  *         Handles the SET_CUR Audio control request.
  * @param  pdev: instance
  * @param  req: setup class request
  * @retval status
  */
static void AUDIO_REQ_SetCurrent(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
  USBD_AUDIO_HandleTypeDef *haudio;
  haudio = (USBD_AUDIO_HandleTypeDef *)pdev->pClassData;

  if (haudio == NULL)
  {
    return;
  }

  if (req->wLength != 0U)
  {
    if (req->wLength > sizeof(haudio->control.data))
    {
      USBD_CtlError(pdev, req);
      return;
    }

    /* Prepare the reception of the buffer over EP0 */
    (void)USBD_CtlPrepareRx(pdev, haudio->control.data, req->wLength);

    haudio->control.cmd = AUDIO_REQ_SET_CUR;     /* Set the request value */
    haudio->control.len = (uint8_t)req->wLength; /* Set the request data length */
    haudio->control.unit = HIBYTE(req->wIndex);  /* Set the request target unit */
  }
}


/**
  * @brief  DeviceQualifierDescriptor
  *         return Device Qualifier descriptor
  * @param  length : pointer data length
  * @retval pointer to descriptor buffer
  */
uint8_t *USBD_AUDIO_GetDeviceQualifierDesc(uint16_t *length)
{
  *length = (uint16_t)sizeof(USBD_AUDIO_DeviceQualifierDesc);

  return (uint8_t *)USBD_AUDIO_DeviceQualifierDesc;
}

/**
  * @brief  USBD_AUDIO_RegisterInterface
  * @param  fops: Audio interface callback
  * @retval status
  */
uint8_t USBD_AUDIO_RegisterInterface(USBD_HandleTypeDef *pdev,
                                     USBD_AUDIO_ItfTypeDef *fops)
{
  if (fops == NULL)
  {
    return (uint8_t)USBD_FAIL;
  }

  pdev->pUserData = fops;

  return (uint8_t)USBD_OK;
}
/**
  * @}
  */


/**
  * @}
  */


/**
  * @}
  */

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
