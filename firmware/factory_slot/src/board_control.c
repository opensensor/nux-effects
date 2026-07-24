#include "factory_board.h"

#include <stdint.h>

#include "MIMXRT1051.h"
#include "fsl_clock.h"
#include "fsl_iomuxc.h"

#ifndef NCR2_FACTORY_SLOT_BOARD_CONTROLS
#define NCR2_FACTORY_SLOT_BOARD_CONTROLS 0
#endif

#if NCR2_FACTORY_SLOT_BOARD_CONTROLS

#define NCR2_GPIO1_IO24_MASK (UINT32_C(1) << 24)
#define NCR2_GPIO1_IO26_MASK (UINT32_C(1) << 26)
#define NCR2_GPIO1_IO31_MASK (UINT32_C(1) << 31)
#define NCR2_GPIO1_CONTROL_MASK \
    (NCR2_GPIO1_IO24_MASK | \
     NCR2_GPIO1_IO26_MASK | \
     NCR2_GPIO1_IO31_MASK)
#define NCR2_GPIO1_INITIAL_HIGH \
    (NCR2_GPIO1_IO24_MASK | NCR2_GPIO1_IO31_MASK)

#define NCR2_GPIO2_IO11_MASK (UINT32_C(1) << 11)
#define NCR2_GPIO2_IO23_MASK (UINT32_C(1) << 23)
#define NCR2_GPIO2_IO24_MASK (UINT32_C(1) << 24)
#define NCR2_GPIO2_IO25_MASK (UINT32_C(1) << 25)
#define NCR2_GPIO2_IO26_MASK (UINT32_C(1) << 26)
#define NCR2_GPIO2_IO27_MASK (UINT32_C(1) << 27)
#define NCR2_GPIO2_CONTROL_MASK \
    (NCR2_GPIO2_IO11_MASK | \
     NCR2_GPIO2_IO23_MASK | \
     NCR2_GPIO2_IO24_MASK | \
     NCR2_GPIO2_IO25_MASK | \
     NCR2_GPIO2_IO26_MASK | \
     NCR2_GPIO2_IO27_MASK)
#define NCR2_GPIO2_INITIAL_HIGH NCR2_GPIO2_IO26_MASK

#define NCR2_BOARD_RELEASE_DELAY_US UINT32_C(100000)
#define NCR2_MICROSECONDS_PER_SECOND UINT32_C(1000000)

static void configure_control_pins(void)
{
    CLOCK_EnableClock(kCLOCK_Iomuxc);

    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_08_GPIO1_IO24, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_10_GPIO1_IO26, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_15_GPIO1_IO31, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B0_11_GPIO2_IO11, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_07_GPIO2_IO23, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_08_GPIO2_IO24, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_09_GPIO2_IO25, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_10_GPIO2_IO26, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_11_GPIO2_IO27, 0U);

    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_AD_B1_08_GPIO1_IO24,
        UINT32_C(0x10b0));
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_AD_B1_10_GPIO1_IO26,
        UINT32_C(0xf0b0));
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_AD_B1_15_GPIO1_IO31,
        UINT32_C(0x10b0));
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B0_11_GPIO2_IO11,
        UINT32_C(0x70b0));
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B1_07_GPIO2_IO23,
        UINT32_C(0x70b0));
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B1_08_GPIO2_IO24,
        UINT32_C(0x70b0));
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B1_09_GPIO2_IO25,
        UINT32_C(0x70b0));
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B1_10_GPIO2_IO26,
        UINT32_C(0x70b0));
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B1_11_GPIO2_IO27,
        UINT32_C(0x70b0));
}

static void delay_microseconds(uint32_t microseconds)
{
    const uint32_t cycles_per_microsecond =
        SystemCoreClock / NCR2_MICROSECONDS_PER_SECOND;
    const uint32_t target =
        cycles_per_microsecond * microseconds;
    const uint32_t start = DWT->CYCCNT;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    while ((uint32_t)(DWT->CYCCNT - start) < target) {
        __NOP();
    }
}

#endif

uint16_t ncr2_factory_board_prepare_audio(void)
{
#if NCR2_FACTORY_SLOT_BOARD_CONTROLS
    configure_control_pins();

    GPIO1->DR =
        (GPIO1->DR & ~NCR2_GPIO1_CONTROL_MASK) |
        NCR2_GPIO1_INITIAL_HIGH;
    GPIO2->DR =
        (GPIO2->DR & ~NCR2_GPIO2_CONTROL_MASK) |
        NCR2_GPIO2_INITIAL_HIGH;
    GPIO1->GDIR |= NCR2_GPIO1_CONTROL_MASK;
    GPIO2->GDIR |= NCR2_GPIO2_CONTROL_MASK;
    return NCR2_FACTORY_BOARD_OK;
#else
    return NCR2_FACTORY_BOARD_DISABLED;
#endif
}

void ncr2_factory_board_release_audio(void)
{
#if NCR2_FACTORY_SLOT_BOARD_CONTROLS
    delay_microseconds(NCR2_BOARD_RELEASE_DELAY_US);
    GPIO1->DR |= NCR2_GPIO1_IO26_MASK;
    __DSB();
#endif
}
