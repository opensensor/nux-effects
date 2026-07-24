#include "ncr2_watchdog.h"

#include "fsl_wdog.h"

void ncr2_board_watchdog_start_trial(void *context)
{
    wdog_config_t config;

    (void)context;
    WDOG_GetDefaultConfig(&config);
    config.enableWdog = true;
    config.workMode.enableWait = true;
    config.workMode.enableStop = true;
    config.workMode.enableDebug = false;
    config.enableInterrupt = false;
    config.timeoutValue =
        NCR2_TRIAL_WATCHDOG_TIMEOUT_VALUE;
    config.interruptTimeValue = UINT16_C(0);
    config.softwareResetExtension = false;
    config.enablePowerDown = false;
    config.enableTimeOutAssert = false;
    WDOG_Init(WDOG1, &config);
}

void ncr2_board_watchdog_refresh(void)
{
    WDOG_Refresh(WDOG1);
}

uint16_t ncr2_board_watchdog_reset_status(void)
{
    return WDOG_GetStatusFlags(WDOG1);
}
