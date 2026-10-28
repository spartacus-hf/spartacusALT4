    .syntax unified
    .cpu cortex-m4
    .thumb
    .section .isr_vector,"a",%progbits
    .word _estack
    .word Reset_Handler
    .rept 126
    .word Default_Handler
    .endr

    .section .text.Reset_Handler,"ax",%progbits
    .global Reset_Handler
    .type Reset_Handler,%function
Reset_Handler:
    ldr sp, =_estack
    bl loader_early_safe
    ldr r0, =_sdata
    ldr r1, =_edata
    ldr r2, =_sidata
1:  cmp r0, r1
    bcs 2f
    ldr r3, [r2], #4
    str r3, [r0], #4
    b 1b
2:  ldr r0, =_sbss
    ldr r1, =_ebss
    movs r2, #0
3:  cmp r0, r1
    bcs 4f
    str r2, [r0], #4
    b 3b
4:  bl loader_main
5:  b 5b

    .section .text.Default_Handler,"ax",%progbits
    .global Default_Handler
    .type Default_Handler,%function
Default_Handler:
    bl loader_early_safe
6:  b 6b
