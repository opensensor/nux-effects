#include "ncr2_board.h"

#include <stddef.h>
#include <stdint.h>

#include "fsl_clock.h"
#include "fsl_common.h"
#include "fsl_gpio.h"
#include "fsl_iomuxc.h"
#include "ncr2_flash_layout.h"
#include "recovery_usb.h"
#include "usb.h"
#include "usb_phy.h"

#define NCR2_USB_PHY_D_CAL UINT8_C(0x0C)
#define NCR2_USB_PHY_TXCAL45DP UINT8_C(0x06)
#define NCR2_USB_PHY_TXCAL45DM UINT8_C(0x06)

/*
 * 100 kOhm pull-up, pull enabled, hysteresis enabled. This establishes
 * deterministic released levels before sampling the active-low primary
 * recovery input and the active-high guard input.
 */
#define NCR2_RECOVERY_PAD_CONFIG \
    (IOMUXC_SW_PAD_CTL_PAD_HYS_MASK | \
     IOMUXC_SW_PAD_CTL_PAD_PKE_MASK | \
     IOMUXC_SW_PAD_CTL_PAD_PUE_MASK | \
     IOMUXC_SW_PAD_CTL_PAD_PUS(2U))

#define NCR2_RECOVERY_INDICATOR_MASK \
    ((UINT32_C(1) << 23) | \
     (UINT32_C(1) << 24) | \
     (UINT32_C(1) << 25) | \
     (UINT32_C(1) << 26) | \
     (UINT32_C(1) << 27))
#define NCR2_RECOVERY_INDICATOR_PAD_CONFIG UINT32_C(0x70b0)
#define NCR2_RECOVERY_TICK_HZ UINT32_C(100)
#define NCR2_RECOVERY_BLINK_TICKS UINT32_C(50)

static volatile uint32_t g_recovery_indicator_ticks;

void ncr2_board_recovery_input_init(void)
{
    const gpio_pin_config_t input = {
        .direction = kGPIO_DigitalInput,
        .outputLogic = UINT8_C(0),
        .interruptMode = kGPIO_NoIntmode,
    };

    CLOCK_EnableClock(kCLOCK_Iomuxc);
    CLOCK_EnableClock(kCLOCK_Gpio1);
    CLOCK_EnableClock(kCLOCK_Gpio3);

    IOMUXC_SetPinMux(
        IOMUXC_GPIO_AD_B1_05_GPIO1_IO21,
        1U);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_AD_B1_05_GPIO1_IO21,
        NCR2_RECOVERY_PAD_CONFIG);
    IOMUXC_SetPinMux(
        IOMUXC_GPIO_SD_B1_02_GPIO3_IO02,
        1U);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_SD_B1_02_GPIO3_IO02,
        NCR2_RECOVERY_PAD_CONFIG);

    GPIO_PinInit(
        GPIO1,
        NCR2_RECOVERY_PRIMARY_GPIO_PIN,
        &input);
    GPIO_PinInit(
        GPIO3,
        NCR2_RECOVERY_GUARD_GPIO_PIN,
        &input);
}

int ncr2_board_recovery_requested(void *context)
{
    uint32_t primary;
    uint32_t guard;

    (void)context;
    primary =
        GPIO_PinRead(
            GPIO1,
            NCR2_RECOVERY_PRIMARY_GPIO_PIN);
    guard =
        GPIO_PinRead(
            GPIO3,
            NCR2_RECOVERY_GUARD_GPIO_PIN);
    return primary == UINT32_C(0) &&
           guard != UINT32_C(0);
}

void ncr2_board_make_recovery_request(
    boot_recovery_request_t *request)
{
    if (request == NULL) {
        return;
    }
    request->mailbox =
        (boot_recovery_mailbox_t *)(uintptr_t)
            NCR2_BOOT_MAILBOX_ADDRESS;
    request->physical_context = NULL;
    request->physical_asserted =
        ncr2_board_recovery_requested;
}

uint16_t ncr2_board_usb_clock_init(void)
{
    usb_phy_config_struct_t phy = {
        .D_CAL = NCR2_USB_PHY_D_CAL,
        .TXCAL45DP = NCR2_USB_PHY_TXCAL45DP,
        .TXCAL45DM = NCR2_USB_PHY_TXCAL45DM,
    };

    if (!CLOCK_EnableUsbhs0PhyPllClock(
            kCLOCK_Usbphy480M,
            UINT32_C(480000000)) ||
        !CLOCK_EnableUsbhs0Clock(
            kCLOCK_Usb480M,
            UINT32_C(480000000))) {
        return NCR2_BOARD_USB_CLOCK_FAILED;
    }
    if (USB_EhciPhyInit(
            (uint8_t)kUSB_ControllerEhci0,
            NCR2_BOARD_XTAL_HZ,
            &phy) != (uint32_t)kStatus_USB_Success) {
        return NCR2_BOARD_USB_PHY_FAILED;
    }
    return NCR2_BOARD_OK;
}

void ncr2_board_usb_irq_enable(void)
{
    NVIC_ClearPendingIRQ(USB_OTG1_IRQn);
    NVIC_SetPriority(
        USB_OTG1_IRQn,
        NCR2_BOARD_USB_IRQ_PRIORITY);
    EnableIRQ(USB_OTG1_IRQn);
}

void ncr2_board_recovery_indicator_init(void)
{
    const uint32_t core_clock = SystemCoreClock;
    uint32_t reload;

    CLOCK_EnableClock(kCLOCK_Iomuxc);
    CLOCK_EnableClock(kCLOCK_Gpio2);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_07_GPIO2_IO23, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_08_GPIO2_IO24, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_09_GPIO2_IO25, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_10_GPIO2_IO26, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_11_GPIO2_IO27, 0U);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B1_07_GPIO2_IO23,
        NCR2_RECOVERY_INDICATOR_PAD_CONFIG);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B1_08_GPIO2_IO24,
        NCR2_RECOVERY_INDICATOR_PAD_CONFIG);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B1_09_GPIO2_IO25,
        NCR2_RECOVERY_INDICATOR_PAD_CONFIG);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B1_10_GPIO2_IO26,
        NCR2_RECOVERY_INDICATOR_PAD_CONFIG);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_B1_11_GPIO2_IO27,
        NCR2_RECOVERY_INDICATOR_PAD_CONFIG);

    GPIO2->DR_CLEAR = NCR2_RECOVERY_INDICATOR_MASK;
    GPIO2->GDIR |= NCR2_RECOVERY_INDICATOR_MASK;
    g_recovery_indicator_ticks = UINT32_C(0);

    if (core_clock == UINT32_C(0)) {
        return;
    }
    reload = core_clock / NCR2_RECOVERY_TICK_HZ;
    if (reload == UINT32_C(0) ||
        reload - UINT32_C(1) > SysTick_LOAD_RELOAD_Msk) {
        return;
    }
    SysTick->LOAD = reload - UINT32_C(1);
    SysTick->VAL = UINT32_C(0);
    SysTick->CTRL =
        SysTick_CTRL_CLKSOURCE_Msk |
        SysTick_CTRL_TICKINT_Msk |
        SysTick_CTRL_ENABLE_Msk;
}

void SysTick_Handler(void)
{
    ++g_recovery_indicator_ticks;
    if (g_recovery_indicator_ticks >=
        NCR2_RECOVERY_BLINK_TICKS) {
        g_recovery_indicator_ticks = UINT32_C(0);
        GPIO2->DR_TOGGLE = NCR2_RECOVERY_INDICATOR_MASK;
    }
}

void USB_OTG1_IRQHandler(void)
{
    ncr2_recovery_usb_isr();
}

__attribute__((noreturn))
void ncr2_board_warm_reset(void *context)
{
    (void)context;
    __disable_irq();
    __DSB();
    NVIC_SystemReset();
    for (;;) {
        __WFI();
    }
}
