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
`4bde669`, and the demo initially selected a 1 us guard. The guarded dedicated
build passed and its object code was inspected.

A second, confirmed software defect was found on 2026-07-13. The SWD clock
delay primitive was an empty C loop:

```c
while (--delay);
```

ESP-IDF 6.0.2's GCC 15.2.0 build at `-Os` removed that loop completely. In the
dedicated object, `SWJ_Sequence()` emitted the SWDIO write, SWCLK clear, and
SWCLK set instructions back-to-back. Therefore the configured 10 kHz or 500
kHz value did not pace dedicated GPIO at all. The regular GPIO backend retained
substantially more MMIO latency, explaining the backend-dependent behavior
without requiring a protocol difference.

The working-tree fix uses `esp_cpu_get_cycle_count()` and stores actual CPU
cycles in `DAP_Data.clock_delay`. The dedicated build now contains
`rsr.ccount` wait loops around both sides of every SWCLK edge. A dedicated 10
kHz build stored 11,998 cycles per half-period; a separate regular-GPIO 500 kHz
build stored 238 cycles. Both builds passed.

The user subsequently confirmed that the CPU-cycle pacing fix makes the
dedicated backend work on Rev 6. The regular translated-GPIO backend also
works. No complete successful log or measured translated-board throughput is
recorded here. The user remembers 600-800 KB/s before adding the translators.

The user then tested a zero translator turnaround guard. SWD initialization
worked, but operation stopped while reading RAM, so zero is not usable on the
current hardware. A 250 ns guard works and compiles to a minimum 60-cycle wait
at 240 MHz. Performance remains below the remembered pre-translator rate.

The defaults now select `CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ=-1` as an explicit
YOLO mode. It selects the existing `SWD_TransferFast()` path, whose
`PIN_DELAY_FAST()` emits no delay instructions. The JTAG-to-SWD and generic SWD
sequence helpers retain their minimum one-cycle delay because the original
fully unpaced dedicated build failed during JTAG-to-SWD entry. The separate
translator ownership guards remain active. `sdkconfig.defaults` retains the
confirmed 250 ns value. The user later reported that a working unpaced build
did not materially improve the roughly 200 KB/s result; no complete YOLO log
was retained here. At the start of profiling on 2026-07-13, the generated
`sdkconfig` selected a paced 32 MHz clock. It initially had a 1 us guard, then
the user selected the hardware-confirmed 250 ns guard before capturing the
baseline below. Preserve and report generated values when comparing results;
changing only `sdkconfig.defaults` does not update an existing configuration.

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
CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ=-1
CONFIG_ESP_SWD_TURNAROUND_DELAY_US=0
CONFIG_ESP_SWD_TURNAROUND_DELAY_NS=250
CONFIG_ESP_SWD_USE_DEDICATED_GPIO=y
CONFIG_ESP_SWD_PERF_INSTRUMENTATION=y
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
did nothing. Commit `4bde669` made it functional. The working tree retains it
as the whole-microsecond part and adds `CONFIG_ESP_SWD_TURNAROUND_DELAY_NS` as
a `0..999 ns` remainder.

The implementation is in `components/swd_esp/cmsis_dap/SW_DP.c`. It converts
the combined duration to CPU cycles, rounding up, and waits using the per-core
cycle counter. This keeps sub-microsecond values available to the `IRAM_ATTR`
transfer helpers without calling a microsecond-only delay API.

Every SWDIO ownership change now performs:

```text
1. Force SWCLK low.
2. Raise U5 /OE (isolate U5).
3. For target ownership, immediately disable the GPIO8 ESP output.
4. Busy-wait the configured microsecond plus nanosecond guard.
5. Change DIR1/DIR2 and configure/preload the ESP output as required.
6. Lower U5 /OE.
7. Busy-wait the same guard.
8. Resume the existing SWD clock sequence.
```

No extra SWCLK pulse is created. SWCLK stays low during both guards. Setting
both values to zero is handled at compile time and emits no delay loops. The
user's zero-delay test stopped during RAM reading.

There are two waits per ownership change and normally two ownership changes
per SWD transaction. At the demo's 250 ns setting, the nominal added guard time
is therefore 1 us per DP/AP transaction. The benchmark should be used to
measure the real throughput impact rather than estimating it from clock
frequency alone.

The dedicated build disassembly was checked after this implementation:

- both target-drive and host-drive helpers contain two `rsr.ccount` loops;
- the 250 ns guard compiles to a minimum 60-cycle wait at 240 MHz;
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

### Dedicated-GPIO cycle profiler

`CONFIG_ESP_SWD_PERF_INSTRUMENTATION` enables diagnostic counters around the
existing SWD implementation. The demo enables it in `sdkconfig.defaults`, but
the component's own Kconfig default is off. Disable it for final throughput and
long stability runs because its counter updates execute in the hot path.

For every measured 8 KiB API call, the demo resets the counters immediately
before timing and snapshots them immediately after timing. Write and read
profiles therefore remain separate. The profiler records:

- CPU cycles for the complete `swd_write_memory()` or `swd_read_memory()` call;
- cycles inside physical `SWD_Transfer()` attempts;
- protocol read and write attempt counts and cycle totals;
- target-drive and host-drive ownership-helper cycles and call counts;
- OK, WAIT, FAULT, parity/protocol-error, and invalid ACK outcomes;
- separate cycle totals for every ACK outcome;
- attempts, OK, and WAIT bucketed by DP/AP, read/write, and A[3:2];
- logical transfer count and consecutive-WAIT streak distribution;
- minimum and maximum physical-transfer cycle counts.

Pattern generation, buffer clearing, byte comparison, logging, recovery, and
the inter-iteration task delay remain outside the measurements. The physical
transfer timing stops before its own aggregate-counter update, while the outer
API timing includes all profiler bookkeeping. The reported `avg outside` value
therefore includes both higher-level memory-loop work and the profiler's own
per-transfer aggregate update.

The first hardware profile was captured with:

```text
CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ=32000000
CONFIG_ESP_SWD_TURNAROUND_DELAY_US=0
CONFIG_ESP_SWD_TURNAROUND_DELAY_NS=250
CONFIG_ESP_SWD_IDLE_CYCLES=0
CONFIG_ESP_SWD_USE_DEDICATED_GPIO=y
CONFIG_ESP_SWD_PERF_INSTRUMENTATION=y
CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ=240
```

The DP IDCODE was `0x6ba02477`. This is not enough to identify the exact target
MCU, which was not recorded. All 100 iterations verified, target RAM was
restored, and no transfer, mismatch, or recovery failures occurred.

Measured baseline:

```text
Write: 209.95 KB/s average, 208.62 minimum, 211.57 maximum
Read:  179.97 KB/s average, 179.86 minimum, 180.07 maximum
Write: 656986 attempts, 206400 OK, 450586 WAIT
Read:  822335 attempts, 206400 OK, 615935 WAIT
Write U5: target/host 212.42/219.11 cycles, 32.45% of SWD cycles
Read U5:  target/host 212.39/218.88 cycles, 34.83% of SWD cycles
```

WAIT represented 68.58% of write attempts and 74.90% of read attempts. Each
OK therefore required 3.1831 physical attempts for writes and 3.9842 for
reads. The exact 206400 OK count is 2064 successful protocol transfers per
8 KiB call, matching the block implementation's 2048 data words plus TAR and
RDBUFF operations at each 1 KiB boundary. The outer memory code and profiler
bookkeeping represented only 6.70% of write call cycles and 6.79% of read call
cycles. The immediate WAIT retry loop is therefore the first optimization
target; high-level batching is already doing the expected minimum work.

`CONFIG_ESP_SWD_IDLE_CYCLES` now exposes the existing CMSIS-DAP post-success
idle clocks. It ranges from 0 through 255 and is separate from the 250 ns U5
turnaround guard. The enhanced profiler also reports OK/WAIT cycle costs,
request classes, and WAIT streaks.

The enhanced zero-idle build was then run on the same hardware. The target was
running when attached and was halted for the stress test. All 100 iterations
verified and RAM restoration succeeded. The additional profiler bookkeeping
changed request cadence, so compare this diagnostic run with the earlier
aggregate-profiler run rather than treating its KB/s as uninstrumented speed:

```text
Write: 215.32 KB/s, 619715 attempts, 206400 OK, 413315 WAIT
Read:  179.87 KB/s, 805672 attempts, 206400 OK, 599272 WAIT
Write ACK cost: OK 2172.76 cycles, WAIT 940.93 cycles
Read ACK cost:  OK 2125.04 cycles, WAIT 941.26 cycles
Write WAIT streaks 0/1/2/3/4-7/8+: 2416/1457/198414/3313/800/0
Read WAIT streaks 0/1/2/3/4-7/8+:  800/192/17144/188264/0/0
```

WAIT attempts consumed approximately 46.44% of write SWD cycles and 56.26%
of read SWD cycles. Ordinary AP DRW writes averaged 1.9933 WAITs per OK and
96.13% of all logical write-side transfers had exactly two WAITs. AP DRW reads
averaged 2.9148 WAITs per OK; 91.21% of read-side logical transfers had exactly
three WAITs and 8.31% had two. TAR writes had no WAITs. The write-side final
DP RDBUFF checks averaged 6.3637 WAITs, but there were only 800 of them versus
204800 AP DRW writes.

Compared with the earlier zero-idle aggregate profile, the enhanced profiler's
extra pacing reduced write WAITs from 450586 to 413315 and increased measured
write throughput from 209.95 to 215.32 KB/s. Read WAITs also fell from 615935
to 599272 while read throughput stayed effectively flat. This is direct
evidence that request timing can trade cheap delay for fewer full WAIT
transactions, but it did not establish that more post-success idle clocks would
monotonically reduce WAITs.

The same enhanced-profiler build was subsequently tested at 2, 4, and 8 idle
cycles. Every run completed 100 verified iterations without transfer failures,
mismatches, or RAM-restore failures:

```text
idle  write KB/s  read KB/s  combined KB/s  write WAIT  read WAIT  write/read OK cycles
   0      215.32     179.87         196.01      413315     599272  2172.76/2125.04
   2      212.95     205.68         209.25      411361     454921  2231.49/2183.83
   4      210.31     191.82         200.64      411010     510151  2289.80/2242.22
   8      205.18     177.43         190.30      410292     567278  2406.30/2357.76
```

The combined number is the harmonic mean for equal-size write and read blocks.
Relative to idle 0, idle 2 reduced write throughput by 1.10%, increased read
throughput by 14.35%, and improved combined throughput by 6.76%. Idle 4 retained
only a 2.36% combined improvement, while idle 8 was 2.91% slower. Each added
idle clock increased successful-transfer cost by approximately 29.2 through
29.4 CPU cycles. Read WAITs were non-monotonic: 599272, 454921, 510151, then
567278. The measurements therefore disprove the simple model that increasing
post-success idle always gives the target more useful completion time. The
exact cause of the cadence-sensitive read behavior is not established by these
ESP-side counters.

The earlier aggregate-profiler code was also checked in two isolated build
directories before the ACK/request/streak counters were added:

- translated regular GPIO with profiling enabled built successfully;
- dedicated GPIO with profiling disabled built successfully;
- the dedicated profiled `SWD_Transfer()` grew from `0x735` to `0x7f6` bytes;
- target/host ownership helpers grew from `0x6e`/`0x81` to `0x8f`/`0xa3`
  bytes respectively.

Do not optimize the ownership helpers until the idle-cycle experiment has
established whether expensive WAIT attempts can be replaced by idle clocks
without U5 ownership changes. After that, three evidence-backed experiments
are available for separate A/B tests:

1. Put U5 `/OE` and `DIR1` in the existing dedicated output bundle. ESP32-S3
   has eight dedicated output channels per CPU and the current bundle uses two.
2. Keep GPIO8 input-enable fixed instead of toggling it during ownership. The
   ESP32-S3 HAL and TRM treat pad input-enable and output-enable as independent;
   only output-enable controls whether GPIO8 drives. GPIO18 is the actual SWDIO
   input. Verify this change electrically before retaining it.
3. Stop rewriting `DIR2` on every ownership change. Normal SWD leaves it low,
   so initialize it once and retain that state.

Do not combine these changes initially, or the cycle profile will not identify
which one helped.

Official dedicated-GPIO reference:

- https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/dedic_gpio.html

## RMT and SPI accelerator feasibility

These options were reviewed against the ESP32-S3 ESP-IDF peripheral APIs and
the Rev 6 translator wiring. Neither backend has been implemented. Treat the
following as an architecture assessment, not measured performance.

### RMT

The ESP32-S3 RMT peripheral can transmit level-duration symbols and ESP-IDF can
synchronize multiple TX channels. In principle, one TX channel could generate
SWCLK while a second generates host SWDIO. This does not map cleanly onto a
complete SWD transaction:

- every request must stop for the host-to-target turnaround and three-bit ACK;
- the ACK determines whether a read data phase, write data phase, WAIT retry,
  FAULT handling, or idle clocks follow;
- Rev 6 must isolate U5, change `DIR1`, and disable or enable the GPIO8 pad
  output driver between those phases;
- RMT RX records level-duration symbols. It does not directly sample SWDIO on
  externally defined SWCLK edges;
- RMT cannot dynamically tri-state GPIO8 or operate U5 `/OE` and `DIR1` as a
  conditional per-symbol side effect.

A two-channel RMT design would therefore consist of many short queued segments
with CPU intervention at every turnaround and ACK. The stock driver queues TX
transactions and reports completion through an ISR, so that setup and
synchronization overhead is likely to dominate a roughly 46-bit SWD transfer.
A direct-register implementation could reduce driver overhead but would still
need the same CPU-controlled barriers and a separate input sampling solution.

Conclusion: RMT is technically usable for a waveform experiment, but it is not
a promising throughput backend for this split-direction SWD link.

Official ESP-IDF references:

- https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/rmt.html
- https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/rmt.html#start-transmission-simultaneously
- https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/rmt.html#initiate-tx-transaction

### SPI

SPI is a closer electrical match because the Rev 6 host-side signals are
already separate: GPIO6 can be SCLK, GPIO8 MOSI, GPIO18 MISO, and active-low
GPIO15 `/OE` can be SPI CS. GPIO17 `DIR1` still has to be controlled in
software. A candidate backend would use SPI mode 0, LSB-first bit order, no
DMA, polling transactions, an acquired bus, and automatic dummy insertion
disabled.

It cannot be one ordinary half-duplex SPI transaction. SWD requires at least
these CPU-separated phases:

```text
TX request -> CS high/SWCLK low -> isolate and turn targetward
RX ACK and optional read data -> CS high/SWCLK low -> isolate and turn hostward
optional TX write data -> host idle
```

The target ACK determines the next phase. Reads can potentially clock a fixed
ACK-plus-data receive phase if WAIT/FAULT dummy-data behavior is deliberately
matched. Writes still require a later transmit phase after ACK. The SPI
driver's `cs_ena_pretrans` field is expressed in SPI clock cycles and could
provide the settling guard after `/OE` is asserted, but the guard after `/OE`
is deasserted and before changing `DIR1` still needs a CPU cycle-counter wait.

There are additional details to prove before implementation:

- ESP32-S3 has documented restrictions for TX lengths congruent to one modulo
  eight, so the 33 data-plus-parity bits need a verified framing strategy;
- automatic dummy bits must remain off because every extra SCLK edge is an SWD
  clock and changes protocol state;
- disabling dummy compensation means the real translator, trace, and MISO
  input delay still limits the usable SPI clock;
- stock `spi_device_polling_transmit()` overhead is documented at roughly 9 us
  for a one-byte polling transaction under Espressif's example conditions.
  Two or three driver transactions per SWD word are therefore unlikely to beat
  the current approximately 20 us/word observed at 200 KB/s.

Conclusion: do not expect the stock SPI master driver to improve throughput. A
specialized SPI2 backend that preconfigures registers and directly starts and
polls each short phase remains a plausible later experiment, after dedicated
GPIO profiling identifies the actual cycle budget. It must retain explicit U5
ownership transitions and be tested for protocol framing and electrical
settling rather than inferred from SPI clock rate.

Official ESP-IDF references:

- https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/spi_master.html#spi-transactions
- https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/spi_master.html#transactions-with-integers-other-than-uint8-t
- https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/spi_master.html#transfer-speed-considerations

## Build evidence and commands

The implementation was compiled with ESP-IDF v6.0.2 from:

```text
/Users/hu/esp/esp-idf
```

The regular translated-GPIO demo built successfully before dedicated GPIO was
enabled. The final dedicated-GPIO plus 1 us delay configuration also built
successfully after the delay implementation. The later CPU-cycle clock-pacing
fix builds with both dedicated and regular translated GPIO under ESP-IDF
v6.0.2.

The enhanced ACK/request/streak profiler and zero-idle configuration also built
successfully on 2026-07-13 with ESP-IDF v6.0.2 from
`/home/jackson/esp/esp-idf`. This is compile evidence only; the enhanced
profiler still needs a hardware run.

Typical setup on the original machine:

```sh
source /Users/hu/esp/esp-idf/export.sh
idf.py build
```

Use the ESP-IDF path installed on the new machine. Prefer a fresh build
directory when comparing configurations, and verify the generated config:

```sh
rg 'ESP_SWD_(PHY_AXC2T245|USE_DEDICATED_GPIO|DEFAULT_CLOCK_HZ|TURNAROUND_DELAY_(US|NS)|IDLE_CYCLES|PERF_INSTRUMENTATION)|ESP_MAIN_TASK_AFFINITY' build/config/sdkconfig.h sdkconfig
```

Useful object-code check for the dedicated backend:

```sh
xtensa-esp32s3-elf-objdump -dr \
  build/esp-idf/swd_esp/CMakeFiles/__idf_swd_esp.dir/cmsis_dap/SW_DP.c.obj \
  | rg 'ee\.wr_mask_gpio_out|ee\.get_gpio_in|rsr\.ccount'
```

The exact build paths can differ by ESP-IDF/CMake version.

## Next hardware-debugging steps

Keep the configured 32 MHz SWD clock, 250 ns translator guard, dedicated GPIO,
240 MHz CPU, RAM range, block size, and iteration count fixed. Enhanced-profiler
runs for idle 0, 2, 4, and 8 are complete. Do not continue upward: test idle 1
and 3 to bracket the measured idle-2 optimum. Do not convert the requested
32 MHz directly into a presumed 31.25 ns software-bit-banged idle clock; the
observed successful-transfer costs show approximately 29.3 CPU cycles per
added idle clock in this build.

The generated `sdkconfig`, rather than `sdkconfig.defaults`, controls the
actual run. Record the complete startup configuration, target initial halt
state, throughput, ACK cost, request-class lines, and WAIT streak histogram for
each value. The useful setting is the one that reduces WAIT attempts enough to
offset its extra idle clocks; do not select a value from WAIT count alone.

After the idle-1 and idle-3 diagnostic runs:

1. Disable `CONFIG_ESP_SWD_PERF_INSTRUMENTATION` and compare idle 0, 1, 2, and
   3, because profiler bookkeeping itself changes request cadence.
2. Run the 100-iteration stability test on the fastest uninstrumented setting.
3. Compare translated regular GPIO by changing only the dedicated-GPIO option.
4. If idle clocks do not help, test a delay after WAIT separately instead of
   changing the electrical turnaround guard.
5. Only then A/B the U5 helper changes listed above, one at a time.

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
8. A working unpaced YOLO build was reported not to improve throughput
   materially, but its complete configuration and profile were not retained.

## Working rules for the next Codex instance

- Do not invent a target MCU memory map, voltage, ACK value, or measured speed.
- Do not claim hardware success from a successful build.
- Preserve the direct-GPIO legacy backend while changing the translated Rev 6
  backend.
- Keep regular GPIO available for A/B diagnosis.
- Keep all dedicated-GPIO SWD calls on one explicitly pinned task/core.
- Keep U5 isolated while changing DIR1 or output ownership.
- Keep SWCLK low during translator settling guards.
- Keep paced SWD clocks derived from `CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ`. Treat
  `-1` as the explicit unpaced transfer sentinel, and keep the separate
  translator-settling delay limited to ownership transitions.
- Use `rg` for source searches and inspect the generated `sdkconfig` before
  diagnosing the selected backend.
- Use official Espressif source/docs for dedicated GPIO behavior and the TI
  SN74AXC2T245 data sheet for translator behavior.
- When the board is available, prefer raw ACK evidence and target-side waveforms
  over further speculative code changes.
