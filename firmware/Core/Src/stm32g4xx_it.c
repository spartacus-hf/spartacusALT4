#include "radio_audio.h"
/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32g4xx_it.c
  * @brief   Interrupt Service Routines.
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
#include "main.h"
#include "radio_pd.h"
#include "stm32g4xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "modulator.h"
#include "ui_lcd.h"
#include "radio_clocks.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
__attribute__((section(".noinit"))) volatile uint32_t g_fault_magic;
__attribute__((section(".noinit"))) volatile uint32_t g_fault_reason;
__attribute__((section(".noinit"))) volatile uint32_t g_fault_cfsr;
__attribute__((section(".noinit"))) volatile uint32_t g_fault_hfsr;
__attribute__((section(".noinit"))) volatile uint32_t g_fault_mmfar;
__attribute__((section(".noinit"))) volatile uint32_t g_fault_bfar;
__attribute__((section(".noinit"))) volatile uint32_t g_fault_exc_lr;
__attribute__((section(".noinit"))) volatile uint32_t g_fault_stacked_lr;
__attribute__((section(".noinit"))) volatile uint32_t g_fault_stacked_pc;
__attribute__((section(".noinit"))) volatile uint32_t g_fault_stacked_xpsr;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
#define FAULT_MARKER_MAGIC 0xF640FA17UL

static void fault_capture_and_reset_ex(uint32_t reason, const uint32_t *stacked, uint32_t exc_lr)
{
  __disable_irq();
  g_fault_magic = FAULT_MARKER_MAGIC;
  g_fault_reason = reason;
  g_fault_cfsr = SCB->CFSR;
  g_fault_hfsr = SCB->HFSR;
  g_fault_mmfar = SCB->MMFAR;
  g_fault_bfar = SCB->BFAR;
  g_fault_exc_lr = exc_lr;
  if (stacked != NULL) {
    g_fault_stacked_lr = stacked[5];
    g_fault_stacked_pc = stacked[6];
    g_fault_stacked_xpsr = stacked[7];
  } else {
    g_fault_stacked_lr = 0U;
    g_fault_stacked_pc = 0U;
    g_fault_stacked_xpsr = 0U;
  }
  __HAL_RCC_GPIOC_CLK_ENABLE();
  BOARD_EmergencyStop();
  for (volatile uint32_t i = 0U; i < 10000U; i++) { }
  NVIC_SystemReset();
  while (1) { }
}

static void fault_capture_and_reset(uint32_t reason)
{
  fault_capture_and_reset_ex(reason, NULL, 0U);
}

void HardFault_HandlerC(uint32_t *stacked, uint32_t exc_lr)
{
  fault_capture_and_reset_ex(2U, stacked, exc_lr);
}

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern PCD_HandleTypeDef hpcd_USB_FS;
extern HRTIM_HandleTypeDef hhrtim1;
extern DMA_HandleTypeDef hdma_tim6_up;
extern DMA_HandleTypeDef hdma_mcmp2;
/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */
  fault_capture_and_reset(1U);

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
__attribute__((naked)) void HardFault_Handler(void)
{
  __asm volatile(
    "tst lr, #4       \n"
    "ite eq           \n"
    "mrseq r0, msp    \n"
    "mrsne r0, psp    \n"
    "mov r1, lr       \n"
    "b HardFault_HandlerC \n"
  );
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */
  fault_capture_and_reset(3U);

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Prefetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */
  fault_capture_and_reset(4U);

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */
  fault_capture_and_reset(5U);

  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles System service call via SWI instruction.
  */
void SVC_Handler(void)
{
  /* USER CODE BEGIN SVCall_IRQn 0 */

  /* USER CODE END SVCall_IRQn 0 */
  /* USER CODE BEGIN SVCall_IRQn 1 */

  /* USER CODE END SVCall_IRQn 1 */
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles Pendable request for system service.
  */
void PendSV_Handler(void)
{
  /* USER CODE BEGIN PendSV_IRQn 0 */
  RADIO_AudioWorker();
  MOD_RealtimeRenderWorker();
  /* USER CODE END PendSV_IRQn 0 */
  /* USER CODE BEGIN PendSV_IRQn 1 */

  /* USER CODE END PendSV_IRQn 1 */
}

/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */

  /* USER CODE END SysTick_IRQn 0 */
  HAL_IncTick();
  /* USER CODE BEGIN SysTick_IRQn 1 */
  MOD_RealtimeHealthTick();
  /* The main loop can stay inside its RF-critical branch for many
   * milliseconds, which is long enough to miss quadrature edges. Sample the
   * encoder here instead: one read per port, no loops. */
  BOARD_EncoderTick();
  /* The CW levers too: debounced and their closures counted here, so the
   * keyer sees a tap however busy the main loop is. */
  MOD_CwInputTick();
  RADIO_PdTick();
  /* USER CODE END SysTick_IRQn 1 */
}

/******************************************************************************/
/* STM32G4xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32g4xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles DMA1 channel1 global interrupt.
  */
void DMA1_Channel1_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Channel1_IRQn 0 */

  /* USER CODE END DMA1_Channel1_IRQn 0 */
  mod_dma_irq_channel1();
  /* USER CODE BEGIN DMA1_Channel1_IRQn 1 */
  RADIO_RefSofAfterIsr();

  /* USER CODE END DMA1_Channel1_IRQn 1 */
}

/**
  * @brief This function handles DMA1 channel2 global interrupt.
  */
void DMA1_Channel2_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Channel2_IRQn 0 */

  /* USER CODE END DMA1_Channel2_IRQn 0 */
  mod_dma_irq_channel2();
  /* USER CODE BEGIN DMA1_Channel2_IRQn 1 */
  RADIO_RefSofAfterIsr();

  /* USER CODE END DMA1_Channel2_IRQn 1 */
}


/**
  * @brief This function handles DMA1 channel3 global interrupt.
  */
void DMA1_Channel3_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Channel3_IRQn 0 */

  /* USER CODE END DMA1_Channel3_IRQn 0 */
  LCDUI_DmaIrq(); /* register-level, no HAL dispatch per panel row */
  /* USER CODE BEGIN DMA1_Channel3_IRQn 1 */
  RADIO_RefSofIsrReturn();

  /* USER CODE END DMA1_Channel3_IRQn 1 */
}

/**
  * @brief This function handles HRTIM common fault interrupt.
  */
void HRTIM1_FLT_IRQHandler(void)
{
  __HAL_RCC_GPIOC_CLK_ENABLE();
  BOARD_EmergencyStop();
  MOD_RecordHrtimFault(HRTIM1->sCommonRegs.ISR);
  /* Only the six fault inputs and the system fault interrupt (main.c), and
   * the stop above is all their callbacks did: clear them here rather than
   * through HAL_HRTIM_IRQHandler's dispatch of every common flag. */
  __HAL_HRTIM_CLEAR_IT(&hhrtim1, HRTIM_IT_FLT1 | HRTIM_IT_FLT2 | HRTIM_IT_FLT3 |
                                 HRTIM_IT_FLT4 | HRTIM_IT_FLT5 | HRTIM_IT_FLT6 |
                                 HRTIM_IT_SYSFLT);
}

/**
  * @brief This function handles USB low priority interrupt remap.
  */
void USB_LP_IRQHandler(void)
{
  /* USER CODE BEGIN USB_LP_IRQn 0 */
  /* The reference's timestamp of a SOF (radio_clocks.c): taken first, before
   * the HAL services whatever else is pending, and only counted as the SOF's
   * time when the SOF was already what raised this interrupt. */
  const uint32_t sof_stamp = TIM5->CNT;
  uint16_t usb_istr = (uint16_t)hpcd_USB_FS.Instance->ISTR;
  if ((usb_istr & USB_ISTR_SOF) != 0U)
    RADIO_RefSofEntry(sof_stamp, (uint16_t)(hpcd_USB_FS.Instance->FNR & USB_FNR_FN));
  /* ERR is a recoverable USB protocol condition and ESOF is expected while
   * the host stops SOFs for disconnect/suspend; the HAL clears both below.
   * Audio packet/frame tracking reports any resulting stream discontinuity.
   * Only PMA over/underrun indicates corrupted endpoint-buffer service that
   * warrants the RF pipeline's hard hardware lockout. */
  uint16_t usb_fault = (uint16_t)(usb_istr & USB_ISTR_PMAOVR);
  if (usb_fault != 0U) {
    MOD_RecordUsbHardwareFault((uint16_t)(hpcd_USB_FS.Instance->FNR & USB_FNR_FN),
                               usb_fault);
  }
  /* USER CODE END USB_LP_IRQn 0 */
  HAL_PCD_IRQHandler(&hpcd_USB_FS);
  /* USER CODE BEGIN USB_LP_IRQn 1 */

  /* USER CODE END USB_LP_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
