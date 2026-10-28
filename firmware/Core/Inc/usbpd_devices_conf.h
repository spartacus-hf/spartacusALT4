/* SPARTACUS REV1: fixed sink on PB4/PB6; no FRS/VCONN. CC termination is a
 * setting: external R3/R4 Rd (UCPD pulls off) or the UCPD internal Rd. */
#ifndef USBPD_DEVICE_CONF_H
#define USBPD_DEVICE_CONF_H
#include "stm32g4xx_hal.h"
#include "stm32g4xx_ll_bus.h"
#include "stm32g4xx_ll_dma.h"
#include "stm32g4xx_ll_gpio.h"
#include "stm32g4xx_ll_ucpd.h"
#include "stm32g4xx_ll_rcc.h"
#include "stm32g4xx_ll_pwr.h"
#include "usbpd_pwr_if.h"
#define UCPD_INSTANCE0 UCPD1
#define UCPDDMA_INSTANCE0_CLOCKENABLE_RX do { __HAL_RCC_DMA1_CLK_ENABLE(); __HAL_RCC_DMAMUX1_CLK_ENABLE(); } while (0)
#define UCPDDMA_INSTANCE0_CLOCKENABLE_TX UCPDDMA_INSTANCE0_CLOCKENABLE_RX
#define UCPDDMA_INSTANCE0_DMA_RX DMA1
#define UCPDDMA_INSTANCE0_DMA_TX DMA1
#define UCPDDMA_INSTANCE0_REQUEST_RX DMA_REQUEST_UCPD1_RX
#define UCPDDMA_INSTANCE0_REQUEST_TX DMA_REQUEST_UCPD1_TX
#define UCPDDMA_INSTANCE0_LL_CHANNEL_RX LL_DMA_CHANNEL_5
#define UCPDDMA_INSTANCE0_LL_CHANNEL_TX LL_DMA_CHANNEL_6
#define UCPDDMA_INSTANCE0_CHANNEL_RX DMA1_Channel5
#define UCPDDMA_INSTANCE0_CHANNEL_TX DMA1_Channel6
#define UCPDFRS_INSTANCE0_FRSCC1 do { } while (0)
#define UCPDFRS_INSTANCE0_FRSCC2 UCPDFRS_INSTANCE0_FRSCC1
/* Priority 1, level with the RF/audio DMA and USB: it cannot preempt them,
 * and they cannot preempt it. The handler is short, but it is where ST's
 * protocol layer sends GoodCRC, which the partner expects within tReceive
 * (0.9-1.1 ms); at 2 it queued behind a whole PendSV receive-DSP pass, so a
 * busy receiver could make the source retry and escalate to a soft or hard
 * reset -- and a hard reset takes VBUS, the MCU's supply, away. The PD
 * timers and policy engine stay on SysTick (3). */
#define UCPD_INSTANCE0_ENABLEIRQ do { NVIC_SetPriority(UCPD1_IRQn, 1U); NVIC_EnableIRQ(UCPD1_IRQn); } while (0)
/* Returns ST's CCENABLE value with internal Rd, or none with external Rd. */
uint32_t REV1_UcpdCcEnable(uint32_t st_value);
#endif
