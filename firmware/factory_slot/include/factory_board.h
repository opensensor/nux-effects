#ifndef NCR2_FACTORY_BOARD_H
#define NCR2_FACTORY_BOARD_H

#include <stdint.h>

enum
{
    NCR2_FACTORY_BOARD_OK = 0,
    NCR2_FACTORY_BOARD_DISABLED = 1,
};

typedef enum
{
    NCR2_FACTORY_INDICATOR_OFF = 0,
    NCR2_FACTORY_INDICATOR_IO24 = 1,
    NCR2_FACTORY_INDICATOR_IO31 = 2,
    NCR2_FACTORY_INDICATOR_BOTH_LOW = 3,
} ncr2_factory_indicator_state_t;

/*
 * Reproduce the factory output-pin configuration while keeping the
 * GPIO1_IO26 candidate control at its initial low level.
 */
uint16_t ncr2_factory_board_prepare_audio(void);

/*
 * Preserve the factory 100 ms low interval, then drive GPIO1_IO26 high.
 * Call only after the digital audio path is ready.
 */
void ncr2_factory_board_release_audio(void);

/*
 * Drive the factory GPIO1_IO24/GPIO1_IO31 bicolor indicator pair.
 * Both outputs high is the factory off state. The two single-low states
 * select the two observed colors. Both-low is also exercised by the stock
 * image and is retained as a safe diagnostic state until its optical result
 * is confirmed on hardware.
 */
void ncr2_factory_board_set_indicator(
    ncr2_factory_indicator_state_t state);

/*
 * Read the primary active-low footswitch used by the open bootloader's
 * physical recovery gesture. The board setup leaves this pad as an input,
 * so the effect application can use the same confirmed wiring at runtime.
 */
uint8_t ncr2_factory_board_switch_pressed(void);

/*
 * Every GPIO output the stock engine is known to drive, in a fixed order.
 * A candidate is identified on hardware by its one-based index, which the
 * diagnostic emits as a blink count.
 *
 * 0 GPIO1_IO24   3 GPIO2_IO11   6 GPIO2_IO25
 * 1 GPIO1_IO26   4 GPIO2_IO23   7 GPIO2_IO26
 * 2 GPIO1_IO31   5 GPIO2_IO24   8 GPIO2_IO27
 */
#define NCR2_FACTORY_BOARD_CANDIDATE_COUNT UINT32_C(9)

uint32_t ncr2_factory_board_candidate_count(void);

/*
 * Restore every candidate to the recovered factory startup level.
 */
void ncr2_factory_board_restore_idle(void);

/*
 * Restore the post-SAI state observed by continuing the stock runtime:
 * GPIO1_IO26 released, GPIO2_IO23/25/27 high, and GPIO2_IO11/24/26 low.
 * IO24/25 form the complementary audio-route pair, so this is the ordinary
 * bypass state. Pass the candidate count in normal operation. A lower value
 * is retained only for the bounded PDN diagnostic.
 */
void ncr2_factory_board_restore_audio_active(
    uint32_t reset_candidate);

/*
 * Change one traced output without disturbing the other candidates. This is
 * used by the hardware diagnostic to lower each plausible PDN briefly and
 * identify the one that resets the codec register map.
 */
void ncr2_factory_board_set_candidate(
    uint32_t index,
    uint8_t high);

/*
 * Drive one candidate to the inverse of its recovered factory startup
 * level and leave the others idle. Toggling relative to the factory level
 * rather than assuming active-low keeps the sweep polarity-agnostic: an
 * indicator that idles lit shows a visible gap instead of a visible pulse.
 */
void ncr2_factory_board_pulse_candidate(uint32_t index);

/*
 * Bounded busy-wait. Uses the cycle counter when it actually advances and
 * falls back to an instruction-count loop otherwise, so a locked or absent
 * DWT cannot silently stall a diagnostic forever.
 */
void ncr2_factory_board_delay_ms(uint32_t milliseconds);

/*
 * Select the stock complementary GPIO2_IO24/IO25 relay state. Bypass selects
 * IO25 (IO24 low); engaged selects IO24 (IO25 low). The pair must never be
 * left high together.
 */
void ncr2_factory_board_set_relay(uint8_t engaged);

/*
 * Drive every traced output high except IO24, while keeping the complementary
 * route on IO25. Used before probing the control bus, since a codec held in
 * reset never acknowledges.
 */
void ncr2_factory_board_release_reset_candidates(void);

#endif
