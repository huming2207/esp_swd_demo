/*
 * Rev 8 adaptor power profiler hardware test.
 *
 * Three INA226s sit on a series shunt chain between VPP_INPUT and VPP_OUTPUT:
 * 50 mohm (HIGH), 4.02 ohm (MID) and 162 ohm (LOW). Two P-FET bypasses select
 * the range. Each is held by one half of a 2-bit shift register:
 *
 *     GPIO19 (D) -> HIGH latch -> MID latch, shared rising-edge clock on GPIO37
 *
 * Up-ranging is done by hardware: a comparator presets a latch within about
 * 1 us. Down-ranging is done here, one clock with D low per range.
 */
#include "power_profiler.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "ina226.hpp"

namespace {

const char *const TAG = "profiler";

constexpr gpio_num_t PIN_DBG_EN = static_cast<gpio_num_t>(CONFIG_SWD_DEMO_PROF_DBG_EN_PIN);
constexpr gpio_num_t PIN_SDA = static_cast<gpio_num_t>(CONFIG_SWD_DEMO_PROF_SDA_PIN);
constexpr gpio_num_t PIN_SCL = static_cast<gpio_num_t>(CONFIG_SWD_DEMO_PROF_SCL_PIN);
constexpr gpio_num_t PIN_ALERT = static_cast<gpio_num_t>(CONFIG_SWD_DEMO_PROF_ALERT_PIN);
constexpr gpio_num_t PIN_RANGE_D = static_cast<gpio_num_t>(CONFIG_SWD_DEMO_PROF_RANGE_D_PIN);
constexpr gpio_num_t PIN_RANGE_CLK = static_cast<gpio_num_t>(CONFIG_SWD_DEMO_PROF_RANGE_CLK_PIN);
constexpr gpio_num_t PIN_HIGH_READBACK = static_cast<gpio_num_t>(CONFIG_SWD_DEMO_PROF_HIGH_RB_PIN);
constexpr gpio_num_t PIN_MID_READBACK = static_cast<gpio_num_t>(CONFIG_SWD_DEMO_PROF_MID_RB_PIN);
constexpr gpio_num_t PIN_SWDIO_OUT = static_cast<gpio_num_t>(CONFIG_ESP_SWD_DATA_OUT_PIN);
constexpr gpio_num_t PIN_SWDIO_IN = static_cast<gpio_num_t>(CONFIG_ESP_SWD_DATA_IN_PIN);

enum range_id : uint8_t { RANGE_LOW = 0, RANGE_MID = 1, RANGE_HIGH = 2, RANGE_COUNT = 3 };

struct range_desc {
    const char *name;
    uint8_t i2c_addr;
    double shunt_ohm;
    double chain_ohm; // Series resistance from VPP_INPUT while in this range
    uint32_t settle_us;
};

constexpr range_desc RANGES[RANGE_COUNT] = {
    {"LOW", 0x44, 162.0, 166.07, 20000}, // 162 ohm x (C17 + target caps) needs the longest
    {"MID", 0x41, 4.02, 4.07, 2000},
    {"HIGH", 0x40, 0.05, 0.05, 1000},
};

constexpr double INA226_SHUNT_LSB_V = 2.5e-6;
constexpr double VPP_MIN_V = 1.7;
constexpr double VPP_MAX_V = 3.7;
constexpr double VBUS_MATCH_V = 0.02;

/* Comparator trip drop is VPP_INPUT / 92, nominally, with the spread from the schematic notes. */
constexpr double VTH_DIVIDER = 92.0;
constexpr double TRIP_BELOW_NOMINAL_V = 0.009;
constexpr double TRIP_ABOVE_NOMINAL_V = 0.016;
constexpr double STEP_DOWN_MARGIN = 0.7;

constexpr uint32_t STEP_DOWN_SAMPLES = 3;
constexpr int64_t STEP_DOWN_HOLDOFF_US = 200000;
constexpr uint32_t SNAPSHOT_SETTLE_MS = 60; // Allows several 8.8 ms conversions after a switch
constexpr uint32_t BIAS_TEST_SETTLE_MS = 100;
constexpr uint32_t BIAS_TEST_WINDOW_MS = 3000;
constexpr double BIAS_TEST_CONTROL_LIMIT_A = 1.5e-6;

double trip_min_amps(double vbus, range_id into)
{
    const double drop = vbus / VTH_DIVIDER - TRIP_BELOW_NOMINAL_V;
    return drop > 0.0 ? drop / RANGES[into].chain_ohm : 0.0;
}

double trip_max_amps(double vbus, range_id into)
{
    return (vbus / VTH_DIVIDER + TRIP_ABOVE_NOMINAL_V) / RANGES[into].chain_ohm;
}

void delay_ms(uint32_t ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks > 0 ? ticks : 1);
}

struct latch_state {
    bool high;
    bool mid;
};

range_id decode_range(latch_state state)
{
    if (state.high) {
        return RANGE_HIGH;
    }
    return state.mid ? RANGE_MID : RANGE_LOW;
}

class range_latch {
public:
    esp_err_t init() const
    {
        gpio_set_level(PIN_RANGE_D, 1);
        gpio_set_level(PIN_RANGE_CLK, 0);

        gpio_config_t output_cfg = {};
        output_cfg.pin_bit_mask = BIT64(PIN_RANGE_D) | BIT64(PIN_RANGE_CLK);
        output_cfg.mode = GPIO_MODE_OUTPUT;

        gpio_config_t input_cfg = {};
        input_cfg.pin_bit_mask = BIT64(PIN_HIGH_READBACK) | BIT64(PIN_MID_READBACK);
        input_cfg.mode = GPIO_MODE_INPUT;

        esp_err_t ret = gpio_config(&output_cfg);
        return ret ?: gpio_config(&input_cfg);
    }

    latch_state read() const
    {
        return {gpio_get_level(PIN_HIGH_READBACK) != 0, gpio_get_level(PIN_MID_READBACK) != 0};
    }

    void clock_in(bool data) const
    {
        gpio_set_level(PIN_RANGE_D, data ? 1 : 0);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_RANGE_CLK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_RANGE_CLK, 0);
        esp_rom_delay_us(1);
    }

    void force_all_bypass() const
    {
        clock_in(true);
        clock_in(true);
    }

    void step_down() const { clock_in(false); }
};

range_latch g_latch;
ina226 g_sensors[RANGE_COUNT];
i2c_master_bus_handle_t g_i2c_bus = nullptr;
bool g_gpio_ready = false;
bool g_sensors_ready = false;

/*
 * FORCE_BYPASS -> SETTLE -> DISCARD -> MEASURE -> STEP_DOWN -> SETTLE ...
 *
 * Every tick first compares the latch readback with the expected range. A
 * difference outside FORCE_BYPASS means a comparator moved the range, so the
 * machine adopts it and settles again.
 */
class auto_ranger {
public:
    void restart()
    {
        *this = auto_ranger{};
    }

    void tick();

    bool faulted() const { return fsm == state::FAULT; }
    range_id range() const { return active; }
    double last_vbus() const { return vbus; }
    uint32_t samples() const { return total_samples; }
    uint32_t steps() const { return step_count; }
    uint32_t rejected() const { return rejected_count; }
    uint32_t glitches() const { return glitch_count; }
    uint32_t overflows() const { return overflow_count; }
    uint32_t trips(range_id into) const { return trip_count[into]; }
    uint32_t range_samples(range_id r) const { return per_range_samples[r]; }
    double average_amps() const { return total_samples == 0 ? 0.0 : sum_amps / total_samples; }
    double minimum_amps() const { return min_amps; }
    double maximum_amps() const { return max_amps; }

    /** Discard results so far without disturbing the range or state. */
    void clear_stats();

    /** Log averages since the previous call and start a new window. */
    void log_window();

private:
    enum class state : uint8_t { FORCE_BYPASS, SETTLE, DISCARD, MEASURE, STEP_DOWN, FAULT };

    void fault(const char *reason);
    void enter_settle();
    void on_latch_changed(range_id hw);
    bool conversion_ready();
    void measure();

    state fsm = state::FORCE_BYPASS;
    range_id active = RANGE_HIGH;
    int64_t settle_until_us = 0;
    int64_t holdoff_until_us = 0;
    bool step_pending = false;
    uint32_t below_limit = 0;
    double vbus = 0.0;

    double window_sum = 0.0;
    double window_min = 0.0;
    double window_max = 0.0;
    uint32_t window_count = 0;

    double sum_amps = 0.0;
    double min_amps = 0.0;
    double max_amps = 0.0;
    uint32_t total_samples = 0;
    uint32_t step_count = 0;
    uint32_t rejected_count = 0;
    uint32_t glitch_count = 0;
    uint32_t overflow_count = 0;
    uint32_t trip_count[RANGE_COUNT] = {};
    uint32_t per_range_samples[RANGE_COUNT] = {};
};

auto_ranger g_ranger;

void auto_ranger::fault(const char *reason)
{
    ESP_LOGE(TAG, "Auto-range fault in %s range: %s", RANGES[active].name, reason);
    fsm = state::FAULT;
}

void auto_ranger::enter_settle()
{
    below_limit = 0;
    settle_until_us = esp_timer_get_time() + RANGES[active].settle_us;

    /* Reading Mask/Enable clears CVRF, so only conversions after this point count. */
    uint16_t flags = 0;
    if (g_sensors[active].read_alert_flag(&flags) != ESP_OK) {
        fault("I2C read failed");
        return;
    }
    fsm = state::SETTLE;
}

void auto_ranger::on_latch_changed(range_id hw)
{
    if (hw > active) {
        for (int into = active + 1; into <= hw; ++into) {
            trip_count[into]++;
        }
        if (step_pending) {
            rejected_count++;
            step_pending = false;
        }
        holdoff_until_us = esp_timer_get_time() + STEP_DOWN_HOLDOFF_US;
        ESP_LOGI(TAG, "Hardware up-range %s -> %s", RANGES[active].name, RANGES[hw].name);
    } else {
        glitch_count++;
        ESP_LOGW(TAG, "Latch dropped unexpectedly %s -> %s", RANGES[active].name, RANGES[hw].name);
    }

    active = hw;
    enter_settle();
}

bool auto_ranger::conversion_ready()
{
    uint16_t flags = 0;
    if (g_sensors[active].read_alert_flag(&flags) != ESP_OK) {
        fault("I2C read failed");
        return false;
    }
    if ((flags & ina226::ALERT_FLAG_OVF) != 0) {
        overflow_count++;
    }
    return (flags & ina226::ALERT_FLAG_CVRF) != 0;
}

void auto_ranger::measure()
{
    double amps = 0.0;
    esp_err_t ret = g_sensors[active].read_current(&amps);
    ret = ret ?: g_sensors[active].read_voltage(&vbus);
    if (ret != ESP_OK) {
        fault("I2C read failed");
        return;
    }

    /* A complete conversion in the new range means a firmware step held. */
    step_pending = false;

    min_amps = total_samples == 0 ? amps : fmin(min_amps, amps);
    max_amps = total_samples == 0 ? amps : fmax(max_amps, amps);
    sum_amps += amps;
    total_samples++;
    per_range_samples[active]++;
    window_sum += amps;
    window_min = window_count == 0 ? amps : fmin(window_min, amps);
    window_max = window_count == 0 ? amps : fmax(window_max, amps);
    window_count++;

    if (active == RANGE_LOW || esp_timer_get_time() < holdoff_until_us) {
        below_limit = 0;
        return;
    }

    const range_id next = static_cast<range_id>(active - 1);
    const double limit = trip_min_amps(vbus, next) * STEP_DOWN_MARGIN;
    if (limit <= 0.0 || amps >= limit) {
        below_limit = 0;
        return;
    }

    below_limit++;
    if (below_limit >= STEP_DOWN_SAMPLES) {
        fsm = state::STEP_DOWN;
    }
}

void auto_ranger::tick()
{
    if (fsm == state::FAULT) {
        return;
    }

    if (fsm != state::FORCE_BYPASS) {
        const range_id hw = decode_range(g_latch.read());
        if (hw != active) {
            on_latch_changed(hw);
            return;
        }
    }

    switch (fsm) {
    case state::FORCE_BYPASS: {
        g_latch.force_all_bypass();
        const latch_state latched = g_latch.read();
        if (!latched.high || !latched.mid) {
            fault("latches did not set");
            return;
        }
        active = RANGE_HIGH;
        enter_settle();
        break;
    }
    case state::SETTLE:
        if (esp_timer_get_time() >= settle_until_us) {
            fsm = state::DISCARD;
        }
        break;
    case state::DISCARD:
        /* This conversion may span the switch, so it is never used. */
        if (conversion_ready() && fsm != state::FAULT) {
            fsm = state::MEASURE;
        }
        break;
    case state::MEASURE:
        if (conversion_ready() && fsm != state::FAULT) {
            measure();
        }
        break;
    case state::STEP_DOWN:
        g_latch.step_down();
        active = static_cast<range_id>(active - 1);
        step_pending = true;
        step_count++;
        enter_settle();
        break;
    case state::FAULT:
        break;
    }
}

void auto_ranger::clear_stats()
{
    sum_amps = 0.0;
    min_amps = 0.0;
    max_amps = 0.0;
    total_samples = 0;
    step_count = 0;
    rejected_count = 0;
    glitch_count = 0;
    overflow_count = 0;
    window_sum = 0.0;
    window_count = 0;
    for (uint8_t r = 0; r < RANGE_COUNT; ++r) {
        trip_count[r] = 0;
        per_range_samples[r] = 0;
    }
}

void auto_ranger::log_window()
{
    double shunt_mv[RANGE_COUNT] = {};
    for (uint8_t r = 0; r < RANGE_COUNT; ++r) {
        if (g_sensors[r].read_volt_shunt_mv(&shunt_mv[r]) != ESP_OK) {
            fault("I2C read failed");
            return;
        }
    }

    const double drop_v = (shunt_mv[RANGE_LOW] + shunt_mv[RANGE_MID] + shunt_mv[RANGE_HIGH]) / 1000.0;
    const double average = window_count == 0 ? 0.0 : window_sum / window_count;

    ESP_LOGI(TAG,
             "range=%s Vin=%.3f V Vtarget=%.3f V I avg/min/max=%.2f/%.2f/%.2f uA samples=%" PRIu32
             " steps=%" PRIu32 " rejected=%" PRIu32,
             RANGES[active].name, vbus, vbus - drop_v, average * 1e6, window_min * 1e6, window_max * 1e6,
             window_count, step_count, rejected_count);

    window_sum = 0.0;
    window_count = 0;
}

enum check_id : uint8_t {
    CHECK_SENSORS,
    CHECK_VPP,
    CHECK_ALERT,
    CHECK_LATCH_FORCE,
    CHECK_LATCH_SHIFT,
    CHECK_STEP_TO_MID,
    CHECK_STEP_TO_LOW,
    CHECK_CROSS_MID,
    CHECK_CROSS_LOW,
    CHECK_TRIP_MID,
    CHECK_TRIP_HIGH,
    CHECK_AUTO_RANGE,
    CHECK_COUNT,
};

enum check_result : uint8_t { RESULT_NOT_EXERCISED, RESULT_PASS, RESULT_FAIL };

constexpr const char *CHECK_NAMES[CHECK_COUNT] = {
    "INA226 0x40/0x41/0x44 init",
    "VPP_INPUT present, sensors agree",
    "Shared /ALERT, each sensor",
    "Latches force both bypasses",
    "Shift register order",
    "HIGH -> MID step behaviour",
    "MID -> LOW step behaviour",
    "HIGH and MID sensors agree",
    "MID and LOW sensors agree",
    "Comparator trip into MID",
    "Comparator trip into HIGH",
    "Auto-range state machine",
};

check_result g_results[CHECK_COUNT] = {};

void record(check_id check, bool pass)
{
    if (!pass) {
        g_results[check] = RESULT_FAIL;
    } else if (g_results[check] == RESULT_NOT_EXERCISED) {
        g_results[check] = RESULT_PASS;
    }
}

struct snapshot {
    double amps[RANGE_COUNT];
    double vbus[RANGE_COUNT];
};

bool take_snapshot(snapshot &out, const char *label)
{
    for (uint8_t r = 0; r < RANGE_COUNT; ++r) {
        esp_err_t ret = g_sensors[r].read_current(&out.amps[r]);
        ret = ret ?: g_sensors[r].read_voltage(&out.vbus[r]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "%s: sensor %s read failed", label, RANGES[r].name);
            record(CHECK_SENSORS, false);
            return false;
        }
    }

    const latch_state latched = g_latch.read();
    ESP_LOGI(TAG,
             "%s: latch HIGH=%d MID=%d, I(HIGH/MID/LOW sensor)=%.1f/%.2f/%.3f uA, Vbus=%.3f/%.3f/%.3f V",
             label, latched.high, latched.mid, out.amps[RANGE_HIGH] * 1e6, out.amps[RANGE_MID] * 1e6,
             out.amps[RANGE_LOW] * 1e6, out.vbus[RANGE_HIGH], out.vbus[RANGE_MID], out.vbus[RANGE_LOW]);
    return true;
}

bool currents_agree(double coarse_amps, double fine_amps, range_id coarse_range)
{
    const double lsb = INA226_SHUNT_LSB_V / RANGES[coarse_range].shunt_ohm;
    return fabs(coarse_amps - fine_amps) <= 4.0 * lsb + 0.25 * fabs(fine_amps);
}

bool check_vpp()
{
    snapshot snap = {};
    if (!take_snapshot(snap, "VPP check")) {
        return false;
    }

    const double vin = snap.vbus[RANGE_HIGH];
    if (vin < VPP_MIN_V || vin > VPP_MAX_V) {
        ESP_LOGE(TAG, "VPP_INPUT is %.3f V, expected 1.8 to 3.6 V. Wire PROG pin 11 to pin 7 (+3V3) "
                      "or a bench supply, and power the target from pin 9.", vin);
        record(CHECK_VPP, false);
        return false;
    }

    const bool agree = fabs(vin - snap.vbus[RANGE_MID]) <= VBUS_MATCH_V &&
                       fabs(vin - snap.vbus[RANGE_LOW]) <= VBUS_MATCH_V;
    record(CHECK_VPP, agree);
    return true;
}

void check_alert_line()
{
    if (gpio_get_level(PIN_ALERT) == 0) {
        ESP_LOGE(TAG, "/ALERT is low with every alert source disabled");
        record(CHECK_ALERT, false);
        return;
    }

    for (uint8_t r = 0; r < RANGE_COUNT; ++r) {
        uint16_t flags = 0;
        esp_err_t ret = g_sensors[r].write_alert_flag(ina226::ALERT_FLAG_CNVR);
        ret = ret ?: g_sensors[r].read_alert_flag(&flags);
        if (ret != ESP_OK) {
            record(CHECK_ALERT, false);
            continue;
        }

        delay_ms(30);
        const bool asserted = gpio_get_level(PIN_ALERT) == 0;

        /* The next conversion could finish just after the read, so allow retries. */
        bool released = false;
        for (uint8_t attempt = 0; attempt < 3 && !released; ++attempt) {
            ret = g_sensors[r].read_alert_flag(&flags);
            esp_rom_delay_us(200);
            released = ret == ESP_OK && gpio_get_level(PIN_ALERT) != 0;
        }

        g_sensors[r].write_alert_flag(0);
        g_sensors[r].read_alert_flag(&flags);
        ESP_LOGI(TAG, "/ALERT from %s sensor: asserted=%d released=%d", RANGES[r].name, asserted, released);
        record(CHECK_ALERT, asserted && released);
    }
}

void run_ladder()
{
    snapshot high = {};
    snapshot mid = {};
    snapshot low = {};

    g_latch.force_all_bypass();
    delay_ms(SNAPSHOT_SETTLE_MS);
    latch_state latched = g_latch.read();
    record(CHECK_LATCH_FORCE, latched.high && latched.mid);
    if (!take_snapshot(high, "HIGH range")) {
        return;
    }
    const double vin = high.vbus[RANGE_HIGH];

    g_latch.step_down();
    delay_ms(SNAPSHOT_SETTLE_MS);
    latched = g_latch.read();
    if (!take_snapshot(mid, "After step to MID")) {
        return;
    }

    if (latched.high) {
        /* The load is above the MID limit, so the comparator took the range back. */
        const bool plausible = high.amps[RANGE_HIGH] >= trip_min_amps(vin, RANGE_MID);
        ESP_LOGI(TAG, "HIGH comparator re-tripped at %.1f uA", high.amps[RANGE_HIGH] * 1e6);
        record(CHECK_TRIP_HIGH, plausible);
        return;
    }
    if (!latched.mid) {
        ESP_LOGE(TAG, "MID latch released on the first step; expected it to keep the old HIGH value");
        record(CHECK_STEP_TO_MID, false);
        return;
    }

    record(CHECK_STEP_TO_MID, mid.amps[RANGE_MID] <= trip_max_amps(vin, RANGE_MID));
    record(CHECK_CROSS_MID, currents_agree(mid.amps[RANGE_HIGH], mid.amps[RANGE_MID], RANGE_HIGH));

    /* From HIGH=0/MID=1, D=1 must give 1/0. D=0 then returns to 0/1. */
    g_latch.clock_in(true);
    esp_rom_delay_us(10);
    const latch_state shifted = g_latch.read();
    g_latch.clock_in(false);
    esp_rom_delay_us(10);
    const latch_state restored = g_latch.read();
    ESP_LOGI(TAG, "Shift order: D=1 gave HIGH=%d MID=%d, D=0 gave HIGH=%d MID=%d",
             shifted.high, shifted.mid, restored.high, restored.mid);
    record(CHECK_LATCH_SHIFT, shifted.high && !shifted.mid && !restored.high && restored.mid);
    delay_ms(SNAPSHOT_SETTLE_MS);

    const double mid_amps = mid.amps[RANGE_MID];
    g_latch.step_down();
    delay_ms(SNAPSHOT_SETTLE_MS);
    latched = g_latch.read();
    if (!take_snapshot(low, "After step to LOW")) {
        return;
    }

    if (latched.mid || latched.high) {
        const bool plausible = mid_amps >= trip_min_amps(vin, RANGE_LOW);
        ESP_LOGI(TAG, "MID comparator re-tripped at %.2f uA", mid_amps * 1e6);
        record(CHECK_TRIP_MID, plausible);
        return;
    }

    record(CHECK_STEP_TO_LOW, mid_amps <= trip_max_amps(vin, RANGE_LOW));
    record(CHECK_CROSS_LOW, currents_agree(low.amps[RANGE_MID], low.amps[RANGE_LOW], RANGE_MID));
}

void run_auto_range()
{
    g_ranger.restart();

    const int64_t started_us = esp_timer_get_time();
    const int64_t end_us = started_us + CONFIG_SWD_DEMO_PROF_RUN_SECONDS * 1000000LL;
    int64_t next_log_us = started_us + 1000000LL;

    while (esp_timer_get_time() < end_us && !g_ranger.faulted()) {
        g_ranger.tick();
        if (esp_timer_get_time() >= next_log_us) {
            next_log_us += 1000000LL;
            g_ranger.log_window();
        }
        vTaskDelay(1);
    }

    if (g_ranger.trips(RANGE_MID) != 0) {
        record(CHECK_TRIP_MID, true);
    }
    if (g_ranger.trips(RANGE_HIGH) != 0) {
        record(CHECK_TRIP_HIGH, true);
    }
    record(CHECK_AUTO_RANGE, !g_ranger.faulted() && g_ranger.samples() != 0 && g_ranger.glitches() == 0);

    ESP_LOGI(TAG,
             "Auto-range: samples LOW/MID/HIGH=%" PRIu32 "/%" PRIu32 "/%" PRIu32
             ", steps=%" PRIu32 ", rejected=%" PRIu32 ", trips into MID/HIGH=%" PRIu32 "/%" PRIu32
             ", latch glitches=%" PRIu32 ", overflows=%" PRIu32,
             g_ranger.range_samples(RANGE_LOW), g_ranger.range_samples(RANGE_MID),
             g_ranger.range_samples(RANGE_HIGH), g_ranger.steps(), g_ranger.rejected(),
             g_ranger.trips(RANGE_MID), g_ranger.trips(RANGE_HIGH), g_ranger.glitches(),
             g_ranger.overflows());
}

bool average_current(range_id sensor, uint32_t duration_ms, double &amps_out)
{
    double sum = 0.0;
    uint32_t count = 0;

    delay_ms(BIAS_TEST_SETTLE_MS);
    const int64_t end_us = esp_timer_get_time() + duration_ms * 1000LL;
    while (esp_timer_get_time() < end_us) {
        double amps = 0.0;
        if (g_sensors[sensor].read_current(&amps) != ESP_OK) {
            return false;
        }
        sum += amps;
        count++;
        delay_ms(10);
    }

    if (count == 0) {
        return false;
    }
    amps_out = sum / count;
    return true;
}

/*
 * Tests whether the INA226 shunt pins explain the open-output current. In LOW
 * range R22 carries U7 Vin- and both U8 pins, while R23 carries only U8 Vin-.
 * Powering down U8 should therefore remove two pins' current from U7's reading,
 * and powering down U7 should leave U8's reading unchanged.
 */
void run_bias_test()
{
    double u7_all_on = 0.0;
    double u8_all_on = 0.0;
    double u7_with_u8_off = 0.0;
    double u8_with_u7_off = 0.0;
    double u7_repeat = 0.0;

    g_latch.force_all_bypass();
    delay_ms(SNAPSHOT_SETTLE_MS);
    g_latch.step_down();
    delay_ms(SNAPSHOT_SETTLE_MS);
    g_latch.step_down();
    delay_ms(SNAPSHOT_SETTLE_MS);

    latch_state latched = g_latch.read();
    if (latched.high || latched.mid) {
        ESP_LOGI(TAG, "Bias test skipped: LOW range did not hold. It needs pin 9 open.");
        return;
    }

    bool ok = average_current(RANGE_MID, BIAS_TEST_WINDOW_MS, u7_all_on) &&
              average_current(RANGE_LOW, BIAS_TEST_WINDOW_MS, u8_all_on);

    ok = ok && g_sensors[RANGE_LOW].set_operate_mode(ina226::OP_MODE_POWER_DOWN) == ESP_OK &&
         average_current(RANGE_MID, BIAS_TEST_WINDOW_MS, u7_with_u8_off);
    ok = g_sensors[RANGE_LOW].set_operate_mode(ina226::OP_MODE_SHUNT_BUS_CONT) == ESP_OK && ok;

    ok = ok && g_sensors[RANGE_MID].set_operate_mode(ina226::OP_MODE_POWER_DOWN) == ESP_OK &&
         average_current(RANGE_LOW, BIAS_TEST_WINDOW_MS, u8_with_u7_off);
    ok = g_sensors[RANGE_MID].set_operate_mode(ina226::OP_MODE_SHUNT_BUS_CONT) == ESP_OK && ok;

    ok = ok && average_current(RANGE_MID, BIAS_TEST_WINDOW_MS, u7_repeat);

    latched = g_latch.read();
    if (!ok || latched.high || latched.mid) {
        ESP_LOGE(TAG, "Bias test invalid: ok=%d, latch HIGH=%d MID=%d", ok, latched.high, latched.mid);
        return;
    }

    const double predicted_drop = 2.0 * u8_all_on;
    const double measured_drop = u7_all_on - u7_with_u8_off;
    const bool consistent = fabs(measured_drop - predicted_drop) <= 0.3 * fabs(predicted_drop) &&
                            fabs(u8_with_u7_off - u8_all_on) <= BIAS_TEST_CONTROL_LIMIT_A;

    ESP_LOGI(TAG, "Bias test: U7 all on=%.2f uA, U7 with U8 off=%.2f uA, U7 repeat=%.2f uA, "
                  "U8 all on=%.2f uA, U8 with U7 off=%.2f uA",
             u7_all_on * 1e6, u7_with_u8_off * 1e6, u7_repeat * 1e6, u8_all_on * 1e6, u8_with_u7_off * 1e6);
    ESP_LOGI(TAG, "Bias test: U7 drop predicted=%.2f uA, measured=%.2f uA, consistent with pin bias=%d",
             predicted_drop * 1e6, measured_drop * 1e6, consistent);
}

void log_results(const char *label)
{
    static constexpr const char *RESULT_NAMES[] = {"NOT EXERCISED", "PASS", "FAIL"};

    ESP_LOGI(TAG, "---- Power profiler results (%s, cumulative) ----", label);
    for (uint8_t check = 0; check < CHECK_COUNT; ++check) {
        if (g_results[check] == RESULT_FAIL) {
            ESP_LOGE(TAG, "%-34s %s", CHECK_NAMES[check], RESULT_NAMES[g_results[check]]);
        } else {
            ESP_LOGI(TAG, "%-34s %s", CHECK_NAMES[check], RESULT_NAMES[g_results[check]]);
        }
    }
}

esp_err_t init_sensors()
{
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = PIN_SDA;
    bus_cfg.scl_io_num = PIN_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &g_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    for (uint8_t addr = 0x40; addr <= 0x4f; ++addr) {
        if (i2c_master_probe(g_i2c_bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02x", addr);
        }
    }

    esp_err_t result = ESP_OK;
    for (uint8_t r = 0; r < RANGE_COUNT; ++r) {
        ret = g_sensors[r].init(g_i2c_bus, PIN_ALERT, RANGES[r].i2c_addr);
        if (ret == ESP_OK) {
            vTaskDelay(1);
            ret = g_sensors[r].configure_shunt(RANGES[r].shunt_ohm);
            ret = ret ?: g_sensors[r].set_sample_mode(ina226::AVG_SAMPLE_4);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "%s sensor at 0x%02x failed: %s", RANGES[r].name, RANGES[r].i2c_addr,
                     esp_err_to_name(ret));
            result = ret;
        }
    }

    record(CHECK_SENSORS, result == ESP_OK);
    return result;
}

TaskHandle_t g_sampler = nullptr;
TaskHandle_t g_requester = nullptr;
esp_err_t g_init_result = ESP_FAIL;
uint32_t g_warmup_ms = 0;

/*
 * Created once at init and pinned to the other core. The I2C interrupt is
 * allocated here too, so current sampling does not interrupt the SWD task.
 */
void sampler_task(void *)
{
    g_init_result = init_sensors();
    xTaskNotifyGive(g_requester);

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        g_ranger.restart();
        const int64_t warmup_end_us = esp_timer_get_time() + g_warmup_ms * 1000LL;
        while (esp_timer_get_time() < warmup_end_us) {
            g_ranger.tick();
            vTaskDelay(1);
        }
        g_ranger.clear_stats();
        xTaskNotifyGive(g_requester);

        while (ulTaskNotifyTake(pdTRUE, 1) == 0) {
            g_ranger.tick();
        }
        xTaskNotifyGive(g_requester);
    }
}

} // namespace

extern "C" void power_profiler_set_debug_lines(bool connect)
{
    if (!g_gpio_ready) {
        return;
    }

    if (connect) {
        gpio_pulldown_dis(PIN_SWDIO_OUT);
        gpio_pulldown_dis(PIN_SWDIO_IN);
        gpio_set_level(PIN_DBG_EN, 1);
        delay_ms(1);
    } else {
        gpio_set_level(PIN_DBG_EN, 0);
        /* The translator A-side inputs must not float while isolated. */
        gpio_pulldown_en(PIN_SWDIO_OUT);
        gpio_pulldown_en(PIN_SWDIO_IN);
    }
}

extern "C" void power_profiler_force_bypass(void)
{
    if (g_gpio_ready) {
        g_latch.force_all_bypass();
    }
}

extern "C" esp_err_t power_profiler_init(void)
{
    gpio_set_level(PIN_DBG_EN, 0);
    gpio_config_t dbg_cfg = {};
    dbg_cfg.pin_bit_mask = BIT64(PIN_DBG_EN);
    dbg_cfg.mode = GPIO_MODE_OUTPUT;

    esp_err_t ret = gpio_config(&dbg_cfg);
    ret = ret ?: g_latch.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    g_gpio_ready = true;

    power_profiler_set_debug_lines(false);
    g_latch.force_all_bypass();
    delay_ms(2);

    g_requester = xTaskGetCurrentTaskHandle();
    if (xTaskCreatePinnedToCore(sampler_task, "prof_sampler", 4096, nullptr, tskIDLE_PRIORITY + 2, &g_sampler,
                                configNUMBER_OF_CORES - 1) != pdPASS) {
        ESP_LOGE(TAG, "Sampler task creation failed");
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    g_sensors_ready = g_init_result == ESP_OK;
    return g_init_result;
}

extern "C" bool power_profiler_sample_begin(uint32_t warmup_ms)
{
    if (!g_sensors_ready) {
        return false;
    }

    g_requester = xTaskGetCurrentTaskHandle();
    g_warmup_ms = warmup_ms;
    xTaskNotifyGive(g_sampler);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return true;
}

extern "C" void power_profiler_sample_end(power_profiler_result_t *result)
{
    g_requester = xTaskGetCurrentTaskHandle();
    xTaskNotifyGive(g_sampler);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    result->ok = !g_ranger.faulted() && g_ranger.samples() != 0;
    result->avg_amps = g_ranger.average_amps();
    result->min_amps = g_ranger.minimum_amps();
    result->max_amps = g_ranger.maximum_amps();
    result->vin_volts = g_ranger.last_vbus();
    result->samples = g_ranger.samples();
    result->low_samples = g_ranger.range_samples(RANGE_LOW);
    result->mid_samples = g_ranger.range_samples(RANGE_MID);
    result->high_samples = g_ranger.range_samples(RANGE_HIGH);
    result->steps = g_ranger.steps();
    result->trips = g_ranger.trips(RANGE_MID) + g_ranger.trips(RANGE_HIGH);
}

extern "C" void power_profiler_run_test(const char *label)
{
    if (!g_sensors_ready) {
        ESP_LOGW(TAG, "Skipping power profiler test (%s): sensors not ready", label);
        log_results(label);
        return;
    }

    ESP_LOGI(TAG, "==== Power profiler test: %s ====", label);
    g_latch.force_all_bypass();
    delay_ms(SNAPSHOT_SETTLE_MS);

    if (check_vpp()) {
        check_alert_line();
        run_ladder();
        run_auto_range();
        run_bias_test();
    }

    g_latch.force_all_bypass();
    log_results(label);
}
