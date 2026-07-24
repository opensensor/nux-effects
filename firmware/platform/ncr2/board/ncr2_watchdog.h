#ifndef NCR2_WATCHDOG_H
#define NCR2_WATCHDOG_H

#include <stdint.h>

/*
 * RT1051 WDOG1 timeout is (value + 1) / 2 seconds. A value of 0x0f gives
 * a pending image eight seconds to reach its health gate.
 */
#define NCR2_TRIAL_WATCHDOG_TIMEOUT_VALUE UINT16_C(0x000F)

void ncr2_board_watchdog_start_trial(void *context);
void ncr2_board_watchdog_refresh(void);
uint16_t ncr2_board_watchdog_reset_status(void);

#endif
