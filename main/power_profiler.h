#pragma once

#include <stdbool.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Set up the debug-line switch, range latches, I2C bus and the three INA226s.
 * Both range bypasses are forced on. The debug-line GPIO is usable even when
 * this returns an error, so the SWD test can still run.
 */
esp_err_t power_profiler_init(void);

/** Connect (DBG_EN high) or isolate the SWD, reset, BOOT and UART target lines. */
void power_profiler_set_debug_lines(bool connect);

/** Force both range bypasses on, so the target is behind the 50 mohm shunt only. */
void power_profiler_force_bypass(void);

/**
 * Run the range ladder and auto-ranging tests, then log cumulative results.
 * Does nothing if power_profiler_init() failed. Leaves both bypasses on.
 */
void power_profiler_run_test(const char *label);

#ifdef __cplusplus
}
#endif
