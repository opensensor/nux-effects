.syntax unified
.cpu cortex-m7
.fpu fpv5-d16
.thumb

.section .isr_vector, "a", %progbits
.align 9
.global g_boot_vectors
.type g_boot_vectors, %object
g_boot_vectors:
    .word __stack_top
    .word Reset_Handler
    /* Core vectors 2..15 and external IRQs 0..112. */
    .rept 127
    .word Default_Handler
    .endr
    /* External IRQ 113: USB OTG1. */
    .word USB_OTG1_IRQHandler
    /* External IRQs 114..159. */
    .rept 46
    .word Default_Handler
    .endr
.size g_boot_vectors, . - g_boot_vectors

.section .text.Reset_Handler, "ax", %progbits
.align 2
.global Reset_Handler
.type Reset_Handler, %function
.thumb_func
Reset_Handler:
    /*
     * The ROM enters through the image vector table but does not form part
     * of our interrupt-routing contract. Install the table explicitly
     * before any board adapter can enable a peripheral interrupt.
     */
    ldr r0, =0xE000ED08
    ldr r1, =g_boot_vectors
    str r1, [r0]
    dsb
    isb

    ldr r0, =__ramfunc_load
    ldr r1, =__ramfunc_start
    ldr r2, =__ramfunc_end
0:
    cmp r1, r2
    bcs 1f
    ldr r3, [r0], #4
    str r3, [r1], #4
    b 0b
1:
    ldr r0, =__data_load
    ldr r1, =__data_start
    ldr r2, =__data_end
2:
    cmp r1, r2
    bcs 3f
    ldr r3, [r0], #4
    str r3, [r1], #4
    b 2b
3:
    ldr r1, =__bss_start
    ldr r2, =__bss_end
    movs r3, #0
4:
    cmp r1, r2
    bcs 5f
    str r3, [r1], #4
    b 4b
5:
    bl bootloader_main
6:
    wfi
    b 6b
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
