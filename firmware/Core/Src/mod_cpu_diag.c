#include "mod_priv.h"

#if MOD_DEEP_DIAG_ENABLE

/* Optional CPU cycle profiler.
 *
 * This is deliberately inert until cfg_cpu_diag_enable is set from the UI
 * settings. When disabled, instrumented call sites only perform the cheap
 * enable check. When enabled, the Cortex-M DWT cycle counter is used to track
 * coarse section costs without adding printf/SWO/USB traffic to real-time paths.
 */

static volatile mod_cpu_diag_section_t cpu_diag_sections[MOD_CPU_DIAG_COUNT];
static volatile uint8_t cpu_diag_dwt_available = 0U;
static volatile uint8_t cpu_scope_depth = 0U;
static volatile mod_cpu_scope_mode_t cpu_scope_active_mode = MOD_CPU_SCOPE_OFF;

#if MOD_CRITICAL_DIAG_ENABLE
static volatile uint32_t critical_diag_enter_cycle = 0U;
volatile uint32_t critical_diag_max_cycles = 0U;
volatile uint32_t critical_diag_last_cycles = 0U;
volatile uint32_t critical_diag_calls = 0U;
#endif

static uint8_t cpu_diag_enable_dwt(void)
{
#if defined(DWT) && defined(CoreDebug_DEMCR_TRCENA_Msk) && defined(DWT_CTRL_CYCCNTENA_Msk)
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    cpu_diag_dwt_available = ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U) ? 1U : 0U;
#else
    cpu_diag_dwt_available = 0U;
#endif
    return cpu_diag_dwt_available;
}

void MOD_ResetCpuDiag(void)
{
    uint32_t pm = enter_critical();
    for (uint32_t i = 0U; i < (uint32_t)MOD_CPU_DIAG_COUNT; i++) {
        cpu_diag_sections[i].calls = 0U;
        cpu_diag_sections[i].last_cycles = 0U;
        cpu_diag_sections[i].avg_cycles = 0U;
        cpu_diag_sections[i].max_cycles = 0U;
    }
    exit_critical(pm);
}

void MOD_SetCpuDiagEnable(uint8_t en)
{
    cfg_cpu_diag_enable = en ? 1U : 0U;
    if (cfg_cpu_diag_enable) {
        MOD_ResetCpuDiag();
        (void)cpu_diag_enable_dwt();
    } else {
        cpu_diag_dwt_available = 0U;
        MOD_ResetCpuDiag();
    }
}

uint8_t MOD_GetCpuDiagEnable(void)
{
    return cfg_cpu_diag_enable ? 1U : 0U;
}

uint32_t MOD_CpuDiagBegin(mod_cpu_diag_id_t id)
{
    if ((cfg_cpu_diag_enable == 0U) || ((uint32_t)id >= (uint32_t)MOD_CPU_DIAG_COUNT)) return 0U;
    if (cpu_diag_dwt_available == 0U) {
        if (cpu_diag_enable_dwt() == 0U) return 0U;
    }
#if defined(DWT)
    return DWT->CYCCNT;
#else
    return 0U;
#endif
}

void MOD_CpuDiagEnd(mod_cpu_diag_id_t id, uint32_t start_cycles)
{
    if ((cfg_cpu_diag_enable == 0U) || (cpu_diag_dwt_available == 0U) ||
        ((uint32_t)id >= (uint32_t)MOD_CPU_DIAG_COUNT)) return;
#if defined(DWT)
    uint32_t elapsed = DWT->CYCCNT - start_cycles;
    volatile mod_cpu_diag_section_t *s = &cpu_diag_sections[(uint32_t)id];
    s->last_cycles = elapsed;
    s->calls++;
    if (elapsed > s->max_cycles) s->max_cycles = elapsed;
    if (s->avg_cycles == 0U) s->avg_cycles = elapsed;
    else s->avg_cycles = (uint32_t)(((uint64_t)s->avg_cycles * 7ULL + (uint64_t)elapsed) >> 3);
#else
    (void)start_cycles;
#endif
}

void MOD_GetCpuDiagSnapshot(mod_cpu_diag_snapshot_t *snapshot)
{
    if (snapshot == NULL) return;
    uint32_t hclk = HAL_RCC_GetHCLKFreq();
    uint32_t pm = enter_critical();
    snapshot->enabled = cfg_cpu_diag_enable ? 1U : 0U;
    snapshot->dwt_available = cpu_diag_dwt_available;
    snapshot->cpu_hz = hclk;
    for (uint32_t i = 0U; i < (uint32_t)MOD_CPU_DIAG_COUNT; i++) {
        snapshot->section[i].calls = cpu_diag_sections[i].calls;
        snapshot->section[i].last_cycles = cpu_diag_sections[i].last_cycles;
        snapshot->section[i].avg_cycles = cpu_diag_sections[i].avg_cycles;
        snapshot->section[i].max_cycles = cpu_diag_sections[i].max_cycles;
    }
    exit_critical(pm);
}

static inline uint8_t cpu_scope_mode_matches(mod_cpu_scope_mode_t configured,
                                             mod_cpu_scope_mode_t mode)
{
    if ((uint32_t)configured >= (uint32_t)MOD_CPU_SCOPE_COUNT) return 0U;
    if (configured == mode) return (mode != MOD_CPU_SCOPE_OFF) ? 1U : 0U;
    if (configured != MOD_CPU_SCOPE_FULL_AUDIO_RF) return 0U;
    return CPU_SCOPE_FULL_STAGE(mode) ? 1U : 0U;
}

static inline void cpu_scope_pin_high(void)
{
    CPU_SCOPE_GPIO_Port->BSRR = CPU_SCOPE_Pin;
}

static inline void cpu_scope_pin_low(void)
{
    if (MOD_PipelineNotificationPending() != 0U) {
        CPU_SCOPE_GPIO_Port->BSRR = CPU_SCOPE_Pin;
    } else {
        CPU_SCOPE_GPIO_Port->BRR = CPU_SCOPE_Pin;
    }
}

void MOD_SetCpuScopeMode(mod_cpu_scope_mode_t mode)
{
    if ((uint32_t)mode >= (uint32_t)MOD_CPU_SCOPE_COUNT) mode = MOD_CPU_SCOPE_OFF;
    cfg_cpu_scope_mode = mode;
    cpu_scope_depth = 0U;
    cpu_scope_active_mode = MOD_CPU_SCOPE_OFF;
    cpu_scope_pin_low();
}

mod_cpu_scope_mode_t MOD_GetCpuScopeMode(void)
{
    return ((uint32_t)cfg_cpu_scope_mode < (uint32_t)MOD_CPU_SCOPE_COUNT)
         ? cfg_cpu_scope_mode : MOD_CPU_SCOPE_OFF;
}

uint8_t MOD_CpuScopeModeActive(mod_cpu_scope_mode_t mode)
{
    return cpu_scope_mode_matches(MOD_GetCpuScopeMode(), mode);
}

const char *MOD_GetCpuScopeModeLabel(mod_cpu_scope_mode_t mode)
{
    switch (mode) {
        case MOD_CPU_SCOPE_USB_SUBMIT:     return "USB";
        case MOD_CPU_SCOPE_RENDER_EXT:     return "RENDER";
        case MOD_CPU_SCOPE_DMA_POP:        return "DMA";
        case MOD_CPU_SCOPE_SERVICE_BG:     return "BG";
        case MOD_CPU_SCOPE_LCD_SERVICE:    return "LCD";
        case MOD_CPU_SCOPE_FULL_AUDIO_RF:  return "FULL";
        case MOD_CPU_SCOPE_OFF:
        default:                           return "OFF";
    }
}

void MOD_CpuScopeBegin(mod_cpu_scope_mode_t mode)
{
    if (cpu_scope_mode_matches(MOD_GetCpuScopeMode(), mode) == 0U) return;
    if (cpu_scope_depth == 0U) {
        cpu_scope_active_mode = mode;
        cpu_scope_pin_high();
    }
    if (cpu_scope_depth < 0xFFU) cpu_scope_depth++;
}

void MOD_CpuScopeEnd(mod_cpu_scope_mode_t mode)
{
    if (cpu_scope_depth == 0U) return;
    if ((cpu_scope_active_mode != mode) &&
        (MOD_GetCpuScopeMode() != MOD_CPU_SCOPE_FULL_AUDIO_RF)) {
        return;
    }
    cpu_scope_depth--;
    if (cpu_scope_depth == 0U) {
        cpu_scope_pin_low();
        cpu_scope_active_mode = MOD_CPU_SCOPE_OFF;
    }
}

#if MOD_CRITICAL_DIAG_ENABLE
void MOD_CriticalDiagEnter(uint32_t primask)
{
    if ((primask & 1U) != 0U) return;
    if (cpu_diag_dwt_available == 0U) {
        if (cpu_diag_enable_dwt() == 0U) return;
    }
#if defined(DWT)
    critical_diag_enter_cycle = DWT->CYCCNT;
#endif
}

void MOD_CriticalDiagExit(uint32_t primask)
{
    if ((primask & 1U) != 0U) return;
#if defined(DWT)
    uint32_t elapsed = DWT->CYCCNT - critical_diag_enter_cycle;
    critical_diag_last_cycles = elapsed;
    critical_diag_calls++;
    if (elapsed > critical_diag_max_cycles) critical_diag_max_cycles = elapsed;
#endif
}
#endif

#endif /* MOD_DEEP_DIAG_ENABLE */
