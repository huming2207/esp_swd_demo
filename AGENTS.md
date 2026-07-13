# Soul Injector Rev 6 SWD bring-up handoff

This file is the working handoff for the Soul Injector Rev 6 SWD proof of
concept. Read it before changing the demo or `components/swd_esp`.

The user explicitly prefers evidence over guesses. Check the local source,
KiCad design, ESP-IDF source, or official vendor documentation before making a
hardware or ESP32-specific claim. If something has not been measured on the
board, label it as unverified.

## Repository state at handoff

Snapshot date: 2026-07-13.

Demo repository:

- Path on the original machine: `/Users/hu/Projects/esp_swd_demo`
- Branch: `feature/si-rev6`
- HEAD: `2612ff2 Enable dedicated GPIO for a test`
- Remote branch was `origin/feature/si-rev6` at the same commit.

Important demo commits, newest first:

- `2612ff2 Enable dedicated GPIO for a test`
- `f6eacc3 Sync submodule`
- `8c2667a Vibe code a stress test`
- `45056a4 Add rev6 demo`
- `cfcbcdd Implement demo` (the older Rev 3-style demo)

SWD component submodule:

- Path: `components/swd_esp`
- Branch: `feature/direction-ctrl-swd`
- HEAD: `4bde669 Implement delay`
- Remote branch was `origin/feature/direction-ctrl-swd` at the same commit.

Important component commits, newest first:

- `4bde669 Implement delay`
- `f161851 Generate README`
- `a2e5e05 Initial implementation on direction controlled SWD`
- `5164870` is the older direct-GPIO line used as historical context.

Both repositories were clean before this `AGENTS.md` file was created. The
parent repository records submodule commit `4bde669`. On another machine run:

```sh
git submodule update --init --recursive
```

When changing the component, commit and push inside `components/swd_esp`
first, then commit the updated submodule pointer in the demo repository.

The production-ish Soul Injector firmware repository used for comparison was:

- `/Users/hu/Projects/soulinjector`
- It contains the Rev 6 CMake/Kconfig defaults and the same SWD component work.

The KiCad project used for the hardware cross-check was:

- `/Users/hu/HardwareProjects/soulinjector-hw`
- Main schematic:
  `/Users/hu/HardwareProjects/soulinjector-hw/SoulInjector/SoulInjector.kicad_sch`

Those absolute paths will probably differ on another machine.

## Immediate objective and current status

The objective is to prove the Rev 6 SN74AXC2T245-based SWD hardware, compare
regular GPIO bit-banging with ESP32-S3 dedicated GPIO, measure read/write
throughput in decimal KB/s, and run repeated RAM write/read verification for
stability.

The static wiring, pin map, reset polarity, translator direction logic, and SWD
ownership transitions were reviewed. No confirmed polarity, mapping, SWD cycle,
or steady-state contention bug was found.

The user then enabled dedicated GPIO and reported this repeated failure:

```text
swd: Set transit fail
swd: JTAG2SWD fail
```

`Set transit fail` in this context occurs because the IDCODE read performed by
`JTAG2SWD()` fails. It is at the first DP transaction after the line reset and
JTAG-to-SWD sequence. The exact raw three-bit ACK was not captured.

A real configurable microsecond guard was subsequently implemented in commit
`4bde669`, and the demo now selects a 1 us guard. The guarded dedicated build
passes and its object code was inspected. There has not yet been a reported
hardware retest after this delay change. Do not claim the delay fixed the
physical failure until the board is rerun.

The regular-GPIO hardware result was not explicitly recorded in this
conversation. The user moved on to dedicated GPIO testing, but do not infer
that regular GPIO passed unless they confirm it or logs show it.

## Confirmed Rev 6 GPIO map

The following mapping was exported from the current KiCad netlist and matches
the firmware defaults exactly:

| ESP32-S3 GPIO | Firmware signal | Hardware function |
| --- | --- | --- |
| GPIO4 | `SWCLK_nOE` | Active-low shared output enable for U7 |
| GPIO5 | `HOST_SWBOOT` | U7 A2, target BOOT channel |
| GPIO6 | `HOST_SWCLK` | U7 A1, target SWCLK channel |
| GPIO7 | `HOST_SW_RST` | Gate drive for target reset pull-down MOSFET |
| GPIO8 | `HOST_SWDATA_OUT` | U5 A1, host-to-target SWDIO data |
| GPIO15 | `SWDATA_nOE` | Active-low U5 output enable |
| GPIO16 | `SWDATA_DIR2` | U5 channel 2 direction; held low |
| GPIO17 | `SWDATA_DIR1` | U5 channel 1 direction |
| GPIO18 | `HOST_SWDATA_IN` | U5 A2, target-to-host SWDIO sample |

The active demo defaults in `sdkconfig.defaults` are:

```text
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0=y
CONFIG_ESP_SWD_PHY_AXC2T245=y
CONFIG_ESP_SWD_CLK_PIN=6
CONFIG_ESP_SWD_DATA_OUT_PIN=8
CONFIG_ESP_SWD_DATA_IN_PIN=18
CONFIG_ESP_SWD_DATA_NOE_PIN=15
CONFIG_ESP_SWD_DATA_DIR1_PIN=17
CONFIG_ESP_SWD_DATA_DIR2_PIN=16
CONFIG_ESP_SWD_CLK_NOE_PIN=4
CONFIG_ESP_SWD_NRST_PIN=7
CONFIG_ESP_SWD_BOOT_PIN=5
CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ=500000
CONFIG_ESP_SWD_TURNAROUND_DELAY_US=1
CONFIG_ESP_SWD_USE_DEDICATED_GPIO=y
```

If a generated `sdkconfig` or an existing build directory is present, changing
`sdkconfig.defaults` alone might not alter that cached configuration. Always
inspect the generated `sdkconfig` before assuming which backend or delay was
built.

## Rev 6 translator wiring and truth table

### U5: SWDIO translator

U5 is an SN74AXC2T245 with both B-side pins connected to target SWDIO:

- A1 is `HOST_SWDATA_OUT` on GPIO8.
- B1 reaches target SWDIO through 47 ohms.
- A2 is `HOST_SWDATA_IN` on GPIO18.
- B2 is connected to target SWDIO.
- `/OE` is active low and driven by GPIO15.
- `DIR1` is GPIO17.
- `DIR2` is GPIO16.

TI direction semantics:

- DIR high selects A to B.
- DIR low selects B to A.
- `/OE` high isolates both channels (ports high impedance).

Firmware modes:

| Mode | `/OE` | DIR1 | DIR2 | Result |
| --- | --- | --- | --- | --- |
| Isolated | high | do not care | do not care | No U5 port drives |
| Host owns SWDIO | low | high | low | A1 drives target; A2 senses target |
| Target owns SWDIO | low | low | low | Target is passed to A1/A2; GPIO8 is input/high-Z |

During target ownership, GPIO8's ESP32 output is disabled before DIR1 changes
to low and before U5 is re-enabled. During host ownership, the first outgoing
bit is preloaded while U5 is disabled, then the ESP output and U5 are enabled.

Official component reference:

- https://www.ti.com/lit/ds/symlink/sn74axc2t245.pdf

The data sheet documents a typical 71 kohm internal pull-down on each data I/O
and recommends an external pull-up of 7 kohm or less when a high default is
needed. Rev 6 uses a 4.7 kohm target SWDIO pull-up, which is appropriate.

### U7: SWCLK and BOOT translator

U7 is another SN74AXC2T245:

- A1 is GPIO6 `HOST_SWCLK`; B1 is target SWCLK through 47 ohms.
- A2 is GPIO5 `HOST_SWBOOT`; B2 is target BOOT.
- Both direction pins are hard-pulled high, so both channels are A to B.
- GPIO4 controls their shared active-low `/OE`.

Firmware preloads SWCLK low and BOOT low before U7 is enabled. Normal Rev 6 SWD
therefore keeps target BOOT low. The shared `/OE` is safe for ordinary SWD as
long as this BOOT-low policy is intentional.

There is currently no Rev 6 BOOT-high rescue sequence. This differs from the
older direct-GPIO behavior that asserted BOOT during connection. If a target
application disables SWD and requires a BOOT-high recovery/reset sequence, the
current Rev 6 implementation does not provide it.

### Target reset

GPIO7 drives the gate of Q2 (AO3400 N-MOSFET) through 100 ohms:

- MOSFET source is ground.
- MOSFET drain is `TARGET_RST`.
- GPIO7 low turns Q2 off and releases target reset.
- GPIO7 high turns Q2 on and asserts target reset low.

The CMSIS-DAP abstraction is active-low, so the translated backend intentionally
inverts it:

```text
PIN_nRESET_OUT(0) -> GPIO7 high -> target reset asserted
PIN_nRESET_OUT(1) -> GPIO7 low  -> target reset released
```

The firmware reset polarity was checked and is correct.

### Electrical constraints and safe defaults

The target VPP rail directly powers the B-side supply of the translators. TI's
allowed SN74AXC2T245 supply range is 0.65 V to 3.6 V. Never connect a 5 V target
VPP to this design.

When VPP is absent or below the device's VCC isolation threshold, the B-side
ports isolate. That is safe but SWD cannot work.

Confirmed passive defaults:

- U5 and U7 `/OE` have 47 kohm pull-ups and start disabled.
- U5 DIR1 and DIR2 have 47 kohm pull-downs.
- U7 direction inputs have 47 kohm pull-ups.
- Target SWCLK has a 47 kohm pull-down.
- Target SWDIO has a 4.7 kohm pull-up to VPP.
- Reset MOSFET gate has a 100 kohm pull-down.
- Target reset has a 47 kohm pull-up to VPP.

The firmware also writes safe GPIO latch values before configuring ESP32 pins
as outputs and isolates U5/U7 in `swd_off()`.

## SWD ownership and protocol audit

The CMSIS-DAP request/turnaround/ACK/data paths in `cmsis_dap/SW_DP.c` were
walked for regular translated GPIO and dedicated GPIO. No confirmed ACK cycle,
direction, parity, or first-data-bit ordering bug was found.

The expected transaction ownership is:

```text
host request (8 bits) -> turnaround -> target ACK (3 bits)
```

For a successful read:

```text
host request -> target ACK -> target data/parity -> turnaround -> host idle
```

For a successful write:

```text
host request -> target ACK -> turnaround -> host data/parity -> host idle
```

The implementation also restores host ownership for WAIT, FAULT, optional
dummy data phases, and invalid ACK handling. For writes, the first data bit is
calculated and preloaded before the host driver is enabled.

The dedicated hot path uses:

```text
ee.wr_mask_gpio_out
ee.get_gpio_in
```

It does not use ordinary `GPIO.out_w1ts/out_w1tc` for the dedicated SWCLK or
SWDOUT value once the bundles are created. The ordinary GPIO output-enable bit
is still changed during SWDIO ownership transitions so GPIO8 can become
high-impedance.

## Implemented turnaround delay

`CONFIG_ESP_SWD_TURNAROUND_DELAY_US` used to be a documented placeholder and
did nothing. Commit `4bde669` made it functional.

The implementation is in `components/swd_esp/cmsis_dap/SW_DP.c` and uses
`esp_rom_delay_us()`. This is a busy wait and the called function is in ROM,
which is suitable for the `IRAM_ATTR` transfer helpers.

Every SWDIO ownership change now performs:

```text
1. Force SWCLK low.
2. Raise U5 /OE (isolate U5).
3. For target ownership, immediately disable the GPIO8 ESP output.
4. Busy-wait CONFIG_ESP_SWD_TURNAROUND_DELAY_US.
5. Change DIR1/DIR2 and configure/preload the ESP output as required.
6. Lower U5 /OE.
7. Busy-wait CONFIG_ESP_SWD_TURNAROUND_DELAY_US.
8. Resume the existing SWD clock sequence.
```

No extra SWCLK pulse is created. SWCLK stays low during both guards. A value of
zero is handled at compile time and emits no delay calls.

At 1 us there are two waits per ownership change and normally two ownership
changes per SWD transaction, so the added cost is approximately 4 us per DP/AP
transaction. The demo benchmark should be used to measure the real throughput
impact rather than estimating it from clock frequency alone.

The dedicated build disassembly was checked after this implementation:

- both target-drive and host-drive helpers contain two calls to
  `esp_rom_delay_us()`;
- dedicated GPIO `ee.wr_mask_gpio_out` instructions are present;
- dedicated GPIO `ee.get_gpio_in` instructions are present.

## Dedicated GPIO and task affinity

ESP32-S3 dedicated GPIO bundles belong to the CPU core that allocates them.
Every later operation on those bundles must execute on that same core.

`swd_esp_port_init()` checks:

- the current task is not `tskNO_AFFINITY`;
- the task's pinned core equals `esp_cpu_get_core_id()`;
- bundle allocation succeeds;
- the actual allocated input/output offsets are queried rather than assumed.

The demo calls every SWD API directly from `app_main()`. Its defaults explicitly
select:

```text
CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0=y
```

ESP-IDF creates the main task with `xTaskCreatePinnedToCore()`, so the current
demo is affinity-safe. `vTaskDelay(1)` between stress iterations does not move a
pinned task to another core.

If SWD work is moved to another task, create that task with
`xTaskCreatePinnedToCore()` and perform initialization, every transfer, recovery,
RAM restoration, and `swd_off()` from that same task/core.

Important component limitation: `swd_port_initialized` causes later init calls
to return without repeating the affinity check. Do not initialize on one pinned
task and then call SWD from another task or core. The demo does not do this.

`swd_off()` keeps the dedicated bundles allocated and only isolates the
translators. That is acceptable for the current one-shot demo, but lifecycle
work would be needed for dynamic teardown/reallocation.

## Demo stress and performance test

The demo is in `main/demo_main.c`. Configuration is in
`main/Kconfig.projbuild`.

Default stress parameters:

```text
CONFIG_SWD_DEMO_RAM_ADDRESS=0x20000000
CONFIG_SWD_DEMO_BLOCK_SIZE=8192
CONFIG_SWD_DEMO_ITERATIONS=100
CONFIG_SWD_DEMO_PROGRESS_INTERVAL=10
```

Do not assume `0x20000000..0x20001fff` is valid scratch RAM for every Cortex-M.
Confirm the attached target's RAM map and choose a region that DMA, another core,
or peripherals will not modify during the test.

The test does the following:

1. Allocate original/write/read buffers on the ESP32.
2. Initialize SWD and read the DP IDCODE.
3. Read DHCSR and record whether the Cortex-M was already halted.
4. Halt the target if it was running.
5. Back up the configured target RAM block.
6. For every iteration, generate a changing xorshift pattern.
7. Time `swd_write_memory()` only.
8. Time `swd_read_memory()` only.
9. Compare every returned byte and record the first mismatch.
10. Print cumulative progress and performance.
11. Restore the original target RAM.
12. Resume the target only if it was originally running and RAM restoration
    succeeded.
13. Isolate the translators with `swd_off()`.

If RAM restoration fails, the demo intentionally leaves the target halted. It
must not resume a target with potentially corrupted RAM.

The test is designed to restore the RAM block, but it is still transiently
destructive. A debugger crash, power loss, broken SWD link, DMA activity, or an
incorrect RAM address can prevent safe restoration. Use a real scratch region
where possible.

Performance reporting:

- read and write rates are separate;
- units are decimal KB/s (1000 bytes per second), not KiB/s;
- average rate is total successful bytes divided by total successful transfer
  call time;
- per-transfer minimum and maximum rates are also reported;
- pattern generation, `memset`, comparison, logging, and `vTaskDelay(1)` are
  outside the measured intervals;
- failed calls are counted as failures and are not included as successful
  transferred bytes;
- after a transfer failure the demo tries `swd_clear_errors()` and stops if
  recovery itself fails.

Stability counters include:

- attempted iterations;
- fully verified iterations;
- write failures;
- read failures;
- mismatch iterations;
- total mismatched bytes;
- SWD recovery failures.

## Build evidence and commands

The implementation was compiled with ESP-IDF v6.0.2 from:

```text
/Users/hu/esp/esp-idf
```

The regular translated-GPIO demo built successfully before dedicated GPIO was
enabled. The final dedicated-GPIO plus 1 us delay configuration also built
successfully after the delay implementation.

Typical setup on the original machine:

```sh
source /Users/hu/esp/esp-idf/export.sh
idf.py build
```

Use the ESP-IDF path installed on the new machine. Prefer a fresh build
directory when comparing configurations, and verify the generated config:

```sh
rg 'ESP_SWD_(PHY_AXC2T245|USE_DEDICATED_GPIO|TURNAROUND_DELAY_US)|ESP_MAIN_TASK_AFFINITY' build/config/sdkconfig.h sdkconfig
```

Useful object-code check for the dedicated backend:

```sh
xtensa-esp32s3-elf-objdump -dr \
  build/esp-idf/swd_esp/CMakeFiles/__idf_swd_esp.dir/cmsis_dap/SW_DP.c.obj \
  | rg 'esp_rom_delay_us|ee\.wr_mask_gpio_out|ee\.get_gpio_in'
```

The exact build paths can differ by ESP-IDF/CMake version.

## Next hardware-debugging steps

First rerun the current committed configuration on Rev 6 and record the full
serial log. The key unanswered question is whether the new 1 us guards change
the `JTAG2SWD` failure.

If it connects:

1. Record the DP IDCODE.
2. Run the default 100-iteration stress test.
3. Save write/read average, minimum, maximum KB/s and all stability counters.
4. Repeat with regular translated GPIO using the same CPU frequency, SWD clock,
   target, block, and iterations.
5. Only then vary the delay or SWD clock.

For a fair regular-vs-dedicated comparison, change only:

```text
CONFIG_ESP_SWD_USE_DEDICATED_GPIO
```

Keep the main task explicitly pinned in both builds to reduce scheduler-related
differences.

If `Set transit fail` / `JTAG2SWD fail` remains unchanged, do not keep adding
arbitrary delay. Capture the actual ACK returned by the first IDCODE DP read.
Expected SWD ACK bit values are:

```text
0b001 = OK
0b010 = WAIT
0b100 = FAULT
```

`0b000`, `0b111`, or unstable values are more consistent with missing target
response, wrong input sampling/routing, line ownership, level/power, or physical
signal problems. WAIT/FAULT points toward a responding target and a different
protocol/debug-state problem.

Avoid timing-heavy `ESP_LOG` calls inside the SWD bit loop. For diagnosis,
store the raw ACK in a temporary variable/global and log it after the transfer,
or temporarily expose it through the higher-level read function.

Scope or logic-analyzer checks should be made on the target side of U5/U7, not
only at ESP32 pins:

1. Confirm VPP is present and between 0.65 V and 3.6 V; normally use 3.3 V.
2. Confirm U7 `/OE` goes low and target SWCLK toggles.
3. Confirm BOOT remains low.
4. Confirm request bits appear at target SWDIO.
5. Confirm U5 `/OE` goes high during each ownership change.
6. Confirm DIR1 is high for host request/data and low for target ACK/read data.
7. Confirm target ACK bits appear after the turnaround clock.
8. Confirm GPIO18/A2 sees the same target-side level.
9. Confirm host and target never visibly drive opposing levels.
10. Confirm GPIO7 high pulls target reset low and GPIO7 low releases it.

If regular translated GPIO works but dedicated GPIO still fails with identical
electrical U5 control, focus next on:

- dedicated input bundle allocation and `g_swd_dedic_data_in_mask`;
- whether GPIO18 is routed to the dedicated input channel expected on the
  executing core;
- output bundle ordering and allocated offset masks;
- GPIO8 pad output enable during target ownership;
- whether all calls truly remain on the allocating core;
- target-side waveforms during ACK sampling.

## Known software limitations and cleanup items

These were identified but were not treated as blockers for the minimal demo:

1. `swd_init_debug()` calls `swd_init()` without checking its return value.
   `swd_init()` does propagate `swd_esp_port_init()` failure, but the higher
   function can continue into JTAG-to-SWD retries after a GPIO/bundle failure.
   Fix this before treating initialization errors as production-safe.
2. After final `swd_init_debug()` failure, the component itself does not always
   isolate the translators. The demo explicitly calls `swd_off()` when
   `swd_init_debug()` returns false.
3. Rev 6 always keeps BOOT low and has no BOOT-high recovery policy.
4. `CONNECT_UNDER_RESET` and soft-reset setters exist, but the reviewed code did
   not show them driving a complete recovery policy.
5. `swd_trigger_nrst()` uses the configured BOOT pin unconditionally in one
   path; a generic configuration using BOOT `-1` needs auditing. Rev 6 uses
   GPIO5, so this does not affect the current defaults.
6. The Kconfig GPIO range permits pins above 31, while some low-level regular
   GPIO expressions use a 32-bit `1 << pin` form. All Rev 6 pins are below 32,
   so this does not affect this board.
7. The component README may contain historical wording saying the top-level
   Soul Injector project lacks Rev 6 defaults. The top-level project was updated
   afterward; verify and refresh that paragraph if maintaining the docs.
8. No hardware result after the microsecond delay commit is recorded yet.

## Working rules for the next Codex instance

- Do not invent a target MCU memory map, voltage, ACK value, or measured speed.
- Do not claim hardware success from a successful build.
- Preserve the direct-GPIO legacy backend while changing the translated Rev 6
  backend.
- Keep regular GPIO available for A/B diagnosis.
- Keep all dedicated-GPIO SWD calls on one explicitly pinned task/core.
- Keep U5 isolated while changing DIR1 or output ownership.
- Keep SWCLK low during translator settling guards.
- Do not add hidden delays to every SWD clock bit; the current delay is only for
  ownership transitions.
- Use `rg` for source searches and inspect the generated `sdkconfig` before
  diagnosing the selected backend.
- Use official Espressif source/docs for dedicated GPIO behavior and the TI
  SN74AXC2T245 data sheet for translator behavior.
- When the board is available, prefer raw ACK evidence and target-side waveforms
  over further speculative code changes.
