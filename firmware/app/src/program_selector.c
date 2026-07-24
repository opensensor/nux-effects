#include "program_selector.h"

#include <stddef.h>

static uint32_t saturating_add(
    uint32_t value,
    uint32_t increment)
{
    if (increment > UINT32_MAX - value) {
        return UINT32_MAX;
    }
    return value + increment;
}

uint16_t program_selector_initialize(
    program_selector_t *selector,
    uint32_t program_count,
    uint32_t initial_program,
    uint32_t hold_threshold_ms,
    uint32_t release_rearm_ms)
{
    if (selector == NULL ||
        program_count == UINT32_C(0) ||
        initial_program >= program_count ||
        hold_threshold_ms == UINT32_C(0) ||
        release_rearm_ms == UINT32_C(0)) {
        return PROGRAM_SELECTOR_INVALID_ARGUMENT;
    }

    selector->program_count = program_count;
    selector->current_program = initial_program;
    selector->hold_threshold_ms = hold_threshold_ms;
    selector->release_rearm_ms = release_rearm_ms;
    selector->pressed_ms = UINT32_C(0);
    selector->released_ms = UINT32_C(0);
    selector->latched = 0;
    return PROGRAM_SELECTOR_OK;
}

uint16_t program_selector_sample(
    program_selector_t *selector,
    int pressed,
    uint32_t elapsed_ms,
    int *program_changed)
{
    if (selector == NULL ||
        program_changed == NULL ||
        selector->program_count == UINT32_C(0)) {
        return PROGRAM_SELECTOR_INVALID_ARGUMENT;
    }
    *program_changed = 0;

    if (pressed == 0) {
        selector->pressed_ms = UINT32_C(0);
        if (selector->latched != 0) {
            selector->released_ms =
                saturating_add(
                    selector->released_ms,
                    elapsed_ms);
            if (selector->released_ms >=
                selector->release_rearm_ms) {
                selector->latched = 0;
                selector->released_ms = UINT32_C(0);
            }
        } else {
            selector->released_ms = UINT32_C(0);
        }
        return PROGRAM_SELECTOR_OK;
    }

    selector->released_ms = UINT32_C(0);
    if (selector->latched != 0) {
        return PROGRAM_SELECTOR_OK;
    }
    selector->pressed_ms =
        saturating_add(selector->pressed_ms, elapsed_ms);
    if (selector->pressed_ms <
        selector->hold_threshold_ms) {
        return PROGRAM_SELECTOR_OK;
    }

    if (selector->program_count > UINT32_C(1)) {
        selector->current_program =
            (selector->current_program + UINT32_C(1)) %
            selector->program_count;
        *program_changed = 1;
    }
    selector->pressed_ms = UINT32_C(0);
    selector->latched = 1;
    return PROGRAM_SELECTOR_OK;
}

uint16_t program_selector_select(
    program_selector_t *selector,
    uint32_t program)
{
    if (selector == NULL ||
        selector->program_count == UINT32_C(0) ||
        program >= selector->program_count) {
        return PROGRAM_SELECTOR_INVALID_ARGUMENT;
    }
    selector->current_program = program;
    selector->pressed_ms = UINT32_C(0);
    selector->released_ms = UINT32_C(0);
    selector->latched = 0;
    return PROGRAM_SELECTOR_OK;
}
