#include "factory_board.h"

#include <stdint.h>

#include "MIMXRT1051.h"
#include "fsl_clock.h"
#include "fsl_iomuxc.h"

#ifndef NCR2_FACTORY_SLOT_BOARD_CONTROLS
#define NCR2_FACTORY_SLOT_BOARD_CONTROLS 0
#endif

#ifndef NCR2_FACTORY_BOARD_EFFECT_ACTIVE_LOW
#define NCR2_FACTORY_BOARD_EFFECT_ACTIVE_LOW 0
#endif

#if NCR2_FACTORY_SLOT_BOARD_CONTROLS

#define NCR2_GPIO1_IO24_MASK (UINT32_C(1) << 24)
#define NCR2_GPIO1_IO26_MASK (UINT32_C(1) << 26)
#define NCR2_GPIO1_IO31_MASK (UINT32_C(1) << 31)
#define NCR2_GPIO1_IO21_MASK (UINT32_C(1) << 21)
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
#define NCR2_GPIO2_AUDIO_ACTIVE_HIGH \
    (NCR2_GPIO2_IO23_MASK | \
     NCR2_GPIO2_IO25_MASK | \
     NCR2_GPIO2_IO27_MASK)

#define NCR2_BOARD_RELEASE_DELAY_US UINT32_C(100000)
#define NCR2_MICROSECONDS_PER_SECOND UINT32_C(1000000)
#define NCR2_MILLISECONDS_PER_SECOND UINT32_C(1000)
#define NCR2_SWITCH_PAD_CONFIG UINT32_C(0x70b0)

static void configure_control_pins(void)
{
    CLOCK_EnableClock(kCLOCK_Iomuxc);
    /*
     * The stock DCD ungates every CCGR, but a diagnostic must not depend
     * on that to explain a dark board.
     */
    CLOCK_EnableClock(kCLOCK_Gpio1);
    CLOCK_EnableClock(kCLOCK_Gpio2);

    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_05_GPIO1_IO21, 1U);
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
        IOMUXC_GPIO_AD_B1_05_GPIO1_IO21,
        NCR2_SWITCH_PAD_CONFIG);
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

/*
 * Some Cortex-M7 implementations ignore DWT control writes until the
 * CoreSight lock is released, and a stopped cycle counter turns every
 * delay into an unbounded spin. Probe once and remember the answer.
 */
#define NCR2_DWT_LOCK_ACCESS (*(volatile uint32_t *)UINT32_C(0xE0001FB0))
#define NCR2_DWT_UNLOCK_KEY UINT32_C(0xC5ACCE55)
#define NCR2_DWT_PROBE_ITERATIONS UINT32_C(256)
#define NCR2_FALLBACK_CYCLES_PER_ITERATION UINT32_C(4)
#define NCR2_DELAY_MAX_MILLISECONDS UINT32_C(4000)

static uint8_t g_cycle_counter_probed;
static uint8_t g_cycle_counter_usable;

static uint8_t cycle_counter_usable(void)
{
    uint32_t first;
    uint32_t second;

    if (g_cycle_counter_probed != UINT8_C(0)) {
        return g_cycle_counter_usable;
    }
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    NCR2_DWT_LOCK_ACCESS = NCR2_DWT_UNLOCK_KEY;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    first = DWT->CYCCNT;
    for (volatile uint32_t index = UINT32_C(0);
         index < NCR2_DWT_PROBE_ITERATIONS;
         ++index) {
        __NOP();
    }
    second = DWT->CYCCNT;
    g_cycle_counter_usable = (second != first) ? UINT8_C(1) : UINT8_C(0);
    g_cycle_counter_probed = UINT8_C(1);
    return g_cycle_counter_usable;
}

/*
 * Service WDOG1 from inside the wait itself. Refreshing only between waits
 * is not enough: SystemCoreClock is a compile-time constant here, so if it
 * overstates the real core clock a single "two second" wait can outlast the
 * watchdog and reset the board mid-diagnostic. Refreshing in the loop makes
 * a miscalibrated delay merely slow instead of fatal, while a genuine hang
 * outside a delay still trips the watchdog as intended.
 */
static void service_watchdog(void)
{
    WDOG1->WSR = UINT16_C(0x5555);
    WDOG1->WSR = UINT16_C(0xAAAA);
}

static void delay_milliseconds(uint32_t milliseconds)
{
    const uint32_t cycles_per_millisecond =
        SystemCoreClock / NCR2_MILLISECONDS_PER_SECOND;
    uint32_t bounded = milliseconds;

    if (bounded > NCR2_DELAY_MAX_MILLISECONDS) {
        bounded = NCR2_DELAY_MAX_MILLISECONDS;
    }
    if (cycle_counter_usable() != UINT8_C(0)) {
        const uint32_t target = cycles_per_millisecond * bounded;
        const uint32_t start = DWT->CYCCNT;

        while ((uint32_t)(DWT->CYCCNT - start) < target) {
            service_watchdog();
        }
        return;
    }
    for (volatile uint32_t remaining =
             (cycles_per_millisecond /
              NCR2_FALLBACK_CYCLES_PER_ITERATION) * bounded;
         remaining != UINT32_C(0);
         --remaining) {
        service_watchdog();
    }
}

static void delay_microseconds(uint32_t microseconds)
{
    delay_milliseconds(
        microseconds /
        (NCR2_MICROSECONDS_PER_SECOND /
         NCR2_MILLISECONDS_PER_SECOND));
}

typedef struct board_output_candidate {
    GPIO_Type *port;
    uint32_t mask;
    uint8_t idle_high;
} board_output_candidate_t;

/*
 * The recovered factory startup levels: GPIO1_IO24 and GPIO1_IO31 idle
 * high, GPIO2_IO26 idles high, and the remaining traced outputs idle low.
 */
static const board_output_candidate_t g_candidates[] = {
    { GPIO1, NCR2_GPIO1_IO24_MASK, UINT8_C(1) },
    { GPIO1, NCR2_GPIO1_IO26_MASK, UINT8_C(0) },
    { GPIO1, NCR2_GPIO1_IO31_MASK, UINT8_C(1) },
    { GPIO2, NCR2_GPIO2_IO11_MASK, UINT8_C(0) },
    { GPIO2, NCR2_GPIO2_IO23_MASK, UINT8_C(0) },
    { GPIO2, NCR2_GPIO2_IO24_MASK, UINT8_C(0) },
    { GPIO2, NCR2_GPIO2_IO25_MASK, UINT8_C(0) },
    { GPIO2, NCR2_GPIO2_IO26_MASK, UINT8_C(1) },
    { GPIO2, NCR2_GPIO2_IO27_MASK, UINT8_C(0) },
};

_Static_assert(
    (sizeof(g_candidates) / sizeof(g_candidates[0])) ==
        NCR2_FACTORY_BOARD_CANDIDATE_COUNT,
    "candidate blink codes must match the documented pin order");

static void drive_candidate(
    const board_output_candidate_t *candidate,
    uint8_t high)
{
    if (high != UINT8_C(0)) {
        candidate->port->DR_SET = candidate->mask;
    } else {
        candidate->port->DR_CLEAR = candidate->mask;
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
    GPIO1->GDIR &= ~NCR2_GPIO1_IO21_MASK;
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
#if NCR2_FACTORY_BOARD_EFFECT_ACTIVE_LOW
    /*
     * GPIO1_IO24 and GPIO1_IO31 are the paired outputs changed by the
     * factory pass/bypass state routine. Keep the recovered high startup
     * level while clocks and DMA settle, then test the active-low effect
     * polarity only in the explicitly gated hardware application.
     */
    GPIO1->DR &=
        ~(NCR2_GPIO1_IO24_MASK | NCR2_GPIO1_IO31_MASK);
#endif
    __DSB();
#endif
}

void ncr2_factory_board_set_indicator(
    ncr2_factory_indicator_state_t state)
{
#if NCR2_FACTORY_SLOT_BOARD_CONTROLS
    const uint32_t indicator_mask =
        NCR2_GPIO1_IO24_MASK | NCR2_GPIO1_IO31_MASK;
    uint32_t low_mask = 0U;

    if (state == NCR2_FACTORY_INDICATOR_IO24) {
        low_mask = NCR2_GPIO1_IO24_MASK;
    } else if (state == NCR2_FACTORY_INDICATOR_IO31) {
        low_mask = NCR2_GPIO1_IO31_MASK;
    } else if (state == NCR2_FACTORY_INDICATOR_BOTH_LOW) {
        low_mask = indicator_mask;
    }

    /*
     * The RT1051 GPIO set/clear aliases avoid exposing an intermediate
     * read-modify-write state to interrupts or future control tasks.
     */
    GPIO1->DR_SET = indicator_mask;
    GPIO1->DR_CLEAR = low_mask;
    __DSB();
#else
    (void)state;
#endif
}

uint8_t ncr2_factory_board_switch_pressed(void)
{
#if NCR2_FACTORY_SLOT_BOARD_CONTROLS
    return ((GPIO1->PSR & NCR2_GPIO1_IO21_MASK) == UINT32_C(0))
               ? UINT8_C(1)
               : UINT8_C(0);
#else
    return UINT8_C(0);
#endif
}

uint32_t ncr2_factory_board_candidate_count(void)
{
#if NCR2_FACTORY_SLOT_BOARD_CONTROLS
    return (uint32_t)(sizeof(g_candidates) / sizeof(g_candidates[0]));
#else
    return UINT32_C(0);
#endif
}

void ncr2_factory_board_restore_idle(void)
{
#if NCR2_FACTORY_SLOT_BOARD_CONTROLS
    for (uint32_t index = UINT32_C(0);
         index < ncr2_factory_board_candidate_count();
         ++index) {
        drive_candidate(
            &g_candidates[index],
            g_candidates[index].idle_high);
    }
    __DSB();
#endif
}

void ncr2_factory_board_restore_audio_active(
    uint32_t reset_candidate)
{
#if NCR2_FACTORY_SLOT_BOARD_CONTROLS
    uint32_t gpio1_high =
        NCR2_GPIO1_INITIAL_HIGH | NCR2_GPIO1_IO26_MASK;
    uint32_t gpio2_high = NCR2_GPIO2_AUDIO_ACTIVE_HIGH;

    /*
     * This is the state reached by the stock runtime after SAI1 starts, not
     * the earlier state captured at the end of the audio initializer. A
     * continued factory trace shows GPIO2_IO23/25/27 asserted while
     * IO11/24/26 are all cleared. IO24/25 are the complementary audio-route
     * controls; this selects the ordinary bypass state.
     *
     * GPIO1_IO26 has completed its factory low-to-high release transition.
     * The reset_candidate argument is retained for the bounded PDN
     * diagnostic below; normal operation passes the candidate count and
     * reproduces the exact traced runtime state.
     */
    if (reset_candidate < ncr2_factory_board_candidate_count()) {
        const board_output_candidate_t *candidate =
            &g_candidates[reset_candidate];

        if (candidate->port == GPIO1) {
            gpio1_high |= candidate->mask;
        } else if (candidate->port == GPIO2) {
            gpio2_high |= candidate->mask;
        }
    }
    GPIO1->DR =
        (GPIO1->DR & ~NCR2_GPIO1_CONTROL_MASK) |
        gpio1_high;
    GPIO2->DR =
        (GPIO2->DR & ~NCR2_GPIO2_CONTROL_MASK) |
        gpio2_high;
    __DSB();
#else
    (void)reset_candidate;
#endif
}

void ncr2_factory_board_set_candidate(
    uint32_t index,
    uint8_t high)
{
#if NCR2_FACTORY_SLOT_BOARD_CONTROLS
    if (index >= ncr2_factory_board_candidate_count()) {
        return;
    }
    drive_candidate(&g_candidates[index], high);
    __DSB();
#else
    (void)index;
    (void)high;
#endif
}

void ncr2_factory_board_pulse_candidate(uint32_t index)
{
#if NCR2_FACTORY_SLOT_BOARD_CONTROLS
    if (index >= ncr2_factory_board_candidate_count()) {
        return;
    }
    ncr2_factory_board_restore_idle();
    drive_candidate(
        &g_candidates[index],
        (g_candidates[index].idle_high != UINT8_C(0))
            ? UINT8_C(0)
            : UINT8_C(1));
    __DSB();
#else
    (void)index;
#endif
}

void ncr2_factory_board_release_reset_candidates(void)
{
#if NCR2_FACTORY_SLOT_BOARD_CONTROLS
    /*
     * The AK4619 holds its control interface in reset while PDN is low, so
     * a scan would find nothing if PDN happens to be one of the traced
     * outputs. Keep the traced bypass route selected: IO25 high, IO24 low.
     */
    for (uint32_t index = UINT32_C(0);
         index < ncr2_factory_board_candidate_count();
         ++index) {
        if (g_candidates[index].port == GPIO2 &&
            g_candidates[index].mask == NCR2_GPIO2_IO24_MASK) {
            continue;
        }
        drive_candidate(&g_candidates[index], UINT8_C(1));
    }
    __DSB();
#endif
}

void ncr2_factory_board_set_relay(uint8_t engaged)
{
#if NCR2_FACTORY_SLOT_BOARD_CONTROLS
    /*
     * The stock bank updater at ITCM 0x5532 treats IO24 and IO25 as a
     * complementary route pair. Its ordinary startup state selects IO25
     * (IO24 low, IO25 high); changing the effect-state byte selects IO24
     * (IO25 low, IO24 high). Driving IO24 high without clearing IO25, as
     * the first source tests did, creates a state the stock firmware never
     * uses and does not switch the audio route.
     *
     * Update the pair with one DR write so the controls are never left
     * asserted together.
     */
    uint32_t route =
        GPIO2->DR &
        ~(NCR2_GPIO2_IO24_MASK | NCR2_GPIO2_IO25_MASK);

    if (engaged != UINT8_C(0)) {
        route |= NCR2_GPIO2_IO24_MASK;
    } else {
        route |= NCR2_GPIO2_IO25_MASK;
    }
    GPIO2->DR = route;
    __DSB();
#else
    (void)engaged;
#endif
}

void ncr2_factory_board_delay_ms(uint32_t milliseconds)
{
#if NCR2_FACTORY_SLOT_BOARD_CONTROLS
    delay_milliseconds(milliseconds);
#else
    (void)milliseconds;
#endif
}
