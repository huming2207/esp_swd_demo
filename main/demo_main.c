#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <esp_cpu.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <swd_host.h>

#define CORTEX_M_DHCSR_ADDRESS 0xe000edf0U

typedef struct {
    uint64_t bytes;
    uint64_t elapsed_us;
    uint64_t min_kb_s_x100;
    uint64_t max_kb_s_x100;
    uint32_t successes;
} transfer_stats_t;

#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
typedef struct {
    swd_perf_counters_t counters;
    uint64_t operation_cycles;
    uint32_t operation_count;
} swd_profile_stats_t;
#endif

static uint64_t transfer_rate_kb_s_x100(size_t bytes, uint64_t elapsed_us)
{
    if (elapsed_us == 0) {
        return 0;
    }

    /* Decimal KB/s, scaled by 100 for two fractional digits. */
    return ((uint64_t)bytes * 100000ULL) / elapsed_us;
}

static void record_transfer(transfer_stats_t *stats, size_t bytes, uint64_t elapsed_us)
{
    const uint64_t rate = transfer_rate_kb_s_x100(bytes, elapsed_us);

    stats->bytes += bytes;
    stats->elapsed_us += elapsed_us;
    stats->successes++;

    if ((stats->min_kb_s_x100 == 0) || (rate < stats->min_kb_s_x100)) {
        stats->min_kb_s_x100 = rate;
    }
    if (rate > stats->max_kb_s_x100) {
        stats->max_kb_s_x100 = rate;
    }
}

static uint64_t average_rate_kb_s_x100(const transfer_stats_t *stats)
{
    return transfer_rate_kb_s_x100(stats->bytes, stats->elapsed_us);
}

static void fill_pattern(uint8_t *buffer, size_t size, uint32_t iteration)
{
    uint32_t state = 0x6d2b79f5U ^ iteration;

    for (size_t i = 0; i < size; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        buffer[i] = (uint8_t)state;
    }
}

static size_t count_mismatches(const uint8_t *expected, const uint8_t *actual,
                               size_t size, size_t *first_mismatch)
{
    size_t mismatches = 0;

    for (size_t i = 0; i < size; ++i) {
        if (expected[i] != actual[i]) {
            if (mismatches == 0) {
                *first_mismatch = i;
            }
            mismatches++;
        }
    }

    return mismatches;
}

static void log_rate(const char *tag, const char *name, const transfer_stats_t *stats)
{
    const uint64_t average = average_rate_kb_s_x100(stats);

    ESP_LOGI(tag,
             "%s: avg=%" PRIu64 ".%02" PRIu64
             " KB/s, min=%" PRIu64 ".%02" PRIu64
             ", max=%" PRIu64 ".%02" PRIu64
             ", successful transfers=%" PRIu32,
             name,
             average / 100, average % 100,
             stats->min_kb_s_x100 / 100, stats->min_kb_s_x100 % 100,
             stats->max_kb_s_x100 / 100, stats->max_kb_s_x100 % 100,
             stats->successes);
}

#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
static void record_swd_profile(swd_profile_stats_t *profile,
                               const swd_perf_counters_t *sample,
                               uint32_t operation_cycles)
{
    swd_perf_counters_t *total = &profile->counters;

    profile->operation_cycles += operation_cycles;
    profile->operation_count++;
    total->transfer_cycles += sample->transfer_cycles;
    total->read_transfer_cycles += sample->read_transfer_cycles;
    total->write_transfer_cycles += sample->write_transfer_cycles;
    total->target_drive_cycles += sample->target_drive_cycles;
    total->host_drive_cycles += sample->host_drive_cycles;
    total->ack_ok_cycles += sample->ack_ok_cycles;
    total->ack_wait_cycles += sample->ack_wait_cycles;
    total->ack_fault_cycles += sample->ack_fault_cycles;
    total->ack_error_cycles += sample->ack_error_cycles;
    total->ack_invalid_cycles += sample->ack_invalid_cycles;
    total->transfer_count += sample->transfer_count;
    total->read_transfer_count += sample->read_transfer_count;
    total->write_transfer_count += sample->write_transfer_count;
    total->target_drive_count += sample->target_drive_count;
    total->host_drive_count += sample->host_drive_count;
    total->ack_ok_count += sample->ack_ok_count;
    total->ack_wait_count += sample->ack_wait_count;
    total->ack_fault_count += sample->ack_fault_count;
    total->ack_error_count += sample->ack_error_count;
    total->ack_invalid_count += sample->ack_invalid_count;
    total->retry_call_count += sample->retry_call_count;
    total->retry_wait_count += sample->retry_wait_count;
    if (sample->retry_max_waits > total->retry_max_waits) {
        total->retry_max_waits = sample->retry_max_waits;
    }

    for (size_t i = 0; i < SWD_PERF_REQUEST_BUCKET_COUNT; ++i) {
        total->request_count[i] += sample->request_count[i];
        total->request_ok_count[i] += sample->request_ok_count[i];
        total->request_wait_count[i] += sample->request_wait_count[i];
    }
    for (size_t i = 0; i < SWD_PERF_WAIT_STREAK_BUCKET_COUNT; ++i) {
        total->retry_wait_streaks[i] += sample->retry_wait_streaks[i];
    }

    if (sample->transfer_count != 0U) {
        if ((total->min_transfer_cycles == 0U) ||
            (sample->min_transfer_cycles < total->min_transfer_cycles)) {
            total->min_transfer_cycles = sample->min_transfer_cycles;
        }
        if (sample->max_transfer_cycles > total->max_transfer_cycles) {
            total->max_transfer_cycles = sample->max_transfer_cycles;
        }
    }
}

static uint64_t average_x100(uint64_t total, uint32_t count)
{
    return count == 0U ? 0U : (total * 100U) / count;
}

static uint64_t percent_x100(uint64_t part, uint64_t whole)
{
    return whole == 0U ? 0U : (part * 10000U) / whole;
}

static void log_swd_profile(const char *tag, const char *name,
                            const swd_profile_stats_t *profile)
{
    const swd_perf_counters_t *stats = &profile->counters;
    const uint64_t outside_cycles =
        profile->operation_cycles > stats->transfer_cycles
            ? profile->operation_cycles - stats->transfer_cycles
            : 0U;
    const uint64_t ownership_cycles =
        stats->target_drive_cycles + stats->host_drive_cycles;
    const uint64_t average_call_cycles =
        average_x100(profile->operation_cycles, profile->operation_count);
    const uint64_t average_outside_cycles =
        average_x100(outside_cycles, profile->operation_count);
    const uint64_t average_transfer_cycles =
        average_x100(stats->transfer_cycles, stats->transfer_count);
    const uint64_t average_target_cycles =
        average_x100(stats->target_drive_cycles, stats->target_drive_count);
    const uint64_t average_host_cycles =
        average_x100(stats->host_drive_cycles, stats->host_drive_count);
    const uint64_t average_ok_cycles =
        average_x100(stats->ack_ok_cycles, stats->ack_ok_count);
    const uint64_t average_wait_cycles =
        average_x100(stats->ack_wait_cycles, stats->ack_wait_count);
    const uint64_t average_waits_per_retry =
        average_x100(stats->retry_wait_count, stats->retry_call_count);
    const uint64_t swd_percent =
        percent_x100(stats->transfer_cycles, profile->operation_cycles);
    const uint64_t ownership_percent =
        percent_x100(ownership_cycles, stats->transfer_cycles);
    const uint64_t average_transfer_ns = stats->transfer_count == 0U
        ? 0U
        : stats->transfer_cycles * 1000U /
              ((uint64_t)CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ *
               stats->transfer_count);

    ESP_LOGI(tag,
             "%s profile: calls=%" PRIu32
             ", avg call=%" PRIu64 ".%02" PRIu64
             " cycles, SWD=%" PRIu64 ".%02" PRIu64
             "%%, avg outside=%" PRIu64 ".%02" PRIu64 " cycles",
             name, profile->operation_count,
             average_call_cycles / 100U, average_call_cycles % 100U,
             swd_percent / 100U, swd_percent % 100U,
             average_outside_cycles / 100U, average_outside_cycles % 100U);
    ESP_LOGI(tag,
             "%s SWD: attempts=%" PRIu32 " (read/write=%" PRIu32
             "/%" PRIu32 "), avg=%" PRIu64 ".%02" PRIu64
             " cycles (%" PRIu64 " ns), min/max=%" PRIu32 "/%" PRIu32,
             name, stats->transfer_count, stats->read_transfer_count,
             stats->write_transfer_count,
             average_transfer_cycles / 100U, average_transfer_cycles % 100U,
             average_transfer_ns,
             stats->min_transfer_cycles, stats->max_transfer_cycles);
    ESP_LOGI(tag,
             "%s translator: target/host=%" PRIu64 ".%02" PRIu64
             "/%" PRIu64 ".%02" PRIu64
             " cycles/change, ownership=%" PRIu64 ".%02" PRIu64
             "%% of SWD; ACK OK/WAIT/FAULT/error/invalid=%" PRIu32
             "/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32,
             name,
             average_target_cycles / 100U, average_target_cycles % 100U,
             average_host_cycles / 100U, average_host_cycles % 100U,
             ownership_percent / 100U, ownership_percent % 100U,
             stats->ack_ok_count, stats->ack_wait_count,
             stats->ack_fault_count, stats->ack_error_count,
             stats->ack_invalid_count);
    ESP_LOGI(tag,
             "%s ACK cost: OK=%" PRIu64 ".%02" PRIu64
             ", WAIT=%" PRIu64 ".%02" PRIu64
             " cycles; logical transfers=%" PRIu32
             ", waits/logical=%" PRIu64 ".%02" PRIu64
             ", max streak=%" PRIu32,
             name,
             average_ok_cycles / 100U, average_ok_cycles % 100U,
             average_wait_cycles / 100U, average_wait_cycles % 100U,
             stats->retry_call_count,
             average_waits_per_retry / 100U, average_waits_per_retry % 100U,
             stats->retry_max_waits);
    ESP_LOGI(tag,
             "%s WAIT streaks 0/1/2/3/4-7/8+=%" PRIu32 "/%" PRIu32
             "/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32,
             name,
             stats->retry_wait_streaks[0], stats->retry_wait_streaks[1],
             stats->retry_wait_streaks[2], stats->retry_wait_streaks[3],
             stats->retry_wait_streaks[4], stats->retry_wait_streaks[5]);

    for (uint32_t request = 0; request < SWD_PERF_REQUEST_BUCKET_COUNT;
         ++request) {
        const uint32_t attempts = stats->request_count[request];
        if (attempts == 0U) {
            continue;
        }

        const uint64_t request_wait_percent = percent_x100(
            stats->request_wait_count[request], attempts);

        ESP_LOGI(tag,
                 "%s request %s %s A=0x%" PRIx32
                 ": attempts=%" PRIu32 ", OK/WAIT=%" PRIu32 "/%" PRIu32
                 ", WAIT=%" PRIu64 ".%02" PRIu64
                 "%%",
                 name,
                 (request & 0x01U) != 0U ? "AP" : "DP",
                 (request & 0x02U) != 0U ? "read" : "write",
                 request & 0x0CU,
                 attempts, stats->request_ok_count[request],
                 stats->request_wait_count[request],
                 request_wait_percent / 100U, request_wait_percent % 100U);
    }
}
#endif

void app_main(void)
{
    static const char *TAG = "main";
    const uint32_t address = CONFIG_SWD_DEMO_RAM_ADDRESS;
    const size_t block_size = CONFIG_SWD_DEMO_BLOCK_SIZE;
    const uint32_t iterations = CONFIG_SWD_DEMO_ITERATIONS;
    const uint32_t progress_interval = CONFIG_SWD_DEMO_PROGRESS_INTERVAL;
    uint8_t *original = NULL;
    uint8_t *write_buffer = NULL;
    uint8_t *read_buffer = NULL;
    bool connected = false;
    bool backup_valid = false;
    bool target_was_halted = false;
    bool target_state_known = false;
    bool safe_to_resume = true;
    transfer_stats_t write_stats = {};
    transfer_stats_t read_stats = {};
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
    swd_profile_stats_t write_profile = {};
    swd_profile_stats_t read_profile = {};
#endif
    uint32_t attempted = 0;
    uint32_t verified = 0;
    uint32_t write_failures = 0;
    uint32_t read_failures = 0;
    uint32_t mismatch_iterations = 0;
    uint64_t mismatched_bytes = 0;
    uint32_t recovery_failures = 0;

    ESP_LOGI(TAG, "Soul Injector Rev 6 SWD transport stress test");
    ESP_LOGI(TAG,
             "RAM range: 0x%08" PRIx32 "..0x%08" PRIx32
             ", block=%u bytes, iterations=%" PRIu32,
             address, address + (uint32_t)block_size - 1U,
             (unsigned)block_size, iterations);
#ifdef CONFIG_ESP_SWD_PHY_AXC2T245
    ESP_LOGI(TAG,
             "SWD config: clock=%d Hz, fast pad=%u NOPs/half-cycle%s, "
             "turnaround=%u ns, idle=%u cycles, dedicated GPIO=%s, "
             "dedicated translator controls=%s, direct SPI2=%s",
             CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ,
             CONFIG_ESP_SWD_FAST_DELAY_NOPS,
#ifdef CONFIG_ESP_SWD_USE_SPI
             " (unused by SPI)",
#else
             "",
#endif
             CONFIG_ESP_SWD_TURNAROUND_DELAY_US * 1000U +
             CONFIG_ESP_SWD_TURNAROUND_DELAY_NS,
             CONFIG_ESP_SWD_IDLE_CYCLES,
#ifdef CONFIG_ESP_SWD_USE_DEDICATED_GPIO
             "yes",
#else
             "no",
#endif
#ifdef CONFIG_ESP_SWD_DEDICATED_TRANSLATOR_CONTROLS
             "yes",
#else
             "no",
#endif
#ifdef CONFIG_ESP_SWD_USE_SPI
             "yes"
#else
             "no"
#endif
    );
#endif
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
    ESP_LOGW(TAG,
             "SWD cycle instrumentation enabled at %u MHz; profiler bookkeeping "
             "reduces measured throughput",
             CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ);
#endif

    if ((block_size == 0) || (address > UINT32_MAX - block_size)) {
        ESP_LOGE(TAG, "Invalid target RAM range");
        return;
    }

    original = malloc(block_size);
    write_buffer = malloc(block_size);
    read_buffer = malloc(block_size);
    if ((original == NULL) || (write_buffer == NULL) || (read_buffer == NULL)) {
        ESP_LOGE(TAG, "Failed to allocate three %u-byte test buffers",
                 (unsigned)block_size);
        goto cleanup;
    }

    if (!swd_init_debug()) {
        ESP_LOGE(TAG, "SWD initialization failed");
        swd_off();
        goto cleanup;
    }
    connected = true;

    uint32_t idcode = 0;
    if (!swd_read_idcode(&idcode)) {
        ESP_LOGE(TAG, "DP IDCODE read failed");
        goto cleanup;
    }
    ESP_LOGI(TAG, "DP IDCODE: 0x%08" PRIx32, idcode);

    uint32_t dhcsr = 0;
    if (!swd_read_word(CORTEX_M_DHCSR_ADDRESS, &dhcsr)) {
        ESP_LOGE(TAG, "Failed to read target halt state");
        goto cleanup;
    }
    target_was_halted = (dhcsr & S_HALT) != 0;
    target_state_known = true;

    if (!target_was_halted) {
        if (!swd_halt_target() || !swd_wait_until_halted()) {
            ESP_LOGE(TAG, "Failed to halt target");
            goto cleanup;
        }
    }
    ESP_LOGI(TAG, "Target was %s; stress test runs with target halted",
             target_was_halted ? "halted" : "running");

    if (!swd_read_memory(address, original, block_size)) {
        ESP_LOGE(TAG, "Failed to back up target RAM; stress test not started");
        goto cleanup;
    }
    backup_valid = true;

    for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
        fill_pattern(write_buffer, block_size, iteration);
        attempted++;

#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
        swd_perf_reset_counters();
#endif
        int64_t started_us = esp_timer_get_time();
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
        const uint32_t write_started_cycles = esp_cpu_get_cycle_count();
#endif
        const bool write_ok = swd_write_memory(address, write_buffer, block_size) != 0;
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
        const uint32_t write_cycles =
            esp_cpu_get_cycle_count() - write_started_cycles;
#endif
        const int64_t finished_us = esp_timer_get_time();
        const uint64_t write_us = (uint64_t)(finished_us - started_us);
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
        swd_perf_counters_t profile_sample;
        swd_perf_get_counters(&profile_sample);
        record_swd_profile(&write_profile, &profile_sample, write_cycles);
#endif

        if (!write_ok) {
            write_failures++;
            ESP_LOGE(TAG, "Iteration %" PRIu32 ": write failed", iteration + 1U);
            if (!swd_clear_errors()) {
                recovery_failures++;
                ESP_LOGE(TAG, "Could not recover the SWD link after write failure");
                break;
            }
            vTaskDelay(1);
            continue;
        }
        record_transfer(&write_stats, block_size, write_us);

        memset(read_buffer, 0xa5, block_size);
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
        swd_perf_reset_counters();
#endif
        started_us = esp_timer_get_time();
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
        const uint32_t read_started_cycles = esp_cpu_get_cycle_count();
#endif
        const bool read_ok = swd_read_memory(address, read_buffer, block_size) != 0;
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
        const uint32_t read_cycles =
            esp_cpu_get_cycle_count() - read_started_cycles;
#endif
        const uint64_t read_us = (uint64_t)(esp_timer_get_time() - started_us);
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
        swd_perf_get_counters(&profile_sample);
        record_swd_profile(&read_profile, &profile_sample, read_cycles);
#endif

        if (!read_ok) {
            read_failures++;
            ESP_LOGE(TAG, "Iteration %" PRIu32 ": read failed", iteration + 1U);
            if (!swd_clear_errors()) {
                recovery_failures++;
                ESP_LOGE(TAG, "Could not recover the SWD link after read failure");
                break;
            }
            vTaskDelay(1);
            continue;
        }
        record_transfer(&read_stats, block_size, read_us);

        size_t first_mismatch = 0;
        const size_t mismatches = count_mismatches(
            write_buffer, read_buffer, block_size, &first_mismatch);
        if (mismatches != 0) {
            mismatch_iterations++;
            mismatched_bytes += mismatches;
            ESP_LOGE(TAG,
                     "Iteration %" PRIu32 ": %u mismatched bytes; first at "
                     "0x%08" PRIx32 " (expected 0x%02x, got 0x%02x)",
                     iteration + 1U, (unsigned)mismatches,
                     address + (uint32_t)first_mismatch,
                     write_buffer[first_mismatch], read_buffer[first_mismatch]);
        } else {
            verified++;
        }

        if (((iteration + 1U) % progress_interval == 0U) ||
            (iteration + 1U == iterations)) {
            ESP_LOGI(TAG,
                     "Progress: %" PRIu32 "/%" PRIu32
                     ", verified=%" PRIu32 ", write/read/mismatch failures=%" PRIu32
                     "/%" PRIu32 "/%" PRIu32,
                     iteration + 1U, iterations, verified,
                     write_failures, read_failures, mismatch_iterations);
            log_rate(TAG, "Write", &write_stats);
            log_rate(TAG, "Read", &read_stats);
        }

        /* Let the idle task run without including the delay in transfer timing. */
        vTaskDelay(1);
    }

    ESP_LOGI(TAG, "Stress test complete");
    ESP_LOGI(TAG,
             "attempted=%" PRIu32 ", verified=%" PRIu32
             ", write failures=%" PRIu32 ", read failures=%" PRIu32
             ", mismatch iterations=%" PRIu32 ", mismatched bytes=%" PRIu64
             ", recovery failures=%" PRIu32,
             attempted, verified, write_failures, read_failures,
             mismatch_iterations, mismatched_bytes, recovery_failures);
    log_rate(TAG, "Write", &write_stats);
    log_rate(TAG, "Read", &read_stats);
#ifdef CONFIG_ESP_SWD_PERF_INSTRUMENTATION
    log_swd_profile(TAG, "Write", &write_profile);
    log_swd_profile(TAG, "Read", &read_profile);
#endif

cleanup:
    if (connected && backup_valid) {
        if (swd_write_memory(address, original, block_size)) {
            ESP_LOGI(TAG, "Original target RAM restored");
        } else {
            ESP_LOGE(TAG, "FAILED to restore original target RAM");
            safe_to_resume = false;
        }
    }

    if (connected && target_state_known && !target_was_halted && safe_to_resume) {
        if (!swd_write_word(CORTEX_M_DHCSR_ADDRESS, DBGKEY | C_DEBUGEN)) {
            ESP_LOGE(TAG, "Failed to resume target");
        }
    } else if (connected && target_state_known && !target_was_halted) {
        ESP_LOGE(TAG, "Target left halted because its RAM could not be restored");
    }

    if (connected) {
        swd_off();
    }
    free(read_buffer);
    free(write_buffer);
    free(original);
}
