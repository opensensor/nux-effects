/*
 * Runtime-only switch monitor copied into the verified zero-filled tail of
 * each factory engine. It never changes the preserved NOR image.
 *
 * The launcher redirects one proven main-loop BL (the stock LED updater) to
 * this monitor. On an ordinary pass the monitor tail-calls that original
 * updater, preserving LR exactly as if the factory BL had not been patched.
 */

.syntax unified
.cpu cortex-m7
.thumb

.equ GPIO1_PSR,              0x401b8008
.equ FOOTSWITCH_MASK,        0x00200000

/* ADC_ETC trigger 0, chain 2 result: the Verb Type/Step selector. */
.equ TYPE_ADC_RESULT_3_2,    0x403b002c

.equ SRC_GPR7,               0x400f8038
.equ SRC_GPR10,              0x400f8044
.equ FACTORY_REQUEST_MAGIC,  0x46414330

/* The stock main loop runs once per 48 audio frames, approximately 1 kHz. */
.equ SELECT_HOLD_LOOPS,      2000

/* Midpoints between the eight measured physical detents. */
.equ TYPE_SLOT_1_MIN,        3326
.equ TYPE_SLOT_2_MIN,        2815
.equ TYPE_SLOT_3_MIN,        2304
.equ TYPE_SLOT_4_MIN,        1793
.equ TYPE_SLOT_5_MIN,        1283
.equ TYPE_SLOT_6_MIN,        772
.equ TYPE_SLOT_7_MIN,        259

.equ SCB_AIRCR,              0xe000ed0c
.equ AIRCR_SYSRESETREQ,      0x05fa0004

.section .text.ncr2_factory_return_monitor, "ax", %progbits
.align 2
.global ncr2_factory_return_monitor_start
ncr2_factory_return_monitor_start:
.global ncr2_factory_return_monitor
.type ncr2_factory_return_monitor, %function
.thumb_func
ncr2_factory_return_monitor:
    /* GPIO inputs must be sampled through PSR, not the DR output latch. */
    ldr     r0, =GPIO1_PSR
    ldr     r1, [r0]
    tst.w   r1, #FOOTSWITCH_MASK
    bne     released

    ldr     r0, =SRC_GPR7
    ldr     r1, [r0]
    adds    r1, r1, #1
    str     r1, [r0]
    b       chain_factory_led

released:
    /* Taps and incomplete holds remain entirely under factory control. */
    ldr     r0, =SRC_GPR7
    ldr     r1, [r0]
    movw    r2, #SELECT_HOLD_LOOPS
    cmp     r1, r2
    blo     clear_counter

    /*
     * One hold duration serves all eight engine slots. Detents 1-4 select
     * the preserved factory engines and detents 5-8 select open engines.
     * After reset the knob returns to selecting effects inside that engine.
     */
    ldr     r2, =TYPE_ADC_RESULT_3_2
    ldr     r1, [r2]
    ubfx    r1, r1, #0, #12
    movs    r3, #0
    movw    r2, #TYPE_SLOT_1_MIN
    cmp     r1, r2
    bhs     stage_slot
    movs    r3, #1
    movw    r2, #TYPE_SLOT_2_MIN
    cmp     r1, r2
    bhs     stage_slot
    movs    r3, #2
    movw    r2, #TYPE_SLOT_3_MIN
    cmp     r1, r2
    bhs     stage_slot
    movs    r3, #3
    movw    r2, #TYPE_SLOT_4_MIN
    cmp     r1, r2
    bhs     stage_slot
    movs    r3, #4
    movw    r2, #TYPE_SLOT_5_MIN
    cmp     r1, r2
    bhs     stage_slot
    movs    r3, #5
    movw    r2, #TYPE_SLOT_6_MIN
    cmp     r1, r2
    bhs     stage_slot
    movs    r3, #6
    movw    r2, #TYPE_SLOT_7_MIN
    cmp     r1, r2
    bhs     stage_slot
    movs    r3, #7

stage_slot:
    ldr     r0, =SRC_GPR10
    ldr     r2, =FACTORY_REQUEST_MAGIC
    orrs    r3, r3, r2
    str     r3, [r0]

reset_after_release:
    ldr     r0, =SRC_GPR7
    movs    r1, #0
    str     r1, [r0]

    /* Reset only after release so boot recovery does not see a held switch. */
    dsb     sy
    ldr     r0, =SCB_AIRCR
    ldr     r1, =AIRCR_SYSRESETREQ
    str     r1, [r0]
    dsb     sy
reset_wait:
    b       reset_wait

clear_counter:
    movs    r1, #0
    str     r1, [r0]

chain_factory_led:
    /* LR still points after the patched factory BL. */
    adr     r0, ncr2_factory_return_monitor_original_led
    ldr     r0, [r0]
    bx      r0
.size ncr2_factory_return_monitor, . - ncr2_factory_return_monitor
.ltorg

/* Per-engine Thumb entry filled only after the blob reaches ITCM. */
.org 0xfc
.align 2
.global ncr2_factory_return_monitor_original_led
ncr2_factory_return_monitor_original_led:
    .word 0

.global ncr2_factory_return_monitor_end
ncr2_factory_return_monitor_end:
