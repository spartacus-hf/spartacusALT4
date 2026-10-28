#pragma GCC optimize ("Os") /* off the real-time paths: built for size */
/**
 * @file    main.c
 * @brief   STM32G474 Chireix outphasing transmitter — system init and top-level loop.
 */

#include "main.h"
#include "radio_pd.h"
#include "usb_device.h"
#include "modulator.h"
#include "hrtim_phase.h"
#include "ui_lcd.h"
#include "ui_app.h"
#include "mod_sdcard_service.h"
#include "cw_pack.h"
#include "mod_sd_store.h"
#include "sd_audio_player.h"
#include "ui_sd.h"
#include <math.h>
#include "radio.h"
#include "radio_audio.h"
#include "radio_digi.h"
#include "radio_analog.h"
#include "radio_clocks.h"
#include "clock_plan.h"
#if HSE_VALUE != CLOCK_HSE_NOMINAL_HZ
#error "HSE_VALUE (stm32g4xx_hal_conf.h) and CLOCK_HSE_NOMINAL_HZ (clock_plan.h) name the same oscillator"
#endif
#include "usb_diag.h"
#include "usb_diag_mode.h"
#include "cat.h"
#include "sctrl_link.h"
#include "fw_update.h"

/* Independent watchdog timeout. */
#define IWDG_TIMEOUT_MS 2000U
#define VREFINT_ADC_CHANNEL 18U
#define VREFINT_CAL_ADDR    0x1FFF75AAUL
#define VREFINT_CAL_VDDA_MV 3000UL
#define BOOT_ADC_TIMEOUT    1000000UL

HRTIM_HandleTypeDef hhrtim1;
TIM_HandleTypeDef   htim6;
DMA_HandleTypeDef   hdma_tim6_up;
DMA_HandleTypeDef   hdma_spi4_tx;

__attribute__((section(".noinit"))) volatile uint32_t g_error_handler_count;
__attribute__((section(".noinit"))) volatile uint32_t g_reset_csr;

/* Which init step is running, so a boot failure names itself on the LCD
 * instead of leaving only a blinking LED. Plain pointer to a literal: no
 * formatting, valid from the first instruction of main(). */
static const char *g_boot_stage = "EARLY";

static volatile uint8_t  g_vref_ready = 0U;
static volatile uint8_t  g_vref_fault = 1U;
static volatile uint16_t g_vref_mv = 0U;

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_TIM6_Init(void);
static void MX_HRTIM1_Init(void);
static void MX_SPI4_Init(void);
static HAL_StatusTypeDef hrtim_dll_calibrate_with_retry(void);
static uint8_t boot_measure_vrefint(void);
static void boot_format_vref_detail(char *dst, uint16_t mv);
static uint8_t boot_math_selftest(void);
static uint8_t boot_rf_geometry_selftest(void);
static uint8_t boot_dma_running_selftest(void);
static void boot_capture_reset_reason(void);

/* ═══════════════ main() ═══════════════ */

/* Independent Watchdog — direct register access.
 *
 * The project's stm32g4xx_hal_conf.h does NOT have HAL_IWDG_MODULE_ENABLED,
 * so HAL_IWDG_* APIs aren't available. IWDG is simple enough to drive
 * directly: LSI is nominally 32 kHz, we use the /32 prescaler → 1 kHz
 * counter, reload 1999 → ~2 s timeout. No HAL or conf file changes needed.
 *
 * IMPORTANT: on STM32G4 the IWDG status flags PVU / RVU / WVU are only
 * clocked by LSI, which is ONLY started when you write 0xCCCC to KR.
 * Polling these bits BEFORE writing 0xCCCC hangs forever. Correct order
 * (per RM0440 §41.4 and the ST HAL driver):
 *   1. 0xCCCC (START)  — enables IWDG and auto-starts LSI
 *   2. 0x5555 (UNLOCK) — enables PR/RLR write access
 *   3. write PR, RLR
 *   4. wait (bounded) for SR to clear
 *   5. 0xAAAA (RELOAD) — refresh counter
 */
#define IWDG_KEY_RELOAD  0x0000AAAAU
#define IWDG_KEY_UNLOCK  0x00005555U
#define IWDG_KEY_START   0x0000CCCCU

static void iwdg_start(void)
{
    IWDG->KR  = IWDG_KEY_START;     /* 0xCCCC: enable IWDG, auto-start LSI */
    IWDG->KR  = IWDG_KEY_UNLOCK;    /* 0x5555: unlock PR/RLR */
    IWDG->PR  = 3U;                 /* prescaler /32 -> ~1 kHz */
    IWDG->RLR = (IWDG_TIMEOUT_MS - 1U); /* reload = ~2 s */

    /* Bounded wait for LSI to propagate the PR/RLR values.
     * LSI startup is <300 µs typical; at 170 MHz, 1M iterations ≈ 6 ms.
     * If the bits never clear (shouldn't happen), we give up and let the
     * watchdog run with whatever values got through — better than hanging
     * boot forever. */
    for (uint32_t i = 0; i < 1000000U; i++) {
        uint32_t sr = IWDG->SR;
        if ((sr & (IWDG_SR_PVU | IWDG_SR_RVU | IWDG_SR_WVU)) == 0U) break;
    }
    IWDG->KR  = IWDG_KEY_RELOAD;    /* 0xAAAA: refresh counter */
}

void APP_WatchdogProgress(void)
{
    IWDG->KR = IWDG_KEY_RELOAD;
}

static inline void iwdg_kick(void)
{
    APP_WatchdogProgress();
}

/* The settings live in the SD card's store (mod_sd_store.c) and there is no
 * store this radio can use: it does not run on defaults by surprise (the
 * user's call, 2026-09-30). RF, the receiver and the watchdog are not
 * started yet; USB-PD keeps negotiating from SysTick. The card is looked at
 * every second -- one put in now carries on, its settings loaded; a blank
 * one with v8.08's flash dump on it (SYSTEM/NVM808.BIN, tools/flash_rev1.py
 * --factory) is imported at once -- and the USB console comes up
 * (`storage ...`). The panel decides too: a click does what the card needs,
 * holding runs without a card until the next boot. */
static void boot_store_prompt(sd_store_state_t st)
{
    const uint8_t import = MOD_StorageImportAvailable();
    switch (st) {
        case SD_STORE_BLANK:
            LCDUI_BootStep("SD CARD", 2U, "NO SETTINGS STORE");
            LCDUI_BootStep("CLICK", 2U, import ? "IMPORT V8.08 SETTINGS" : "NEW SETTINGS STORE");
            break;
        case SD_STORE_FOREIGN:
            LCDUI_BootStep("SD CARD", 0U, "ANOTHER RADIO'S STORE");
            LCDUI_BootStep("CLICK", 2U, "USE ITS SETTINGS + CAL");
            break;
        case SD_STORE_DAMAGED:
            LCDUI_BootStep("SD CARD", 0U, "STORE HEADERS LOST");
            LCDUI_BootStep("CLICK", 2U, "REPAIR, KEEP RECORDS");
            break;
        case SD_STORE_LAYOUT:
            LCDUI_BootStep("SD CARD", 0U, "LAYOUT: FORMAT IT");
            break;
        case SD_STORE_IO:
            LCDUI_BootStep("SD CARD", 0U, "READ ERROR");
            break;
        case SD_STORE_READY: /* held when an import or restore was cut short, or the settings did not read */
            if (MOD_StorageSettingsUnread()) {
                LCDUI_BootStep("SD STORE", 0U, "SETTINGS READ ERROR");
                LCDUI_BootStep("CLICK", 2U, "READ AGAIN");
                break;
            }
            LCDUI_BootStep("SD STORE", 0U, MOD_StorageIncomplete() == 2U ? "RESTORE CUT SHORT"
                                                                          : "IMPORT CUT SHORT");
            LCDUI_BootStep("CLICK", 2U, MOD_StorageResumeAction());
            break;
        case SD_STORE_NO_CARD:
        default:
            LCDUI_BootStep("SD CARD", 0U, "NONE - INSERT CARD");
            break;
    }
    LCDUI_BootStep("HOLD", 2U, "RUN WITHOUT CARD");
}

static void boot_store_click(sd_store_state_t st)
{
    if (st == SD_STORE_BLANK) {
        if (MOD_StorageImportAvailable()) (void)MOD_StorageImport(0U);
        else (void)MOD_StorageCreate();
    } else if (st == SD_STORE_FOREIGN || st == SD_STORE_DAMAGED) {
        (void)MOD_StorageAdopt();
    } else if (st == SD_STORE_READY) {
        if (MOD_StorageSettingsUnread()) (void)MOD_StorageRereadSettings();
        else (void)MOD_StorageResume();
    } else {
        SD_ServiceRefresh();
    }
}

/* Returns 1: USB is initialised. */
static uint8_t boot_wait_for_store(void)
{
    LCDUI_BootStep("SETTINGS", 2U, "LIVE ON THE CARD");
    LCDUI_BootStep("USB CONSOLE", 2U, "STORAGE ...");
    MX_USB_Device_Init();
    HAL_NVIC_SetPriority(USB_LP_IRQn, 1, 0);
    const uint32_t start = HAL_GetTick();
    uint32_t next_try = start;
    uint8_t tries = 0U;
    uint8_t console = 0U;
    sd_store_state_t shown = (sd_store_state_t)0xFFU;
    while (MOD_StorageCardRequired()) {
        const uint32_t now = HAL_GetTick();
        if (!console && (now - start) > 1500U) console = (uint8_t)(USB_DiagModeEnterAtBoot() || (now - start) > 10000U);
        USB_DiagService(now);
        USB_DiagModeTick(now);
        const sd_store_state_t st = SD_StoreState();
        if (st != shown) {
            shown = st;
            boot_store_prompt(st);
        }
        const uint8_t choice = UI_AppBootChoice(now);
        if (choice == 2U) {
            (void)MOD_StorageRunWithoutCard();
            break;
        }
        if (choice == 1U) boot_store_click(st);
        if ((int32_t)(now - next_try) >= 0) {
            next_try = now + 1000U;
            LED_GPIO_Port->ODR ^= LED_Pin;
            /* A card swapped while it waits is seen too: a missing or
             * unreadable one is looked at every second, any other every
             * three (a blank card's open reads the whole area). */
            ++tries;
            if (st == SD_STORE_NO_CARD || st == SD_STORE_IO || (st != SD_STORE_READY && (tries % 3U) == 0U))
                SD_ServiceRefresh();
            if (SD_StoreState() == SD_STORE_BLANK && MOD_StorageImportAvailable() == 2U) (void)MOD_StorageImport(1U);
            if (SD_StoreState() == SD_STORE_READY && MOD_StorageSettingsUnread()) (void)MOD_StorageRereadSettings();
        }
        /* The store this radio can use: its settings, once, on the way out. */
        if (!MOD_StorageCardRequired() && !MOD_StorageRunsWithoutCard()) (void)MOD_LoadSettings();
    }
    LCDUI_BootStep("SD CARD", MOD_StorageRunsWithoutCard() ? 2U : 1U,
                   MOD_StorageRunsWithoutCard() ? "NONE - NOTHING SAVED" : SD_GetStatusString());
    return 1U;
}

static void boot_capture_reset_reason(void)
{
    g_reset_csr = RCC->CSR;
    __HAL_RCC_CLEAR_RESET_FLAGS();
}

int main(void)
{
    /* Flush-to-zero and default-NaN. The receive filters are leaky
     * integrators and biquads: on a quiet band, or into a dummy load, their
     * state decays through the denormal range, and denormal operands are the
     * slow path. FPDSCR sets the default for every exception context, so the
     * PendSV worker that runs the DSP gets it too. */
    FPU->FPDSCR |= (1UL << 24) | (1UL << 25);
    __set_FPSCR(__get_FPSCR() | (1UL << 24) | (1UL << 25));

    HAL_Init();
    /* SysTick sits BELOW the audio path, not alongside it. It carries
     * RADIO_PdTick -- the USB-PD state machine -- at 1 kHz, and at priority 1
     * that preempted the PendSV worker doing the receive DSP: on REV1 the
     * receiver missed 449 of 2767 I2S halves a second with PD enabled and 1.5
     * with it disabled, for identical worker time. At 3 it is below PendSV (2)
     * and the DMA channels (1), and still above thread. The tick can now be
     * late by up to one worker pass, a few hundred microseconds, which the HAL
     * tick, the encoder sampling and the PD millisecond timers all tolerate. */
    HAL_NVIC_SetPriority(SysTick_IRQn, 3, 0);
    HAL_NVIC_SetPriority(PendSV_IRQn, 2, 0);
    boot_capture_reset_reason();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_DMA_Init();
    MX_SPI4_Init();
    LCDUI_Init(&hdma_spi4_tx);
    BOARD_SetBacklight(70U);
    LCDUI_BootBegin();
    LCDUI_BootStep("CLOCK", 1U, "SYS 170M");
    LCDUI_BootStep("GPIO/DMA/SPI", 1U, "LCD OK");
    const char *boot_fault = FW_UpdateBootFault();
    if (boot_fault != NULL) {
        /* A debugger can start an application ELF without ever programming
         * the first-stage vector or its CRC footer. Keep PB10 latched and
         * make that incomplete flash visible before enabling RF or USB. */
        LCDUI_BootStep("FLASH SETUP", 0U, boot_fault);
        LCDUI_BootStep("USE FACTORY", 0U, "IMAGE");
        for (;;) {
            APP_WatchdogProgress();
            LED_GPIO_Port->ODR ^= LED_Pin;
            HAL_Delay(250U);
        }
    }
    LCDUI_BootStep("BOOT IMAGE", 1U, "CRC OK");

    MX_TIM6_Init();
    LCDUI_BootStep("TIM6", 1U, "48K BASE");
    MX_HRTIM1_Init();
    LCDUI_BootStep("HRTIM DLL", 1U, "LOCKED");
    char vref_detail[16];
    const uint8_t vref_ok = RADIO_ReferenceInit() && boot_measure_vrefint();
    boot_format_vref_detail(vref_detail, MOD_GetVrefMillivolts());
    LCDUI_BootStep("VREFINT", vref_ok ? 1U : 2U,
                   vref_ok ? vref_detail : "MEAS FAIL");
    SD_ServiceInit();
    SD_ServiceBootIo(1U);
    LCDUI_BootStep("SD CARD", SD_IsMounted() ? 1U : 2U,
                   SD_IsMounted() ? SD_GetStatusString() : SD_ServiceStatusString());

    /* Settings may live on the card (MOD_STORAGE_SD), so the mount comes
     * first; USB-PD then joins as soon as its settings and the VIN ADC are up.
     * A source that has just applied VBUS repeats its capabilities for a few
     * seconds and then hard-resets, and a hard reset takes VBUS -- the MCU's
     * only supply -- away. From here on the SysTick-driven supervisor
     * negotiates while the rest of boot runs. */
    MOD_Init();
    RADIO_Init();
    RADIO_PdInit();
    /* The settings are loaded: turn the boot log to the stored mounting. */
    LCDUI_SetRotation((uint8_t)((RADIO_Config()->boot_flags & RADIO_BOOT_FLAG_LCD_ROT270) != 0U));
    /* RADIO_PdInit has already sensed the CC lines, so the splash can say what
     * the radio is plugged into before anything downstream needs the rail. */
    {
        const radio_supply_t supply = RADIO_PdSupply();
        const uint8_t settled = (uint8_t)(supply == RADIO_SUPPLY_DC_JACK ||
                                          supply == RADIO_SUPPLY_USB_PD ||
                                          RADIO_PdProbe() == RADIO_PD_PROBE_ACTIVE);
        LCDUI_BootStep("SUPPLY", settled ? 1U : 2U, RADIO_PdSupplyName());
    }
    SD_AudioPlayerInit();
    LCDUI_BootStep("SETTINGS/RF", boot_rf_geometry_selftest() ? 1U : 0U,
                   MOD_CarrierFrequencyReadyForTx() ? "GEOM OK" : "TX HOLD");
    LCDUI_BootStep("CORDIC", MOD_RunCordicSelfTest() ? 1U : 2U,
                   MOD_GetCordicEnabled() ? "HW OK" : "SW FALLBACK");
    LCDUI_BootStep("MATH", boot_math_selftest() ? 1U : 0U, "DSP OK");
    UI_AppInit();
    UI_SDInit();
    LCDUI_BootStep("UI", 1U, "HOME READY");
    /* The files this image needs on the card (HELP.BIN, CWPACK.BIN): one
     * missing or from another build brings the USB console up at boot, as
     * DIAG ON BOOT does, so tools/flash_rev1.py can put it there after a
     * flash. */
    const uint8_t card_setup = (uint8_t)(SD_IsMounted() && (UI_HelpCardState() != 2U || CWPACK_CardState() != 2U));
    if (SD_IsMounted()) LCDUI_BootStep("SD FILES", card_setup ? 2U : 1U, card_setup ? "SETUP OVER USB" : "COMPLETE");
    /* The settings store is the card (since v8.08; the only one since
     * v8.09) and there is none this radio can use: stop here, before RF DMA
     * and the watchdog, until there is -- or the operator says to run
     * without. */
    const uint8_t usb_up = MOD_StorageCardRequired() ? boot_wait_for_store() : 0U;
    if (!usb_up) MX_USB_Device_Init();
    HAL_NVIC_SetPriority(USB_LP_IRQn, 1, 0);
    LCDUI_BootStep("USB AUDIO", 1U, "48K READY");
    SD_ServiceBootIo(0U); /* from here card I/O waits for the RF and audio paths */
    MOD_StartDMA();
    LCDUI_BootStep("RF DMA", boot_dma_running_selftest() ? 1U : 0U,
#if MOD_DEEP_DIAG_ENABLE
                   "RF INHIBIT");
#else
                   "PARKED");
#endif
    LCDUI_BootStep("BOOT", 1U, "DONE");

    /* Start the watchdog AFTER USB / DMA / display / splash are up
     * so a slow boot doesn't trip it. */
    iwdg_start();

    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);
    HAL_Delay(80);
    if (MOD_PipelineNotificationPending() == 0U) {
        HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
    }

    uint8_t diag_boot_pending = 1U;
    while (1)
    {
        uint32_t now = HAL_GetTick();
        if (RADIO_State() == RADIO_OFF) { iwdg_kick(); __WFI(); continue; }
        RADIO_Service(now);
        RADIO_RefTempService(now);
        RADIO_AudioService();
        RADIO_DigiService(now);
        UI_AppButtonsPoll(now);
        UI_SDService(now);
        SD_AudioPlayerService();
        MOD_ServiceBackground(now);
        /* Every pass, the realtime branch included: an RX; must land while
         * transmitting. Bounded and non-blocking (cat.h). */
        CAT_Service(now);
        /* The S-CTRL LINK, every pass too: its transmit guard and STATUS run
         * while keyed; the rest waits inside. One flag test with no LINK. */
        SCTRL_LinkService(now);

        if (MOD_AudioRealtimeCritical(now)) {
            /* The console's virtual paddle keys the radio from here too: its
             * key releases must be read while RF is up. No-op otherwise. */
            USB_DiagPaddlePoll(now);
            if (MOD_PipelineNotificationPending() != 0U || UI_SDPlayerPageActive() != 0U) UI_AppHandle();
            else { UI_AppConsumeEvents(); UI_AppServiceTxCue(now); }
            SD_AudioPlayerService();
            MOD_ServiceBackground(now);
            MOD_ServiceBackground(now);
            if (UI_SDPlayerPageActive() != 0U && MOD_NoncriticalUiAllowed(now) != 0U) {
                UI_AppServiceDisplay(now);
            }
            iwdg_kick();
            continue;
        }

        UI_AppHandle();
        /* Gated internally: it returns immediately unless the console is
         * enumerated, and emits nothing while the audio path is critical.
         * The tick is the other half: it retakes the port after an `audio`
         * window, which is the only path back that needs no operator. */
        USB_DiagService(now);
        USB_DiagModeTick(now);
        /* DIAG ON BOOT: a bench board is driven over the console, and getting
         * back to it after a reset otherwise means walking the LCD menu. Try
         * once USB has settled, and stop trying either way. */
        if (diag_boot_pending && now > 2000U) {
            if (!((RADIO_Config()->boot_flags & 1U) || card_setup) || USB_DiagModeEnterAtBoot() || now > 10000U)
                diag_boot_pending = 0U;
        }
        if (MOD_NoncriticalUiAllowed(now) != 0U) {
#if MOD_DEEP_DIAG_ENABLE
            if (MOD_CpuScopeModeActive(MOD_CPU_SCOPE_LCD_SERVICE) != 0U) {
                MOD_CpuScopeBegin(MOD_CPU_SCOPE_LCD_SERVICE);
                UI_AppServiceDisplay(now);
                MOD_CpuScopeEnd(MOD_CPU_SCOPE_LCD_SERVICE);
            } else {
                UI_AppServiceDisplay(now);
            }
#else
            UI_AppServiceDisplay(now);
#endif
        }
        MOD_ServiceBackground(now);
        if (MOD_BackgroundIoAllowed(now) != 0U) {
            SD_ServiceBackground();
            FW_UpdateConfirmService(now);
        }
        /* The settings on the card follow the operator, and what went wrong
         * goes into its event log; both gate themselves off TX and streams. */
        MOD_StorageAutosaveService(now);
        MOD_StorageLogService(now);
        MOD_CalReloadService(now);
        /* The active RF stream has a separate 3 ms progress guard. Keep the
         * IWDG margin above the display controller's bounded recovery path. */
        /* Kick the watchdog. This loop must stay comfortably under 2 s
         * or the MCU will reset — which is exactly what we want if
         * something hangs the render path. */
        iwdg_kick();
    }
}

uint8_t MOD_GetVrefEnabled(void)
{
    return 1U;
}

uint32_t MOD_GetResetCsr(void)
{
    return g_reset_csr;
}

const char *MOD_GetResetReasonLabel(void)
{
    if ((g_reset_csr & RCC_CSR_IWDGRSTF) != 0U) return "IWDG";
    if ((g_reset_csr & RCC_CSR_WWDGRSTF) != 0U) return "WWDG";
    if ((g_reset_csr & RCC_CSR_SFTRSTF) != 0U) return "SW";
    if ((g_reset_csr & RCC_CSR_BORRSTF) != 0U) return "BOR";
    if ((g_reset_csr & RCC_CSR_PINRSTF) != 0U) return "PIN";
    if ((g_reset_csr & RCC_CSR_OBLRSTF) != 0U) return "OBL";
    if ((g_reset_csr & RCC_CSR_LPWRRSTF) != 0U) return "LPWR";
    return "POR";
}

uint8_t MOD_GetVrefReady(void)
{
    return g_vref_ready ? 1U : 0U;
}

uint8_t MOD_GetVrefFault(void)
{
    return g_vref_fault ? 1U : 0U;
}

uint16_t MOD_GetVrefMillivolts(void)
{
    return g_vref_mv;
}

static uint8_t boot_wait_reg_set(volatile uint32_t *reg, uint32_t mask)
{
    for (uint32_t i = 0U; i < BOOT_ADC_TIMEOUT; i++) {
        if ((*reg & mask) == mask) return 1U;
    }
    return 0U;
}

static uint8_t boot_wait_reg_clear(volatile uint32_t *reg, uint32_t mask)
{
    for (uint32_t i = 0U; i < BOOT_ADC_TIMEOUT; i++) {
        if ((*reg & mask) == 0U) return 1U;
    }
    return 0U;
}

static void boot_adc_vref_cleanup(uint32_t ccr_saved, uint32_t ccipr_saved, uint8_t clock_was_enabled)
{
    if ((ADC1->CR & ADC_CR_ADSTART) != 0U) {
        ADC1->CR |= ADC_CR_ADSTP;
        (void)boot_wait_reg_clear(&ADC1->CR, ADC_CR_ADSTART);
    }
    if ((ADC1->CR & ADC_CR_ADEN) != 0U) {
        ADC1->CR |= ADC_CR_ADDIS;
        (void)boot_wait_reg_clear(&ADC1->CR, ADC_CR_ADEN);
    }
    ADC12_COMMON->CCR = ccr_saved;
    RCC->CCIPR = (RCC->CCIPR & ~RCC_CCIPR_ADC12SEL) | ccipr_saved;
    ADC1->CR &= ~ADC_CR_ADVREGEN;
    ADC1->CR |= ADC_CR_DEEPPWD;
    if (clock_was_enabled == 0U) {
        __HAL_RCC_ADC12_CLK_DISABLE();
    }
}

static uint8_t boot_measure_vrefint(void)
{
    uint64_t acc = 0ULL;
    const uint32_t ccipr_saved = RCC->CCIPR & RCC_CCIPR_ADC12SEL;
    const uint8_t clock_was_enabled = __HAL_RCC_ADC12_IS_CLK_ENABLED() ? 1U : 0U;

    g_vref_ready = 0U;
    g_vref_fault = 1U;
    g_vref_mv = 0U;

    __HAL_RCC_ADC12_CLK_ENABLE();
    const uint32_t ccr_saved = ADC12_COMMON->CCR;
    __HAL_RCC_ADC12_CONFIG(RCC_ADC12CLKSOURCE_SYSCLK);

    ADC1->CR &= ~ADC_CR_DEEPPWD;
    ADC1->CR |= ADC_CR_ADVREGEN;
    HAL_Delay(1U);

    if ((ADC1->CR & ADC_CR_ADEN) != 0U) {
        ADC1->CR |= ADC_CR_ADDIS;
        if (!boot_wait_reg_clear(&ADC1->CR, ADC_CR_ADEN)) goto fail;
    }

    ADC12_COMMON->CCR = (ccr_saved & ~(ADC_CCR_CKMODE | ADC_CCR_PRESC))
                      | ADC_CCR_CKMODE_0 | ADC_CCR_CKMODE_1 | ADC_CCR_VREFEN;
    HAL_Delay(1U);

    ADC1->DIFSEL &= ~(1UL << VREFINT_ADC_CHANNEL);
    ADC1->CR &= ~ADC_CR_ADCALDIF;
    ADC1->CR |= ADC_CR_ADCAL;
    if (!boot_wait_reg_clear(&ADC1->CR, ADC_CR_ADCAL)) goto fail;

    ADC1->CFGR = 0U;
    ADC1->SMPR2 = (ADC1->SMPR2 & ~ADC_SMPR2_SMP18) | ADC_SMPR2_SMP18;
    ADC1->SQR1 = ((uint32_t)VREFINT_ADC_CHANNEL << ADC_SQR1_SQ1_Pos);
    ADC1->ISR = ADC_ISR_ADRDY | ADC_ISR_EOC | ADC_ISR_EOS | ADC_ISR_OVR;

    ADC1->CR |= ADC_CR_ADEN;
    if (!boot_wait_reg_set(&ADC1->ISR, ADC_ISR_ADRDY)) goto fail;
    ADC1->ISR = ADC_ISR_ADRDY;

    for (uint8_t i = 0U; i < 8U; i++) {
        ADC1->CR |= ADC_CR_ADSTART;
        if (!boot_wait_reg_set(&ADC1->ISR, ADC_ISR_EOC)) goto fail;
        acc += (uint16_t)ADC1->DR;
        ADC1->ISR = ADC_ISR_EOC | ADC_ISR_EOS;
    }

    {
        uint32_t raw = (uint32_t)((acc + 4ULL) / 8ULL);
        uint32_t cal = *(const uint16_t *)VREFINT_CAL_ADDR;
        if ((raw == 0U) || (cal < 1000U) || (cal > 2000U)) goto fail;
        uint32_t mv = (uint32_t)((VREFINT_CAL_VDDA_MV * cal + (raw / 2U)) / raw);
        if ((mv < 1800U) || (mv > 3800U)) goto fail;
        g_vref_mv = (uint16_t)mv;
        g_vref_ready = 1U;
        g_vref_fault = 0U;
    }

    boot_adc_vref_cleanup(ccr_saved, ccipr_saved, clock_was_enabled);
    return 1U;

fail:
    boot_adc_vref_cleanup(ccr_saved, ccipr_saved, clock_was_enabled);
    return 0U;
}

static void boot_format_vref_detail(char *dst, uint16_t mv)
{
    dst[0] = (char)('0' + (mv / 1000U) % 10U);
    dst[1] = '.';
    dst[2] = (char)('0' + (mv / 100U) % 10U);
    dst[3] = (char)('0' + (mv / 10U) % 10U);
    dst[4] = (char)('0' + mv % 10U);
    dst[5] = 'V';
    dst[6] = 0;
}

static uint8_t boot_math_selftest(void)
{
    float s = sinf(0.5235987756f);
    float c = cosf(1.0471975512f);
    float h = sqrtf(0.25f);
    return ((fabsf(s - 0.5f) < 0.0005f) &&
            (fabsf(c - 0.5f) < 0.0005f) &&
            (fabsf(h - 0.5f) < 0.0005f)) ? 1U : 0U;
}

static uint8_t boot_rf_geometry_selftest(void)
{
    uint16_t period = MOD_GetCarrierPeriodTicks();
    uint32_t hclk = MOD_GetHrtimInputClockHz();
    uint32_t actual = MOD_GetActualCarrierFreqHz();
    return ((period > 0U) &&
            (hclk >= 100000000UL) &&
            (actual >= 1000000UL) &&
            (actual <= 28333333UL)) ? 1U : 0U;
}

static uint8_t boot_dma_running_selftest(void)
{
    return MOD_RfDmaStaticIdleParked();
}

/* ═══════════════ Cube-generated init ═══════════════ */

void SystemClock_Config(void)
{
    g_boot_stage = "CLOCK";
    RCC_OscInitTypeDef o = {0};
    RCC_ClkInitTypeDef c = {0};
    RCC_CRSInitTypeDef cr = {0};

    HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

    o.OscillatorType      = RCC_OSCILLATORTYPE_HSI48 | RCC_OSCILLATORTYPE_HSE;
    o.HSEState            = RCC_HSE_BYPASS;
    o.HSI48State          = RCC_HSI48_ON;
    o.PLL.PLLState        = RCC_PLL_ON;
    o.PLL.PLLSource       = RCC_PLLSOURCE_HSE;
    o.PLL.PLLM            = CLOCK_BOOT_PLL_M;     /* RCC_PLLM_DIVx is the divider itself */
    o.PLL.PLLN            = CLOCK_BOOT_PLL_N;
    o.PLL.PLLP            = RCC_PLLP_DIV2;
    o.PLL.PLLQ            = RCC_PLLQ_DIV6;
    o.PLL.PLLR            = CLOCK_BOOT_PLL_R;
    if (HAL_RCC_OscConfig(&o) != HAL_OK) Error_Handler();

    c.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    c.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    c.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    c.APB1CLKDivider = RCC_HCLK_DIV1;
    c.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&c, FLASH_LATENCY_4) != HAL_OK) Error_Handler();
    /* Loss of the external oscillator enters NMI and clamps RF before reset. */
    HAL_RCC_EnableCSS();
    __HAL_RCC_USB_CONFIG(RCC_USBCLKSOURCE_HSI48);

    __HAL_RCC_CRS_CLK_ENABLE();
    cr.Prescaler             = RCC_CRS_SYNC_DIV1;
    cr.Source                = RCC_CRS_SYNC_SOURCE_USB;
    cr.Polarity              = RCC_CRS_SYNC_POLARITY_RISING;
    cr.ReloadValue           = 47999;
    cr.ErrorLimitValue       = 34;
    cr.HSI48CalibrationValue = 32;
    HAL_RCCEx_CRSConfig(&cr);
}

static void MX_HRTIM1_Init(void)
{
    g_boot_stage = "HRTIM";
    hhrtim1.Instance                     = HRTIM1;
    hhrtim1.Init.HRTIMInterruptRequests  = HRTIM_IT_FLT1 | HRTIM_IT_FLT2 |
                                           HRTIM_IT_FLT3 | HRTIM_IT_FLT4 |
                                           HRTIM_IT_FLT5 | HRTIM_IT_FLT6 |
                                           HRTIM_IT_SYSFLT;
    hhrtim1.Init.SyncOptions             = HRTIM_SYNCOPTION_NONE;
    if (HAL_HRTIM_Init(&hhrtim1) != HAL_OK) Error_Handler();
    /* HAL_HRTIM_Init only stores the request; HAL enables it in
     * HAL_HRTIM_WaveformCountStart_IT, which this firmware does not use, so
     * IER stayed 0 and HRTIM1_FLT_IRQHandler never ran (bench, 2026-10-07).
     * No fault input is enabled; the system fault is the live source. */
    __HAL_HRTIM_ENABLE_IT(&hhrtim1, hhrtim1.Init.HRTIMInterruptRequests);
    /* Nothing the CPU runs can stop the PA drive when the CPU itself stops:
     * the HRTIM and its DMA run on. With the HRTIM frozen on a halt, the TX
     * outputs (fault-protected, hrtim_phase.c) drop to inactive the moment a
     * debugger, a breakpoint or a hot-plug flash halts the core -- the IWDG
     * would not help there, debuggers freeze it -- and a core LOCKUP or a
     * flash double ECC error does the same through the system fault, before
     * any handler runs. DBGMCU survives a system reset; set every boot. */
    DBGMCU->APB2FZ |= DBGMCU_APB2FZ_DBG_HRTIM1_STOP;
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    SYSCFG->CFGR2 |= SYSCFG_CFGR2_CLL | SYSCFG_CFGR2_ECCL;

    HAL_NVIC_SetPriority(HRTIM1_FLT_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(HRTIM1_FLT_IRQn);

    if (hrtim_dll_calibrate_with_retry() != HAL_OK) Error_Handler();

    HAL_HRTIM_MspPostInit(&hhrtim1);
}

static HAL_StatusTypeDef hrtim_dll_calibrate_with_retry(void)
{
    for (uint32_t attempt = 0U; attempt < MOD_HRTIM_DLL_CAL_RETRIES; attempt++) {
        if (HRTIM_DllCalibrate(&hhrtim1, MOD_HRTIM_DLL_CAL_TIMEOUT_MS) == HAL_OK) return HAL_OK;
        HAL_Delay(1U);
    }
    return HAL_ERROR;
}


static void MX_SPI4_Init(void)
{
    __HAL_RCC_SPI4_CLK_ENABLE();

    SPI4->CR1 = 0U;
    SPI4->CR2 = 0U;

    /* 8-bit, software NSS, master, mode 0, 1-line transmit only, /4 prescaler:
     * 42.5 MHz at 170 MHz HCLK, inside the ST7789's 16 ns serial write cycle.
     * A full landscape frame takes ~30 ms instead of the ~120 ms at /16 that
     * showed as a visible top-to-bottom scan, and the clock no longer sits in
     * HF (/16 put its second harmonic at 21.25 MHz, in the 15 m band). */
    SPI4->CR1 = SPI_CR1_MSTR | SPI_CR1_SSI | SPI_CR1_SSM | SPI_CR1_BR_0 |
                SPI_CR1_BIDIMODE | SPI_CR1_BIDIOE;
    SPI4->CR2 = SPI_CR2_DS_0 | SPI_CR2_DS_1 | SPI_CR2_DS_2 | SPI_CR2_FRXTH;

    SPI4->CR1 |= SPI_CR1_SPE;
}


static void MX_TIM6_Init(void)
{
    g_boot_stage = "TIM6";
    TIM_MasterConfigTypeDef sm = {0};
    htim6.Instance               = TIM6;
    htim6.Init.Prescaler         = 0;
    htim6.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim6.Init.Period            = 3541;
    htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim6) != HAL_OK) Error_Handler();
    sm.MasterOutputTrigger = TIM_TRGO_UPDATE;
    sm.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim6, &sm) != HAL_OK) Error_Handler();
}

static void MX_DMA_Init(void)
{
    g_boot_stage = "DMA";
    __HAL_RCC_DMAMUX1_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    hdma_spi4_tx.Instance                 = DMA1_Channel3;
    hdma_spi4_tx.Init.Request             = DMA_REQUEST_SPI4_TX;
    hdma_spi4_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    hdma_spi4_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_spi4_tx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_spi4_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_spi4_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_spi4_tx.Init.Mode                = DMA_NORMAL;
    hdma_spi4_tx.Init.Priority            = DMA_PRIORITY_LOW;
    if (HAL_DMA_Init(&hdma_spi4_tx) != HAL_OK) Error_Handler();

    HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);
    HAL_NVIC_SetPriority(DMA1_Channel2_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel2_IRQn);
    /* The panel's row DMA: above the receive worker (PendSV, 2), so a row
     * done starts the next without waiting the worker out (ui_lcd.c). */
    HAL_NVIC_SetPriority(DMA1_Channel3_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel3_IRQn);
}



static void MX_GPIO_Init(void)
{
    BOARD_GPIOInit();
}


/* A boot-time init failure. RF is parked first, then the board stays here on
 * purpose: this is a hardware fault the operator has to see, not something to
 * retry. Two things matter beyond the original blink.
 *   - The IWDG is kicked. Without that, a failure after iwdg_start() became a
 *     ~2 s reset loop, and BOARD_EarlyInit re-asserts PB10 on every reset, so
 *     the board cycled forever with nothing on screen.
 *   - The failing stage is written to the boot list. LCDUI_BootStep is
 *     blocking and returns immediately if the display was never initialised,
 *     so it is safe here and safe before the LCD exists. */
void Error_Handler(void)
{
    g_error_handler_count++;
    BOARD_EmergencyStop();
    HAL_HRTIM_WaveformOutputStop(&hhrtim1,
        HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2 |
        HRTIM_OUTPUT_TE1 | HRTIM_OUTPUT_TE2);
    __disable_irq();
    LCDUI_BootStep("BOOT FAILED", 0U, g_boot_stage);
    LCDUI_BootStep("HALTED", 0U, MOD_GetResetReasonLabel());
    while (1) {
        APP_WatchdogProgress();
        for (volatile uint32_t i = 0; i < 500000; i++) { }
        LED_GPIO_Port->ODR ^= LED_Pin;
    }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *f, uint32_t l) { (void)f; (void)l; }
#endif
