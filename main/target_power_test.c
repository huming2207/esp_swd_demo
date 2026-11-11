#include "target_power_test.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <swd_host.h>

#include "power_profiler.h"

/* Register addresses and bits from RM0461 and ST's stm32wle5xx.h. */
#define DBGMCU_IDCODE 0xE0042000U
#define DBGMCU_CR 0xE0042004U
#define DBGMCU_CR_DBG_STOP (1U << 1)
#define DBGMCU_DEV_ID_MASK 0xFFFU
#define DBGMCU_DEV_ID_STM32WLE 0x497U

#define PWR_CR1 0x58000400U
#define PWR_CR1_LPMS_MASK 0x7U
#define PWR_CR1_LPMS_STOP2 0x2U
#define PWR_CR1_LPMS_STANDBY 0x3U
#define PWR_CR1_DBP (1U << 8)
#define PWR_SCR 0x58000418U
#define PWR_SCR_CLEAR_ALL ((0x7U << 0) | (1U << 8) | (1U << 11))
#define PWR_EXTSCR 0x58000488U
#define PWR_EXTSCR_C1CSSF (1U << 0)
#define PWR_EXTSCR_C1SBF (1U << 8)
#define PWR_EXTSCR_C1STOP2F (1U << 9)

#define RCC_BDCR 0x58000090U
#define RCC_BDCR_LSEON (1U << 0)
#define RCC_BDCR_LSESYSEN (1U << 7)
#define RCC_BDCR_RTCEN (1U << 15)
#define RCC_BDCR_LSCOEN (1U << 24)
#define RCC_CSR 0x58000094U
#define RCC_CSR_LSION (1U << 0)
#define RCC_CSR_RFRST (1U << 15)

#define SCB_AIRCR 0xE000ED0CU
#define SCB_SCR 0xE000ED10U
#define SCB_SCR_SLEEPDEEP (1U << 2)
#define CORE_DHCSR 0xE000EDF0U
#define CORE_DEMCR 0xE000EDFCU
#define CORE_REG_PC 15U

#define SWD_DP_CTRL_STAT_ADDR 0x04U
#define DHCSR_S_HALT_TIMEOUT_MS 1000

#define IDLE_WARMUP_MS 1000U
#define ACTIVE_WARMUP_MS 500U
#define SLEEP_WARMUP_MS 3000U
#define SLEEP_MEASURE_MS 5000U
#define ACTIVE_MEASURE_US 3000000LL
#define RESET_PULSE_MS 20U

static const char *TAG = "target_power";

/*
 * movw r0, #0xffff
 * loop: subs r0, #1
 *       bne loop
 * wfi_loop: dsb
 *       wfi
 *       b wfi_loop
 *
 * The delay leaves time to drop the debug connection before Standby.
 */
static const uint16_t WFI_STUB[] = {
    0xF64F, 0x70FF, 0x3801, 0xD1FD, 0xF3BF, 0x8F4F, 0xBF30, 0xE7FB,
};

typedef struct {
    const char *name;
    power_profiler_result_t power;
    uint32_t kb_per_s;
    bool measured;
} phase_result_t;

enum { PHASE_HALTED, PHASE_WRITE, PHASE_READ, PHASE_STOP2, PHASE_STANDBY, PHASE_COUNT };

static phase_result_t s_phases[PHASE_COUNT] = {
    [PHASE_HALTED] = {.name = "Halted, SWD idle"},
    [PHASE_WRITE] = {.name = "Halted, SWD writing"},
    [PHASE_READ] = {.name = "Halted, SWD reading"},
    [PHASE_STOP2] = {.name = "Stop 2, debug on"},
    [PHASE_STANDBY] = {.name = "Standby, debug/RTC off"},
};

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static bool read_modify_write(uint32_t address, uint32_t clear, uint32_t set)
{
    uint32_t value = 0;

    if (!swd_read_word(address, &value)) {
        return false;
    }
    return swd_write_word(address, (value & ~clear) | set) != 0;
}

static bool wait_for_halt(void)
{
    for (uint32_t waited = 0; waited < DHCSR_S_HALT_TIMEOUT_MS; waited += 10) {
        uint32_t dhcsr = 0;

        if (swd_read_word(CORE_DHCSR, &dhcsr) && (dhcsr & S_HALT) != 0) {
            return true;
        }
        swd_clear_errors();
        delay_ms(10);
    }
    return false;
}

/* The reset returns clocks, peripherals and the radio to their defaults. */
static bool reset_and_halt(void)
{
    if (!swd_write_word(CORE_DHCSR, DBGKEY | C_DEBUGEN) ||
        !swd_write_word(CORE_DEMCR, VC_CORERESET) ||
        !swd_write_word(SCB_AIRCR, VECTKEY | SYSRESETREQ)) {
        ESP_LOGE(TAG, "Could not request target reset");
        return false;
    }

    delay_ms(RESET_PULSE_MS);
    if (!wait_for_halt()) {
        ESP_LOGE(TAG, "Target did not halt at the reset vector");
        return false;
    }
    return swd_write_word(CORE_DEMCR, 0) != 0;
}

static bool prepare_low_power(uint32_t lpms, uint32_t dbgmcu_cr)
{
    return read_modify_write(RCC_CSR, 0, RCC_CSR_RFRST) &&
           swd_write_word(DBGMCU_CR, dbgmcu_cr) &&
           swd_write_word(PWR_SCR, PWR_SCR_CLEAR_ALL) &&
           swd_write_word(PWR_EXTSCR, PWR_EXTSCR_C1CSSF) &&
           read_modify_write(PWR_CR1, PWR_CR1_LPMS_MASK, lpms) &&
           swd_write_word(SCB_SCR, SCB_SCR_SLEEPDEEP);
}

static bool load_wfi_stub(void)
{
    uint8_t code[sizeof(WFI_STUB)];

    memcpy(code, WFI_STUB, sizeof(code));
    return swd_write_memory(CONFIG_SWD_DEMO_RAM_ADDRESS, code, sizeof(code)) &&
           swd_write_core_register(CORE_REG_PC, CONFIG_SWD_DEMO_RAM_ADDRESS);
}

static bool disable_rtc_and_lse(void)
{
    uint32_t bdcr = 0;

    if (!read_modify_write(PWR_CR1, 0, PWR_CR1_DBP) ||
        !read_modify_write(RCC_BDCR, RCC_BDCR_LSEON | RCC_BDCR_LSESYSEN | RCC_BDCR_RTCEN | RCC_BDCR_LSCOEN, 0) ||
        !read_modify_write(RCC_CSR, RCC_CSR_LSION, 0) ||
        !swd_read_word(RCC_BDCR, &bdcr)) {
        return false;
    }

    ESP_LOGI(TAG, "RCC_BDCR after disabling RTC and LSE: 0x%08" PRIx32, bdcr);
    return (bdcr & (RCC_BDCR_LSEON | RCC_BDCR_RTCEN)) == 0;
}

static void measure_for(phase_result_t *phase, uint32_t warmup_ms, uint32_t measure_ms)
{
    if (!power_profiler_sample_begin(warmup_ms)) {
        ESP_LOGE(TAG, "%s: power profiler not ready", phase->name);
        return;
    }
    delay_ms(measure_ms);
    power_profiler_sample_end(&phase->power);
    phase->measured = true;
}

static void measure_transfers(phase_result_t *phase, uint8_t *buffer, size_t size, bool write)
{
    uint64_t bytes = 0;
    uint32_t failures = 0;

    if (!power_profiler_sample_begin(ACTIVE_WARMUP_MS)) {
        ESP_LOGE(TAG, "%s: power profiler not ready", phase->name);
        return;
    }

    const int64_t started_us = esp_timer_get_time();
    int64_t elapsed_us = 0;
    while (elapsed_us < ACTIVE_MEASURE_US) {
        const bool ok = write ? swd_write_memory(CONFIG_SWD_DEMO_RAM_ADDRESS, buffer, size) != 0
                              : swd_read_memory(CONFIG_SWD_DEMO_RAM_ADDRESS, buffer, size) != 0;
        if (ok) {
            bytes += size;
        } else {
            failures++;
            swd_clear_errors();
        }
        elapsed_us = esp_timer_get_time() - started_us;
    }

    power_profiler_sample_end(&phase->power);
    phase->measured = true;
    phase->kb_per_s = (uint32_t)((bytes * 1000ULL) / (uint64_t)elapsed_us);
    if (failures != 0) {
        ESP_LOGE(TAG, "%s: %" PRIu32 " transfers failed", phase->name, failures);
    }
}

static bool run_transfer_phases(void)
{
    const size_t size = CONFIG_SWD_DEMO_BLOCK_SIZE;
    uint8_t *buffer = malloc(size);

    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate the %u-byte transfer buffer", (unsigned)size);
        return false;
    }

    for (size_t i = 0; i < size; ++i) {
        buffer[i] = (uint8_t)(i * 167U + 13U);
    }

    measure_for(&s_phases[PHASE_HALTED], IDLE_WARMUP_MS, 3000);
    measure_transfers(&s_phases[PHASE_WRITE], buffer, size, true);
    measure_transfers(&s_phases[PHASE_READ], buffer, size, false);

    free(buffer);
    return true;
}

static bool run_stop2_phase(void)
{
    uint32_t dhcsr = 0;
    uint32_t extscr_stopped = 0;
    uint32_t extscr_halted = 0;

    if (!reset_and_halt() ||
        !prepare_low_power(PWR_CR1_LPMS_STOP2, DBGMCU_CR_DBG_STOP) ||
        !load_wfi_stub() ||
        !swd_write_word(CORE_DHCSR, DBGKEY | C_DEBUGEN)) {
        ESP_LOGE(TAG, "Stop 2 setup failed");
        return false;
    }

    delay_ms(200);
    measure_for(&s_phases[PHASE_STOP2], SLEEP_WARMUP_MS, SLEEP_MEASURE_MS);

    if (!swd_read_word(CORE_DHCSR, &dhcsr) || !swd_read_word(PWR_EXTSCR, &extscr_stopped)) {
        ESP_LOGE(TAG, "SWD lost during Stop 2, so debug did not stay alive");
        return false;
    }
    swd_write_word(CORE_DHCSR, DBGKEY | C_DEBUGEN | C_HALT);
    wait_for_halt();
    swd_read_word(PWR_EXTSCR, &extscr_halted);

    ESP_LOGI(TAG, "Stop 2: SWD alive, DHCSR=0x%08" PRIx32 " S_SLEEP=%d, EXTSCR while stopped=0x%08" PRIx32
                  ", after halt=0x%08" PRIx32 ", C1STOP2F=%d",
             dhcsr, (dhcsr & S_SLEEP) != 0, extscr_stopped, extscr_halted,
             ((extscr_stopped | extscr_halted) & PWR_EXTSCR_C1STOP2F) != 0);
    return true;
}

static void set_target_reset(bool asserted)
{
    /* HOST_SW_RST drives a MOSFET gate, so high asserts target reset. */
    gpio_set_level(CONFIG_ESP_SWD_NRST_PIN, asserted ? 1 : 0);
}

/* Reconnect under reset so C1SBF is read before the firmware can clear it. */
static void verify_standby(void)
{
    uint32_t extscr = 0;
    bool halted = false;

    set_target_reset(true);
    delay_ms(RESET_PULSE_MS);

    if (!swd_init_debug()) {
        set_target_reset(false);
        ESP_LOGE(TAG, "Could not reconnect after Standby");
        return;
    }

    const bool catch_set = swd_write_word(CORE_DHCSR, DBGKEY | C_DEBUGEN) &&
                           swd_write_word(CORE_DEMCR, VC_CORERESET);
    set_target_reset(false);
    delay_ms(RESET_PULSE_MS);

    if (catch_set) {
        halted = wait_for_halt();
    }

    if (swd_read_word(PWR_EXTSCR, &extscr)) {
        ESP_LOGI(TAG, "Standby: EXTSCR after wake=0x%08" PRIx32 ", C1SBF=%d, halted before firmware=%d",
                 extscr, (extscr & PWR_EXTSCR_C1SBF) != 0, halted);
    } else {
        ESP_LOGE(TAG, "Could not read PWR_EXTSCR after Standby");
    }
}

static bool run_standby_phase(void)
{
    if (!reset_and_halt() ||
        !disable_rtc_and_lse() ||
        !prepare_low_power(PWR_CR1_LPMS_STANDBY, 0) ||
        !load_wfi_stub() ||
        !swd_write_word(CORE_DEMCR, 0) ||
        !swd_write_word(CORE_DHCSR, DBGKEY)) {
        ESP_LOGE(TAG, "Standby setup failed");
        return false;
    }

    /* The stub's delay covers this. A failed write here does no harm. */
    swd_write_dp(SWD_DP_CTRL_STAT_ADDR, 0);
    power_profiler_set_debug_lines(false);
    delay_ms(200);

    measure_for(&s_phases[PHASE_STANDBY], SLEEP_WARMUP_MS, SLEEP_MEASURE_MS);

    power_profiler_set_debug_lines(true);
    verify_standby();
    return true;
}

static void restart_target(void)
{
    /* DEMCR survives everything except power-on, so clear the vector catch. */
    swd_write_word(CORE_DEMCR, 0);
    swd_write_word(CORE_DHCSR, DBGKEY);
    swd_write_word(DBGMCU_CR, 0);

    set_target_reset(true);
    delay_ms(RESET_PULSE_MS);
    set_target_reset(false);
    delay_ms(RESET_PULSE_MS);
}

static void log_results(void)
{
    ESP_LOGI(TAG, "---- Target current results ----");
    for (uint32_t i = 0; i < PHASE_COUNT; ++i) {
        const phase_result_t *phase = &s_phases[i];
        const power_profiler_result_t *power = &phase->power;

        if (!phase->measured || !power->ok) {
            ESP_LOGE(TAG, "%-24s no valid measurement", phase->name);
            continue;
        }

        ESP_LOGI(TAG,
                 "%-24s avg=%.3f uA min=%.3f max=%.3f Vin=%.3f V samples LOW/MID/HIGH=%" PRIu32 "/%" PRIu32 "/%" PRIu32
                 " steps=%" PRIu32 " trips=%" PRIu32 " SWD=%" PRIu32 " KB/s",
                 phase->name, power->avg_amps * 1e6, power->min_amps * 1e6, power->max_amps * 1e6,
                 power->vin_volts, power->low_samples, power->mid_samples, power->high_samples,
                 power->steps, power->trips, phase->kb_per_s);
    }
}

void target_power_test_run(void)
{
    uint32_t idcode = 0;

    ESP_LOGI(TAG, "==== STM32WLE5 current test ====");

    if (!swd_read_word(DBGMCU_IDCODE, &idcode)) {
        ESP_LOGE(TAG, "Could not read DBGMCU_IDCODE");
        return;
    }
    ESP_LOGI(TAG, "DBGMCU_IDCODE: 0x%08" PRIx32, idcode);
    if ((idcode & DBGMCU_DEV_ID_MASK) != DBGMCU_DEV_ID_STM32WLE) {
        ESP_LOGE(TAG, "Target is not an STM32WLEx; not touching its power registers");
        return;
    }

    const bool ready = reset_and_halt() && run_transfer_phases();
    if (ready && run_stop2_phase()) {
        run_standby_phase();
    }

    restart_target();
    log_results();
}
