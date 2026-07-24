/*
 * Copyright-neutral reconstruction of the NCR-2 factory launcher's
 * pre-engine hardware preparation.
 *
 * The values below were recovered from the user's verified factory image.
 * This source contains no copied factory instructions or data.
 */

#include <stdint.h>

#include "MIMXRT1051.h"
#include "fsl_clock.h"
#include "fsl_iomuxc.h"

#ifndef NCR2_FACTORY_COMPAT_DIAGNOSTIC
#define NCR2_FACTORY_COMPAT_DIAGNOSTIC 0
#endif

#define NCR2_FACTORY_ARM_PLL_DIVIDER UINT32_C(100)
#define NCR2_FACTORY_DCDC_TARGET UINT32_C(0x12)

#define NCR2_DIAGNOSTIC_GPIO_MASK \
    ((UINT32_C(1) << 23) | \
     (UINT32_C(1) << 24) | \
     (UINT32_C(1) << 25) | \
     (UINT32_C(1) << 26) | \
     (UINT32_C(1) << 27))
#define NCR2_DIAGNOSTIC_HALF_PERIOD_CYCLES UINT32_C(60000000)

static void configure_factory_mpu(void)
{
    SCB_DisableICache();
    SCB_DisableDCache();
    ARM_MPU_Disable();

    MPU->RBAR = UINT32_C(0xc0000010);
    MPU->RASR = UINT32_C(0x03100039);
    MPU->RBAR = UINT32_C(0x80000011);
    MPU->RASR = UINT32_C(0x0310003b);
    MPU->RBAR = UINT32_C(0x60000012);
    MPU->RASR = UINT32_C(0x03030039);
    MPU->RBAR = UINT32_C(0x00000013);
    MPU->RASR = UINT32_C(0x0310003b);
    MPU->RBAR = UINT32_C(0x00000014);
    MPU->RASR = UINT32_C(0x03030021);
    MPU->RBAR = UINT32_C(0x20000015);
    MPU->RASR = UINT32_C(0x03030021);
    MPU->RBAR = UINT32_C(0x20200016);
    MPU->RASR = UINT32_C(0x03030023);

    ARM_MPU_Enable(MPU_CTRL_PRIVDEFENA_Msk);
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
}

static void configure_factory_pin_mux(void)
{
    CLOCK_EnableClock(kCLOCK_Iomuxc);
    CLOCK_EnableClock(kCLOCK_IomuxcSnvs);
    CLOCK_EnableClock(kCLOCK_Gpio1);
    CLOCK_EnableClock(kCLOCK_Gpio2);
    CLOCK_EnableClock(kCLOCK_Gpio3);

    IOMUXC_SetPinMux(
        IOMUXC_GPIO_AD_B1_05_GPIO1_IO21,
        0U);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_AD_B1_05_GPIO1_IO21,
        UINT32_C(0x70b0));
    IOMUXC_SetPinMux(
        IOMUXC_GPIO_SD_B1_02_GPIO3_IO02,
        0U);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_SD_B1_02_GPIO3_IO02,
        UINT32_C(0x70b0));
    IOMUXC_SetPinMux(
        IOMUXC_GPIO_B0_07_GPIO2_IO07,
        0U);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B0_07_GPIO2_IO07,
        UINT32_C(0xd0b0));
    IOMUXC_SetPinMux(
        IOMUXC_GPIO_B0_08_GPIO2_IO08,
        0U);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B0_08_GPIO2_IO08,
        UINT32_C(0xd0b0));
    IOMUXC_SetPinMux(
        IOMUXC_GPIO_B1_14_GPIO2_IO30,
        0U);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B1_14_GPIO2_IO30,
        UINT32_C(0x70b0));
    IOMUXC_SetPinMux(
        IOMUXC_GPIO_B1_15_GPIO2_IO31,
        0U);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B1_15_GPIO2_IO31,
        UINT32_C(0x70b0));
    IOMUXC_SetPinMux(
        IOMUXC_GPIO_AD_B1_10_GPIO1_IO26,
        0U);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_AD_B1_10_GPIO1_IO26,
        UINT32_C(0xf0b0));
    IOMUXC_SetPinMux(
        IOMUXC_GPIO_B0_11_GPIO2_IO11,
        0U);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B0_11_GPIO2_IO11,
        UINT32_C(0x70b0));

    GPIO1->GDIR &= ~(UINT32_C(1) << 21);
    GPIO3->GDIR &= ~(UINT32_C(1) << 2);
}

static void configure_factory_core_clock(void)
{
    const clock_arm_pll_config_t arm_pll = {
        .loopDivider = NCR2_FACTORY_ARM_PLL_DIVIDER,
        .src = (uint8_t)kCLOCK_PllClkSrc24M,
    };

    CLOCK_SetXtalFreq(UINT32_C(24000000));
    CLOCK_SetRtcXtalFreq(UINT32_C(32768));

    /*
     * Move the core clock to the crystal while changing ARM PLL.  The
     * factory launcher also reinitialized SYS PLL, but this bridge executes
     * from SDRAM whose SEMC clock is sourced by SYS PLL.  The stock DCD has
     * already initialized that PLL, so touching it here is both redundant
     * and unsafe.
     */
    CLOCK_SetMux(kCLOCK_PeriphClk2Mux, 1U);
    CLOCK_SetMux(kCLOCK_PeriphMux, 1U);

    DCDC->REG3 =
        (DCDC->REG3 & ~DCDC_REG3_TRG_MASK) |
        DCDC_REG3_TRG(NCR2_FACTORY_DCDC_TARGET);
    while ((DCDC->REG0 & DCDC_REG0_STS_DC_OK_MASK) == 0U) {
    }

    CLOCK_InitArmPll(&arm_pll);
    CLOCK_SetDiv(kCLOCK_ArmDiv, 1U);
    CLOCK_SetDiv(kCLOCK_AhbDiv, 0U);
    CLOCK_SetDiv(kCLOCK_IpgDiv, 3U);
    CLOCK_SetMux(kCLOCK_PrePeriphMux, 3U);
    CLOCK_SetMux(kCLOCK_PeriphMux, 0U);
    CLOCK_SetMux(kCLOCK_UartMux, 0U);
    CLOCK_SetDiv(kCLOCK_UartDiv, 0U);
}

static void enable_cycle_counter(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

#if NCR2_FACTORY_COMPAT_DIAGNOSTIC
static void wait_cycles(uint32_t cycles)
{
    const uint32_t start = DWT->CYCCNT;

    while ((uint32_t)(DWT->CYCCNT - start) < cycles) {
        __NOP();
    }
}

static void pulse_candidate_indicators(void)
{
    const uint32_t saved_direction = GPIO2->GDIR;
    const uint32_t saved_output = GPIO2->DR;

    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_07_GPIO2_IO23, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_08_GPIO2_IO24, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_09_GPIO2_IO25, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_10_GPIO2_IO26, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_11_GPIO2_IO27, 0U);
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
    GPIO2->GDIR = saved_direction | NCR2_DIAGNOSTIC_GPIO_MASK;
    for (uint32_t count = 0U; count < 3U; ++count) {
        GPIO2->DR = saved_output & ~NCR2_DIAGNOSTIC_GPIO_MASK;
        __DSB();
        wait_cycles(NCR2_DIAGNOSTIC_HALF_PERIOD_CYCLES);
        GPIO2->DR = saved_output | NCR2_DIAGNOSTIC_GPIO_MASK;
        __DSB();
        wait_cycles(NCR2_DIAGNOSTIC_HALF_PERIOD_CYCLES);
    }
    GPIO2->DR = saved_output;
    GPIO2->GDIR = saved_direction;
    __DSB();
}
#endif

void ncr2_factory_compat_prepare(void)
{
    configure_factory_mpu();
    configure_factory_pin_mux();
    configure_factory_core_clock();
    enable_cycle_counter();
#if NCR2_FACTORY_COMPAT_DIAGNOSTIC
    pulse_candidate_indicators();
#endif
}
