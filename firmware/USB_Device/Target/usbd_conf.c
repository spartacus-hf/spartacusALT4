/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : Target/usbd_conf.c
  * @version        : v3.0_Cube
  * @brief          : This file implements the board support package for the USB device library
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "stm32g4xx.h"
#include "stm32g4xx_hal.h"
#include "usbd_def.h"
#include "usbd_core.h"
#include "modulator.h"
#include "usb_device.h"

#include "usbd_audio.h"
#include "usbd_msc.h"
#include "usbd_cdc_min.h"
#include "usbd_cat.h"
#include "radio_clocks.h"
#include "radio_digi.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
#define USB_PMA_ALIGN8(v_)              (((v_) + 7U) & ~7U)
/* Packet memory, audio personality (1 KB, buffer table in the first 64 B):
 *   0x040 EP0 OUT 32, 0x060 EP0 IN 32
 *   0x080 playback 2 x 200, 0x210 feedback 2 x 4, 0x218 capture 2 x 200
 *   0x3A8 the CAT ports' notification buffer, shared: nothing is ever sent
 *   0x3B0 port 0 OUT 8 + IN 32, 0x3D8 port 1 OUT 8 + IN 32, ending at 0x400.
 * The ports fit only because EP0 went from 64 to 32 bytes (usbd_conf.h). */
#define USB_EP0_OUT_PMA                 0x0040U
#define USB_EP0_IN_PMA                  (USB_EP0_OUT_PMA + USB_MAX_EP0_SIZE)
#define USB_AUDIO_OUT_PMA0              (USB_EP0_IN_PMA + USB_MAX_EP0_SIZE)
#define USB_AUDIO_OUT_PMA_STRIDE        USB_PMA_ALIGN8((uint32_t)AUDIO_OUT_PACKET)
#define USB_AUDIO_OUT_PMA1              (USB_AUDIO_OUT_PMA0 + USB_AUDIO_OUT_PMA_STRIDE)
#define USB_AUDIO_FEEDBACK_PMA          (USB_AUDIO_OUT_PMA1 + USB_AUDIO_OUT_PMA_STRIDE)
#define USB_AUDIO_FEEDBACK_PMA1         (USB_AUDIO_FEEDBACK_PMA + 4U)
#define USB_AUDIO_CAPTURE_PMA0          USB_PMA_ALIGN8(USB_AUDIO_FEEDBACK_PMA1 + AUDIO_FEEDBACK_PACKET)
#define USB_AUDIO_CAPTURE_PMA_STRIDE     USB_PMA_ALIGN8((uint32_t)AUDIO_CAPTURE_PACKET)
#define USB_AUDIO_CAPTURE_PMA1          (USB_AUDIO_CAPTURE_PMA0 + USB_AUDIO_CAPTURE_PMA_STRIDE)
#define USB_AUDIO_OUT_DBL_PMA           (((uint32_t)USB_AUDIO_OUT_PMA1 << 16) | USB_AUDIO_OUT_PMA0)
#define USB_AUDIO_CAPTURE_DBL_PMA       (((uint32_t)USB_AUDIO_CAPTURE_PMA1 << 16) | USB_AUDIO_CAPTURE_PMA0)
#define USB_AUDIO_FEEDBACK_DBL_PMA      (((uint32_t)USB_AUDIO_FEEDBACK_PMA1 << 16) | USB_AUDIO_FEEDBACK_PMA)

_Static_assert(USB_AUDIO_OUT_PMA_STRIDE >= AUDIO_OUT_PACKET,
               "USB audio PMA stride must hold the full OUT packet");
_Static_assert(USB_AUDIO_FEEDBACK_PMA >= (USB_AUDIO_OUT_PMA1 + AUDIO_OUT_PACKET),
               "USB feedback PMA must not overlap audio OUT double buffer");
_Static_assert(USB_AUDIO_FEEDBACK_PMA1 >= (USB_AUDIO_FEEDBACK_PMA + AUDIO_FEEDBACK_PACKET),
               "USB feedback banks must not overlap");
_Static_assert(USB_AUDIO_CAPTURE_PMA0 >= (USB_AUDIO_FEEDBACK_PMA1 + AUDIO_FEEDBACK_PACKET),
               "USB capture PMA must not overlap feedback");
_Static_assert((USB_AUDIO_CAPTURE_PMA1 + AUDIO_CAPTURE_PACKET) <= 0x400U,
               "USB audio PMA allocation exceeds STM32 FS PMA");
#define USB_CAT_NOTIFY_PMA              USB_PMA_ALIGN8(USB_AUDIO_CAPTURE_PMA1 + AUDIO_CAPTURE_PACKET)
#define USB_CAT_OUT_PMA(p_)             (USB_CAT_NOTIFY_PMA + USBD_CAT_NOTIFY_PACKET + \
                                         (p_) * (USBD_CAT_OUT_PACKET + USBD_CAT_IN_PACKET))
#define USB_CAT_IN_PMA(p_)              (USB_CAT_OUT_PMA(p_) + USBD_CAT_OUT_PACKET)
_Static_assert((USB_CAT_IN_PMA(USBD_CAT_PORTS - 1U) + USBD_CAT_IN_PACKET) <= 0x400U,
               "USB CAT ports exceed STM32 FS PMA");
_Static_assert((USBD_CAT_OUT_PACKET % 8U) == 0U && (USBD_CAT_IN_PACKET % 8U) == 0U,
               "CAT port buffers keep the 8-byte PMA alignment");

/* USER CODE END PV */

PCD_HandleTypeDef hpcd_USB_FS;
void Error_Handler(void);

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* Exported function prototypes ----------------------------------------------*/

/* USER CODE BEGIN PFP */
/* Private function prototypes -----------------------------------------------*/

/* USER CODE END PFP */

/* Private functions ---------------------------------------------------------*/
static USBD_StatusTypeDef USBD_Get_USB_Status(HAL_StatusTypeDef hal_status);
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/*******************************************************************************
                       LL Driver Callbacks (PCD -> USB Device Library)
*******************************************************************************/
/* MSP Init */

#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void HAL_PCD_MspInit(PCD_HandleTypeDef* pcdHandle)
#else
void HAL_PCD_MspInit(PCD_HandleTypeDef* pcdHandle)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  if(pcdHandle->Instance==USB)
  {
  /* USER CODE BEGIN USB_MspInit 0 */

  /* USER CODE END USB_MspInit 0 */

  /** The 48 MHz clock from HSI48 (on since SystemClock_Config): the one
   * CCIPR field HAL_RCCEx_PeriphCLKConfig would write for RCC_PERIPHCLK_USB,
   * without linking its every-peripheral body.
  */
    __HAL_RCC_USB_CONFIG(RCC_USBCLKSOURCE_HSI48);

    /* Peripheral clock enable */
    __HAL_RCC_USB_CLK_ENABLE();

    /* Peripheral interrupt init */
    HAL_NVIC_SetPriority(USB_LP_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(USB_LP_IRQn);
  /* USER CODE BEGIN USB_MspInit 1 */

  /* USER CODE END USB_MspInit 1 */
  }
}

#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void HAL_PCD_MspDeInit(PCD_HandleTypeDef* pcdHandle)
#else
void HAL_PCD_MspDeInit(PCD_HandleTypeDef* pcdHandle)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  if(pcdHandle->Instance==USB)
  {
  /* USER CODE BEGIN USB_MspDeInit 0 */

  /* USER CODE END USB_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USB_CLK_DISABLE();

    /* Peripheral interrupt Deinit*/
    HAL_NVIC_DisableIRQ(USB_LP_IRQn);

  /* USER CODE BEGIN USB_MspDeInit 1 */

  /* USER CODE END USB_MspDeInit 1 */
  }
}

/**
  * @brief  Setup stage callback
  * @param  hpcd: PCD handle
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd)
#else
void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_SetupStageCallback_PreTreatment */

  /* USER CODE END  HAL_PCD_SetupStageCallback_PreTreatment */
  USBD_LL_SetupStage((USBD_HandleTypeDef*)hpcd->pData, (uint8_t *)hpcd->Setup);
  /* USER CODE BEGIN HAL_PCD_SetupStageCallback_PostTreatment */

  /* USER CODE END  HAL_PCD_SetupStageCallback_PostTreatment */
}

/**
  * @brief  Data Out stage callback.
  * @param  hpcd: PCD handle
  * @param  epnum: Endpoint number
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
#else
void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_DataOutStageCallback_PreTreatment */

  /* USER CODE END HAL_PCD_DataOutStageCallback_PreTreatment */
  USBD_LL_DataOutStage((USBD_HandleTypeDef*)hpcd->pData, epnum, hpcd->OUT_ep[epnum].xfer_buff);
  /* USER CODE BEGIN HAL_PCD_DataOutStageCallback_PostTreatment */

  /* USER CODE END HAL_PCD_DataOutStageCallback_PostTreatment */
}

/**
  * @brief  Data In stage callback.
  * @param  hpcd: PCD handle
  * @param  epnum: Endpoint number
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
#else
void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_DataInStageCallback_PreTreatment */

  /* USER CODE END HAL_PCD_DataInStageCallback_PreTreatment */
  USBD_LL_DataInStage((USBD_HandleTypeDef*)hpcd->pData, epnum, hpcd->IN_ep[epnum].xfer_buff);
  /* USER CODE BEGIN HAL_PCD_DataInStageCallback_PostTreatment  */

  /* USER CODE END HAL_PCD_DataInStageCallback_PostTreatment */
}

/**
  * @brief  SOF callback.
  * @param  hpcd: PCD handle
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_SOFCallback(PCD_HandleTypeDef *hpcd)
#else
void HAL_PCD_SOFCallback(PCD_HandleTypeDef *hpcd)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_SOFCallback_PreTreatment */
  RADIO_RefSofTick((uint16_t)(hpcd->Instance->FNR & USB_FNR_FN));
  /* USER CODE END HAL_PCD_SOFCallback_PreTreatment */
  if (USB_DeviceCurrentMode() == USB_DEVICE_MODE_AUDIO) {
    MOD_UsbSofTick((uint16_t)(hpcd->Instance->FNR & USB_FNR_FN));
  }
  /* Update loss tracking and queue feedback before the class SOF callback can
   * submit this frame's explicit feedback packet. */
  USBD_LL_SOF((USBD_HandleTypeDef*)hpcd->pData);
  /* USER CODE BEGIN HAL_PCD_SOFCallback_PostTreatment */

  /* USER CODE END HAL_PCD_SOFCallback_PostTreatment */
}

/**
  * @brief  Reset callback.
  * @param  hpcd: PCD handle
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_ResetCallback(PCD_HandleTypeDef *hpcd)
#else
void HAL_PCD_ResetCallback(PCD_HandleTypeDef *hpcd)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_ResetCallback_PreTreatment */

  /* USER CODE END HAL_PCD_ResetCallback_PreTreatment */
  USBD_SpeedTypeDef speed = USBD_SPEED_FULL;

  if (hpcd->Init.speed != PCD_SPEED_FULL)
  {
    /* This callback runs inside USB_LP_IRQHandler. Never enter the blocking
     * global Error_Handler from an interrupt; record a hard pipeline fault and
     * leave the device unconfigured instead. */
    MOD_RecordUsbHardwareFault(0U, 0xFFFFU);
    return;
  }
    /* Set Speed. */
  USBD_LL_SetSpeed((USBD_HandleTypeDef*)hpcd->pData, speed);

  /* Reset Device. */
  USBD_LL_Reset((USBD_HandleTypeDef*)hpcd->pData);
  if (USB_DeviceCurrentMode() == USB_DEVICE_MODE_AUDIO) {
    MOD_UsbStreamReset();
  }
  /* USER CODE BEGIN HAL_PCD_ResetCallback_PostTreatment */
  RADIO_DigiUsbReset();
  /* USER CODE END HAL_PCD_ResetCallback_PostTreatment */
}

/**
  * @brief  Suspend callback.
  * When Low power mode is enabled the debug cannot be used (IAR, Keil doesn't support it)
  * @param  hpcd: PCD handle
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_SuspendCallback(PCD_HandleTypeDef *hpcd)
#else
void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *hpcd)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_SuspendCallback_PreTreatment */

  /* USER CODE END HAL_PCD_SuspendCallback_PreTreatment */
  /* Inform USB library that core enters in suspend Mode. */
  USBD_LL_Suspend((USBD_HandleTypeDef*)hpcd->pData);
  /* USB low-power STOP mode is intentionally disabled at initialization.
   * Entering it here would make the resume IRQ reconfigure clocks. */
  /* USER CODE BEGIN HAL_PCD_SuspendCallback_PostTreatment */

  /* USER CODE END HAL_PCD_SuspendCallback_PostTreatment */
}

/**
  * @brief  Resume callback.
  * When Low power mode is enabled the debug cannot be used (IAR, Keil doesn't support it)
  * @param  hpcd: PCD handle
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_ResumeCallback(PCD_HandleTypeDef *hpcd)
#else
void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *hpcd)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_ResumeCallback_PreTreatment */

  /* USER CODE END HAL_PCD_ResumeCallback_PreTreatment */

  USBD_LL_Resume((USBD_HandleTypeDef*)hpcd->pData);
  /* USER CODE BEGIN HAL_PCD_ResumeCallback_PostTreatment */

  /* USER CODE END HAL_PCD_ResumeCallback_PostTreatment */
}

/**
  * @brief  ISOOUTIncomplete callback.
  * @param  hpcd: PCD handle
  * @param  epnum: Endpoint number
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
#else
void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_ISOOUTIncompleteCallback_PreTreatment */

  /* USER CODE END HAL_PCD_ISOOUTIncompleteCallback_PreTreatment */
  USBD_LL_IsoOUTIncomplete((USBD_HandleTypeDef*)hpcd->pData, epnum);
  /* USER CODE BEGIN HAL_PCD_ISOOUTIncompleteCallback_PostTreatment */

  /* USER CODE END HAL_PCD_ISOOUTIncompleteCallback_PostTreatment */
}

/**
  * @brief  ISOINIncomplete callback.
  * @param  hpcd: PCD handle
  * @param  epnum: Endpoint number
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
#else
void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_ISOINIncompleteCallback_PreTreatment */

  /* USER CODE END HAL_PCD_ISOINIncompleteCallback_PreTreatment */
  USBD_LL_IsoINIncomplete((USBD_HandleTypeDef*)hpcd->pData, epnum);
  /* USER CODE BEGIN HAL_PCD_ISOINIncompleteCallback_PostTreatment */

  /* USER CODE END HAL_PCD_ISOINIncompleteCallback_PostTreatment */
}

/**
  * @brief  Connect callback.
  * @param  hpcd: PCD handle
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_ConnectCallback(PCD_HandleTypeDef *hpcd)
#else
void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *hpcd)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_ConnectCallback_PreTreatment */

  /* USER CODE END HAL_PCD_ConnectCallback_PreTreatment */
  USBD_LL_DevConnected((USBD_HandleTypeDef*)hpcd->pData);
  if (USB_DeviceCurrentMode() == USB_DEVICE_MODE_AUDIO) {
    MOD_UsbStreamReset();
  }
  /* USER CODE BEGIN HAL_PCD_ConnectCallback_PostTreatment */

  /* USER CODE END HAL_PCD_ConnectCallback_PostTreatment */
}

/**
  * @brief  Disconnect callback.
  * @param  hpcd: PCD handle
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCD_DisconnectCallback(PCD_HandleTypeDef *hpcd)
#else
void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *hpcd)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN HAL_PCD_DisconnectCallback_PreTreatment */

  /* USER CODE END HAL_PCD_DisconnectCallback_PreTreatment */
  USBD_LL_DevDisconnected((USBD_HandleTypeDef*)hpcd->pData);
  if (USB_DeviceCurrentMode() == USB_DEVICE_MODE_AUDIO) {
    MOD_UsbStreamReset();
  }
  /* USER CODE BEGIN HAL_PCD_DisconnectCallback_PostTreatment */

  /* USER CODE END HAL_PCD_DisconnectCallback_PostTreatment */
}

  /* USER CODE BEGIN LowLevelInterface */

  /* USER CODE END LowLevelInterface */

/*******************************************************************************
                       LL Driver Interface (USB Device Library --> PCD)
*******************************************************************************/

/**
  * @brief  Initializes the low level portion of the device driver.
  * @param  pdev: Device handle
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *pdev)
{
  /* Init USB Ip. */
  hpcd_USB_FS.pData = pdev;
  /* Link the driver to the stack. */
  pdev->pData = &hpcd_USB_FS;

  hpcd_USB_FS.Instance = USB;
  hpcd_USB_FS.Init.dev_endpoints = 8;
  hpcd_USB_FS.Init.speed = PCD_SPEED_FULL;
  hpcd_USB_FS.Init.phy_itface = PCD_PHY_EMBEDDED;
  hpcd_USB_FS.Init.Sof_enable = ENABLE;
  hpcd_USB_FS.Init.low_power_enable = DISABLE;
  hpcd_USB_FS.Init.lpm_enable = DISABLE;
  hpcd_USB_FS.Init.battery_charging_enable = DISABLE;

  #if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
  /* register Msp Callbacks (before the Init) */
  HAL_PCD_RegisterCallback(&hpcd_USB_FS, HAL_PCD_MSPINIT_CB_ID, PCD_MspInit);
  HAL_PCD_RegisterCallback(&hpcd_USB_FS, HAL_PCD_MSPDEINIT_CB_ID, PCD_MspDeInit);
  #endif /* USE_HAL_PCD_REGISTER_CALLBACKS */

  if (HAL_PCD_Init(&hpcd_USB_FS) != HAL_OK)
  {
    Error_Handler( );
  }

#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
  /* Register USB PCD CallBacks */
  HAL_PCD_RegisterCallback(&hpcd_USB_FS, HAL_PCD_SOF_CB_ID, PCD_SOFCallback);
  HAL_PCD_RegisterCallback(&hpcd_USB_FS, HAL_PCD_SETUPSTAGE_CB_ID, PCD_SetupStageCallback);
  HAL_PCD_RegisterCallback(&hpcd_USB_FS, HAL_PCD_RESET_CB_ID, PCD_ResetCallback);
  HAL_PCD_RegisterCallback(&hpcd_USB_FS, HAL_PCD_SUSPEND_CB_ID, PCD_SuspendCallback);
  HAL_PCD_RegisterCallback(&hpcd_USB_FS, HAL_PCD_RESUME_CB_ID, PCD_ResumeCallback);
  HAL_PCD_RegisterCallback(&hpcd_USB_FS, HAL_PCD_CONNECT_CB_ID, PCD_ConnectCallback);
  HAL_PCD_RegisterCallback(&hpcd_USB_FS, HAL_PCD_DISCONNECT_CB_ID, PCD_DisconnectCallback);
  /* USER CODE BEGIN RegisterCallBackFirstPart */

  /* USER CODE END RegisterCallBackFirstPart */
  HAL_PCD_RegisterLpmCallback(&hpcd_USB_FS, PCDEx_LPM_Callback);
  HAL_PCD_RegisterDataOutStageCallback(&hpcd_USB_FS, PCD_DataOutStageCallback);
  HAL_PCD_RegisterDataInStageCallback(&hpcd_USB_FS, PCD_DataInStageCallback);
  HAL_PCD_RegisterIsoOutIncpltCallback(&hpcd_USB_FS, PCD_ISOOUTIncompleteCallback);
  HAL_PCD_RegisterIsoInIncpltCallback(&hpcd_USB_FS, PCD_ISOINIncompleteCallback);
  /* USER CODE BEGIN RegisterCallBackSecondPart */

  /* USER CODE END RegisterCallBackSecondPart */
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
  /* USER CODE BEGIN EndPoint_Configuration */
  HAL_PCDEx_PMAConfig((PCD_HandleTypeDef*)pdev->pData, 0x00, PCD_SNG_BUF, USB_EP0_OUT_PMA);
  HAL_PCDEx_PMAConfig((PCD_HandleTypeDef*)pdev->pData, 0x80, PCD_SNG_BUF, USB_EP0_IN_PMA);
  /* USER CODE END EndPoint_Configuration */
  /* USER CODE BEGIN EndPoint_Configuration_AUDIO */
  if (USB_DeviceCurrentMode() == USB_DEVICE_MODE_MSC) {
    HAL_PCDEx_PMAConfig((PCD_HandleTypeDef*)pdev->pData, MSC_EPIN_ADDR, PCD_SNG_BUF, 0xC0);
    HAL_PCDEx_PMAConfig((PCD_HandleTypeDef*)pdev->pData, MSC_EPOUT_ADDR, PCD_SNG_BUF, 0x100);
  } else if (USB_DeviceCurrentMode() == USB_DEVICE_MODE_DIAG) {
    /* Exclusive with audio and mass storage, so it reuses the same packet
     * memory: two 64-byte bulk buffers and an 8-byte notification buffer the
     * console never writes. */
    HAL_PCDEx_PMAConfig((PCD_HandleTypeDef*)pdev->pData, CDC_MIN_IN_EP, PCD_SNG_BUF, 0xC0);
    HAL_PCDEx_PMAConfig((PCD_HandleTypeDef*)pdev->pData, CDC_MIN_OUT_EP, PCD_SNG_BUF, 0x100);
    HAL_PCDEx_PMAConfig((PCD_HandleTypeDef*)pdev->pData, CDC_MIN_CMD_EP, PCD_SNG_BUF, 0x140);
  } else {
    HAL_PCDEx_PMAConfig((PCD_HandleTypeDef*)pdev->pData, AUDIO_OUT_EP, PCD_DBL_BUF, USB_AUDIO_OUT_DBL_PMA);
    /* Isochronous endpoints alternate between two banks whatever the
     * configuration. Configured single, the feedback EP's second bank was
     * its unused RX descriptor, and every other poll sent whatever that
     * held; both banks now carry the value (USBD_LL_LoadIsoInBoth). */
    HAL_PCDEx_PMAConfig((PCD_HandleTypeDef*)pdev->pData, AUDIO_FEEDBACK_EP, PCD_DBL_BUF,
                        USB_AUDIO_FEEDBACK_DBL_PMA);
    /* Legacy USB FS isochronous endpoints alternate PMA banks by frame. A
     * single capture bank therefore supplies only every other 1 ms host
     * transaction (24 ksps). Keep both 196-byte banks populated just as the
     * playback endpoint does. */
    HAL_PCDEx_PMAConfig((PCD_HandleTypeDef*)pdev->pData, AUDIO_CAPTURE_EP,
                        PCD_DBL_BUF, USB_AUDIO_CAPTURE_DBL_PMA);
    for (uint8_t p = 0U; p < USBD_CAT_PORTS; ++p) {
      HAL_PCDEx_PMAConfig((PCD_HandleTypeDef*)pdev->pData, USBD_CAT_NOTIFY_EP(p), PCD_SNG_BUF, USB_CAT_NOTIFY_PMA);
      HAL_PCDEx_PMAConfig((PCD_HandleTypeDef*)pdev->pData, USBD_CAT_OUT_EP(p), PCD_SNG_BUF, USB_CAT_OUT_PMA(p));
      HAL_PCDEx_PMAConfig((PCD_HandleTypeDef*)pdev->pData, USBD_CAT_IN_EP(p), PCD_SNG_BUF, USB_CAT_IN_PMA(p));
    }
  }
  /* USER CODE END EndPoint_Configuration_AUDIO */
  return USBD_OK;
}

/**
  * @brief  De-Initializes the low level portion of the device driver.
  * @param  pdev: Device handle
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef *pdev)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_DeInit(pdev->pData);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Starts the low level portion of the device driver.
  * @param  pdev: Device handle
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef *pdev)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_Start(pdev->pData);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Stops the low level portion of the device driver.
  * @param  pdev: Device handle
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef *pdev)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_Stop(pdev->pData);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Opens an endpoint of the low level driver.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @param  ep_type: Endpoint type
  * @param  ep_mps: Endpoint max packet size
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr, uint8_t ep_type, uint16_t ep_mps)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_EP_Open(pdev->pData, ep_addr, ep_mps, ep_type);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Closes an endpoint of the low level driver.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_EP_Close(pdev->pData, ep_addr);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Flushes an endpoint of the Low Level Driver.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_EP_Flush(pdev->pData, ep_addr);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Sets a Stall condition on an endpoint of the Low Level Driver.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_EP_SetStall(pdev->pData, ep_addr);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Clears a Stall condition on an endpoint of the Low Level Driver.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_EP_ClrStall(pdev->pData, ep_addr);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Returns Stall condition.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @retval Stall (1: Yes, 0: No)
  */
uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  PCD_HandleTypeDef *hpcd = (PCD_HandleTypeDef*) pdev->pData;

  if((ep_addr & 0x80) == 0x80)
  {
    return hpcd->IN_ep[ep_addr & 0x7F].is_stall;
  }
  else
  {
    return hpcd->OUT_ep[ep_addr & 0x7F].is_stall;
  }
}

/**
  * @brief  Assigns a USB address to the device.
  * @param  pdev: Device handle
  * @param  dev_addr: Device address
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *pdev, uint8_t dev_addr)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_SetAddress(pdev->pData, dev_addr);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Transmits data over an endpoint.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @param  pbuf: Pointer to data to be sent
  * @param  size: Data size
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *pdev, uint8_t ep_addr, uint8_t *pbuf, uint32_t size)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_EP_Transmit(pdev->pData, ep_addr, pbuf, size);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

uint8_t USBD_LL_RefillIsoIn(void *pcd_data, uint8_t ep_addr,
                            uint8_t *pbuf, uint16_t size)
{
  PCD_HandleTypeDef *hpcd = (PCD_HandleTypeDef *)pcd_data;
  if ((hpcd == NULL) || (pbuf == NULL) || !(ep_addr & 0x80U)) return 0U;

  PCD_EPTypeDef *ep = &hpcd->IN_ep[ep_addr & EP_ADDR_MSK];
  if (!ep->doublebuffer || !ep->is_in || ep->type != EP_TYPE_ISOC ||
      size > ep->maxpacket) return 0U;

  /* RM0440 table 425: after a transaction DTOG_TX names the bank owned by
   * the peripheral; software owns and must refill the opposite bank. The HAL
   * ISO path clears that just-sent bank's count before invoking DataIn. */
  if ((PCD_GET_ENDPOINT(hpcd->Instance, ep->num) & USB_EP_DTOG_TX) != 0U)
  {
    USB_WritePMA(hpcd->Instance, pbuf, ep->pmaaddr0, size);
    PCD_SET_EP_DBUF0_CNT(hpcd->Instance, ep->num, ep->is_in, size);
  }
  else
  {
    USB_WritePMA(hpcd->Instance, pbuf, ep->pmaaddr1, size);
    PCD_SET_EP_DBUF1_CNT(hpcd->Instance, ep->num, ep->is_in, size);
  }
  PCD_SET_EP_TX_STATUS(hpcd->Instance, ep->num, USB_EP_TX_VALID);
  return 1U;
}

/* Load one packet into both banks of a double-buffered isochronous IN
 * endpoint, so whichever the peripheral sends next carries it -- for a
 * value the host polls (USB audio feedback), not a stream: a bank the host
 * has taken goes out empty until the next load (the HAL ISO path clears its
 * count), and a zero-length feedback packet is skipped by the host. */
uint8_t USBD_LL_LoadIsoInBoth(void *pcd_data, uint8_t ep_addr,
                              uint8_t *pbuf, uint16_t size)
{
  PCD_HandleTypeDef *hpcd = (PCD_HandleTypeDef *)pcd_data;
  if ((hpcd == NULL) || (pbuf == NULL) || !(ep_addr & 0x80U)) return 0U;

  PCD_EPTypeDef *ep = &hpcd->IN_ep[ep_addr & EP_ADDR_MSK];
  if (!ep->doublebuffer || !ep->is_in || ep->type != EP_TYPE_ISOC ||
      size > ep->maxpacket) return 0U;

  /* is_in is checked above: the counts are TX counts (a constant direction
   * leaves the macros' OUT block-size path out). */
  USB_WritePMA(hpcd->Instance, pbuf, ep->pmaaddr0, size);
  PCD_SET_EP_DBUF0_CNT(hpcd->Instance, ep->num, 1U, size);
  USB_WritePMA(hpcd->Instance, pbuf, ep->pmaaddr1, size);
  PCD_SET_EP_DBUF1_CNT(hpcd->Instance, ep->num, 1U, size);
  PCD_SET_EP_TX_STATUS(hpcd->Instance, ep->num, USB_EP_TX_VALID);
  return 1U;
}

/**
  * @brief  Prepares an endpoint for reception.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @param  pbuf: Pointer to data to be received
  * @param  size: Data size
  * @retval USBD status
  */
USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *pdev, uint8_t ep_addr, uint8_t *pbuf, uint32_t size)
{
  HAL_StatusTypeDef hal_status = HAL_OK;
  USBD_StatusTypeDef usb_status = USBD_OK;

  hal_status = HAL_PCD_EP_Receive(pdev->pData, ep_addr, pbuf, size);

  usb_status =  USBD_Get_USB_Status(hal_status);

  return usb_status;
}

/**
  * @brief  Returns the last transferred packet size.
  * @param  pdev: Device handle
  * @param  ep_addr: Endpoint number
  * @retval Received Data Size
  */
uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
  return HAL_PCD_EP_GetRxCount((PCD_HandleTypeDef*) pdev->pData, ep_addr);
}

/**
  * @brief  Send LPM message to user layer
  * @param  hpcd: PCD handle
  * @param  msg: LPM message
  * @retval None
  */
#if (USE_HAL_PCD_REGISTER_CALLBACKS == 1U)
static void PCDEx_LPM_Callback(PCD_HandleTypeDef *hpcd, PCD_LPM_MsgTypeDef msg)
#else
void HAL_PCDEx_LPM_Callback(PCD_HandleTypeDef *hpcd, PCD_LPM_MsgTypeDef msg)
#endif /* USE_HAL_PCD_REGISTER_CALLBACKS */
{
  /* USER CODE BEGIN LPM_Callback */
  switch (msg)
  {
  case PCD_LPM_L0_ACTIVE:
    USBD_LL_Resume(hpcd->pData);
    break;

  case PCD_LPM_L1_ACTIVE:
    USBD_LL_Suspend(hpcd->pData);

    break;
  }
  /* USER CODE END LPM_Callback */
}

/**
  * @brief  Delays routine for the USB Device Library.
  * @param  Delay: Delay in ms
  * @retval None
  */
void USBD_LL_Delay(uint32_t Delay)
{
  HAL_Delay(Delay);
}

/**
  * @brief  Static single allocation.
  * @param  size: Size of allocated memory
  * @retval None
  */
void *USBD_static_malloc(uint32_t size)
{
  /* One buffer shared by whichever personality is enumerated; they are
     mutually exclusive, so it is sized to the largest of the three. */
#define USBD_CLASS_MAX2(a, b) ((a) > (b) ? (a) : (b))
  static uint32_t mem[USBD_CLASS_MAX2(USBD_CLASS_MAX2(sizeof(USBD_AUDIO_HandleTypeDef),
                                                      sizeof(USBD_MSC_BOT_HandleTypeDef)),
                                      CDC_MIN_HANDLE_SIZE) / 4U + 1U];
  UNUSED(size);
  return mem;
}

/**
  * @brief  Dummy memory free
  * @param  p: Pointer to allocated  memory address
  * @retval None
  */
void USBD_static_free(void *p)
{

}

/**
  * @brief  Returns the USB status depending on the HAL status:
  * @param  hal_status: HAL status
  * @retval USB status
  */
USBD_StatusTypeDef USBD_Get_USB_Status(HAL_StatusTypeDef hal_status)
{
  USBD_StatusTypeDef usb_status = USBD_OK;

  switch (hal_status)
  {
    case HAL_OK :
      usb_status = USBD_OK;
    break;
    case HAL_ERROR :
      usb_status = USBD_FAIL;
    break;
    case HAL_BUSY :
      usb_status = USBD_BUSY;
    break;
    case HAL_TIMEOUT :
      usb_status = USBD_FAIL;
    break;
    default :
      usb_status = USBD_FAIL;
    break;
  }
  return usb_status;
}
