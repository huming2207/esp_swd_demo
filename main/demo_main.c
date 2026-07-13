#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

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
    uint32_t attempted = 0;
    uint32_t verified = 0;
    uint32_t write_failures = 0;
    uint32_t read_failures = 0;
    uint32_t mismatch_iterations = 0;
    uint64_t mismatched_bytes = 0;
    uint32_t recovery_failures = 0;

    ESP_LOGI(TAG, "Soul Injector Rev 6 SWD GPIO stress test");
    ESP_LOGI(TAG,
             "RAM range: 0x%08" PRIx32 "..0x%08" PRIx32
             ", block=%u bytes, iterations=%" PRIu32,
             address, address + (uint32_t)block_size - 1U,
             (unsigned)block_size, iterations);

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

    if (!swd_read_memory(address, original, block_size)) {
        ESP_LOGE(TAG, "Failed to back up target RAM; stress test not started");
        goto cleanup;
    }
    backup_valid = true;

    for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
        fill_pattern(write_buffer, block_size, iteration);
        attempted++;

        int64_t started_us = esp_timer_get_time();
        const bool write_ok = swd_write_memory(address, write_buffer, block_size) != 0;
        const uint64_t write_us = (uint64_t)(esp_timer_get_time() - started_us);

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
        started_us = esp_timer_get_time();
        const bool read_ok = swd_read_memory(address, read_buffer, block_size) != 0;
        const uint64_t read_us = (uint64_t)(esp_timer_get_time() - started_us);

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
