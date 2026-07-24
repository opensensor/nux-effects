/*
 * Minimal Core Deluxe engine picker.
 *
 * The stock launcher copies 0x1e000 bytes from selected flash slot 1 to
 * ITCM address 0, then branches through vector[1].  This image replaces
 * slot 1.  It reads a one-shot engine request from reset-retained SRC GPR9,
 * records the selected engine in SRC GPR7, clears the hold counter in GPR8,
 * and branches to a flash-resident copy/handoff stub.
 *
 * The copy/handoff stub must execute from FlexSPI XIP.  Calling memcpy from
 * this ITCM image would overwrite the return address before the call could
 * return.
 */

.syntax unified
.cpu cortex-m7
.thumb

.equ STACK_TOP,            0x20020000
.equ SRC_GPR7,             0x400f8038
.equ SRC_GPR8,             0x400f803c
.equ SRC_GPR9,             0x400f8040
.equ REQUEST_MAGIC,        0x4e555800
.equ REQUEST_MAGIC_MASK,   0xfffffffc
.equ DEFAULT_ENGINE,       1
.equ ENGINE_COPY_SIZE,     0x0001e000
.equ XIP_COPY_HANDOFF,     0x60081001

.section .vectors, "a", %progbits
.align 2
.global picker_vectors
picker_vectors:
    .word STACK_TOP
    .word Reset_Handler
    .rept 174
    .word Default_Handler
    .endr

.section .text.Reset_Handler, "ax", %progbits
.align 2
.global Reset_Handler
.type Reset_Handler, %function
.thumb_func
Reset_Handler:
    cpsid   i

    /* The launcher called us rather than performing an architectural reset. */
    ldr     r0, =STACK_TOP
    msr     msp, r0

    /* Consume the request before loading an engine, preventing reset loops. */
    ldr     r0, =SRC_GPR9
    ldr     r1, [r0]
    movs    r2, #0
    str     r2, [r0]

    ldr     r2, =REQUEST_MAGIC_MASK
    ands    r2, r2, r1
    ldr     r3, =REQUEST_MAGIC
    cmp     r2, r3
    bne     use_default
    ands    r3, r1, #3
    b       target_valid

use_default:
    movs    r3, #DEFAULT_ENGINE

target_valid:
    /* GPR7=current engine, GPR8=long-hold sample counter. */
    ldr     r0, =SRC_GPR7
    str     r3, [r0]
    ldr     r0, =SRC_GPR8
    movs    r2, #0
    str     r2, [r0]

    adr     r0, engine_sources
    ldr     r1, [r0, r3, lsl #2]
    movs    r0, #0
    ldr     r2, =ENGINE_COPY_SIZE
    ldr     r3, =XIP_COPY_HANDOFF
    bx      r3

.align 2
engine_sources:
    /* Delay, relocated reverb, modulation, metal. */
    .word   0x60060000
    .word   0x600e0000
    .word   0x600a0000
    .word   0x600c0000

.size Reset_Handler, . - Reset_Handler

.section .text.Default_Handler, "ax", %progbits
.align 2
.global Default_Handler
.type Default_Handler, %function
.thumb_func
Default_Handler:
    b       Default_Handler
.size Default_Handler, . - Default_Handler

