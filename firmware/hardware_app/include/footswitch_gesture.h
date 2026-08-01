#ifndef NCR2_FOOTSWITCH_GESTURE_H
#define NCR2_FOOTSWITCH_GESTURE_H

#include <stdint.h>

#define NCR2_FOOTSWITCH_HOLD_MS UINT32_C(2000)

typedef struct {
    uint32_t held_ms;
    uint8_t pressed;
    uint8_t hold_reported;
} ncr2_footswitch_gesture_t;

enum {
    NCR2_FOOTSWITCH_EVENT_NONE = 0,
    NCR2_FOOTSWITCH_EVENT_TAP = 1,
    NCR2_FOOTSWITCH_EVENT_HOLD = 2,
};

/* An initially held switch is ignored until release (power-on recovery). */
void ncr2_footswitch_gesture_init(
    ncr2_footswitch_gesture_t *gesture,
    uint8_t initially_pressed);

uint8_t ncr2_footswitch_gesture_update(
    ncr2_footswitch_gesture_t *gesture,
    uint8_t pressed,
    uint32_t elapsed_ms);

#endif
