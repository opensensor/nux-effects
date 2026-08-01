/* Copy a validated engine to ITCM, then separately sync and enter it. */

.syntax unified
.cpu cortex-m7
.fpu fpv5-d16
.thumb

.equ FACTORY_ENGINE_COPY_SIZE, 0x0001e000
.equ SCB_ICIALLU,              0xe000ef50

.section .text.ncr2_factory_engine_copy_to_itcm, "ax", %progbits
.align 2
.global ncr2_factory_engine_copy_to_itcm
.type ncr2_factory_engine_copy_to_itcm, %function
.thumb_func
ncr2_factory_engine_copy_to_itcm:
    cpsid   i
    mov     r3, r0
    movs    r0, #0
    ldr     r2, =FACTORY_ENGINE_COPY_SIZE
    lsrs    r2, r2, #2
1:
    ldr     r12, [r3], #4
    str     r12, [r0], #4
    subs    r2, r2, #1
    bne     1b
    dsb     sy
    bx      lr
.size ncr2_factory_engine_copy_to_itcm, . - ncr2_factory_engine_copy_to_itcm

.section .text.ncr2_factory_engine_sync_and_jump, "ax", %progbits
.align 2
.global ncr2_factory_engine_sync_and_jump
.type ncr2_factory_engine_sync_and_jump, %function
.thumb_func
ncr2_factory_engine_sync_and_jump:
    dsb     sy
    isb     sy
    mov     r1, r0
    ldr     r0, =SCB_ICIALLU
    movs    r2, #0
    str     r2, [r0]
    dsb     sy
    isb     sy

    /* The proprietary reset handler installs its own VTOR and MSP. */
    bx      r1
.size ncr2_factory_engine_sync_and_jump, . - ncr2_factory_engine_sync_and_jump
