#include "ncr2_board.h"

#include <stddef.h>
#include <stdint.h>

#include "MIMXRT1051.h"
#include "fsl_clock.h"
#include "fsl_common.h"
#include "fsl_gpio.h"
#include "fsl_iomuxc.h"
#include "ncr2_flash_layout.h"
#include "recovery_engine.h"
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

/*
 * The stock Reverb engine scans ADC1 channels 5, 8, 9 and 11 in that order
 * and treats them as Decay, Tweak, Type and Level. Only the Type channel is
 * a stepped ladder; recovery samples all four so a host can confirm the whole
 * scan is alive before trusting any single reading.
 */
static const uint8_t g_knob_channel[NCR2_BOARD_KNOB_COUNT] = {
    5U, 8U, 9U, 11U,
};

/* 100 kOhm keeper off, no pull, hysteresis off: the stock analog pad value. */
#define NCR2_KNOB_PAD_CONFIG UINT32_C(0x000000b0)
#define NCR2_KNOB_ADC_TIMEOUT UINT32_C(100000)
#define NCR2_KNOB_DISABLED_CHANNEL 31U

static uint8_t g_knob_adc_ready;
static uint32_t g_knob_sample_index;

static void knob_pads_init(void)
{
    CLOCK_EnableClock(kCLOCK_Iomuxc);
    CLOCK_EnableClock(kCLOCK_Gpio1);
    CLOCK_EnableClock(kCLOCK_Adc1);

    /*
     * The analog connection is live while the digital mux stays GPIO, which
     * is exactly how the stock image leaves these pads. Every direction is
     * forced to input so recovery can never drive a control pin.
     */
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_00_GPIO1_IO16, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_03_GPIO1_IO19, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_04_GPIO1_IO20, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_06_GPIO1_IO22, 0U);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_AD_B1_00_GPIO1_IO16, NCR2_KNOB_PAD_CONFIG);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_AD_B1_03_GPIO1_IO19, NCR2_KNOB_PAD_CONFIG);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_AD_B1_04_GPIO1_IO20, NCR2_KNOB_PAD_CONFIG);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_AD_B1_06_GPIO1_IO22, NCR2_KNOB_PAD_CONFIG);
    GPIO1->GDIR &= ~(
        (UINT32_C(1) << 16) |
        (UINT32_C(1) << 19) |
        (UINT32_C(1) << 20) |
        (UINT32_C(1) << 22));
}

static uint8_t knob_adc_init(void)
{
    knob_pads_init();

    /* 12-bit, asynchronous clock, 32-sample hardware averaging. */
    ADC1->CFG =
        ADC_CFG_ADICLK(3U) |
        ADC_CFG_MODE(2U) |
        ADC_CFG_AVGS(3U);
    ADC1->GC =
        ADC_GC_ADACKEN(1U) |
        ADC_GC_AVGE(1U);
    for (uint32_t index = UINT32_C(0);
         index < ADC_HC_COUNT;
         ++index) {
        ADC1->HC[index] = ADC_HC_ADCH(NCR2_KNOB_DISABLED_CHANNEL);
    }

    ADC1->GS = ADC_GS_CALF_MASK;
    ADC1->GC |= ADC_GC_CAL_MASK;
    for (uint32_t timeout = UINT32_C(0);
         timeout < NCR2_KNOB_ADC_TIMEOUT;
         ++timeout) {
        if ((ADC1->GC & ADC_GC_CAL_MASK) == UINT32_C(0)) {
            const uint8_t valid =
                ((ADC1->GS & ADC_GS_CALF_MASK) == UINT32_C(0) &&
                 (ADC1->HS & ADC_HS_COCO0_MASK) != UINT32_C(0))
                    ? UINT8_C(1)
                    : UINT8_C(0);

            /* Calibration leaves COCO0 set; clear it before the first read. */
            (void)ADC1->R[0];
            ADC1->HC[0] =
                ADC_HC_ADCH(NCR2_KNOB_DISABLED_CHANNEL);
            return valid;
        }
    }
    return UINT8_C(0);
}

static uint8_t knob_adc_read(uint8_t channel, uint16_t *value)
{
    ADC1->HC[0] = ADC_HC_ADCH(channel);
    for (uint32_t timeout = UINT32_C(0);
         timeout < NCR2_KNOB_ADC_TIMEOUT;
         ++timeout) {
        if ((ADC1->HS & ADC_HS_COCO0_MASK) != UINT32_C(0)) {
            *value = (uint16_t)(ADC1->R[0] & ADC_R_CDATA_MASK);
            return UINT8_C(1);
        }
    }
    ADC1->HC[0] = ADC_HC_ADCH(NCR2_KNOB_DISABLED_CHANNEL);
    return UINT8_C(0);
}

/*
 * Fill every field the host needs even when a conversion fails, so a failed
 * capture is still a well-formed payload carrying valid == 0 rather than an
 * error the host has to guess the meaning of.
 */
static uint8_t knob_capture(recovery_knob_sample_t *sample)
{
    uint16_t selector = UINT16_C(0);

    for (uint32_t index = UINT32_C(0);
         index < sizeof(*sample);
         ++index) {
        ((uint8_t *)sample)[index] = UINT8_C(0);
    }
    sample->magic = RECOVERY_KNOB_SAMPLE_MAGIC;
    sample->burst = (uint8_t)NCR2_BOARD_KNOB_BURST;
    sample->adc_bits = UINT8_C(12);
    for (uint32_t index = UINT32_C(0);
         index < NCR2_BOARD_KNOB_COUNT;
         ++index) {
        sample->channel[index] = g_knob_channel[index];
    }

    if (g_knob_adc_ready == UINT8_C(0)) {
        g_knob_adc_ready = knob_adc_init();
        if (g_knob_adc_ready == UINT8_C(0)) {
            return UINT8_C(0);
        }
    }

    /*
     * The payload is packed for the wire, so its members cannot be handed
     * out by address. Convert through an aligned local instead.
     */
    for (uint32_t index = UINT32_C(0);
         index < NCR2_BOARD_KNOB_COUNT;
         ++index) {
        uint16_t knob = UINT16_C(0);

        if (knob_adc_read(g_knob_channel[index], &knob) == UINT8_C(0)) {
            return UINT8_C(0);
        }
        sample->value[index] = knob;
    }

    /*
     * Burst the selector alone. The spread across a stationary detent is the
     * measurement that sets a movement threshold no resting knob can trip.
     */
    sample->selector_min = UINT16_MAX;
    sample->selector_max = UINT16_C(0);
    for (uint32_t index = UINT32_C(0);
         index < NCR2_BOARD_KNOB_BURST;
         ++index) {
        if (knob_adc_read(
                g_knob_channel[NCR2_BOARD_KNOB_SELECTOR_INDEX],
                &selector) == UINT8_C(0)) {
            sample->selector_min = UINT16_C(0);
            sample->selector_max = UINT16_C(0);
            return UINT8_C(0);
        }
        if (selector < sample->selector_min) {
            sample->selector_min = selector;
        }
        if (selector > sample->selector_max) {
            sample->selector_max = selector;
        }
    }
    sample->value[NCR2_BOARD_KNOB_SELECTOR_INDEX] = selector;
    return UINT8_C(1);
}

int ncr2_board_recovery_read_knobs(
    void *context,
    void *destination,
    uint32_t capacity)
{
    recovery_knob_sample_t sample;

    (void)context;
    if (destination == NULL ||
        capacity < sizeof(sample)) {
        return -1;
    }

    sample.valid = knob_capture(&sample);
    sample.sample_index = ++g_knob_sample_index;
    ADC1->HC[0] = ADC_HC_ADCH(NCR2_KNOB_DISABLED_CHANNEL);

    for (uint32_t index = UINT32_C(0);
         index < sizeof(sample);
         ++index) {
        ((uint8_t *)destination)[index] =
            ((const uint8_t *)&sample)[index];
    }
    return (int)sizeof(sample);
}
