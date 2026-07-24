/*
 * Flash-resident copy/handoff stub for the Core Deluxe engine picker.
 *
 * This code lives in the picker source slot at 0x60081000 and is never
 * overwritten when an engine is copied into ITCM address 0.
 */

.syntax unified
.cpu cortex-m7
.thumb

.equ LAUNCHER_MEMCPY,      0x600029f9
.equ LAUNCHER_HANDOFF,     0x60003ff9

.section .xip_loader, "ax", %progbits
.align 2
.global xip_copy_handoff
.type xip_copy_handoff, %function
.thumb_func
xip_copy_handoff:
    /* r0=0 (ITCM destination), r1=engine XIP source, r2=0x1e000. */
    ldr     r3, =LAUNCHER_MEMCPY
    blx     r3
    dsb     sy
    isb     sy
    ldr     r3, =LAUNCHER_HANDOFF
    blx     r3
1:
    b       1b
.size xip_copy_handoff, . - xip_copy_handoff
