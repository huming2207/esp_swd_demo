#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ok;
    double avg_amps;
    double min_amps;
    double max_amps;
    double vin_volts;
    uint32_t samples;
    uint32_t low_samples;
    uint32_t mid_samples;
    uint32_t high_samples;
    uint32_t steps;
    uint32_t trips;
} power_profiler_result_t;

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

/**
 * Start auto-ranged current sampling on the other CPU core. Blocks through the
 * warm-up, whose samples are discarded. Returns false if the sensors are not ready.
 */
bool power_profiler_sample_begin(uint32_t warmup_ms);

/** Stop sampling and return the results since the warm-up ended. */
void power_profiler_sample_end(power_profiler_result_t *result);

#ifdef __cplusplus
}
#endif
