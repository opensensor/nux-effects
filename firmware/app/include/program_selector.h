#ifndef NCR2_PROGRAM_SELECTOR_H
#define NCR2_PROGRAM_SELECTOR_H

#include <stdint.h>

#define PROGRAM_SELECTOR_DEFAULT_HOLD_MS UINT32_C(5000)
#define PROGRAM_SELECTOR_DEFAULT_RELEASE_MS UINT32_C(50)

enum program_selector_status {
    PROGRAM_SELECTOR_OK = 0,
    PROGRAM_SELECTOR_INVALID_ARGUMENT = 1,
};

typedef struct program_selector {
    uint32_t program_count;
    uint32_t current_program;
    uint32_t hold_threshold_ms;
    uint32_t release_rearm_ms;
    uint32_t pressed_ms;
    uint32_t released_ms;
    int latched;
} program_selector_t;

uint16_t program_selector_initialize(
    program_selector_t *selector,
    uint32_t program_count,
    uint32_t initial_program,
    uint32_t hold_threshold_ms,
    uint32_t release_rearm_ms);
uint16_t program_selector_sample(
    program_selector_t *selector,
    int pressed,
    uint32_t elapsed_ms,
    int *program_changed);
uint16_t program_selector_select(
    program_selector_t *selector,
    uint32_t program);

#endif
