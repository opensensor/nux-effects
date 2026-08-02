#include <stddef.h>
#include <stdint.h>

#include "factory_audio.h"
#include "factory_board.h"
#include "MIMXRT1051.h"
#include "fsl_clock.h"
#include "fsl_iomuxc.h"

#include "codec_probe.h"
#include "footswitch_gesture.h"
#include "usb_audio_capture.h"

#include "boot_recovery_request.h"
#include "boot_trial.h"
#include "factory_engine_launcher.h"
#include "factory_engine_request.h"
#include "ncr2_flash_layout.h"

/* RT1051 WDOG1 service sequence and an eight second timeout, (WT+1)/2. */
#define NCR2_WDOG_REFRESH_FIRST UINT16_C(0x5555)
#define NCR2_WDOG_REFRESH_SECOND UINT16_C(0xAAAA)
#define NCR2_WDOG_TIMEOUT_VALUE UINT16_C(0x000F)

#define NCR2_DIAGNOSTIC_GAIN INT32_C(4)
#define NCR2_DIAGNOSTIC_LIMIT INT64_C(0x10000000)
#define NCR2_LED_LEAD_IN_MS UINT32_C(2000)
#define NCR2_LED_PULSE_MS UINT32_C(260)
#define NCR2_LED_GAP_MS UINT32_C(260)
#define NCR2_LED_GROUP_MS UINT32_C(1400)

/* Confirmed on hardware: both indicator pins idle high and light when low. */
#define NCR2_LED_GREEN NCR2_FACTORY_INDICATOR_IO24
#define NCR2_LED_RED NCR2_FACTORY_INDICATOR_IO31
#define NCR2_LED_BOTH NCR2_FACTORY_INDICATOR_BOTH_LOW
#define NCR2_LED_OFF NCR2_FACTORY_INDICATOR_OFF

#define NCR2_LED_HOLD_MS UINT32_C(2500)
#define NCR2_LED_REST_MS UINT32_C(800)
#define NCR2_LED_ALTERNATE_MS UINT32_C(200)
#define NCR2_LED_ALTERNATE_COUNT UINT32_C(6)

/* The physical audio path is proven; keep lengthy blink sweeps in bring-up. */
#define NCR2_RUNTIME_DIAGNOSTIC_FLASHES 0

#define NCR2_LABEL_ON_MS UINT32_C(180)
#define NCR2_LABEL_OFF_MS UINT32_C(220)
#define NCR2_LABEL_SETTLE_MS UINT32_C(700)
#define NCR2_BIT_ON_MS UINT32_C(320)
#define NCR2_BIT_OFF_MS UINT32_C(220)
#define NCR2_NIBBLE_GAP_MS UINT32_C(650)

/*
 * A latching relay coil expects a short pulse, not a steady drive, so the
 * relay hunt energises each candidate only briefly and rests between tries.
 */
#define NCR2_RELAY_PULSE_MS UINT32_C(60)
#define NCR2_RELAY_REST_MS UINT32_C(500)
#define NCR2_RELAY_PULSE_COUNT UINT32_C(3)
#define NCR2_RELAY_GROUP_MS UINT32_C(1500)

/* Candidate indices 3..8 are the GPIO2 outputs. */
#define NCR2_RELAY_FIRST_CANDIDATE UINT32_C(3)

#define NCR2_SAMPLE_LIMIT INT32_C(0x3FFFFFFF)

/*
 * The proof-of-path fuzz emitted +/-0x40000000 for every nonzero sample.
 * That was useful once, but it was both four times the observed instrument
 * level and effectively a full-scale square wave. Internal effects remain
 * 12 dB below it; the final Level trim gets a separate bounded ceiling.
 */
#define NCR2_SAFE_OUTPUT_PEAK INT32_C(0x10000000)
#define NCR2_DAC_OUTPUT_PEAK INT32_C(0x20000000)
#define NCR2_DRIVE_Q12_ONE UINT32_C(4096)
#define NCR2_PARAMETER_Q15_ONE UINT32_C(32768)
#define NCR2_OUTPUT_GAIN_Q15_MAX UINT32_C(65536)
#define NCR2_SHINE_DRIVE_Q12_RANGE UINT32_C(94208)
#define NCR2_WALL_FUZZ_Q12_MIN UINT32_C(16384)
#define NCR2_WALL_FUZZ_Q12_RANGE UINT32_C(376832)
#define NCR2_RAGE_DRIVE_Q12_MIN UINT32_C(8192)
#define NCR2_RAGE_DRIVE_Q12_RANGE UINT32_C(155648)
#define NCR2_WHAMMY_DRIVE_Q12_RANGE UINT32_C(126976)
#define NCR2_EFFECT_RAMP_STEP UINT32_C(64)
#define NCR2_SELECTOR_RAMP_STEP UINT32_C(128)
#define NCR2_EFFECT_ROUTE_SETTLE_MS UINT32_C(20)
/* Eight physical Type detents select programs inside the current engine. */
#define NCR2_EFFECT_COUNT UINT32_C(8)
#define NCR2_OPEN_EFFECT_COUNT UINT32_C(32)
#define NCR2_SELECTOR_SETTLE_SAMPLES UINT32_C(3)
/*
 * Measured detent spacing is about 511 counts and resting noise never
 * exceeded 5, so requiring the reading to sit a quarter of a step past the
 * midpoint before switching is far outside anything a stationary knob can
 * produce, while still leaving three quarters of a step of travel to act in.
 */
#define NCR2_SELECTOR_HYSTERESIS UINT32_C(128)
#define NCR2_DELAY_BUFFER_FRAMES UINT32_C(32768)
#define NCR2_DELAY_BUFFER_MASK \
    (NCR2_DELAY_BUFFER_FRAMES - UINT32_C(1))
#define NCR2_WHAMMY_WINDOW_FRAMES UINT32_C(1536)
#define NCR2_WHAMMY_MIN_DELAY_FRAMES UINT32_C(96)
#define NCR2_WHAMMY_PHASE_INCREMENT UINT32_C(2796203)

/*
 * The original Reverb engine configures ADC1 channels 5, 8, 9 and 11 in
 * that order. Its parameter path identifies them as Decay, Tweak, Type and
 * Level. The native Type control has eight fixed positions. In normal use it
 * selects one of the current engine's eight effects; during the hold gesture
 * the same position selects one of eight engine slots:
 *
 *   Decay -> Amount, Tweak -> Character, Type -> Effect/Engine, Level -> Output.
 */
#define NCR2_KNOB_AMOUNT_CHANNEL UINT32_C(5)
#define NCR2_KNOB_CHARACTER_CHANNEL UINT32_C(8)
#define NCR2_KNOB_SELECTOR_CHANNEL UINT32_C(9)
#define NCR2_KNOB_OUTPUT_CHANNEL UINT32_C(11)
#define NCR2_KNOB_ADC_MAX UINT32_C(4095)
#define NCR2_KNOB_ADC_TIMEOUT UINT32_C(100000)
#define NCR2_KNOB_PAD_CONFIG UINT32_C(0x000000b0)
#define NCR2_KNOB_FILTER_SHIFT UINT32_C(3)
#define NCR2_SWITCH_POLL_MS UINT32_C(10)
#define NCR2_SWITCH_DEBOUNCE_MS UINT32_C(40)
#define NCR2_TONE_PHASE_MS UINT32_C(2500)
#define NCR2_TONE_CYCLES UINT32_C(2)
#define NCR2_DRIVE_DURATION_MS UINT32_C(30000)

/*
 * Input meter thresholds. The indicator lights while the captured level is
 * above the floor, so the capture path proves itself visually even if the
 * echo is too quiet to judge by ear.
 */
#define NCR2_INPUT_FLOOR INT32_C(0x00200000)
#define NCR2_METER_INTERVAL_MS UINT32_C(60)
#define NCR2_METER_DURATION_MS UINT32_C(40000)

#define NCR2_STATE_HOLD_MS UINT32_C(3000)
#define NCR2_STATE_CYCLES UINT32_C(6)
#define NCR2_DMA_SETTLE_MS UINT32_C(500)
/* The datasheet requires at least 10 ms after PDN is released. */
#define NCR2_CODEC_SETTLE_MS UINT32_C(50)
/* Audio I/F Format register and the TDM128 value for our SAI. */
#define NCR2_AK4619_REG_FORMAT UINT8_C(0x01)
#define NCR2_AK4619_FORMAT_TDM128 UINT8_C(0xAC)
#define NCR2_AK4619_REG_POWER UINT8_C(0x00)
#define NCR2_AK4619_POWER_ALL UINT8_C(0x37)
#define NCR2_AK4619_REG_MIC_GAIN UINT8_C(0x04)
#define NCR2_AK4619_MIC_GAIN_DEFAULT UINT8_C(0x22)
#define NCR2_AK4619_REG_DAC_ROUTE UINT8_C(0x12)
#define NCR2_AK4619_DAC_ROUTE_FACTORY UINT8_C(0x04)
#define NCR2_MILLISECONDS_PER_SECOND UINT32_C(1000)

volatile uint32_t g_hardware_app_heartbeat;
volatile uint32_t g_hardware_app_audio_status;
volatile uint32_t g_hardware_app_board_status;
volatile uint32_t g_hardware_app_ready;
volatile uint32_t g_hardware_app_processed_blocks;
volatile uint32_t g_hardware_app_led_phase;
volatile uint32_t g_hardware_app_trial_status;
volatile uint32_t g_hardware_app_core_clock_hz;
volatile uint32_t g_hardware_app_codec_found;
volatile uint32_t g_hardware_app_codec_candidate;
volatile uint32_t g_hardware_app_codec_address;
volatile uint32_t g_hardware_app_codec_failures;
volatile uint32_t g_hardware_app_codec_write;
volatile uint32_t g_hardware_app_codec_readback;
volatile uint32_t g_hardware_app_codec_power_readback;
volatile uint32_t g_hardware_app_codec_power_status;
volatile uint32_t g_hardware_app_codec_mic_gain_readback;
volatile uint32_t g_hardware_app_codec_mic_gain_status;
volatile uint32_t g_hardware_app_codec_format_status;
volatile uint32_t g_hardware_app_codec_reset_candidate =
    NCR2_FACTORY_BOARD_CANDIDATE_COUNT;
volatile uint32_t g_hardware_app_peak_hold;
volatile uint32_t g_hardware_app_knob_adc_ready;
volatile uint32_t g_hardware_app_knob_amount = UINT32_C(1024);
volatile uint32_t g_hardware_app_knob_character = UINT32_C(1024);
volatile uint32_t g_hardware_app_knob_selector;
volatile uint32_t g_hardware_app_knob_output = UINT32_C(1024);
volatile uint32_t g_hardware_app_selector_position;
volatile uint32_t g_hardware_app_open_engine_index;
volatile uint32_t g_hardware_app_effect_index;
volatile uint32_t g_hardware_app_factory_request_status;
volatile uint32_t g_hardware_app_factory_launch_status;
volatile uint32_t g_hardware_app_usb_audio_status =
    NCR2_USB_AUDIO_DISABLED;
static uint32_t g_selector_candidate;
static uint32_t g_selector_candidate_samples;

_Static_assert(
    sizeof(ncr2_factory_engine_mailbox_t) <=
        NCR2_FACTORY_REQUEST_MAILBOX_SIZE,
    "factory request exceeds SRC GPR10");

/*
 * A trial boot arms WDOG1 for eight seconds. Every wait in this build is
 * short enough that servicing the watchdog before each one keeps a trial
 * image alive through a sweep that runs far longer than the timeout.
 * Writing the sequence when no watchdog is enabled is harmless.
 */
static void diagnostic_delay_ms(uint32_t milliseconds)
{
    WDOG1->WSR = NCR2_WDOG_REFRESH_FIRST;
    WDOG1->WSR = NCR2_WDOG_REFRESH_SECOND;
    ncr2_factory_board_delay_ms(milliseconds);
}

static void show_indicator(
    ncr2_factory_indicator_state_t state,
    uint32_t milliseconds)
{
    ncr2_factory_board_set_indicator(state);
    diagnostic_delay_ms(milliseconds);
}


/*
 * Report a status code on the now-proven indicator. Using the LED as a
 * console means a precondition failure announces itself instead of being
 * mistaken for a result of the test that follows.
 */
static void flash_code(
    ncr2_factory_indicator_state_t state,
    uint32_t count)
{
    for (uint32_t flash = UINT32_C(0); flash < count; ++flash) {
        show_indicator(state, NCR2_LABEL_ON_MS);
        show_indicator(NCR2_LED_OFF, NCR2_LABEL_OFF_MS);
    }
    diagnostic_delay_ms(NCR2_LABEL_SETTLE_MS);
}

/*
 * Emit a register value MSB first as eight unambiguous colored bits:
 * green is one and red is zero, with an extra pause between nibbles.
 */
static void flash_byte(uint8_t value)
{
    for (uint32_t bit = UINT32_C(0); bit < UINT32_C(8); ++bit) {
        const uint8_t mask =
            (uint8_t)(UINT8_C(0x80) >> bit);

        show_indicator(
            ((value & mask) != UINT8_C(0))
                ? NCR2_LED_GREEN
                : NCR2_LED_RED,
            NCR2_BIT_ON_MS);
        show_indicator(NCR2_LED_OFF, NCR2_BIT_OFF_MS);
        if (bit == UINT32_C(3)) {
            diagnostic_delay_ms(NCR2_NIBBLE_GAP_MS);
        }
    }
    diagnostic_delay_ms(NCR2_LABEL_SETTLE_MS);
}

/*
 * A yellow count identifies the register, then one green means its I2C read
 * completed and the following eight pulses are its value. A red count
 * instead is the I2C error and no byte follows.
 */
static void report_register_byte(
    uint32_t marker,
    int status,
    uint8_t value)
{
    flash_code(NCR2_LED_BOTH, marker);
    if (status != NCR2_I2C_OK) {
        flash_code(NCR2_LED_RED, (uint32_t)status);
        return;
    }
    flash_code(NCR2_LED_GREEN, UINT32_C(1));
    flash_byte(value);
}



/*
 * Arm the bootloader's SRC_GPR8/GPR9 warm-reset mailbox. SRC general
 * purpose registers survive a warm reset, so any reset from here on lands
 * in open recovery. Arming on entry rather than at the end means a hang
 * partway through still reports itself once the watchdog fires.
 */
static void arm_recovery_request(void)
{
    volatile uint32_t *const mailbox =
        (volatile uint32_t *)(uintptr_t)NCR2_BOOT_MAILBOX_ADDRESS;

    mailbox[1] = ~BOOT_RECOVERY_REQUEST_MAGIC;
    mailbox[0] = BOOT_RECOVERY_REQUEST_MAGIC;
    __DSB();
}

/*
 * A pending application must return through the bootloader once after
 * arming its confirmation token. Clear the separate recovery request first
 * so that one-time reset confirms and immediately boots the application
 * again instead of stopping in USB recovery.
 */
static void clear_recovery_request(void)
{
    volatile uint32_t *const mailbox =
        (volatile uint32_t *)(uintptr_t)NCR2_BOOT_MAILBOX_ADDRESS;

    mailbox[0] = UINT32_C(0);
    mailbox[1] = UINT32_C(0);
    __DSB();
}

/*
 * Replace the diagnostic warm-reset recovery request with a one-shot
 * factory-engine request. The reset gives the proprietary engine the same
 * clean peripheral state as a normal boot; the open app consumes GPR10 and
 * validates the preserved vectors before copying anything into ITCM.
 */
static void reset_into_engine_slot(uint8_t engine_slot)
{
    ncr2_factory_engine_mailbox_t *const mailbox =
        (ncr2_factory_engine_mailbox_t *)(uintptr_t)
            NCR2_FACTORY_REQUEST_MAILBOX_ADDRESS;

    clear_recovery_request();
    g_hardware_app_factory_request_status =
        (uint32_t)ncr2_factory_engine_request_arm(
            mailbox,
            engine_slot);
    if (g_hardware_app_factory_request_status !=
        NCR2_FACTORY_REQUEST_OK) {
        arm_recovery_request();
        return;
    }

    __DSB();
    NVIC_SystemReset();
    for (;;) {
        __asm volatile("wfi");
    }
}

/*
 * Turn a silent hang into an observable event. Combined with the armed
 * mailbox, stalling anywhere in the sweep resets the pedal into recovery
 * within the timeout instead of leaving a dead board.
 */
static void enable_hang_watchdog(void)
{
    WDOG1->WCR = (uint16_t)(
        WDOG_WCR_SRS_MASK |
        WDOG_WCR_WDA_MASK |
        WDOG_WCR_WT(NCR2_WDOG_TIMEOUT_VALUE) |
        WDOG_WCR_WDE_MASK);
}

static void configure_knob_pads(void)
{
    const uint32_t knob_gpio_mask =
        (UINT32_C(1) << 16) |
        (UINT32_C(1) << 19) |
        (UINT32_C(1) << 20) |
        (UINT32_C(1) << 22);

    CLOCK_EnableClock(kCLOCK_Iomuxc);
    CLOCK_EnableClock(kCLOCK_Gpio1);
    CLOCK_EnableClock(kCLOCK_Adc1);

    /*
     * These are the four pad settings executed by the stock Reverb image.
     * The ADC connection is available while the digital mux remains GPIO;
     * explicitly leave every digital direction as an input.
     */
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_00_GPIO1_IO16, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_03_GPIO1_IO19, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_04_GPIO1_IO20, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_06_GPIO1_IO22, 0U);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_AD_B1_00_GPIO1_IO16,
        NCR2_KNOB_PAD_CONFIG);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_AD_B1_03_GPIO1_IO19,
        NCR2_KNOB_PAD_CONFIG);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_AD_B1_04_GPIO1_IO20,
        NCR2_KNOB_PAD_CONFIG);
    IOMUXC_SetPinConfig(
        IOMUXC_GPIO_AD_B1_06_GPIO1_IO22,
        NCR2_KNOB_PAD_CONFIG);
    GPIO1->GDIR &= ~knob_gpio_mask;
}

static uint8_t initialize_knob_adc(void)
{
    configure_knob_pads();

    /*
     * Match the factory converter setup except for ADTRG: the original uses
     * ADC_ETC/PIT hardware triggers, while this low-rate control path starts
     * one blocking conversion from the 10 ms foreground loop.
     */
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
        ADC1->HC[index] = ADC_HC_ADCH(31U);
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

            /* Calibration leaves COCO0 set; clear it before the first knob. */
            (void)ADC1->R[0];
            ADC1->HC[0] = ADC_HC_ADCH(31U);
            return valid;
        }
    }
    return UINT8_C(0);
}

static uint8_t read_knob_adc(
    uint32_t channel,
    uint32_t *value)
{
    ADC1->HC[0] = ADC_HC_ADCH(channel);
    for (uint32_t timeout = UINT32_C(0);
         timeout < NCR2_KNOB_ADC_TIMEOUT;
         ++timeout) {
        if ((ADC1->HS & ADC_HS_COCO0_MASK) != UINT32_C(0)) {
            *value = ADC1->R[0] & ADC_R_CDATA_MASK;
            return UINT8_C(1);
        }
    }
    ADC1->HC[0] = ADC_HC_ADCH(31U);
    return UINT8_C(0);
}

static uint32_t smooth_knob(
    uint32_t previous,
    uint32_t sample)
{
    const int32_t difference =
        (int32_t)sample - (int32_t)previous;

    return (uint32_t)(
        (int32_t)previous +
        (difference / (int32_t)(
            UINT32_C(1) << NCR2_KNOB_FILTER_SHIFT)));
}

/*
 * Measured on hardware 2026-07-27 over RECOVERY_COMMAND_READ_KNOBS, stepping
 * every physical detent. See docs/hardware/HARDWARE_MAP.md.
 *
 * The ladder descends with knob position and spans 2..3581 rather than the
 * full 0..4095, so the equal 512-count bins this build previously used could
 * not fit it: positions 2 and 3 both landed in bin 5, and the top detent fell
 * three counts short of bin 7. Index 0 is the topmost printed position.
 */
static const uint16_t g_selector_detent[NCR2_EFFECT_COUNT] = {
    UINT16_C(3581), /* position 1, top */
    UINT16_C(3071), /* position 2 */
    UINT16_C(2560), /* position 3 */
    UINT16_C(2049), /* position 4 */
    UINT16_C(1538), /* position 5 */
    UINT16_C(1028), /* position 6 */
    UINT16_C(516),  /* position 7 */
    UINT16_C(2),    /* position 8, bottom */
};

enum
{
    /* Engine slot 5: Open Amp Studio. */
    NCR2_EFFECT_AMP_GLASS_CLEAN = 0,
    NCR2_EFFECT_AMP_TWEED_BLOOM = 1,
    NCR2_EFFECT_AMP_CLASS_A_CHIME = 2,
    NCR2_EFFECT_AMP_BRIT_STACK = 3,
    NCR2_EFFECT_AMP_BROWN_LEAD = 4,
    NCR2_EFFECT_AMP_CALI_RECTO = 5,
    NCR2_EFFECT_AMP_BASS_FORGE = 6,
    NCR2_EFFECT_AMP_ACOUSTIC_IR = 7,

    /* Engine slot 6: Drive and Dynamics. */
    NCR2_EFFECT_SHINE_DRIVE = 8,
    NCR2_EFFECT_WALL_FUZZ = 9,
    NCR2_EFFECT_RAGE_DRIVE = 10,
    NCR2_EFFECT_COCKED_WAH = 11,
    NCR2_EFFECT_STUDIO_COMP = 12,
    NCR2_EFFECT_OCTAVE_FUZZ = 13,
    NCR2_EFFECT_STRING_ENSEMBLE = 14,
    NCR2_EFFECT_NOISE_GATE = 15,

    /* Engine slot 7: Motion and Pitch. */
    NCR2_EFFECT_BREATHE_VIBE = 16,
    NCR2_EFFECT_GUERRILLA_TREM = 17,
    NCR2_EFFECT_DIMENSION_CHORUS = 18,
    NCR2_EFFECT_JET_FLANGER = 19,
    NCR2_EFFECT_PHASE_ORBIT = 20,
    NCR2_EFFECT_ROTARY_CAB = 21,
    NCR2_EFFECT_AUTO_WAH = 22,
    NCR2_EFFECT_WHAMMY_FUZZ = 23,

    /* Engine slot 8: Echo and Space. */
    NCR2_EFFECT_ECHOES_TAPE = 24,
    NCR2_EFFECT_DIGITAL_DELAY = 25,
    NCR2_EFFECT_ANALOG_DELAY = 26,
    NCR2_EFFECT_REVERSE_DELAY = 27,
    NCR2_EFFECT_HALL_REVERB = 28,
    NCR2_EFFECT_PLATE_REVERB = 29,
    NCR2_EFFECT_SHIMMER_SPACE = 30,
    NCR2_EFFECT_SPRING_TANK = 31,
};

/*
 * Each open engine owns all eight Type positions, and every default effect
 * has one home. No algorithm is repeated between engine slots.
 */
static const uint8_t g_open_engine_effects
    [NCR2_OPEN_ENGINE_COUNT][NCR2_EFFECT_COUNT] = {
    {
        NCR2_EFFECT_AMP_GLASS_CLEAN,
        NCR2_EFFECT_AMP_TWEED_BLOOM,
        NCR2_EFFECT_AMP_CLASS_A_CHIME,
        NCR2_EFFECT_AMP_BRIT_STACK,
        NCR2_EFFECT_AMP_BROWN_LEAD,
        NCR2_EFFECT_AMP_CALI_RECTO,
        NCR2_EFFECT_AMP_BASS_FORGE,
        NCR2_EFFECT_AMP_ACOUSTIC_IR,
    },
    {
        NCR2_EFFECT_SHINE_DRIVE,
        NCR2_EFFECT_WALL_FUZZ,
        NCR2_EFFECT_RAGE_DRIVE,
        NCR2_EFFECT_COCKED_WAH,
        NCR2_EFFECT_STUDIO_COMP,
        NCR2_EFFECT_OCTAVE_FUZZ,
        NCR2_EFFECT_STRING_ENSEMBLE,
        NCR2_EFFECT_NOISE_GATE,
    },
    {
        NCR2_EFFECT_BREATHE_VIBE,
        NCR2_EFFECT_GUERRILLA_TREM,
        NCR2_EFFECT_DIMENSION_CHORUS,
        NCR2_EFFECT_JET_FLANGER,
        NCR2_EFFECT_PHASE_ORBIT,
        NCR2_EFFECT_ROTARY_CAB,
        NCR2_EFFECT_AUTO_WAH,
        NCR2_EFFECT_WHAMMY_FUZZ,
    },
    {
        NCR2_EFFECT_ECHOES_TAPE,
        NCR2_EFFECT_DIGITAL_DELAY,
        NCR2_EFFECT_ANALOG_DELAY,
        NCR2_EFFECT_REVERSE_DELAY,
        NCR2_EFFECT_HALL_REVERB,
        NCR2_EFFECT_PLATE_REVERB,
        NCR2_EFFECT_SHIMMER_SPACE,
        NCR2_EFFECT_SPRING_TANK,
    },
};

static uint32_t open_effect_for_position(
    uint32_t engine,
    uint32_t position)
{
    if (engine >= NCR2_OPEN_ENGINE_COUNT ||
        position >= NCR2_EFFECT_COUNT) {
        return NCR2_EFFECT_SHINE_DRIVE;
    }
    return g_open_engine_effects[engine][position];
}

static uint32_t selector_distance(uint32_t sample, uint32_t effect)
{
    const uint32_t detent = g_selector_detent[effect];

    return (sample > detent)
               ? sample - detent
               : detent - sample;
}

/*
 * Nearest measured detent rather than an equal-width bin. This is what makes
 * the selector independent of where the ladder starts, how wide it is, and
 * which direction it runs.
 */
static uint32_t quantize_selector(uint32_t sample)
{
    uint32_t best = UINT32_C(0);
    uint32_t best_distance = selector_distance(sample, UINT32_C(0));

    for (uint32_t effect = UINT32_C(1);
         effect < NCR2_EFFECT_COUNT;
         ++effect) {
        const uint32_t distance = selector_distance(sample, effect);

        if (distance < best_distance) {
            best_distance = distance;
            best = effect;
        }
    }
    return best;
}

static uint32_t update_selector(
    uint32_t sample,
    uint32_t current)
{
    const uint32_t selected = quantize_selector(sample);

    /*
     * Switch only once the reading is clearly nearer another detent than the
     * one in use. Hysteresis is applied against the live distance to the
     * current detent rather than a latched sample, so a control that is
     * merely noisy can never accumulate its way across a boundary, and three
     * consecutive 10 ms samples still reject contact chatter while the
     * wiper crosses.
     */
    if (selected == current ||
        selector_distance(sample, selected) +
                NCR2_SELECTOR_HYSTERESIS >
            selector_distance(sample, current)) {
        g_selector_candidate = current;
        g_selector_candidate_samples = UINT32_C(0);
        return current;
    }
    if (selected != g_selector_candidate) {
        g_selector_candidate = selected;
        g_selector_candidate_samples = UINT32_C(1);
        return current;
    }
    if (g_selector_candidate_samples <
        NCR2_SELECTOR_SETTLE_SAMPLES) {
        ++g_selector_candidate_samples;
    }
    if (g_selector_candidate_samples >=
        NCR2_SELECTOR_SETTLE_SAMPLES) {
        g_selector_candidate = selected;
        g_selector_candidate_samples = UINT32_C(0);
        return selected;
    }
    return current;
}

static uint8_t sample_knobs(uint8_t first_sample)
{
    uint32_t amount;
    uint32_t character;
    uint32_t selector;
    uint32_t output;

    if (read_knob_adc(
            NCR2_KNOB_AMOUNT_CHANNEL, &amount) == UINT8_C(0) ||
        read_knob_adc(
            NCR2_KNOB_CHARACTER_CHANNEL, &character) == UINT8_C(0) ||
        read_knob_adc(
            NCR2_KNOB_SELECTOR_CHANNEL, &selector) == UINT8_C(0) ||
        read_knob_adc(NCR2_KNOB_OUTPUT_CHANNEL, &output) == UINT8_C(0)) {
        return UINT8_C(0);
    }

    if (first_sample != UINT8_C(0)) {
        const uint32_t position = quantize_selector(selector);

        g_hardware_app_knob_amount = amount;
        g_hardware_app_knob_character = character;
        g_hardware_app_knob_selector = selector;
        g_hardware_app_knob_output = output;
        g_hardware_app_selector_position = position;
        g_hardware_app_effect_index = open_effect_for_position(
            g_hardware_app_open_engine_index,
            position);
        g_selector_candidate = position;
        g_selector_candidate_samples = UINT32_C(0);
    } else {
        const uint32_t position = update_selector(
            selector,
            g_hardware_app_selector_position);

        g_hardware_app_knob_amount =
            smooth_knob(g_hardware_app_knob_amount, amount);
        g_hardware_app_knob_character =
            smooth_knob(g_hardware_app_knob_character, character);
        g_hardware_app_knob_selector = selector;
        g_hardware_app_knob_output =
            smooth_knob(g_hardware_app_knob_output, output);
        g_hardware_app_selector_position = position;
        g_hardware_app_effect_index = open_effect_for_position(
            g_hardware_app_open_engine_index,
            position);
    }
    __DMB();
    return UINT8_C(1);
}

__attribute__((noreturn))
static void reset_into_recovery(void)
{
    arm_recovery_request();
    NVIC_SystemReset();
    for (;;) {
        __asm volatile("wfi");
    }
}

volatile int32_t g_hardware_app_input_peak;

/*
 * The AK4619 supplies four TDM ADC slots, but the mono instrument is not
 * present at equal level in all four. Averaging every slot imposed 6-12 dB
 * of attenuation before DSP. Select the strongest slot for each mono frame;
 * this is unity gain when one or more inputs carry the same guitar signal
 * and remains independent of the board's physical ADC channel routing.
 */
static int32_t capture_frame(const int32_t *frame_input)
{
    int32_t selected = INT32_C(0);
    uint32_t selected_magnitude = UINT32_C(0);

    for (size_t slot = 0U;
         slot < NCR2_FACTORY_AUDIO_SLOTS;
         ++slot) {
        const int32_t sample = frame_input[slot];
        const uint32_t magnitude = (uint32_t)(
            (sample < INT32_C(0))
                ? -(int64_t)sample
                : (int64_t)sample);

        if (magnitude > selected_magnitude) {
            selected = sample;
            selected_magnitude = magnitude;
        }
    }
    return selected;
}

volatile uint32_t g_hardware_app_emit_tone;
volatile uint32_t g_hardware_app_enable_effect;

typedef struct ncr2_effect_parameters
{
    uint32_t amount_q15;
    uint32_t character_q15;
    uint32_t output_q15;
    uint32_t shine_drive_q12;
    uint32_t wall_fuzz_q12;
    uint32_t rage_drive_q12;
    uint32_t vibe_increment;
    uint32_t tape_increment;
    uint32_t tremolo_increment;
    uint32_t tape_delay_frames;
    uint32_t wah_coefficient_q15;
} ncr2_effect_parameters_t;

static int32_t g_effect_dc_estimate;
static int32_t g_effect_tone_state;
static int32_t g_effect_pre_state;
static int32_t g_effect_aux_state;
static int32_t g_delay_filter_state;
static int32_t g_wah_low_state;
static int32_t g_wah_band_state;
static uint32_t g_vibe_phase;
static uint32_t g_tape_phase;
static uint32_t g_tremolo_phase;
static uint32_t g_whammy_phase;
static uint32_t g_effect_ramp;
static uint32_t g_selector_ramp = NCR2_PARAMETER_Q15_ONE;
static uint32_t g_current_effect;
static uint32_t g_delay_write_index;
static uint32_t g_cabinet_write_index;
static int32_t g_multi_state[8];
static int32_t g_cabinet_history[8];
static int32_t g_delay_buffer[NCR2_DELAY_BUFFER_FRAMES]
    __attribute__((section(".sdram_bss"), aligned(32)));

static uint32_t clamp_knob(uint32_t value)
{
    return (value > NCR2_KNOB_ADC_MAX)
               ? NCR2_KNOB_ADC_MAX
               : value;
}

static int64_t clamp_symmetric(
    int64_t value,
    int64_t limit)
{
    if (value > limit) {
        return limit;
    }
    if (value < -limit) {
        return -limit;
    }
    return value;
}

static uint32_t phase_increment(uint32_t frequency_millihz)
{
    return (uint32_t)(
        ((uint64_t)frequency_millihz << 32) /
        ((uint64_t)NCR2_FACTORY_AUDIO_SAMPLE_RATE_HZ *
         UINT64_C(1000)));
}

static int32_t triangle_q15(uint32_t phase)
{
    const uint32_t saw = phase >> 16;

    if (saw < UINT32_C(32768)) {
        return (int32_t)(saw * UINT32_C(2)) - INT32_C(32768);
    }
    return INT32_C(98303) -
        (int32_t)(saw * UINT32_C(2));
}

static int32_t highpass_input(int32_t dry)
{
    const int64_t difference =
        (int64_t)dry - (int64_t)g_effect_dc_estimate;

    /*
     * A slow DC blocker keeps converter offset from turning into asymmetric
     * high-gain hash, while remaining far below the guitar's lowest note.
     */
    g_effect_dc_estimate +=
        (int32_t)(difference / INT64_C(1024));
    return (int32_t)clamp_symmetric(
        difference,
        (int64_t)NCR2_SAFE_OUTPUT_PEAK);
}

static int32_t lowpass_sample(
    int32_t input,
    int32_t *state,
    uint32_t alpha_q15)
{
    const int64_t difference =
        (int64_t)input - (int64_t)(*state);
    const int64_t next =
        (int64_t)(*state) +
        difference * (int64_t)alpha_q15 /
            (int64_t)NCR2_PARAMETER_Q15_ONE;

    *state = (int32_t)clamp_symmetric(
        next,
        INT64_C(4) * (int64_t)NCR2_SAFE_OUTPUT_PEAK);
    return *state;
}

static int32_t blend_samples_q15(
    int32_t dry,
    int32_t wet,
    uint32_t wet_q15)
{
    const int64_t mixed =
        (int64_t)dry *
            (int64_t)(NCR2_PARAMETER_Q15_ONE - wet_q15) +
        (int64_t)wet * (int64_t)wet_q15;

    return (int32_t)clamp_symmetric(
        mixed / (int64_t)NCR2_PARAMETER_Q15_ONE,
        (int64_t)NCR2_SAFE_OUTPUT_PEAK);
}

static int32_t shape_drive(
    int32_t input,
    uint32_t drive_q12,
    uint32_t tone_alpha_q15,
    uint32_t character_q15)
{
    int64_t driven;
    int64_t magnitude;
    int64_t soft;
    int64_t hard;
    int64_t shaped;
    int64_t filtered;

    driven =
        (int64_t)input *
        (int64_t)drive_q12 /
        (int64_t)NCR2_DRIVE_Q12_ONE;
    driven = clamp_symmetric(
        driven,
        INT64_C(8) * (int64_t)NCR2_SAFE_OUTPUT_PEAK);
    magnitude = (driven < INT64_C(0)) ? -driven : driven;

    /*
     * x/(1+abs(x)) gives a continuous soft knee with no gate and no
     * discontinuity at zero. Character blends toward a conventional hard
     * clip. The preset-specific gain ranges reach 24x for Shine, 96x for
     * Wall, 40x for Rage and 32x for the Whammy fuzz branch. Every branch
     * still shares the exact strict output ceiling physically proven in
     * v0.9.0.
     */
    soft =
        driven * (int64_t)NCR2_SAFE_OUTPUT_PEAK /
        ((int64_t)NCR2_SAFE_OUTPUT_PEAK + magnitude);
    hard = clamp_symmetric(
        driven,
        (int64_t)NCR2_SAFE_OUTPUT_PEAK);
    shaped =
        (soft *
             (int64_t)(
                 NCR2_PARAMETER_Q15_ONE - character_q15) +
         hard * (int64_t)character_q15) /
        (int64_t)NCR2_PARAMETER_Q15_ONE;

    /*
     * Character also opens the post-clip bandwidth as it moves toward the
     * harder branch. The coefficient never reaches an unstable endpoint.
     */
    filtered =
        (int64_t)g_effect_tone_state +
        ((shaped - (int64_t)g_effect_tone_state) *
         (int64_t)tone_alpha_q15) /
            (int64_t)NCR2_PARAMETER_Q15_ONE;
    g_effect_tone_state = (int32_t)filtered;
    return (int32_t)clamp_symmetric(
        filtered,
        (int64_t)NCR2_SAFE_OUTPUT_PEAK);
}

/*
 * Original low-latency amp/cab voices for Open Amp Studio. This is a
 * clean-room design, not an extraction of NUX's TSAC-HD models. Each voice
 * has its own gain structure, pre-emphasis and compact 8-tap cabinet IR.
 * Amount is preamp gain; Character moves from direct amp to cabinet tone.
 */
static int32_t process_amp_voice(
    int32_t input,
    const ncr2_effect_parameters_t *parameters,
    uint32_t voice)
{
    static const uint32_t base_drive_q12[8] = {
        UINT32_C(4096), UINT32_C(6144), UINT32_C(7168), UINT32_C(10240),
        UINT32_C(12288), UINT32_C(16384), UINT32_C(5120), UINT32_C(4096),
    };
    static const uint32_t drive_range_q12[8] = {
        UINT32_C(12288), UINT32_C(24576), UINT32_C(28672), UINT32_C(53248),
        UINT32_C(69632), UINT32_C(94208), UINT32_C(20480), UINT32_C(8192),
    };
    static const uint32_t pre_alpha_q15[8] = {
        UINT32_C(2600), UINT32_C(3400), UINT32_C(5200), UINT32_C(7000),
        UINT32_C(8200), UINT32_C(9600), UINT32_C(1800), UINT32_C(12000),
    };
    static const uint32_t tone_alpha_q15[8] = {
        UINT32_C(13000), UINT32_C(9000), UINT32_C(17000), UINT32_C(10500),
        UINT32_C(12500), UINT32_C(8500), UINT32_C(6500), UINT32_C(22000),
    };
    static const int16_t cabinet_ir_q15[8][8] = {
        { 2200, 5600, 9000, 7800, 5000, 2300, -900, -2200 },
        { 1200, 4800, 9400, 8500, 5600, 1000, -2600, -4200 },
        { 1800, 7000, 9800, 6000, 1000, -2800, -2100, 2400 },
        { 800, 4000, 8500, 8800, 5000, 0, -3300, -3600 },
        { 500, 3000, 7600, 9300, 6500, 800, -3900, -4800 },
        { 300, 2400, 6500, 9000, 7400, 2300, -3300, -6300 },
        { 3200, 7400, 9000, 5900, 2600, 200, -900, -600 },
        { 6200, 10500, 7200, 1800, -3000, -3000, -200, 1800 },
    };
    int64_t convolved = INT64_C(0);

    if (voice >= UINT32_C(8)) {
        voice = UINT32_C(0);
    }
    const int32_t body = lowpass_sample(
        input,
        &g_effect_pre_state,
        pre_alpha_q15[voice]);
    const int32_t emphasized = (voice == UINT32_C(6))
        ? body
        : (int32_t)clamp_symmetric(
            (int64_t)input +
                ((int64_t)input - (int64_t)body) / INT64_C(2),
            (int64_t)NCR2_SAFE_OUTPUT_PEAK);
    const uint32_t drive_q12 =
        base_drive_q12[voice] +
        (parameters->amount_q15 * drive_range_q12[voice]) /
            NCR2_PARAMETER_Q15_ONE;
    const int32_t preamp = shape_drive(
        emphasized,
        drive_q12,
        tone_alpha_q15[voice],
        (voice == UINT32_C(7))
            ? UINT32_C(2048)
            : parameters->amount_q15 / UINT32_C(2));

    g_cabinet_history[g_cabinet_write_index] = preamp;
    for (uint32_t tap = UINT32_C(0); tap < UINT32_C(8); ++tap) {
        const uint32_t index =
            (g_cabinet_write_index - tap) & UINT32_C(7);
        convolved +=
            (int64_t)g_cabinet_history[index] *
            (int64_t)cabinet_ir_q15[voice][tap];
    }
    g_cabinet_write_index =
        (g_cabinet_write_index + UINT32_C(1)) & UINT32_C(7);
    const int32_t cabinet = (int32_t)clamp_symmetric(
        convolved / INT64_C(32768),
        (int64_t)NCR2_SAFE_OUTPUT_PEAK);
    const uint32_t cabinet_mix =
        UINT32_C(16384) + parameters->character_q15 / UINT32_C(2);

    return blend_samples_q15(preamp, cabinet, cabinet_mix);
}

static void reset_transient_effect_state(void)
{
    g_effect_tone_state = INT32_C(0);
    g_effect_pre_state = INT32_C(0);
    g_effect_aux_state = INT32_C(0);
    g_delay_filter_state = INT32_C(0);
    g_wah_low_state = INT32_C(0);
    g_wah_band_state = INT32_C(0);
    g_vibe_phase = UINT32_C(0);
    g_tape_phase = UINT32_C(0);
    g_tremolo_phase = UINT32_C(0);
    g_whammy_phase = UINT32_C(0);
    g_cabinet_write_index = UINT32_C(0);
    for (uint32_t index = UINT32_C(0);
         index < UINT32_C(8);
         ++index) {
        g_multi_state[index] = INT32_C(0);
    }
    for (uint32_t index = UINT32_C(0);
         index < UINT32_C(8);
         ++index) {
        g_cabinet_history[index] = INT32_C(0);
    }
}

static void initialize_effect_processor(void)
{
    for (uint32_t frame = UINT32_C(0);
         frame < NCR2_DELAY_BUFFER_FRAMES;
         ++frame) {
        g_delay_buffer[frame] = INT32_C(0);
    }
    g_delay_write_index = UINT32_C(0);
    g_effect_dc_estimate = INT32_C(0);
    g_effect_ramp = UINT32_C(0);
    g_selector_ramp = NCR2_PARAMETER_Q15_ONE;
    g_current_effect =
        (g_hardware_app_effect_index < NCR2_OPEN_EFFECT_COUNT)
            ? g_hardware_app_effect_index
            : UINT32_C(0);
    reset_transient_effect_state();
}

static void update_effect_selection(uint32_t requested)
{
    if (requested >= NCR2_OPEN_EFFECT_COUNT) {
        requested = NCR2_OPEN_EFFECT_COUNT - UINT32_C(1);
    }
    if (requested != g_current_effect) {
        if (g_selector_ramp > NCR2_SELECTOR_RAMP_STEP) {
            g_selector_ramp -= NCR2_SELECTOR_RAMP_STEP;
        } else {
            /*
             * Change algorithms only at zero gain. Delay memory is retained
             * deliberately, but short-lived filter, oscillator and sample
             * hold state starts clean.
             */
            g_selector_ramp = UINT32_C(0);
            g_current_effect = requested;
            reset_transient_effect_state();
        }
    } else if (g_selector_ramp <
               NCR2_PARAMETER_Q15_ONE - NCR2_SELECTOR_RAMP_STEP) {
        g_selector_ramp += NCR2_SELECTOR_RAMP_STEP;
    } else {
        g_selector_ramp = NCR2_PARAMETER_Q15_ONE;
    }
}

static ncr2_effect_parameters_t make_effect_parameters(
    uint32_t amount_knob,
    uint32_t character_knob,
    uint32_t output_knob)
{
    ncr2_effect_parameters_t parameters;

    parameters.amount_q15 =
        (amount_knob * NCR2_PARAMETER_Q15_ONE) /
        NCR2_KNOB_ADC_MAX;
    parameters.character_q15 =
        (character_knob * NCR2_PARAMETER_Q15_ONE) /
        NCR2_KNOB_ADC_MAX;
    /* Level is unity at the physical midpoint and reaches +6 dB at max. */
    parameters.output_q15 =
        (output_knob * NCR2_OUTPUT_GAIN_Q15_MAX) /
        NCR2_KNOB_ADC_MAX;
    parameters.shine_drive_q12 =
        NCR2_DRIVE_Q12_ONE +
        (amount_knob * NCR2_SHINE_DRIVE_Q12_RANGE) /
            NCR2_KNOB_ADC_MAX;
    parameters.wall_fuzz_q12 =
        NCR2_WALL_FUZZ_Q12_MIN +
        (amount_knob * NCR2_WALL_FUZZ_Q12_RANGE) /
            NCR2_KNOB_ADC_MAX;
    parameters.rage_drive_q12 =
        NCR2_RAGE_DRIVE_Q12_MIN +
        (amount_knob * NCR2_RAGE_DRIVE_Q12_RANGE) /
            NCR2_KNOB_ADC_MAX;
    parameters.vibe_increment = phase_increment(
        UINT32_C(350) +
        (amount_knob * UINT32_C(4650)) /
            NCR2_KNOB_ADC_MAX);
    parameters.tape_increment = phase_increment(UINT32_C(350));
    parameters.tremolo_increment = phase_increment(
        UINT32_C(1000) +
        (amount_knob * UINT32_C(15000)) /
            NCR2_KNOB_ADC_MAX);
    parameters.tape_delay_frames =
        UINT32_C(5760) +
        (amount_knob * UINT32_C(24240)) /
            NCR2_KNOB_ADC_MAX;
    parameters.wah_coefficient_q15 =
        UINT32_C(1100) +
        (amount_knob * UINT32_C(7900)) /
            NCR2_KNOB_ADC_MAX;
    return parameters;
}

static int32_t process_selected_effect(
    int32_t input,
    const ncr2_effect_parameters_t *parameters)
{
    int64_t sample = input;
    int64_t delay_write = input;

    switch (g_current_effect) {
    case NCR2_EFFECT_AMP_GLASS_CLEAN:
    case NCR2_EFFECT_AMP_TWEED_BLOOM:
    case NCR2_EFFECT_AMP_CLASS_A_CHIME:
    case NCR2_EFFECT_AMP_BRIT_STACK:
    case NCR2_EFFECT_AMP_BROWN_LEAD:
    case NCR2_EFFECT_AMP_CALI_RECTO:
    case NCR2_EFFECT_AMP_BASS_FORGE:
    case NCR2_EFFECT_AMP_ACOUSTIC_IR:
        sample = process_amp_voice(
            input,
            parameters,
            g_current_effect - NCR2_EFFECT_AMP_GLASS_CLEAN);
        break;

    case NCR2_EFFECT_SHINE_DRIVE:
        {
            const uint32_t tone_alpha_q15 =
                UINT32_C(1800) +
                (parameters->character_q15 * UINT32_C(13200)) /
                    NCR2_PARAMETER_Q15_ONE;
            const int32_t driven = shape_drive(
                input,
                parameters->shine_drive_q12,
                tone_alpha_q15,
                parameters->character_q15 / UINT32_C(2));

            /*
             * Keep the internal gain range, but return it in parallel with
             * the clean attack. The previous nearly-all-wet mix measured
             * roughly ten times louder than the two modulation presets that
             * sounded right on the physical tube amp.
             */
            sample =
                blend_samples_q15(
                    input,
                    driven,
                    UINT32_C(8192)) /
                INT32_C(2);
        }
        break;

    case NCR2_EFFECT_WALL_FUZZ:
        {
            const uint32_t tone_alpha_q15 =
                UINT32_C(1000) +
                (parameters->character_q15 * UINT32_C(7000)) /
                    NCR2_PARAMETER_Q15_ONE;
            const uint32_t clip_character =
                UINT32_C(8192) +
                parameters->character_q15 / UINT32_C(2);
            const int32_t rounded_input = lowpass_sample(
                input,
                &g_effect_pre_state,
                UINT32_C(3500));

            const int32_t fuzzed = shape_drive(
                rounded_input,
                parameters->wall_fuzz_q12,
                tone_alpha_q15,
                clip_character);
            sample =
                blend_samples_q15(
                    input,
                    fuzzed,
                    UINT32_C(28672)) /
                INT32_C(4);
        }
        break;

    case NCR2_EFFECT_BREATHE_VIBE:
        {
            const int32_t triangle =
                triangle_q15(g_vibe_phase);
            const uint32_t sweep =
                (uint32_t)(triangle + INT32_C(32768));
            const uint32_t delay_depth =
                UINT32_C(96) +
                (parameters->character_q15 * UINT32_C(576)) /
                    NCR2_PARAMETER_Q15_ONE;
            const uint32_t delay_frames =
                UINT32_C(96) +
                (sweep * delay_depth) / UINT32_C(65535);
            const uint32_t read_index =
                (g_delay_write_index - delay_frames) &
                NCR2_DELAY_BUFFER_MASK;
            const int32_t delayed = g_delay_buffer[read_index];
            const uint32_t wet =
                UINT32_C(8192) +
                (parameters->character_q15 * UINT32_C(12288)) /
                    NCR2_PARAMETER_Q15_ONE;
            const uint32_t tremolo_depth =
                UINT32_C(2048) +
                (parameters->character_q15 * UINT32_C(6144)) /
                    NCR2_PARAMETER_Q15_ONE;
            const int64_t gain =
                (int64_t)(
                    NCR2_PARAMETER_Q15_ONE - tremolo_depth) +
                (int64_t)sweep * (int64_t)tremolo_depth /
                    INT64_C(65535);
            const int32_t swirled =
                blend_samples_q15(input, delayed, wet);

            g_vibe_phase += parameters->vibe_increment;
            sample =
                (int64_t)swirled * gain /
                (int64_t)NCR2_PARAMETER_Q15_ONE;
        }
        break;

    case NCR2_EFFECT_ECHOES_TAPE:
        {
            const int32_t flutter =
                triangle_q15(g_tape_phase);
            const int32_t modulation =
                (int32_t)(
                    (int64_t)flutter *
                    (int64_t)(
                        UINT32_C(3) +
                        parameters->character_q15 /
                            UINT32_C(2048)) /
                    INT64_C(32768));
            const uint32_t long_delay =
                (uint32_t)(
                    (int32_t)parameters->tape_delay_frames +
                    modulation);
            const uint32_t short_delay =
                (long_delay * UINT32_C(3)) / UINT32_C(4);
            const uint32_t long_index =
                (g_delay_write_index - long_delay) &
                NCR2_DELAY_BUFFER_MASK;
            const uint32_t short_index =
                (g_delay_write_index - short_delay) &
                NCR2_DELAY_BUFFER_MASK;
            const int32_t long_tap = g_delay_buffer[long_index];
            const int32_t short_tap = g_delay_buffer[short_index];
            const int32_t heads = (int32_t)(
                ((int64_t)long_tap * INT64_C(3) +
                 (int64_t)short_tap * INT64_C(2)) /
                INT64_C(5));
            const uint32_t repeat_alpha =
                UINT32_C(6500) -
                (parameters->character_q15 * UINT32_C(3500)) /
                    NCR2_PARAMETER_Q15_ONE;
            const int32_t dark_heads = lowpass_sample(
                heads,
                &g_delay_filter_state,
                repeat_alpha);
            const uint32_t wet =
                UINT32_C(8192) +
                (parameters->character_q15 * UINT32_C(6144)) /
                    NCR2_PARAMETER_Q15_ONE;
            const uint32_t feedback =
                UINT32_C(6554) +
                (parameters->character_q15 * UINT32_C(16384)) /
                    NCR2_PARAMETER_Q15_ONE;

            g_tape_phase += parameters->tape_increment;
            sample = blend_samples_q15(input, dark_heads, wet);
            delay_write =
                (int64_t)input +
                (int64_t)dark_heads * (int64_t)feedback /
                    (int64_t)NCR2_PARAMETER_Q15_ONE;
        }
        break;

    case NCR2_EFFECT_RAGE_DRIVE:
        {
            const int32_t body = lowpass_sample(
                input,
                &g_effect_pre_state,
                UINT32_C(2200));
            const int32_t edge = input - body;
            const uint32_t edge_gain =
                UINT32_C(32768) +
                parameters->character_q15;
            const int32_t tightened =
                (int32_t)clamp_symmetric(
                    (int64_t)input +
                    (int64_t)edge * (int64_t)edge_gain /
                        (int64_t)NCR2_PARAMETER_Q15_ONE,
                    (int64_t)NCR2_SAFE_OUTPUT_PEAK);
            const uint32_t tone_alpha_q15 =
                UINT32_C(7000) +
                (parameters->character_q15 * UINT32_C(18000)) /
                    NCR2_PARAMETER_Q15_ONE;
            const uint32_t clip_character =
                UINT32_C(16384) +
                parameters->character_q15 / UINT32_C(2);
            const int32_t distorted = shape_drive(
                tightened,
                parameters->rage_drive_q12,
                tone_alpha_q15,
                clip_character);

            sample =
                blend_samples_q15(
                    input,
                    distorted,
                    UINT32_C(28672)) /
                INT32_C(4);
        }
        break;

    case NCR2_EFFECT_COCKED_WAH:
        {
            const uint32_t damping_q15 =
                UINT32_C(28672) -
                (parameters->character_q15 * UINT32_C(20480)) /
                    NCR2_PARAMETER_Q15_ONE;
            const int64_t low =
                (int64_t)g_wah_low_state +
                (int64_t)parameters->wah_coefficient_q15 *
                    (int64_t)g_wah_band_state /
                    (int64_t)NCR2_PARAMETER_Q15_ONE;
            const int64_t high =
                (int64_t)input - low -
                (int64_t)damping_q15 *
                    (int64_t)g_wah_band_state /
                    (int64_t)NCR2_PARAMETER_Q15_ONE;
            const int64_t band =
                (int64_t)g_wah_band_state +
                (int64_t)parameters->wah_coefficient_q15 * high /
                    (int64_t)NCR2_PARAMETER_Q15_ONE;
            const uint32_t wet =
                UINT32_C(28672) +
                parameters->character_q15 / UINT32_C(8);
            const int32_t resonant = (int32_t)clamp_symmetric(
                band *
                    (int64_t)(
                        UINT32_C(16384) +
                        parameters->character_q15) /
                    (int64_t)NCR2_PARAMETER_Q15_ONE,
                INT64_C(2) * (int64_t)NCR2_SAFE_OUTPUT_PEAK);
            const int32_t filtered = blend_samples_q15(
                input,
                resonant,
                wet);
            const uint32_t wah_drive_q12 =
                NCR2_DRIVE_Q12_ONE +
                (parameters->character_q15 * UINT32_C(12288)) /
                    NCR2_PARAMETER_Q15_ONE;

            g_wah_low_state = (int32_t)clamp_symmetric(
                low,
                INT64_C(4) * (int64_t)NCR2_SAFE_OUTPUT_PEAK);
            g_wah_band_state = (int32_t)clamp_symmetric(
                band,
                INT64_C(4) * (int64_t)NCR2_SAFE_OUTPUT_PEAK);
            const int32_t wah = shape_drive(
                filtered,
                wah_drive_q12,
                UINT32_C(20000),
                parameters->character_q15 / UINT32_C(2));
            const int32_t lifted_wah =
                (int32_t)clamp_symmetric(
                    (int64_t)wah * INT64_C(2),
                    (int64_t)NCR2_SAFE_OUTPUT_PEAK);

            sample = clamp_symmetric(
                (int64_t)blend_samples_q15(input, lifted_wah, wet) *
                    INT64_C(2),
                (int64_t)NCR2_SAFE_OUTPUT_PEAK);
        }
        break;

    case NCR2_EFFECT_STUDIO_COMP:
        {
            const int32_t magnitude = (input < INT32_C(0))
                ? (int32_t)(-(int64_t)input)
                : input;
            const uint32_t attack = UINT32_C(6000) +
                parameters->character_q15 / UINT32_C(2);
            const uint32_t release = UINT32_C(80) +
                parameters->character_q15 / UINT32_C(128);
            const uint32_t coefficient =
                (magnitude > g_effect_aux_state) ? attack : release;
            const int32_t envelope = lowpass_sample(
                magnitude,
                &g_effect_aux_state,
                coefficient);
            const int64_t threshold =
                INT64_C(0x00100000) +
                ((int64_t)(NCR2_PARAMETER_Q15_ONE -
                    parameters->amount_q15) * INT64_C(0x00800000)) /
                    INT64_C(32768);
            int64_t gain_q15 = INT64_C(32768);

            if ((int64_t)envelope > threshold) {
                gain_q15 =
                    INT64_C(32768) * threshold /
                    (int64_t)envelope;
                gain_q15 = INT64_C(16384) + gain_q15 / INT64_C(2);
            }
            const int64_t makeup_q15 =
                INT64_C(32768) +
                (int64_t)parameters->amount_q15 * INT64_C(2);
            sample = clamp_symmetric(
                (int64_t)input * gain_q15 * makeup_q15 /
                    (INT64_C(32768) * INT64_C(32768)),
                (int64_t)NCR2_SAFE_OUTPUT_PEAK);
        }
        break;

    case NCR2_EFFECT_OCTAVE_FUZZ:
        {
            const int32_t fuzz = shape_drive(
                input,
                UINT32_C(32768) + parameters->amount_q15 * UINT32_C(3),
                UINT32_C(10000),
                UINT32_C(28672));
            const int32_t rectified = (fuzz < INT32_C(0))
                ? (int32_t)(-(int64_t)fuzz)
                : fuzz;
            const int32_t octave = rectified -
                lowpass_sample(
                    rectified,
                    &g_effect_aux_state,
                    UINT32_C(900));
            sample = blend_samples_q15(
                fuzz,
                octave,
                UINT32_C(12288) +
                    parameters->character_q15 / UINT32_C(2));
        }
        break;

    case NCR2_EFFECT_STRING_ENSEMBLE:
        {
            /*
             * A pick-suppressed, octave-rich ensemble voice. This is an
             * intentionally synthetic instrument transformation rather
             * than a pitch-tracked sampler: Amount lengthens the bowed
             * swell and Character opens the body/ensemble brightness.
             */
            const int32_t magnitude = (input < INT32_C(0))
                ? (int32_t)(-(int64_t)input)
                : input;
            const int32_t envelope = lowpass_sample(
                magnitude,
                &g_effect_aux_state,
                (magnitude > g_effect_aux_state)
                    ? UINT32_C(6000)
                    : UINT32_C(50));
            const int32_t threshold =
                INT32_C(0x00040000) +
                (int32_t)(
                    ((NCR2_PARAMETER_Q15_ONE -
                      parameters->amount_q15) *
                     UINT32_C(0x00180000)) /
                    NCR2_PARAMETER_Q15_ONE);
            const int32_t bow_target = (envelope > threshold)
                ? (int32_t)NCR2_PARAMETER_Q15_ONE
                : INT32_C(0);
            const uint32_t bow_attack =
                UINT32_C(24) +
                ((NCR2_PARAMETER_Q15_ONE -
                  parameters->amount_q15) * UINT32_C(120)) /
                    NCR2_PARAMETER_Q15_ONE;
            const int32_t bow_envelope = lowpass_sample(
                bow_target,
                &g_effect_pre_state,
                (bow_target > g_effect_pre_state)
                    ? bow_attack
                    : UINT32_C(8));
            const uint32_t body_alpha =
                UINT32_C(2600) +
                (parameters->character_q15 * UINT32_C(7200)) /
                    NCR2_PARAMETER_Q15_ONE;
            const int32_t body = lowpass_sample(
                input,
                &g_effect_tone_state,
                body_alpha);
            const int32_t rectified = (input < INT32_C(0))
                ? (int32_t)(-(int64_t)input)
                : input;
            const int32_t octave = rectified - lowpass_sample(
                rectified,
                &g_wah_low_state,
                UINT32_C(420));
            const int32_t harmonized = blend_samples_q15(
                body,
                octave,
                UINT32_C(12288) +
                    parameters->character_q15 / UINT32_C(4));
            const uint32_t sweep = (uint32_t)(
                triangle_q15(g_vibe_phase) + INT32_C(32768));
            const uint32_t delay_frames =
                UINT32_C(480) +
                (sweep * (UINT32_C(96) +
                    parameters->character_q15 / UINT32_C(256))) /
                    UINT32_C(65535);
            const int32_t ensemble = g_delay_buffer[
                (g_delay_write_index - delay_frames) &
                NCR2_DELAY_BUFFER_MASK];
            const int32_t voice = blend_samples_q15(
                harmonized,
                ensemble,
                UINT32_C(16384) +
                    parameters->character_q15 / UINT32_C(4));

            g_vibe_phase += parameters->tape_increment;
            delay_write = harmonized;
            sample = clamp_symmetric(
                (int64_t)voice * (int64_t)bow_envelope * INT64_C(6) /
                    INT64_C(32768),
                (int64_t)NCR2_SAFE_OUTPUT_PEAK);
        }
        break;

    case NCR2_EFFECT_NOISE_GATE:
        {
            const int32_t magnitude = (input < INT32_C(0))
                ? (int32_t)(-(int64_t)input)
                : input;
            const int32_t envelope = lowpass_sample(
                magnitude,
                &g_effect_aux_state,
                (magnitude > g_effect_aux_state)
                    ? UINT32_C(12000)
                    : UINT32_C(160));
            const int32_t threshold =
                INT32_C(0x00100000) +
                (int32_t)(
                    (parameters->amount_q15 * UINT32_C(0x02800000)) /
                    NCR2_PARAMETER_Q15_ONE);
            const uint32_t open_target =
                (envelope > threshold)
                    ? NCR2_PARAMETER_Q15_ONE
                    : UINT32_C(0);
            const int32_t smoothed_gate = lowpass_sample(
                (int32_t)open_target,
                &g_effect_pre_state,
                (open_target > (uint32_t)g_effect_pre_state)
                    ? UINT32_C(16000)
                    : UINT32_C(400) +
                        parameters->character_q15 / UINT32_C(16));
            sample =
                (int64_t)input * (int64_t)smoothed_gate /
                INT64_C(32768);
        }
        break;

    case NCR2_EFFECT_GUERRILLA_TREM:
        {
            const int32_t triangle =
                triangle_q15(g_tremolo_phase) + INT32_C(32768);
            const int32_t square =
                (triangle >= INT32_C(32768))
                    ? INT32_C(65535)
                    : INT32_C(0);
            const int64_t waveform =
                ((int64_t)triangle *
                     (int64_t)(
                         NCR2_PARAMETER_Q15_ONE -
                         parameters->character_q15) +
                 (int64_t)square *
                     (int64_t)parameters->character_q15) /
                (int64_t)NCR2_PARAMETER_Q15_ONE;
            const uint32_t depth =
                UINT32_C(16384) +
                parameters->character_q15 / UINT32_C(2);
            const int64_t gain =
                (int64_t)(NCR2_PARAMETER_Q15_ONE - depth) +
                waveform * (int64_t)depth / INT64_C(65535);

            g_tremolo_phase += parameters->tremolo_increment;
            sample =
                (int64_t)input * gain /
                (int64_t)NCR2_PARAMETER_Q15_ONE;
        }
        break;

    case NCR2_EFFECT_DIMENSION_CHORUS:
        {
            const int32_t triangle = triangle_q15(g_vibe_phase);
            const uint32_t sweep =
                (uint32_t)(triangle + INT32_C(32768));
            const uint32_t depth = UINT32_C(72) +
                (parameters->character_q15 * UINT32_C(240)) /
                    NCR2_PARAMETER_Q15_ONE;
            const uint32_t delay_a = UINT32_C(720) +
                (sweep * depth) / UINT32_C(65535);
            const uint32_t delay_b = UINT32_C(960) +
                ((UINT32_C(65535) - sweep) * depth) /
                    UINT32_C(65535);
            const int32_t chorus = (int32_t)(
                ((int64_t)g_delay_buffer[
                    (g_delay_write_index - delay_a) &
                    NCR2_DELAY_BUFFER_MASK] +
                 (int64_t)g_delay_buffer[
                    (g_delay_write_index - delay_b) &
                    NCR2_DELAY_BUFFER_MASK]) /
                INT64_C(2));

            g_vibe_phase += phase_increment(
                UINT32_C(180) +
                (parameters->amount_q15 * UINT32_C(850)) /
                    NCR2_PARAMETER_Q15_ONE);
            sample = blend_samples_q15(
                input,
                chorus,
                UINT32_C(8192) +
                    parameters->character_q15 / UINT32_C(4));
        }
        break;

    case NCR2_EFFECT_JET_FLANGER:
        {
            const int32_t triangle = triangle_q15(g_vibe_phase);
            const uint32_t sweep =
                (uint32_t)(triangle + INT32_C(32768));
            const uint32_t delay_frames = UINT32_C(24) +
                (sweep * (UINT32_C(48) +
                    parameters->character_q15 / UINT32_C(256))) /
                    UINT32_C(65535);
            const int32_t delayed = g_delay_buffer[
                (g_delay_write_index - delay_frames) &
                NCR2_DELAY_BUFFER_MASK];
            const uint32_t feedback = UINT32_C(8192) +
                parameters->character_q15 / UINT32_C(2);

            g_vibe_phase += phase_increment(
                UINT32_C(80) +
                (parameters->amount_q15 * UINT32_C(1200)) /
                    NCR2_PARAMETER_Q15_ONE);
            sample = blend_samples_q15(input, delayed, UINT32_C(16384));
            delay_write = (int64_t)input +
                (int64_t)delayed * (int64_t)feedback /
                    INT64_C(32768);
        }
        break;

    case NCR2_EFFECT_PHASE_ORBIT:
        {
            const int32_t triangle = triangle_q15(g_vibe_phase);
            const uint32_t sweep =
                (uint32_t)(triangle + INT32_C(32768));
            const uint32_t coefficient = UINT32_C(2500) +
                (sweep * (UINT32_C(11000) +
                    parameters->character_q15 / UINT32_C(3))) /
                    UINT32_C(65535);
            int32_t phased = input;

            for (uint32_t stage = UINT32_C(0);
                 stage < UINT32_C(4);
                 ++stage) {
                const int64_t next =
                    (int64_t)g_multi_state[stage] +
                    ((int64_t)phased -
                     (int64_t)g_multi_state[stage]) *
                        (int64_t)coefficient / INT64_C(32768);
                const int32_t low = (int32_t)clamp_symmetric(
                    next,
                    (int64_t)NCR2_SAFE_OUTPUT_PEAK);
                g_multi_state[stage] = low;
                phased = phased - low;
            }
            g_vibe_phase += phase_increment(
                UINT32_C(70) +
                (parameters->amount_q15 * UINT32_C(1700)) /
                    NCR2_PARAMETER_Q15_ONE);
            sample = blend_samples_q15(
                input,
                phased,
                UINT32_C(16384) +
                    parameters->character_q15 / UINT32_C(4));
        }
        break;

    case NCR2_EFFECT_ROTARY_CAB:
        {
            const int32_t triangle = triangle_q15(g_vibe_phase);
            const uint32_t sweep =
                (uint32_t)(triangle + INT32_C(32768));
            const uint32_t delay_frames = UINT32_C(48) +
                (sweep * UINT32_C(120)) / UINT32_C(65535);
            const int32_t horn = g_delay_buffer[
                (g_delay_write_index - delay_frames) &
                NCR2_DELAY_BUFFER_MASK];
            const int32_t drum = lowpass_sample(
                input,
                &g_effect_aux_state,
                UINT32_C(1800));
            const int64_t horn_gain = INT64_C(24576) +
                (int64_t)triangle / INT64_C(4);
            const int64_t drum_gain = INT64_C(24576) -
                (int64_t)triangle / INT64_C(8);
            const int32_t rotary = (int32_t)clamp_symmetric(
                ((int64_t)horn * horn_gain +
                 (int64_t)drum * drum_gain) /
                    INT64_C(49152),
                (int64_t)NCR2_SAFE_OUTPUT_PEAK);

            g_vibe_phase += phase_increment(
                UINT32_C(450) +
                (parameters->amount_q15 * UINT32_C(6200)) /
                    NCR2_PARAMETER_Q15_ONE);
            sample = blend_samples_q15(
                input,
                rotary,
                UINT32_C(16384) +
                    parameters->character_q15 / UINT32_C(4));
        }
        break;

    case NCR2_EFFECT_AUTO_WAH:
        {
            const int32_t magnitude = (input < INT32_C(0))
                ? (int32_t)(-(int64_t)input)
                : input;
            const int32_t envelope = lowpass_sample(
                magnitude,
                &g_effect_aux_state,
                (magnitude > g_effect_aux_state)
                    ? UINT32_C(9000)
                    : UINT32_C(120));
            const uint32_t normalized_raw = (uint32_t)(
                ((uint64_t)(uint32_t)envelope * UINT64_C(32768)) /
                (uint64_t)NCR2_SAFE_OUTPUT_PEAK);
            const uint32_t normalized =
                (normalized_raw > NCR2_PARAMETER_Q15_ONE)
                    ? NCR2_PARAMETER_Q15_ONE
                    : normalized_raw;
            const uint32_t coefficient = UINT32_C(900) +
                (normalized * (UINT32_C(6000) +
                    parameters->amount_q15 / UINT32_C(3))) /
                    NCR2_PARAMETER_Q15_ONE;
            const int64_t low = (int64_t)g_wah_low_state +
                (int64_t)coefficient *
                    (int64_t)g_wah_band_state / INT64_C(32768);
            const int64_t high = (int64_t)input - low -
                (int64_t)(UINT32_C(18000) -
                    parameters->character_q15 / UINT32_C(3)) *
                    (int64_t)g_wah_band_state / INT64_C(32768);
            const int64_t band = (int64_t)g_wah_band_state +
                (int64_t)coefficient * high / INT64_C(32768);

            g_wah_low_state = (int32_t)clamp_symmetric(
                low,
                INT64_C(4) * (int64_t)NCR2_SAFE_OUTPUT_PEAK);
            g_wah_band_state = (int32_t)clamp_symmetric(
                band,
                INT64_C(4) * (int64_t)NCR2_SAFE_OUTPUT_PEAK);
            sample = blend_samples_q15(
                input,
                (int32_t)clamp_symmetric(
                    band,
                    (int64_t)NCR2_SAFE_OUTPUT_PEAK),
                UINT32_C(24576));
        }
        break;

    case NCR2_EFFECT_WHAMMY_FUZZ:
        {
            const uint32_t phase_a = g_whammy_phase;
            const uint32_t phase_b =
                phase_a + UINT32_C(0x80000000);
            const uint32_t position_a = phase_a >> 16;
            const uint32_t position_b = phase_b >> 16;
            const uint32_t window_a =
                (position_a < UINT32_C(32768))
                    ? position_a * UINT32_C(2)
                    : (UINT32_C(65535) - position_a) *
                        UINT32_C(2);
            const uint32_t window_b =
                (position_b < UINT32_C(32768))
                    ? position_b * UINT32_C(2)
                    : (UINT32_C(65535) - position_b) *
                        UINT32_C(2);
            const uint32_t delay_a =
                NCR2_WHAMMY_MIN_DELAY_FRAMES +
                ((UINT32_C(65535) - position_a) *
                 NCR2_WHAMMY_WINDOW_FRAMES) /
                    UINT32_C(65536);
            const uint32_t delay_b =
                NCR2_WHAMMY_MIN_DELAY_FRAMES +
                ((UINT32_C(65535) - position_b) *
                 NCR2_WHAMMY_WINDOW_FRAMES) /
                    UINT32_C(65536);
            const int32_t head_a = g_delay_buffer[
                (g_delay_write_index - delay_a) &
                NCR2_DELAY_BUFFER_MASK];
            const int32_t head_b = g_delay_buffer[
                (g_delay_write_index - delay_b) &
                NCR2_DELAY_BUFFER_MASK];
            const uint32_t window_sum = window_a + window_b;
            const int32_t shifted = (int32_t)(
                ((int64_t)head_a * (int64_t)window_a +
                 (int64_t)head_b * (int64_t)window_b) /
                (int64_t)window_sum);
            const uint32_t pitch_drive_q12 =
                NCR2_DRIVE_Q12_ONE +
                (parameters->character_q15 *
                 NCR2_WHAMMY_DRIVE_Q12_RANGE) /
                    NCR2_PARAMETER_Q15_ONE;
            const uint32_t tone_alpha_q15 =
                UINT32_C(5000) +
                (parameters->character_q15 * UINT32_C(12000)) /
                    NCR2_PARAMETER_Q15_ONE;
            const int32_t smooth_shifted = lowpass_sample(
                shifted,
                &g_effect_aux_state,
                UINT32_C(12000));
            const int32_t pitched_fuzz = shape_drive(
                smooth_shifted,
                pitch_drive_q12,
                tone_alpha_q15,
                (parameters->character_q15 * UINT32_C(3)) /
                    UINT32_C(4));

            g_whammy_phase += NCR2_WHAMMY_PHASE_INCREMENT;
            const int32_t pitched_mix = blend_samples_q15(
                input,
                pitched_fuzz,
                parameters->amount_q15);
            sample = blend_samples_q15(
                input,
                pitched_mix,
                UINT32_C(8192));
        }
        break;

    case NCR2_EFFECT_DIGITAL_DELAY:
        {
            const uint32_t delay_frames = UINT32_C(2400) +
                (parameters->amount_q15 * UINT32_C(26400)) /
                    NCR2_PARAMETER_Q15_ONE;
            const int32_t delayed = g_delay_buffer[
                (g_delay_write_index - delay_frames) &
                NCR2_DELAY_BUFFER_MASK];
            const uint32_t feedback = UINT32_C(4096) +
                (parameters->character_q15 * UINT32_C(23552)) /
                    NCR2_PARAMETER_Q15_ONE;

            sample = blend_samples_q15(
                input,
                delayed,
                UINT32_C(12288));
            delay_write = (int64_t)input +
                (int64_t)delayed * (int64_t)feedback /
                    INT64_C(32768);
        }
        break;

    case NCR2_EFFECT_ANALOG_DELAY:
        {
            const uint32_t delay_frames = UINT32_C(3600) +
                (parameters->amount_q15 * UINT32_C(25200)) /
                    NCR2_PARAMETER_Q15_ONE;
            const int32_t delayed = g_delay_buffer[
                (g_delay_write_index - delay_frames) &
                NCR2_DELAY_BUFFER_MASK];
            const int32_t dark = lowpass_sample(
                delayed,
                &g_delay_filter_state,
                UINT32_C(1800) +
                    (NCR2_PARAMETER_Q15_ONE -
                     parameters->character_q15) / UINT32_C(8));
            const uint32_t feedback = UINT32_C(8192) +
                parameters->character_q15 / UINT32_C(2);

            sample = blend_samples_q15(input, dark, UINT32_C(14336));
            delay_write = (int64_t)input +
                (int64_t)dark * (int64_t)feedback /
                    INT64_C(32768);
        }
        break;

    case NCR2_EFFECT_REVERSE_DELAY:
        {
            const uint32_t window = UINT32_C(4096) +
                (parameters->amount_q15 * UINT32_C(12288)) /
                    NCR2_PARAMETER_Q15_ONE;
            const uint32_t phase = g_delay_write_index % window;
            const uint32_t reverse_index =
                (g_delay_write_index + window - UINT32_C(1) -
                 UINT32_C(2) * phase) & NCR2_DELAY_BUFFER_MASK;
            const int32_t reversed = g_delay_buffer[reverse_index];
            const uint32_t edge = (phase < window / UINT32_C(2))
                ? phase
                : window - phase;
            const uint32_t fade_q15 = (edge < UINT32_C(256))
                ? edge * UINT32_C(128)
                : NCR2_PARAMETER_Q15_ONE;
            const int32_t faded = (int32_t)(
                (int64_t)reversed * (int64_t)fade_q15 /
                INT64_C(32768));

            sample = blend_samples_q15(
                input,
                faded,
                UINT32_C(12288) +
                    parameters->character_q15 / UINT32_C(4));
        }
        break;

    case NCR2_EFFECT_HALL_REVERB:
        {
            const int32_t tap_a = g_delay_buffer[
                (g_delay_write_index - UINT32_C(1493)) &
                NCR2_DELAY_BUFFER_MASK];
            const int32_t tap_b = g_delay_buffer[
                (g_delay_write_index - UINT32_C(4217)) &
                NCR2_DELAY_BUFFER_MASK];
            const int32_t tap_c = g_delay_buffer[
                (g_delay_write_index - UINT32_C(7013)) &
                NCR2_DELAY_BUFFER_MASK];
            const int32_t tap_d = g_delay_buffer[
                (g_delay_write_index - UINT32_C(11003)) &
                NCR2_DELAY_BUFFER_MASK];
            const int32_t field = (int32_t)(
                ((int64_t)tap_a + (int64_t)tap_b +
                 (int64_t)tap_c + (int64_t)tap_d) /
                INT64_C(4));
            const int32_t dark = lowpass_sample(
                field,
                &g_delay_filter_state,
                UINT32_C(2200) +
                    parameters->character_q15 / UINT32_C(8));
            const uint32_t decay = UINT32_C(15000) +
                parameters->amount_q15 / UINT32_C(3);

            sample = blend_samples_q15(
                input,
                dark,
                UINT32_C(8192) +
                    parameters->character_q15 / UINT32_C(4));
            delay_write = (int64_t)input +
                (int64_t)dark * (int64_t)decay /
                    INT64_C(32768);
        }
        break;

    case NCR2_EFFECT_PLATE_REVERB:
        {
            const int32_t tap_a = g_delay_buffer[
                (g_delay_write_index - UINT32_C(1051)) &
                NCR2_DELAY_BUFFER_MASK];
            const int32_t tap_b = g_delay_buffer[
                (g_delay_write_index - UINT32_C(2203)) &
                NCR2_DELAY_BUFFER_MASK];
            const int32_t tap_c = g_delay_buffer[
                (g_delay_write_index - UINT32_C(3469)) &
                NCR2_DELAY_BUFFER_MASK];
            const int32_t tap_d = g_delay_buffer[
                (g_delay_write_index - UINT32_C(4787)) &
                NCR2_DELAY_BUFFER_MASK];
            const int32_t diffusion = (int32_t)clamp_symmetric(
                ((int64_t)tap_a * INT64_C(3) -
                 (int64_t)tap_b * INT64_C(2) +
                 (int64_t)tap_c * INT64_C(2) +
                 (int64_t)tap_d) /
                    INT64_C(8),
                (int64_t)NCR2_SAFE_OUTPUT_PEAK);
            const uint32_t decay = UINT32_C(13500) +
                parameters->amount_q15 / UINT32_C(3);

            sample = blend_samples_q15(
                input,
                diffusion,
                UINT32_C(10240) +
                    parameters->character_q15 / UINT32_C(4));
            delay_write = (int64_t)input +
                (int64_t)diffusion * (int64_t)decay /
                    INT64_C(32768);
        }
        break;

    case NCR2_EFFECT_SHIMMER_SPACE:
        {
            const uint32_t position = g_whammy_phase >> 16;
            const uint32_t delay_a = UINT32_C(160) +
                ((UINT32_C(65535) - position) * UINT32_C(1400)) /
                    UINT32_C(65536);
            const uint32_t delay_b = UINT32_C(160) +
                (position * UINT32_C(1400)) / UINT32_C(65536);
            const int32_t shifted = (int32_t)(
                ((int64_t)g_delay_buffer[
                    (g_delay_write_index - delay_a) &
                    NCR2_DELAY_BUFFER_MASK] +
                 (int64_t)g_delay_buffer[
                    (g_delay_write_index - delay_b) &
                    NCR2_DELAY_BUFFER_MASK]) /
                INT64_C(2));
            const int32_t cloud = lowpass_sample(
                shifted,
                &g_delay_filter_state,
                UINT32_C(5000));
            const uint32_t feedback = UINT32_C(13000) +
                parameters->amount_q15 / UINT32_C(3);

            g_whammy_phase += UINT32_C(4194304);
            sample = blend_samples_q15(
                input,
                cloud,
                UINT32_C(8192) +
                    parameters->character_q15 / UINT32_C(3));
            delay_write = (int64_t)input +
                (int64_t)cloud * (int64_t)feedback /
                    INT64_C(32768);
        }
        break;

    case NCR2_EFFECT_SPRING_TANK:
        {
            const int32_t early = g_delay_buffer[
                (g_delay_write_index - UINT32_C(613)) &
                NCR2_DELAY_BUFFER_MASK];
            const int32_t middle = g_delay_buffer[
                (g_delay_write_index - UINT32_C(1297)) &
                NCR2_DELAY_BUFFER_MASK];
            const int32_t late = g_delay_buffer[
                (g_delay_write_index - UINT32_C(2381)) &
                NCR2_DELAY_BUFFER_MASK];
            const int32_t splash = (int32_t)clamp_symmetric(
                ((int64_t)early * INT64_C(3) -
                 (int64_t)middle * INT64_C(2) +
                 (int64_t)late) /
                    INT64_C(4),
                (int64_t)NCR2_SAFE_OUTPUT_PEAK);
            const int32_t body = lowpass_sample(
                splash,
                &g_effect_aux_state,
                UINT32_C(3200));
            const int32_t boing = splash - body;
            const uint32_t feedback = UINT32_C(10000) +
                parameters->amount_q15 / UINT32_C(4);

            sample = blend_samples_q15(
                input,
                boing,
                UINT32_C(8192) +
                    parameters->character_q15 / UINT32_C(3));
            delay_write = (int64_t)input +
                (int64_t)boing * (int64_t)feedback /
                    INT64_C(32768);
        }
        break;

    default:
        sample = input;
        break;
    }

    g_delay_buffer[g_delay_write_index] =
        (int32_t)clamp_symmetric(
            delay_write,
            (int64_t)NCR2_SAFE_OUTPUT_PEAK);
    g_delay_write_index =
        (g_delay_write_index + UINT32_C(1)) &
        NCR2_DELAY_BUFFER_MASK;
    return (int32_t)clamp_symmetric(
        sample,
        (int64_t)NCR2_SAFE_OUTPUT_PEAK);
}

static int32_t apply_effect_output(
    int32_t dry,
    int32_t processed,
    const ncr2_effect_parameters_t *parameters)
{
    /*
     * Program changes crossfade through clean converter audio. Even if a
     * noisy Type detent requests changes continuously, the selector ramp
     * can now produce only clean guitar at its midpoint, never silence.
     */
    const int32_t transitioned = blend_samples_q15(
        dry,
        processed,
        g_selector_ramp);
    int64_t sample =
        (int64_t)transitioned *
        (int64_t)parameters->output_q15 /
        (int64_t)NCR2_PARAMETER_Q15_ONE;

    sample =
        sample * (int64_t)g_effect_ramp /
        (int64_t)NCR2_PARAMETER_Q15_ONE;
    return (int32_t)clamp_symmetric(
        sample,
        (int64_t)NCR2_DAC_OUTPUT_PEAK);
}

void ncr2_factory_audio_process_block(
    const int32_t *input,
    int32_t *output,
    size_t frames)
{
    int32_t peak = g_hardware_app_input_peak;
    const uint32_t enabled = g_hardware_app_enable_effect;
    const uint32_t amount_knob =
        clamp_knob(g_hardware_app_knob_amount);
    const uint32_t character_knob =
        clamp_knob(g_hardware_app_knob_character);
    const uint32_t output_knob =
        clamp_knob(g_hardware_app_knob_output);
    const ncr2_effect_parameters_t parameters =
        make_effect_parameters(
            amount_knob,
            character_knob,
            output_knob);

    for (size_t frame = 0U; frame < frames; ++frame) {
        const size_t base = frame * NCR2_FACTORY_AUDIO_SLOTS;
        const int32_t dry = capture_frame(&input[base]);
        /* A full USB ring drops this copy; it can never stall analog audio. */
        ncr2_usb_audio_capture_push(dry);
        const int32_t filtered_input = highpass_input(dry);
        const int32_t magnitude =
            (dry == INT32_MIN)
                ? INT32_MAX
                : ((dry < 0) ? -dry : dry);
        int32_t processed = filtered_input;
        int32_t sample;

        /*
         * Analog bypass does not need a speculative DSP render. Avoiding it
         * also guarantees that a newly installed, CPU-heavy user engine can
         * still boot, confirm, and accept control before it is engaged. The
         * existing 20 ms pre-relay settle starts processing and ramps the
         * selected effect while the dry route is still connected.
         */
        if (enabled != UINT32_C(0) ||
            g_effect_ramp != UINT32_C(0)) {
            processed = process_selected_effect(
                filtered_input,
                &parameters);
        }

        if (magnitude > peak) {
            peak = magnitude;
        }

        if (enabled != UINT32_C(0)) {
            if (g_effect_ramp <
                NCR2_PARAMETER_Q15_ONE - NCR2_EFFECT_RAMP_STEP) {
                g_effect_ramp += NCR2_EFFECT_RAMP_STEP;
            } else {
                g_effect_ramp = NCR2_PARAMETER_Q15_ONE;
            }
        } else if (g_effect_ramp > NCR2_EFFECT_RAMP_STEP) {
            g_effect_ramp -= NCR2_EFFECT_RAMP_STEP;
        } else {
            g_effect_ramp = UINT32_C(0);
        }

        update_effect_selection(g_hardware_app_effect_index);
        sample = apply_effect_output(
            filtered_input,
            processed,
            &parameters);

        for (size_t slot = 0U;
             slot < NCR2_FACTORY_AUDIO_SLOTS;
             ++slot) {
            output[base + slot] = sample;
        }
    }
    g_hardware_app_input_peak = peak;
    ++g_hardware_app_processed_blocks;
}

/*
 * Run the physically verified open audio path as an ordinary stompbox.
 * The early status flashes remain useful if codec or DMA initialization
 * regresses, after which the footswitch owns bypass and the four panel
 * controls drive the selected open processor indefinitely.
 */
void application_main(void)
{
#if NCR2_RUNTIME_DIAGNOSTIC_FLASHES
    uint32_t blocks_before;
    uint32_t tx_blocks_before;
#endif
    uint8_t requested_engine_slot = UINT8_C(0);
    ncr2_codec_bus_t codec_bus = { UINT32_C(0), UINT8_C(0) };

    /*
     * Consume the launch request before arming diagnostic recovery or
     * touching audio hardware. Success transfers control and never returns;
     * a changed vector or random GPR value fails closed into the open app.
     */
    g_hardware_app_factory_request_status =
        (uint32_t)ncr2_factory_engine_request_consume(
            (ncr2_factory_engine_mailbox_t *)(uintptr_t)
                NCR2_FACTORY_REQUEST_MAILBOX_ADDRESS,
            &requested_engine_slot);
    if (g_hardware_app_factory_request_status ==
        NCR2_FACTORY_REQUEST_OK) {
        if (requested_engine_slot < NCR2_FACTORY_ENGINE_COUNT) {
            g_hardware_app_factory_launch_status =
                (uint32_t)ncr2_factory_engine_launch(
                    requested_engine_slot);
        } else {
            g_hardware_app_open_engine_index =
                (uint32_t)(
                    requested_engine_slot - NCR2_OPEN_ENGINE_FIRST);
            g_hardware_app_factory_launch_status =
                NCR2_FACTORY_LAUNCH_OK;
        }
    }

    /*
     * Report execution before doing anything that could stall: from here
     * on, either the sweep finishes and resets, or the watchdog resets us,
     * and both land in open recovery.
     */
    arm_recovery_request();
    enable_hang_watchdog();

    /*
     * SystemCoreClock is only a compile-time default until this runs. Every
     * diagnostic wait is scaled by it, so leaving it wrong stretches the
     * sweep by whatever factor the real clock differs by.
     */
    SystemCoreClockUpdate();
    g_hardware_app_core_clock_hz = SystemCoreClock;

    /*
     * Turn a pending handoff token into a confirmation before anything
     * that could stall, so reaching main() is enough to mark the image
     * healthy. On a confirmed boot there is no token and this is a no-op.
     */
    g_hardware_app_trial_status =
        (uint32_t)boot_trial_arm_confirmation(
            (boot_trial_mailbox_t *)(uintptr_t)
                NCR2_BOOT_TRIAL_MAILBOX_ADDRESS);
    if (g_hardware_app_trial_status == BOOT_TRIAL_OK) {
        clear_recovery_request();
        NVIC_SystemReset();
        for (;;) {
            __asm volatile("wfi");
        }
    }

    g_hardware_app_board_status =
        (uint32_t)ncr2_factory_board_prepare_audio();
    ncr2_factory_board_restore_idle();
    ncr2_factory_board_set_relay(UINT8_C(0));
    if (g_hardware_app_factory_request_status ==
            NCR2_FACTORY_REQUEST_OK &&
        g_hardware_app_factory_launch_status !=
            NCR2_FACTORY_LAUNCH_OK) {
        /* A guarded launch refusal must not resemble a normal open boot. */
        flash_code(
            NCR2_LED_RED,
            g_hardware_app_factory_launch_status);
    }
#if NCR2_RUNTIME_DIAGNOSTIC_FLASHES
    diagnostic_delay_ms(NCR2_LED_LEAD_IN_MS);
#endif

    /*
     * Report the two preconditions on the indicator before the routing
     * test, so a failure in either cannot be mistaken for a routing
     * result. Two green flashes mean SAI and eDMA initialised; two red
     * mean they did not.
     */
    g_hardware_app_audio_status =
        (uint32_t)ncr2_factory_audio_init();
    if (g_hardware_app_audio_status == NCR2_FACTORY_AUDIO_OK) {
        ncr2_factory_board_release_audio();
#if NCR2_RUNTIME_DIAGNOSTIC_FLASHES
        flash_code(NCR2_LED_GREEN, UINT32_C(2));
#endif
    } else {
        flash_code(NCR2_LED_RED, UINT32_C(2));
        reset_into_recovery();
    }

#if NCR2_RUNTIME_DIAGNOSTIC_FLASHES
    /*
     * Three yellow flashes mean transmit blocks are actually being
     * consumed, so the tone is genuinely leaving the DMA. Three red mean
     * the callback never ran and silence proves nothing about routing.
     */
    blocks_before = g_hardware_app_processed_blocks;
    tx_blocks_before = g_ncr2_factory_audio_counters.tx_blocks;
    diagnostic_delay_ms(NCR2_DMA_SETTLE_MS);
    flash_code(
        (g_hardware_app_processed_blocks != blocks_before &&
         g_ncr2_factory_audio_counters.tx_blocks != tx_blocks_before)
            ? NCR2_LED_BOTH
            : NCR2_LED_RED,
        UINT32_C(3));
#endif

    /*
     * Release whichever traced output is the codec's PDN, then wait past
     * the datasheet's 10 ms settling requirement before touching the bus.
     */
    ncr2_factory_board_release_reset_candidates();
    ncr2_factory_board_set_relay(UINT8_C(0));
    diagnostic_delay_ms(NCR2_CODEC_SETTLE_MS);

    g_hardware_app_codec_found = ncr2_codec_probe(&codec_bus);
    g_hardware_app_codec_candidate = codec_bus.candidate;
    g_hardware_app_codec_address = codec_bus.address;

#if NCR2_RUNTIME_DIAGNOSTIC_FLASHES
    flash_code(NCR2_LED_BOTH, codec_bus.candidate + UINT32_C(1));
#endif

    if (g_hardware_app_codec_found != NCR2_CODEC_PROBE_FOUND) {
        /* Nothing to configure; say so and stop rather than mislead. */
        flash_code(NCR2_LED_RED, NCR2_CODEC_BUS_CANDIDATE_COUNT);
        reset_into_recovery();
    }

    /*
     * Configure while all plausible reset candidates remain high. The
     * v0.7.3 result showed that writes acknowledge but both format and
     * power validation fail before any candidate is lowered. Report the
     * actual bytes instead of collapsing that evidence into one color.
     */
    g_hardware_app_codec_failures = ncr2_codec_configure();
    if (g_hardware_app_codec_failures == UINT32_C(0)) {
        uint8_t format = UINT8_C(0);
        uint8_t power = UINT8_C(0);
        uint8_t route = UINT8_C(0);

        /*
         * The probe state releases every plausible control, but it is not a
         * valid running audio state. The continued stock trace reaches a
         * different GPIO2 bank after SAI startup: IO23/25/27 high and
         * IO11/24/26 low. Restore that state after writing the complete
         * factory codec image, then verify the three registers that define
         * its clocks, power, and DAC source selection.
         */
        ncr2_factory_board_restore_audio_active(
            NCR2_FACTORY_BOARD_CANDIDATE_COUNT);
        ncr2_factory_board_set_relay(UINT8_C(0));
        diagnostic_delay_ms(UINT32_C(2));

        if (ncr2_codec_read_register(
                NCR2_AK4619_REG_FORMAT, &format) != NCR2_I2C_OK ||
            format != NCR2_AK4619_FORMAT_TDM128) {
            ++g_hardware_app_codec_failures;
        }
        if (ncr2_codec_read_register(
                NCR2_AK4619_REG_POWER, &power) != NCR2_I2C_OK ||
            power != NCR2_AK4619_POWER_ALL) {
            ++g_hardware_app_codec_failures;
        }
        if (ncr2_codec_read_register(
                NCR2_AK4619_REG_DAC_ROUTE, &route) != NCR2_I2C_OK ||
            route != NCR2_AK4619_DAC_ROUTE_FACTORY) {
            ++g_hardware_app_codec_failures;
        }
    }
    if (g_hardware_app_codec_failures == UINT32_C(0)) {
#if NCR2_RUNTIME_DIAGNOSTIC_FLASHES
        flash_code(NCR2_LED_GREEN, UINT32_C(1));
#endif
    } else {
        flash_code(
            NCR2_LED_RED,
            g_hardware_app_codec_failures);
    }

    /*
     * Candidate 3 and the AK4619 reset signature are now proven on the
     * physical pedal. Stop turning the user into a register decoder: as
     * soon as configuration and post-routing readback succeed, hold the
     * exact stock post-SAI control state and expose an ordinary stompbox
     * control indefinitely. The
     * active-low main footswitch is debounced. A short press toggles on its
     * release. During a two-second hold, Type positions 1-8 select engine
     * slots 1-8. Slots 1-4 are factory and slots 5-8 are open. After the
     * selected engine loads, all eight Type positions select effects inside
     * that engine. No second hold duration exists. Off
     * selects the traced IO25 bypass route and emits no DSP signal. On
     * selects the complementary IO24 route, enables the bounded multi-effect
     * processor, and lights the red die. A physical recovery boot remains
     * independently available with a footswitch hold during power-on.
     */
    if (g_hardware_app_codec_failures == UINT32_C(0)) {
        uint8_t raw_pressed =
            ncr2_factory_board_switch_pressed();
        uint8_t debounced_pressed = raw_pressed;
        uint8_t effect_enabled = UINT8_C(0);
        uint8_t engine_select_armed = UINT8_C(0);
        uint32_t stable_ms = UINT32_C(0);
        ncr2_footswitch_gesture_t footswitch_gesture;

        ncr2_footswitch_gesture_init(
            &footswitch_gesture,
            debounced_pressed);

        g_hardware_app_knob_adc_ready =
            (uint32_t)initialize_knob_adc();
        if (g_hardware_app_knob_adc_ready != UINT32_C(0)) {
            (void)sample_knobs(UINT8_C(1));
        }
        initialize_effect_processor();
        /* USB capture is optional and deliberately non-fatal. The pedal's
         * analog path remains usable if no host is attached or USB fails. */
        g_hardware_app_usb_audio_status =
            (uint32_t)ncr2_usb_audio_capture_start();
        g_hardware_app_emit_tone = UINT32_C(0);
        g_hardware_app_enable_effect = UINT32_C(0);
        g_hardware_app_ready = UINT32_C(0x46555A5A);
        ncr2_factory_board_set_relay(UINT8_C(0));
        ncr2_factory_board_set_indicator(NCR2_LED_OFF);
        for (;;) {
            const uint8_t pressed =
                ncr2_factory_board_switch_pressed();

            if (pressed == raw_pressed) {
                if (stable_ms < NCR2_SWITCH_DEBOUNCE_MS) {
                    stable_ms += NCR2_SWITCH_POLL_MS;
                }
            } else {
                raw_pressed = pressed;
                stable_ms = UINT32_C(0);
            }
            if (stable_ms >= NCR2_SWITCH_DEBOUNCE_MS &&
                debounced_pressed != raw_pressed) {
                debounced_pressed = raw_pressed;
            }
            const uint8_t footswitch_event =
                ncr2_footswitch_gesture_update(
                    &footswitch_gesture,
                    debounced_pressed,
                    NCR2_SWITCH_POLL_MS);
            if (footswitch_event == NCR2_FOOTSWITCH_EVENT_TAP) {
                effect_enabled =
                    (effect_enabled == UINT8_C(0))
                        ? UINT8_C(1)
                        : UINT8_C(0);
                if (effect_enabled != UINT8_C(0)) {
                    /*
                     * Let the DSP ramp and current algorithm settle behind
                     * the dry route before switching the jack.
                     */
                    g_hardware_app_enable_effect = UINT32_C(1);
                    diagnostic_delay_ms(NCR2_EFFECT_ROUTE_SETTLE_MS);
                    ncr2_factory_board_set_relay(UINT8_C(1));
                } else {
                    /*
                     * Restore analog bypass before ramping down converter
                     * output, so disengaging cannot make a gap.
                     */
                    ncr2_factory_board_set_relay(UINT8_C(0));
                    g_hardware_app_enable_effect = UINT32_C(0);
                }
                ncr2_factory_board_set_indicator(
                    (effect_enabled != UINT8_C(0))
                        ? NCR2_LED_RED
                        : NCR2_LED_OFF);
            } else if (
                footswitch_event == NCR2_FOOTSWITCH_EVENT_HOLD) {
                /*
                 * Match the factory monitor: reaching two seconds arms a
                 * selection, and release commits it. The release boundary
                 * keeps a held switch out of power-on recovery detection.
                 */
                engine_select_armed = UINT8_C(1);
                effect_enabled = UINT8_C(0);
                ncr2_factory_board_set_relay(UINT8_C(0));
                g_hardware_app_enable_effect = UINT32_C(0);
                ncr2_factory_board_set_indicator(NCR2_LED_BOTH);
            } else if (engine_select_armed != UINT8_C(0) &&
                       debounced_pressed == UINT8_C(0)) {
                if (g_hardware_app_knob_adc_ready != UINT32_C(0)) {
                    (void)sample_knobs(UINT8_C(0));
                }
                const uint32_t selector_position =
                    g_hardware_app_selector_position;
                const uint8_t engine_slot =
                    (uint8_t)selector_position;

                /* Announce the final Type position and load that slot. */
                ncr2_factory_board_set_indicator(NCR2_LED_OFF);
                flash_code(
                    NCR2_LED_BOTH,
                    (uint32_t)engine_slot + UINT32_C(1));
                reset_into_engine_slot(engine_slot);
            }
            if (g_hardware_app_knob_adc_ready != UINT32_C(0)) {
                (void)sample_knobs(UINT8_C(0));
            }
            diagnostic_delay_ms(NCR2_SWITCH_POLL_MS);
            ++g_hardware_app_heartbeat;
        }
    }

    {
        uint8_t mic_gain = UINT8_C(0);
        uint8_t format = UINT8_C(0);
        uint8_t power = UINT8_C(0);
        const int mic_gain_status = ncr2_codec_read_register(
            NCR2_AK4619_REG_MIC_GAIN, &mic_gain);
        const int format_status = ncr2_codec_read_register(
            NCR2_AK4619_REG_FORMAT, &format);
        const int power_status = ncr2_codec_read_register(
            NCR2_AK4619_REG_POWER, &power);

        g_hardware_app_codec_mic_gain_readback = mic_gain;
        g_hardware_app_codec_mic_gain_status =
            (uint32_t)mic_gain_status;
        g_hardware_app_codec_readback = format;
        g_hardware_app_codec_format_status =
            (uint32_t)format_status;
        g_hardware_app_codec_power_readback = power;
        g_hardware_app_codec_power_status =
            (uint32_t)power_status;

        report_register_byte(
            UINT32_C(1), mic_gain_status, mic_gain);
        report_register_byte(
            UINT32_C(2), format_status, format);
        report_register_byte(
            UINT32_C(3), power_status, power);

        if (mic_gain_status != NCR2_I2C_OK ||
            mic_gain != NCR2_AK4619_MIC_GAIN_DEFAULT ||
            format_status != NCR2_I2C_OK ||
            format != NCR2_AK4619_FORMAT_TDM128 ||
            power_status != NCR2_I2C_OK ||
            power != NCR2_AK4619_POWER_ALL) {
            ++g_hardware_app_codec_failures;
        }
    }
    if (g_hardware_app_codec_failures != UINT32_C(0)) {
        /*
         * Four red here is now unambiguous: configuration failed while
         * every reset candidate was still released, before GPIO restore.
         */
        flash_code(NCR2_LED_RED, UINT32_C(4));
        reset_into_recovery();
    }

    /*
     * The recovered normal state differs from the all-high probe state on
     * four already-swept GPIO2 candidates. Lower them one at a time and
     * read the live power register. The candidate that makes the read fail
     * or return anything other than 0x37 is the codec PDN.
     */
    {
        static const uint32_t candidates[] = {
            UINT32_C(3), /* GPIO2_IO11 */
            UINT32_C(4), /* GPIO2_IO23 */
            UINT32_C(6), /* GPIO2_IO25 */
            UINT32_C(8), /* GPIO2_IO27 */
        };

        for (uint32_t probe = UINT32_C(0);
             probe < (sizeof(candidates) / sizeof(candidates[0]));
             ++probe) {
            const uint32_t candidate = candidates[probe];
            uint8_t power = UINT8_C(0);
            int status;

            ncr2_factory_board_set_candidate(candidate, UINT8_C(0));
            diagnostic_delay_ms(UINT32_C(2));
            status = ncr2_codec_read_register(
                NCR2_AK4619_REG_POWER, &power);
            ncr2_factory_board_set_candidate(candidate, UINT8_C(1));

            if (status != NCR2_I2C_OK ||
                power != NCR2_AK4619_POWER_ALL) {
                g_hardware_app_codec_reset_candidate = candidate;
                diagnostic_delay_ms(NCR2_CODEC_SETTLE_MS);
                g_hardware_app_codec_failures =
                    ncr2_codec_configure();
                break;
            }
            diagnostic_delay_ms(UINT32_C(2));
        }
    }

    if (g_hardware_app_codec_reset_candidate >=
        NCR2_FACTORY_BOARD_CANDIDATE_COUNT) {
        /* Nine red: none of the four changed GPIOs explains v0.7.2. */
        flash_code(
            NCR2_LED_RED,
            NCR2_FACTORY_BOARD_CANDIDATE_COUNT);
        reset_into_recovery();
    }

    /*
     * Yellow identifies the discovered control using the documented
     * one-based candidate number. Restore all other pins to their recovered
     * running levels while atomically preserving this one high.
     */
    flash_code(
        NCR2_LED_BOTH,
        g_hardware_app_codec_reset_candidate + UINT32_C(1));
    ncr2_factory_board_restore_audio_active(
        g_hardware_app_codec_reset_candidate);
    ncr2_factory_board_set_relay(UINT8_C(0));
    diagnostic_delay_ms(UINT32_C(2));

    {
        uint8_t power = UINT8_C(0);
        const int status = ncr2_codec_read_register(
            NCR2_AK4619_REG_POWER, &power);

        g_hardware_app_codec_power_readback = power;
        g_hardware_app_codec_power_status = (uint32_t)status;
        if (status != NCR2_I2C_OK ||
            power != NCR2_AK4619_POWER_ALL) {
            ++g_hardware_app_codec_failures;
        }
    }
    flash_code(
        (g_hardware_app_codec_failures == UINT32_C(0))
            ? NCR2_LED_GREEN
            : NCR2_LED_RED,
        UINT32_C(4));

    /*
     * First make the output path unmistakable with two short tone/silence
     * cycles. Then immediately run the requested hard-clipping overdrive;
     * no second diagnostic image is needed once the tone is heard.
     */
    for (uint32_t cycle = UINT32_C(0);
         cycle < NCR2_TONE_CYCLES;
         ++cycle) {
        g_hardware_app_emit_tone = UINT32_C(1);
        show_indicator(NCR2_LED_BOTH, NCR2_TONE_PHASE_MS);
        g_hardware_app_emit_tone = UINT32_C(0);
        show_indicator(NCR2_LED_OFF, NCR2_TONE_PHASE_MS);
        ++g_hardware_app_heartbeat;
    }

    g_hardware_app_enable_effect = UINT32_C(1);
    for (uint32_t elapsed = UINT32_C(0);
         elapsed < NCR2_DRIVE_DURATION_MS;
         elapsed += NCR2_METER_INTERVAL_MS) {
        const int32_t peak = g_hardware_app_input_peak;

        if (peak > (int32_t)g_hardware_app_peak_hold) {
            g_hardware_app_peak_hold = (uint32_t)peak;
        }
        g_hardware_app_input_peak = INT32_C(0);
        show_indicator(
            (peak > NCR2_INPUT_FLOOR) ? NCR2_LED_GREEN : NCR2_LED_OFF,
            NCR2_METER_INTERVAL_MS);
        ++g_hardware_app_heartbeat;
    }
    g_hardware_app_enable_effect = UINT32_C(0);

    /*
     * Report the largest captured magnitude as a bit count, so silence and
     * a signal too small to trip the meter are no longer the same result.
     * One red means the capture buffer held exactly zero.
     */
    {
        uint32_t bits = UINT32_C(0);
        uint32_t value = g_hardware_app_peak_hold;

        while (value != UINT32_C(0)) {
            ++bits;
            value >>= 1;
        }
        if (bits == UINT32_C(0)) {
            flash_code(NCR2_LED_RED, UINT32_C(1));
        } else {
            flash_code(NCR2_LED_BOTH, bits);
        }
    }

    ncr2_factory_board_set_relay(UINT8_C(0));
    ncr2_factory_board_restore_idle();
    diagnostic_delay_ms(NCR2_LED_GROUP_MS);
    g_hardware_app_ready = UINT32_C(0x4f50454e);
    reset_into_recovery();
}
