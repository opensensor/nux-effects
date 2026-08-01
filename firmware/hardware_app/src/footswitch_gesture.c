#include "footswitch_gesture.h"

#include <stdint.h>

void ncr2_footswitch_gesture_init(
    ncr2_footswitch_gesture_t *gesture,
    uint8_t initially_pressed)
{
    if (gesture == (ncr2_footswitch_gesture_t *)0) {
        return;
    }

    gesture->held_ms = UINT32_C(0);
    gesture->pressed =
        (initially_pressed != UINT8_C(0)) ? UINT8_C(1) : UINT8_C(0);
    gesture->hold_reported = gesture->pressed;
}

uint8_t ncr2_footswitch_gesture_update(
    ncr2_footswitch_gesture_t *gesture,
    uint8_t pressed,
    uint32_t elapsed_ms)
{
    const uint8_t normalized =
        (pressed != UINT8_C(0)) ? UINT8_C(1) : UINT8_C(0);

    if (gesture == (ncr2_footswitch_gesture_t *)0) {
        return UINT8_C(NCR2_FOOTSWITCH_EVENT_NONE);
    }

    if (normalized != gesture->pressed) {
        const uint8_t was_pressed = gesture->pressed;
        const uint8_t hold_reported = gesture->hold_reported;

        gesture->pressed = normalized;
        gesture->held_ms =
            (normalized != UINT8_C(0) &&
             elapsed_ms < NCR2_FOOTSWITCH_HOLD_MS)
                ? elapsed_ms
                : ((normalized != UINT8_C(0))
                    ? NCR2_FOOTSWITCH_HOLD_MS
                    : UINT32_C(0));
        gesture->hold_reported = UINT8_C(0);
        if (was_pressed != UINT8_C(0) &&
            normalized == UINT8_C(0) &&
            hold_reported == UINT8_C(0)) {
            return UINT8_C(NCR2_FOOTSWITCH_EVENT_TAP);
        }
        if (normalized != UINT8_C(0) &&
            gesture->held_ms >= NCR2_FOOTSWITCH_HOLD_MS) {
            gesture->hold_reported = UINT8_C(1);
            return UINT8_C(NCR2_FOOTSWITCH_EVENT_HOLD);
        }
        return UINT8_C(NCR2_FOOTSWITCH_EVENT_NONE);
    }

    if (normalized == UINT8_C(0) ||
        gesture->hold_reported != UINT8_C(0)) {
        return UINT8_C(NCR2_FOOTSWITCH_EVENT_NONE);
    }

    if (gesture->held_ms < NCR2_FOOTSWITCH_HOLD_MS) {
        const uint32_t remaining =
            NCR2_FOOTSWITCH_HOLD_MS - gesture->held_ms;
        gesture->held_ms +=
            (elapsed_ms < remaining) ? elapsed_ms : remaining;
    }
    if (gesture->held_ms >= NCR2_FOOTSWITCH_HOLD_MS) {
        gesture->hold_reported = UINT8_C(1);
        return UINT8_C(NCR2_FOOTSWITCH_EVENT_HOLD);
    }
    return UINT8_C(NCR2_FOOTSWITCH_EVENT_NONE);
}
