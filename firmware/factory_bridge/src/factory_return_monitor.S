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
.equ OPEN_HOLD_LOOPS,        5000

/* Midpoints between the measured pairs of the eight physical detents. */
.equ TYPE_DELAY_MIN,         2815
.equ TYPE_REVERB_MIN,        1793
.equ TYPE_MOD_MIN,           772

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

    /* Five seconds always returns to Open Effects. */
    ldr     r0, =SRC_GPR10
    movw    r2, #OPEN_HOLD_LOOPS
    cmp     r1, r2
    bhs     stage_open

    /*
     * A two-to-five-second hold selects a factory engine with the Type knob:
     * detents 1-2 Delay, 3-4 Reverb, 5-6 Modulation, 7-8 fourth engine.
     */
    ldr     r2, =TYPE_ADC_RESULT_3_2
    ldr     r1, [r2]
    ubfx    r1, r1, #0, #12
    movw    r2, #TYPE_DELAY_MIN
    cmp     r1, r2
    bhs     select_delay
    movw    r2, #TYPE_REVERB_MIN
    cmp     r1, r2
    bhs     select_reverb
    movw    r2, #TYPE_MOD_MIN
    cmp     r1, r2
    bhs     select_mod
    movs    r1, #3
    b       stage_factory

select_delay:
    movs    r1, #0
    b       stage_factory
select_reverb:
    movs    r1, #1
    b       stage_factory
select_mod:
    movs    r1, #2

stage_factory:
    ldr     r2, =FACTORY_REQUEST_MAGIC
    orrs    r1, r1, r2
    str     r1, [r0]
    b       reset_after_release

stage_open:
    movs    r1, #0
    str     r1, [r0]

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
