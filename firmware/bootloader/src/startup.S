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
    .rept 174
    .word Default_Handler
    .endr
.size g_boot_vectors, . - g_boot_vectors

.section .text.Reset_Handler, "ax", %progbits
.align 2
.global Reset_Handler
.type Reset_Handler, %function
.thumb_func
Reset_Handler:
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
