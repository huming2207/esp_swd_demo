#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Measure STM32WLE5 current during SWD traffic, in Stop 2 with debug kept
 * alive, and in Standby with debug and the RTC off.
 *
 * Needs an open SWD session and a ready power profiler. Resets the target
 * repeatedly, turns off its RTC and LSE, and restarts it at the end.
 */
void target_power_test_run(void);

#ifdef __cplusplus
}
#endif
