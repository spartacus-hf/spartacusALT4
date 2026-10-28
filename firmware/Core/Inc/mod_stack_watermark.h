#ifndef MOD_STACK_WATERMARK_H
#define MOD_STACK_WATERMARK_H

#include <stdint.h>

/* Optional debug-only stack watermark helper.
 * Disabled by default; call MOD_StackWatermarkInit() very early only in a
 * debug build where MOD_STACK_WATERMARK_ENABLE is set to 1.
 */
#ifndef MOD_STACK_WATERMARK_ENABLE
#define MOD_STACK_WATERMARK_ENABLE 0
#endif

void MOD_StackWatermarkInit(void);
uint32_t MOD_StackWatermarkUnusedBytes(void);

#endif /* MOD_STACK_WATERMARK_H */
