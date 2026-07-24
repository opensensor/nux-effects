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
