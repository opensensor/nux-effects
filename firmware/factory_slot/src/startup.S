.syntax unified
.cpu cortex-m7
.fpu fpv5-d16
.thumb

.section .isr_vector, "a", %progbits
.align 9
.global g_factory_slot_vectors
.type g_factory_slot_vectors, %object
g_factory_slot_vectors:
    .word __stack_top
    .word Reset_Handler
    /* Cortex-M system exceptions, vector words 2 through 15. */
    .rept 14
    .word Default_Handler
    .endr
    /* RT1051 external interrupt 0: shared eDMA channels 0 and 16. */
    .word DMA0_DMA16_IRQHandler
    .rept 159
    .word Default_Handler
    .endr
.size g_factory_slot_vectors, . - g_factory_slot_vectors

.section .text.Reset_Handler, "ax", %progbits
.align 2
.global Reset_Handler
.type Reset_Handler, %function
.thumb_func
Reset_Handler:
    cpsid   i

    /*
     * The factory launcher normally installs the engine MSP before jumping,
     * but make this source image self-contained at the handoff boundary.
     */
    ldr     r0, =__stack_top
    msr     msp, r0

    /* ITCM is the vector address after the factory launcher's fixed copy. */
    ldr     r0, =0xe000ed08
    movs    r1, #0
    str     r1, [r0]
    dsb     sy
    isb     sy

    /*
     * Pinned RT1051 early-core setup: FPU, watchdog/SysTick cleanup, and
     * instruction cache. The audio BSP will own the eventual MPU/D-cache
     * policy explicitly.
     */
    bl      SystemInit

    ldr     r0, =__data_load
    ldr     r1, =__data_start
    ldr     r2, =__data_end
1:
    cmp     r1, r2
    bcs     2f
    ldr     r3, [r0], #4
    str     r3, [r1], #4
    b       1b
2:
    ldr     r1, =__bss_start
    ldr     r2, =__bss_end
    movs    r3, #0
3:
    cmp     r1, r2
    bcs     4f
    str     r3, [r1], #4
    b       3b
4:
    bl      application_main
5:
    wfi
    b       5b
.size Reset_Handler, . - Reset_Handler

.section .text.Default_Handler, "ax", %progbits
.align 2
.global Default_Handler
.type Default_Handler, %function
.thumb_func
Default_Handler:
    b       .
.size Default_Handler, . - Default_Handler
