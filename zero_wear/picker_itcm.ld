ENTRY(Reset_Handler)

MEMORY
{
    ITCM (rx) : ORIGIN = 0x00000000, LENGTH = 0x00001000
}

SECTIONS
{
    .vectors 0x00000000 :
    {
        KEEP(*(.vectors))
    } > ITCM

    .text :
    {
        *(.text*)
        *(.rodata*)
    } > ITCM

    /DISCARD/ :
    {
        *(.ARM.attributes)
        *(.comment)
        *(.data*)
        *(.bss*)
    }

    ASSERT(SIZEOF(.vectors) == (176 * 4),
           "picker vector table must contain 176 entries")
    ASSERT(SIZEOF(.vectors) + SIZEOF(.text) <= LENGTH(ITCM),
           "picker ITCM image overlaps the XIP runtime storage offset")
}

