/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : usbd_audio_if.c
  * @version        : v3.0_Cube
  * @brief          : Generic media access layer.
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
#include "usbd_audio_if.h"
#include "radio_audio.h"

/* USER CODE BEGIN INCLUDE */
#include "modulator.h"
/* USER CODE END INCLUDE */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* Private variables ---------------------------------------------------------*/

/* USER CODE END PV */

/** @addtogroup STM32_USB_OTG_DEVICE_LIBRARY
  * @brief Usb device library.
  * @{
  */

/** @addtogroup USBD_AUDIO_IF
  * @{
  */

/** @defgroup USBD_AUDIO_IF_Private_TypesDefinitions USBD_AUDIO_IF_Private_TypesDefinitions
  * @brief Private types.
  * @{
  */

/* USER CODE BEGIN PRIVATE_TYPES */

/* USER CODE END PRIVATE_TYPES */

/**
  * @}
  */

/** @defgroup USBD_AUDIO_IF_Private_Defines USBD_AUDIO_IF_Private_Defines
  * @brief Private defines.
  * @{
  */

/* USER CODE BEGIN PRIVATE_DEFINES */

/* USER CODE END PRIVATE_DEFINES */

/**
  * @}
  */

/** @defgroup USBD_AUDIO_IF_Private_Macros USBD_AUDIO_IF_Private_Macros
  * @brief Private macros.
  * @{
  */

/* USER CODE BEGIN PRIVATE_MACRO */

/* USER CODE END PRIVATE_MACRO */

/**
  * @}
  */

/** @defgroup USBD_AUDIO_IF_Private_Variables USBD_AUDIO_IF_Private_Variables
  * @brief Private variables.
  * @{
  */

/* USER CODE BEGIN PRIVATE_VARIABLES */
#define USB_AUDIO_MAX_MONO_SAMPLES (AUDIO_OUT_PACKET / AUDIO_OUT_BYTES_PER_FRAME)
static int16_t mono_buf[USB_AUDIO_MAX_MONO_SAMPLES];

static inline int16_t usb_audio_sat16(int32_t v)
{
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

static inline int16_t usb_audio_s16le(const uint8_t *p)
{
  uint16_t u = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
  return (int16_t)u;
}

static inline uint32_t usb_audio_abs16(int16_t v)
{
  return (v < 0) ? (uint32_t)(-(int32_t)v) : (uint32_t)v;
}

static inline int16_t usb_audio_average_or_stronger(int16_t l, int16_t r)
{
  uint32_t al = usb_audio_abs16(l);
  uint32_t ar = usb_audio_abs16(r);
  uint32_t strong = (al > ar) ? al : ar;
  uint32_t weak = (al > ar) ? ar : al;
  int16_t avg = usb_audio_sat16(((int32_t)l + (int32_t)r) / 2);

  if ((strong > 512U) && ((weak * 8U) < strong)) {
    return (al >= ar) ? l : r;
  }
  if ((strong > 512U) && ((l < 0) != (r < 0)) &&
      ((usb_audio_abs16(avg) * 8U) < strong)) {
    return (al >= ar) ? l : r;
  }
  return avg;
}
/* USER CODE END PRIVATE_VARIABLES */

/**
  * @}
  */

/** @defgroup USBD_AUDIO_IF_Exported_Variables USBD_AUDIO_IF_Exported_Variables
  * @brief Public variables.
  * @{
  */

extern USBD_HandleTypeDef hUsbDeviceFS;

/* USER CODE BEGIN EXPORTED_VARIABLES */

/* USER CODE END EXPORTED_VARIABLES */

/**
  * @}
  */

/** @defgroup USBD_AUDIO_IF_Private_FunctionPrototypes USBD_AUDIO_IF_Private_FunctionPrototypes
  * @brief Private functions declaration.
  * @{
  */

static int8_t AUDIO_Init_FS(uint32_t AudioFreq, uint32_t Volume, uint32_t options);
static int8_t AUDIO_DeInit_FS(uint32_t options);
static int8_t AUDIO_AudioCmd_FS(uint8_t* pbuf, uint32_t size, uint8_t cmd);
static int8_t AUDIO_VolumeCtl_FS(uint8_t vol);
static int8_t AUDIO_MuteCtl_FS(uint8_t cmd);
static int8_t AUDIO_PeriodicTC_FS(uint8_t *pbuf, uint32_t size, uint8_t cmd);
static int8_t AUDIO_GetState_FS(void);
static uint32_t AUDIO_GetFeedback10_14_FS(void);
static void AUDIO_Notify_FS(uint8_t event, uint32_t value);

/* USER CODE BEGIN PRIVATE_FUNCTIONS_DECLARATION */

/* USER CODE END PRIVATE_FUNCTIONS_DECLARATION */

/**
  * @}
  */

const USBD_AUDIO_ItfTypeDef USBD_AUDIO_fops_FS =
{
  AUDIO_Init_FS,
  AUDIO_DeInit_FS,
  AUDIO_AudioCmd_FS,
  AUDIO_VolumeCtl_FS,
  AUDIO_MuteCtl_FS,
  AUDIO_PeriodicTC_FS,
  AUDIO_GetState_FS,
  AUDIO_GetFeedback10_14_FS,
  AUDIO_Notify_FS,
  RADIO_CapturePacket,
};

/* Private functions ---------------------------------------------------------*/
/**
  * @brief  Initializes the AUDIO media low layer over USB FS IP
  * @param  AudioFreq: Audio frequency used to play the audio stream.
  * @param  Volume: Initial volume level (from 0 (Mute) to 100 (Max))
  * @param  options: Reserved for future use
  * @retval USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t AUDIO_Init_FS(uint32_t AudioFreq, uint32_t Volume, uint32_t options)
{
  /* USER CODE BEGIN 0 */
  UNUSED(AudioFreq);
  UNUSED(Volume);
  UNUSED(options);
  MOD_UsbStreamReset();
  return (USBD_OK);
  /* USER CODE END 0 */
}

/**
  * @brief  De-Initializes the AUDIO media low layer
  * @param  options: Reserved for future use
  * @retval USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t AUDIO_DeInit_FS(uint32_t options)
{
  /* USER CODE BEGIN 1 */
  UNUSED(options);
  MOD_UsbStreamReset();
  return (USBD_OK);
  /* USER CODE END 1 */
}

/**
  * @brief  Handles AUDIO command.
  * @param  pbuf: Pointer to buffer of data to be sent
  * @param  size: Number of data to be sent (in bytes)
  * @param  cmd: Command opcode
  * @retval USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t AUDIO_AudioCmd_FS(uint8_t* pbuf, uint32_t size, uint8_t cmd)
{
  /* USER CODE BEGIN 2 */
  /* Intentionally empty — this path is dead on ST USB Audio middleware.
   * The SOF callback is a stub that never calls Sync, so AudioCmd
   * only fires once.  All real audio processing is in PeriodicTC. */
  (void)pbuf;
  (void)size;
  if ((cmd == AUDIO_CMD_START) || (cmd == AUDIO_CMD_STOP))
  {
    MOD_UsbStreamReset();
  }
  return (USBD_OK);
  /* USER CODE END 2 */
}

/**
  * @brief  Controls AUDIO Volume.
  * @param  vol: volume level (0..100)
  * @retval USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t AUDIO_VolumeCtl_FS(uint8_t vol)
{
  /* USER CODE BEGIN 3 */
  UNUSED(vol);
  return (USBD_OK);
  /* USER CODE END 3 */
}

/**
  * @brief  Controls AUDIO Mute.
  * @param  cmd: command opcode
  * @retval USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t AUDIO_MuteCtl_FS(uint8_t cmd)
{
  /* USER CODE BEGIN 4 */
  UNUSED(cmd);
  return (USBD_OK);
  /* USER CODE END 4 */
}

/**
  * @brief  AUDIO_PeriodicT_FS
  * @param  cmd: Command opcode
  * @retval USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t AUDIO_PeriodicTC_FS(uint8_t *pbuf, uint32_t size, uint8_t cmd)
{
  /* USER CODE BEGIN 5 */

  (void)cmd;  /* unused */

  /* USB Audio sends stereo S16_LE:
   *   [L0_lo, L0_hi, R0_lo, R0_hi, L1_lo, L1_hi, R1_lo, R1_hi, ...]
   * Each stereo frame = 4 bytes, so sample_count = size / 4.
   * Exact packet sizes are accepted; LEFT / RIGHT / average mono extraction
   * does not pad packets to fixed 1 ms boundaries.
   */
  uint16_t usb_frame_number = (uint16_t)(USB->FNR & USB_FNR_FN);
  if (pbuf == NULL || size == 0U)
  {
    MOD_RecordUsbPacketAnomaly(usb_frame_number, 0U);
    return (USBD_OK);
  }

#if MOD_DEEP_DIAG_ENABLE
  MOD_CpuScopeBegin(MOD_CPU_SCOPE_USB_SUBMIT);
#endif

  if ((size % AUDIO_OUT_BYTES_PER_FRAME) != 0U)
  {
    MOD_RecordUsbPacketAnomaly(usb_frame_number, size);
#if MOD_DEEP_DIAG_ENABLE
    MOD_CpuScopeEnd(MOD_CPU_SCOPE_USB_SUBMIT);
#endif
    return (USBD_OK);
  }

  uint32_t sample_count = size / AUDIO_OUT_BYTES_PER_FRAME;
  if ((sample_count < MOD_USB_AUDIO_LEGAL_MIN_SAMPLES) ||
      (sample_count > MOD_USB_AUDIO_LEGAL_MAX_SAMPLES) ||
      (sample_count > USB_AUDIO_MAX_MONO_SAMPLES))
  {
    MOD_RecordUsbPacketAnomaly(usb_frame_number, size);
#if MOD_DEEP_DIAG_ENABLE
    MOD_CpuScopeEnd(MOD_CPU_SCOPE_USB_SUBMIT);
#endif
    return (USBD_OK);
  }

  if (sample_count > 0U)
  {
    usb_audio_channel_mode_t mode = MOD_GetUsbChannelMode();
    if (mode >= USB_AUDIO_CHAN_COUNT)
    {
      MOD_RecordUsbPacketAnomaly(usb_frame_number, size);
#if MOD_DEEP_DIAG_ENABLE
      MOD_CpuScopeEnd(MOD_CPU_SCOPE_USB_SUBMIT);
#endif
      return (USBD_OK);
    }
    if (mode == USB_AUDIO_CHAN_LEFT) {
      for (uint32_t i = 0U; i < sample_count; i++) {
        mono_buf[i] = usb_audio_s16le(&pbuf[i * AUDIO_OUT_BYTES_PER_FRAME]);
      }
    } else if (mode == USB_AUDIO_CHAN_RIGHT) {
      for (uint32_t i = 0U; i < sample_count; i++) {
        mono_buf[i] = usb_audio_s16le(&pbuf[i * AUDIO_OUT_BYTES_PER_FRAME +
                                             AUDIO_OUT_BYTES_PER_SAMPLE]);
      }
    } else if (mode == USB_AUDIO_CHAN_AVERAGE) {
      for (uint32_t i = 0U; i < sample_count; i++) {
        uint32_t base = i * AUDIO_OUT_BYTES_PER_FRAME;
        int16_t l = usb_audio_s16le(&pbuf[base]);
        int16_t r = usb_audio_s16le(&pbuf[base + AUDIO_OUT_BYTES_PER_SAMPLE]);
        mono_buf[i] = usb_audio_sat16(((int32_t)l + (int32_t)r) / 2);
      }
    } else {
      for (uint32_t i = 0U; i < sample_count; i++) {
        uint32_t base = i * AUDIO_OUT_BYTES_PER_FRAME;
        int16_t l = usb_audio_s16le(&pbuf[base]);
        int16_t r = usb_audio_s16le(&pbuf[base + AUDIO_OUT_BYTES_PER_SAMPLE]);
        mono_buf[i] = usb_audio_average_or_stronger(l, r);
      }
    }

    MOD_SubmitUsbPacket(mono_buf, sample_count, usb_frame_number);
  }

#if MOD_DEEP_DIAG_ENABLE
  MOD_CpuScopeEnd(MOD_CPU_SCOPE_USB_SUBMIT);
#endif
  return (USBD_OK);

  /* USER CODE END 5 */
}

/**
  * @brief  Gets AUDIO State.
  * @retval USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t AUDIO_GetState_FS(void)
{
  /* USER CODE BEGIN 6 */
  return (USBD_OK);
  /* USER CODE END 6 */
}

static uint32_t AUDIO_GetFeedback10_14_FS(void)
{
  return MOD_GetUsbFeedback10_14();
}

static void AUDIO_Notify_FS(uint8_t event, uint32_t value)
{
  switch (event)
  {
    case USBD_AUDIO_EVENT_ZERO_PACKET:
      MOD_RecordUsbPacketAnomaly((uint16_t)(USB->FNR & USB_FNR_FN), 0U);
      break;
    case USBD_AUDIO_EVENT_OUT_INCOMPLETE:
      MOD_RecordUsbPipelineEvent(MOD_USB_PIPE_EVENT_OUT_INCOMPLETE, value);
      break;
    case USBD_AUDIO_EVENT_REARM_FAILED:
      MOD_RecordUsbPipelineEvent(MOD_USB_PIPE_EVENT_REARM_FAILED, value);
      break;
    case USBD_AUDIO_EVENT_FEEDBACK_TX_FAILED:
      MOD_RecordUsbPipelineEvent(MOD_USB_PIPE_EVENT_FEEDBACK_TX_FAILED, value);
      break;
    case USBD_AUDIO_EVENT_STREAM_ACTIVE:
      MOD_RecordUsbPipelineEvent(MOD_USB_PIPE_EVENT_STREAM_ACTIVE,
                                 (uint32_t)(USB->FNR & USB_FNR_FN));
      break;
    case USBD_AUDIO_EVENT_STREAM_INACTIVE:
      MOD_RecordUsbPipelineEvent(MOD_USB_PIPE_EVENT_STREAM_INACTIVE, value);
      break;
    default:
      MOD_RecordUsbPacketAnomaly((uint16_t)(USB->FNR & USB_FNR_FN), value);
      break;
  }
}

/**
  * @brief  Manages the DMA full transfer complete event.
  * @retval None
  */
void TransferComplete_CallBack_FS(void)
{
  /* USER CODE BEGIN 7 */
  USBD_AUDIO_Sync(&hUsbDeviceFS, AUDIO_OFFSET_FULL);
  /* USER CODE END 7 */
}

/**
  * @brief  Manages the DMA Half transfer complete event.
  * @retval None
  */
void HalfTransfer_CallBack_FS(void)
{
  /* USER CODE BEGIN 8 */
  USBD_AUDIO_Sync(&hUsbDeviceFS, AUDIO_OFFSET_HALF);
  /* USER CODE END 8 */
}

/* USER CODE BEGIN PRIVATE_FUNCTIONS_IMPLEMENTATION */

/* USER CODE END PRIVATE_FUNCTIONS_IMPLEMENTATION */

/**
  * @}
  */

/**
  * @}
  */
