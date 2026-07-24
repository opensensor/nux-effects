.syntax unified
.cpu cortex-m7
.fpu fpv5-d16
.thumb

.section .isr_vector, "a", %progbits
.align 9
.global g_ram_recovery_vectors
.type g_ram_recovery_vectors, %object
g_ram_recovery_vectors:
    .word __stack_top
    .word Reset_Handler
    /* Core vectors 2..14. */
    .rept 13
    .word Default_Handler
    .endr
    /* Core vector 15: SysTick indicator heartbeat. */
    .word SysTick_Handler
    /* External IRQs 0..112. */
    .rept 113
    .word Default_Handler
    .endr
    /* External IRQ 113: USB OTG1. */
    .word USB_OTG1_IRQHandler
    /* External IRQs 114..159. */
    .rept 46
    .word Default_Handler
    .endr
.size g_ram_recovery_vectors, . - g_ram_recovery_vectors

.section .text.Reset_Handler, "ax", %progbits
.align 2
.global Reset_Handler
.type Reset_Handler, %function
.thumb_func
Reset_Handler:
    cpsid i

    /* Install the SDRAM vector table before enabling any peripheral. */
    ldr r0, =0xE000ED08
    ldr r1, =g_ram_recovery_vectors
    str r1, [r0]
    dsb
    isb

    /* Preserve the bootloader's clocks/SEMC setup; only enable the FPU. */
    ldr r0, =0xE000ED88
    ldr r1, [r0]
    orr r1, r1, #(0xF << 20)
    str r1, [r0]
    dsb
    isb

    ldr r0, =__data_load
    ldr r1, =__data_start
    ldr r2, =__data_end
0:
    cmp r1, r2
    bcs 1f
    ldr r3, [r0], #4
    str r3, [r1], #4
    b 0b
1:
    ldr r1, =__bss_start
    ldr r2, =__bss_end
    movs r3, #0
2:
    cmp r1, r2
    bcs 3f
    str r3, [r1], #4
    b 2b
3:
    cpsie i
    bl ram_recovery_main
4:
    wfi
    b 4b
.size Reset_Handler, . - Reset_Handler

.section .text.Default_Handler, "ax", %progbits
.align 2
.global Default_Handler
.type Default_Handler, %function
.thumb_func
Default_Handler:
    b .
.size Default_Handler, . - Default_Handler

.weak USB_OTG1_IRQHandler
.thumb_set USB_OTG1_IRQHandler, Default_Handler

.weak SysTick_Handler
.thumb_set SysTick_Handler, Default_Handler
