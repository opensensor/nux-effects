/*
 * GPT1 long-hold monitor injected into an unused ITCM code cave in each
 * patched engine.  Keeping this code in ITCM avoids depending on the
 * target engine's post-startup MPU permissions for FlexSPI XIP.
 */

.syntax unified
.cpu cortex-m7
.thumb

.equ GPT1_SR,              0x401ec008
.equ GPT1_BASE,            0x401ec000
.equ NVIC_ISER3,           0xe000e10c
.equ GPIO1_DR,             0x401b8000
.equ FOOTSWITCH_MASK,      0x00200000

.equ SRC_GPR7,             0x400f8038
.equ SRC_GPR8,             0x400f803c
.equ SRC_GPR9,             0x400f8040
.equ REQUEST_MAGIC,        0x4e555800
.equ REQUEST_MAGIC_MASK,   0xfffffffc

.equ HOLD_SAMPLES,         275
.equ SCB_AIRCR,            0xe000ed0c
.equ AIRCR_SYSRESETREQ,    0x05fa0004

.section .monitor, "ax", %progbits
.align 2
.global gpt1_long_hold_monitor
.type gpt1_long_hold_monitor, %function
.thumb_func
gpt1_long_hold_monitor:
    /*
     * This is installed directly in vector[16 + GPT1_IRQn].  Exception
     * entry already saved r0-r3/r12/lr, so the handler uses only r0-r3.
     */

    /* Preserve the stock handler's only behavior: W1C GPT1 compare flag 1. */
    ldr     r0, =GPT1_SR
    movs    r1, #1
    str     r1, [r0]

    /* The boot launcher established GPIO1_IO21 as an active-low switch. */
    ldr     r0, =GPIO1_DR
    ldr     r1, [r0]
    tst.w   r1, #FOOTSWITCH_MASK
    bne     released

    /*
     * Once a request is staged, wait for release.  Resetting while this
     * switch is still low could make the untouched launcher enter DFU.
     */
    ldr     r0, =SRC_GPR9
    ldr     r1, [r0]
    cmp     r1, #0
    bne     monitor_return

    ldr     r0, =SRC_GPR8
    ldr     r1, [r0]
    adds    r1, r1, #1
    str     r1, [r0]
    movw    r2, #HOLD_SAMPLES
    cmp     r1, r2
    blo     monitor_return

    movs    r1, #0
    str     r1, [r0]

    /* Advance through Delay -> Reverb -> Modulation -> Metal -> Delay. */
    ldr     r0, =SRC_GPR7
    ldr     r1, [r0]
    adds    r1, r1, #1
    ands    r1, r1, #3
    ldr     r2, =REQUEST_MAGIC
    orrs    r1, r1, r2

submit_request:
    ldr     r0, =SRC_GPR9
    str     r1, [r0]
    b       monitor_return

released:
    ldr     r0, =SRC_GPR8
    movs    r1, #0
    str     r1, [r0]

    /* A valid staged request is consumed only after switch release. */
    ldr     r0, =SRC_GPR9
    ldr     r1, [r0]
    ldr     r2, =REQUEST_MAGIC_MASK
    ands    r2, r2, r1
    ldr     r3, =REQUEST_MAGIC
    cmp     r2, r3
    bne     monitor_return

    /* CMSIS NVIC_SystemReset semantics, without returning to the engine. */
    dsb     sy
    ldr     r0, =SCB_AIRCR
    ldr     r1, =AIRCR_SYSRESETREQ
    str     r1, [r0]
    dsb     sy
reset_wait:
    b       reset_wait

monitor_return:
    bx      lr
.size gpt1_long_hold_monitor, . - gpt1_long_hold_monitor

/*
 * The factory starts GPT1 for polling but leaves its interrupt mask and
 * NVIC line disabled.  Each engine's GPT_StartTimer wrapper branches here.
 */
.org 0xa0
.align 2
.global gpt1_start_and_enable
.type gpt1_start_and_enable, %function
.thumb_func
gpt1_start_and_enable:
    ldr     r0, =GPT1_BASE
    ldr     r1, [r0]
    orr     r1, r1, #1
    str     r1, [r0]

    movs    r1, #1
    str     r1, [r0, #0x0c]

    ldr     r0, =NVIC_ISER3
    movs    r1, #0x10
    str     r1, [r0]
    dsb     sy
    bx      lr
.size gpt1_start_and_enable, . - gpt1_start_and_enable
