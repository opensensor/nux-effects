ENTRY(gpt1_long_hold_monitor)

MEMORY
{
    ITCM_CAVE (rx) : ORIGIN = 0x0001df00, LENGTH = 0x00000100
}

SECTIONS
{
    .monitor 0x0001df00 :
    {
        KEEP(*(.monitor))
    } > ITCM_CAVE

    /DISCARD/ :
    {
        *(.ARM.attributes)
        *(.comment)
        *(.data*)
        *(.bss*)
    }

    ASSERT(SIZEOF(.monitor) <= LENGTH(ITCM_CAVE),
           "engine monitor exceeds the verified zero-filled code cave")
}
