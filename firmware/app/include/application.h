#ifndef NCR2_APPLICATION_H
#define NCR2_APPLICATION_H

#include <stdint.h>

void application_main(void);
uint16_t application_programs_ready(
    uint32_t program_count,
    uint32_t initial_program);
uint16_t application_navigation_sample(
    int pressed,
    uint32_t elapsed_ms);

#endif
