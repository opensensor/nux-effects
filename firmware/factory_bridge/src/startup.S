/*
 * Copyright-neutral compatibility bridge for the preserved factory Metal
 * engine. The bridge contains no factory code or data. It executes from the
 * open application's SDRAM load address, validates the preserved engine's
 * first two vectors, copies the audited 0x1e000-byte engine image from XIP
 * NOR to ITCM address zero, and transfers control to its reset handler.
 */

.syntax unified
.cpu cortex-m7
.fpu fpv5-d16
.thumb

.equ FACTORY_ENGINE_XIP,       0x600c0000
.equ FACTORY_ENGINE_COPY_SIZE, 0x0001e000
.equ FACTORY_ENGINE_STACK,     0x20018000
.equ FACTORY_ENGINE_RESET,     0x0000e4b5

.global ncr2_factory_engine_source
.equ ncr2_factory_engine_source, FACTORY_ENGINE_XIP
.global ncr2_factory_engine_copy_size
.equ ncr2_factory_engine_copy_size, FACTORY_ENGINE_COPY_SIZE
.global ncr2_factory_engine_stack
.equ ncr2_factory_engine_stack, FACTORY_ENGINE_STACK
.global ncr2_factory_engine_reset
.equ ncr2_factory_engine_reset, FACTORY_ENGINE_RESET

.section .isr_vector, "a", %progbits
.align 9
.global g_factory_bridge_vectors
.type g_factory_bridge_vectors, %object
g_factory_bridge_vectors:
    .word __stack_top
    .word Reset_Handler
    .rept 174
    .word Default_Handler
    .endr
.size g_factory_bridge_vectors, . - g_factory_bridge_vectors

.section .text.Reset_Handler, "ax", %progbits
.align 2
.global Reset_Handler
.type Reset_Handler, %function
.thumb_func
Reset_Handler:
    cpsid   i

    /*
     * Fail closed if the preserved factory region does not match the
     * audited Metal engine. Do this before touching ITCM.
     */
    ldr     r1, =FACTORY_ENGINE_XIP
    ldr     r2, [r1]
    ldr     r3, =FACTORY_ENGINE_STACK
    cmp     r2, r3
    bne     bridge_fault
    ldr     r2, [r1, #4]
    ldr     r3, =FACTORY_ENGINE_RESET
    cmp     r2, r3
    bne     bridge_fault

    movs    r0, #0
    ldr     r2, =FACTORY_ENGINE_COPY_SIZE
    lsrs    r2, r2, #2
1:
    ldr     r3, [r1], #4
    str     r3, [r0], #4
    subs    r2, r2, #1
    bne     1b

    dsb     sy
    isb     sy

    /*
     * Match the stock launcher: interrupts stay disabled and the factory
     * reset handler installs VTOR/MSP and reinitializes the engine runtime.
     */
    ldr     r0, =FACTORY_ENGINE_RESET
    bx      r0

bridge_fault:
    wfi
    b       bridge_fault
.size Reset_Handler, . - Reset_Handler

.section .text.Default_Handler, "ax", %progbits
.align 2
.global Default_Handler
.type Default_Handler, %function
.thumb_func
Default_Handler:
    b       Default_Handler
.size Default_Handler, . - Default_Handler
