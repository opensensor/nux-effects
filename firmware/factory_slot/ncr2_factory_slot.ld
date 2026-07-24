ENTRY(Reset_Handler)

/*
 * The factory Core Deluxe launcher copies 0x1e000 bytes from the selected
 * 0x20000-byte NOR engine slot into ITCM at address zero, then enters the
 * copied vector table. Keep the remaining 0x2000 bytes outside the executable
 * contract.
 */
MEMORY
{
    ITCM (rx)   : ORIGIN = 0x00000000, LENGTH = 0x0001e000
    DTCM (rwx)  : ORIGIN = 0x20000000, LENGTH = 0x00020000
}

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(512);
        KEEP(*(.isr_vector))
    } > ITCM

    .text :
    {
        *(.text*)
        *(.rodata*)
        *(.glue_7)
        *(.glue_7t)
        *(.vfp11_veneer)
        *(.v4_bx)
        *(.iplt)
        *(.igot.plt)
        *(.rel.iplt)
        . = ALIGN(4);
    } > ITCM

    .data :
    {
        . = ALIGN(4);
        __data_start = .;
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } > DTCM AT > ITCM
    __data_load = LOADADDR(.data);

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > DTCM

    __stack_top = ORIGIN(DTCM) + LENGTH(DTCM);
    __factory_slot_image_end = LOADADDR(.data) + SIZEOF(.data);

    .ARM.attributes 0 :
    {
        *(.ARM.attributes)
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
    }

    ASSERT(__factory_slot_image_end <= ORIGIN(ITCM) + LENGTH(ITCM),
           "factory-slot application exceeds the 0x1e000-byte copy budget")
}
