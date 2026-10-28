#include "mod_stack_watermark.h"

#if MOD_STACK_WATERMARK_ENABLE

extern uint32_t _ebss;
extern uint32_t _estack;

#define STACK_WATERMARK_WORD 0xA5A5A5A5UL
#define STACK_WATERMARK_GUARD_WORDS 64U

static uint32_t *stack_scan_start = 0;
static uint32_t *stack_scan_end = 0;

static uint32_t *current_sp(void)
{
    uint32_t *sp;
    __asm volatile ("mrs %0, msp" : "=r" (sp));
    return sp;
}

void MOD_StackWatermarkInit(void)
{
    uint32_t *start = (uint32_t *)&_ebss;
    uint32_t *end = current_sp();

    if (end > (uint32_t *)&_estack) end = (uint32_t *)&_estack;
    if (end > start + STACK_WATERMARK_GUARD_WORDS) {
        end -= STACK_WATERMARK_GUARD_WORDS;
    } else {
        end = start;
    }

    stack_scan_start = start;
    stack_scan_end = end;

    for (uint32_t *p = stack_scan_start; p < stack_scan_end; p++) {
        *p = STACK_WATERMARK_WORD;
    }
}

uint32_t MOD_StackWatermarkUnusedBytes(void)
{
    uint32_t words = 0U;

    if (!stack_scan_start || !stack_scan_end || stack_scan_end <= stack_scan_start) {
        return 0U;
    }

    for (uint32_t *p = stack_scan_start; p < stack_scan_end; p++) {
        if (*p != STACK_WATERMARK_WORD) break;
        words++;
    }

    return words * (uint32_t)sizeof(uint32_t);
}

#else

void MOD_StackWatermarkInit(void) {}
uint32_t MOD_StackWatermarkUnusedBytes(void) { return 0U; }

#endif
