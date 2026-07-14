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

The generated configuration was later changed to
`CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ=-1`. A zero-padding capture proved that
back-to-back dedicated-GPIO clock writes are not usable on this board: only
4 ns and 5 ns low pulses were observable at the logic-analyzer probe before
the first transfer stopped decoding and the target did not reply. Fast mode
now has `CONFIG_ESP_SWD_FAST_DELAY_NOPS`; four NOPs after each SWCLK transition
completed the 100-iteration stress test. Setup sequences retain cycle-counter
pacing, and the separate 250 ns translator guards remain active.

The four-NOP run did not improve throughput because its faster retries caused
more target WAIT responses. The current pending hardware A/B puts the SWDIO
translator `/OE`, `DIR1`, and `DIR2` pins in the existing dedicated output
bundle behind `CONFIG_ESP_SWD_DEDICATED_TRANSLATOR_CONTROLS`. It is
compile-checked but not yet hardware-validated. Preserve and report generated
values when comparing results; changing only `sdkconfig.defaults` does not
update an existing configuration.

Three direct-RMT waveform experiments were subsequently attempted. None
reached a working IDCODE transaction, and the user restored the component to
the last working dedicated-GPIO state. The RMT source was removed. The current
experiment is a direct SPI2 HAL/LL backend at 10 MHz, described below. Two
hardware runs reached a target-generated OK response but failed to decode it in
SPI2 RX. The latest receive realignment is uncompiled and unmeasured. Do not
describe the SPI backend as working until it reads IDCODE and completes the RAM
test.

## Confirmed Rev 6 GPIO map

The following mapping was exported from the current KiCad netlist and matches
the firmware defaults exactly:

| ESP32-S3 GPIO | Firmware signal | Hardware function |
| --- | --- | --- |
| GPIO4 | `SWCLK_nOE` | Active-low shared output enable for the SWCLK/BOOT translator |
| GPIO5 | `HOST_SWBOOT` | SWCLK/BOOT translator A2, target BOOT channel |
| GPIO6 | `HOST_SWCLK` | SWCLK/BOOT translator A1, target SWCLK channel |
| GPIO7 | `HOST_SW_RST` | Gate drive for target reset pull-down MOSFET |
| GPIO8 | `HOST_SWDATA_OUT` | SWDIO translator A1, host-to-target SWDIO data |
| GPIO15 | `SWDATA_nOE` | Active-low SWDIO translator output enable |
| GPIO16 | `SWDATA_DIR2` | SWDIO translator channel 2 direction; held low |
| GPIO17 | `SWDATA_DIR1` | SWDIO translator channel 1 direction |
| GPIO18 | `HOST_SWDATA_IN` | SWDIO translator A2, target-to-host SWDIO sample |

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
CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ=10000000
CONFIG_ESP_SWD_FAST_DELAY_NOPS=4
CONFIG_ESP_SWD_TURNAROUND_DELAY_US=0
CONFIG_ESP_SWD_TURNAROUND_DELAY_NS=250
CONFIG_ESP_SWD_IDLE_CYCLES=1
CONFIG_ESP_SWD_USE_DEDICATED_GPIO=y
CONFIG_ESP_SWD_DEDICATED_TRANSLATOR_CONTROLS=y
CONFIG_ESP_SWD_USE_SPI=y
CONFIG_ESP_SWD_PERF_INSTRUMENTATION=y
```

If a generated `sdkconfig` or an existing build directory is present, changing
`sdkconfig.defaults` alone might not alter that cached configuration. Always
inspect the generated `sdkconfig` before assuming which backend or delay was
built.

## Rev 6 translator wiring and truth table

### SWDIO translator

The SWDIO translator is an SN74AXC2T245 with both B-side pins connected to
target SWDIO:

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
| Isolated | high | do not care | do not care | No translator port drives |
| Host owns SWDIO | low | high | low | A1 drives target; A2 senses target |
| Target owns SWDIO | low | low | low | Target is passed to A1/A2; GPIO8 is input/high-Z |

During target ownership, GPIO8's ESP32 output is disabled before DIR1 changes
to low and before the translator is re-enabled. During host ownership, the
first outgoing bit is preloaded while the translator is disabled, then the ESP
output and translator are enabled.

Official component reference:

- https://www.ti.com/lit/ds/symlink/sn74axc2t245.pdf

The data sheet documents a typical 71 kohm internal pull-down on each data I/O
and recommends an external pull-up of 7 kohm or less when a high default is
needed. Rev 6 uses a 4.7 kohm target SWDIO pull-up, which is appropriate.

### SWCLK and BOOT translator

The SWCLK and BOOT translator is another SN74AXC2T245:

- A1 is GPIO6 `HOST_SWCLK`; B1 is target SWCLK through 47 ohms.
- A2 is GPIO5 `HOST_SWBOOT`; B2 is target BOOT.
- Both direction pins are hard-pulled high, so both channels are A to B.
- GPIO4 controls their shared active-low `/OE`.

Firmware preloads SWCLK low and BOOT low before the translator is enabled.
Normal Rev 6 SWD therefore keeps target BOOT low. The shared `/OE` is safe for
ordinary SWD as long as this BOOT-low policy is intentional.

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

- Both translators' `/OE` pins have 47 kohm pull-ups and start disabled.
- The SWDIO translator's DIR1 and DIR2 have 47 kohm pull-downs.
- The SWCLK/BOOT translator's direction inputs have 47 kohm pull-ups.
- Target SWCLK has a 47 kohm pull-down.
- Target SWDIO has a 4.7 kohm pull-up to VPP.
- Reset MOSFET gate has a 100 kohm pull-down.
- Target reset has a 47 kohm pull-up to VPP.

The firmware also writes safe GPIO latch values before configuring ESP32 pins
as outputs and isolates both translators in `swd_off()`.

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
2. Raise the SWDIO translator /OE (isolate the translator).
3. For target ownership, immediately disable the GPIO8 ESP output.
4. Busy-wait the configured microsecond plus nanosecond guard.
5. Change DIR1/DIR2 and configure/preload the ESP output as required.
6. Lower the SWDIO translator /OE.
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
Write translator: target/host 212.42/219.11 cycles, 32.45% of SWD cycles
Read translator:  target/host 212.39/218.88 cycles, 34.83% of SWD cycles
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
idle clocks. It ranges from 0 through 255 and is separate from the 250 ns
translator turnaround guard. The enhanced profiler also reports OK/WAIT cycle
costs, request classes, and WAIT streaks.

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

### Fast-path capture and four-NOP result

The zero-padding fast-mode capture files were:

```text
/media/jackson/Ventoy/decoder--260713-163054.csv
/media/jackson/Ventoy/DSLogic U3Pro32-la-260713-163054.csv
```

The raw capture has a 1 GHz sample rate. The decoder reached the first
`RDBUFF`, then reported `NOREPLY`. At the measured SWCLK point, the first fast
transfer contained observable low pulses of 4 ns and 5 ns before SWCLK stayed
high while SWDIO continued changing. The analyzer channel-to-schematic probe
mapping was not recorded, so this establishes waveform collapse at the probe
but does not by itself locate it on the A or B side of the SWCLK/BOOT
translator.

`CONFIG_ESP_SWD_FAST_DELAY_NOPS=4` restored operation with fast mode, idle 1,
the 250 ns guard, dedicated GPIO, and profiling enabled. All 100 iterations
verified and RAM restoration succeeded:

```text
Write: 210.03 KB/s, 821095 attempts, 206400 OK, 614695 WAIT
Read:  190.10 KB/s, 938499 attempts, 206400 OK, 732099 WAIT
Write ACK cost: OK 1739.60 cycles, WAIT 785.06 cycles
Read ACK cost:  OK 1729.62 cycles, WAIT 786.23 cycles
Write translator target/host: 213.82/219.35 cycles
Read translator target/host:  213.90/219.36 cycles
```

The four-NOP clock reduced physical-attempt cost, but writes rose to 2.98 WAITs
per logical transfer and reads to 3.55. Translator ownership changes therefore
grew to 42.26% of write SWD cycles and 43.60% of read SWD cycles. Four NOPs fix
the zero-padding electrical/protocol failure; they do not solve request cadence
or ownership overhead.

The earlier aggregate-profiler code was also checked in two isolated build
directories before the ACK/request/streak counters were added:

- translated regular GPIO with profiling enabled built successfully;
- dedicated GPIO with profiling disabled built successfully;
- the dedicated profiled `SWD_Transfer()` grew from `0x735` to `0x7f6` bytes;
- target/host ownership helpers grew from `0x6e`/`0x81` to `0x8f`/`0xa3`
  bytes respectively.

The first ownership-helper A/B is now implemented behind
`CONFIG_ESP_SWD_DEDICATED_TRANSLATOR_CONTROLS`. When enabled, the output bundle
has five of the ESP32-S3's eight per-CPU output channels in the bit-bang mode,
in this order: SWCLK, SWDIO output, translator `/OE`, translator `DIR1`, and
translator `DIR2`. In the SPI experiment, SPI2 owns SWCLK, SWDIO output,
SWDIO input, and translator `/OE`; the dedicated output bundle contains only
translator `DIR1` and `DIR2`. During bundle creation, the direction pad drivers
are disabled, the dedicated values are initialized, and only then are the pad
drivers re-enabled.

The dedicated-control build succeeded. Disassembly shows dedicated output
instructions for translator `/OE` and direction changes. The GPIO8 pad
output-enable and input-enable operations remain ordinary GPIO register
operations. Hardware throughput and electrical behavior are not yet verified.

After measuring that A/B, the remaining separate experiments are:

1. Keep GPIO8 input-enable fixed instead of toggling it during ownership. The
   ESP32-S3 HAL and TRM treat pad input-enable and output-enable as independent;
   only output-enable controls whether GPIO8 drives. GPIO18 is the actual SWDIO
   input. Verify this change electrically before retaining it.
2. Stop rewriting `DIR2` on every ownership change. Normal SWD leaves it low,
   so initialize it once and retain that state.

Do not combine these changes initially, or the cycle profile will not identify
which one helped.

Official dedicated-GPIO reference:

- https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/dedic_gpio.html

## RMT and SPI accelerator feasibility

These options were reviewed against the ESP32-S3 ESP-IDF peripheral APIs and
the Rev 6 translator wiring. The first direct dual-channel RMT experiment was
compiled and failed on hardware. A first carrier-envelope replacement also
compiled and failed on hardware, but its logic-analyser capture identified a
separate carrier-phase error. A continuous-carrier clock-gating replacement
also failed and the RMT changes were discarded. A direct SPI2 HAL/LL backend is
now implemented and has been compiled and run, but its receive path has not yet
completed an IDCODE read.

### RMT

The earlier assessment rejected the stock RMT driver because each SWD request
has a CPU-controlled ACK barrier and the driver's queued transaction and ISR
completion overhead would dominate short phases. Inspection of epdiy's direct
RMT setup established the useful LL pattern: write RMT memory directly, start
the channel through `rmt_ll_*`, and poll raw completion flags. No epdiy source
was copied.

The first `CONFIG_ESP_SWD_USE_RMT` implementation used TX0 for SWCLK, TX1 for
request/sequences, and TX2 for write data. It encoded every SWCLK bit as a RAM
symbol with four 80 MHz ticks low and four ticks high, and used synchronized TX
starts for host-owned SWCLK plus SWDIO.

That implementation compiled on ESP-IDF 6.0.2 but failed before IDCODE on
2026-07-14 at both tightened and relaxed translator guards. The captured files
were:

```text
/home/jackson/Downloads/decoder--260714-112632.csv
/home/jackson/Downloads/DSLogic U3Pro32-la-260714-112632.csv
```

The target-side SWCLK period was approximately 100 ns, but finite host phases
contained the wrong number of clocks. The intended 51-clock reset split at the
47-symbol software boundary appeared as bursts of 64 and 6 clocks. The intended
16-bit `0xe79e` JTAG-to-SWD selection emitted 22 clocks. Its SWDIO run lengths
were stretched, and the decoder consequently reported AP accesses and NOREPLY
instead of JTAG-to-SWD plus IDCODE. The later request/ACK waveform was already
downstream of this malformed selection sequence.

Espressif's ESP32-S3 TRM explains why the direct RAM symbols were invalid. For
a non-zero RMT pulse-code period it requires:

```text
5 * T_apb_clk + 6 * T_rmt_sclk < period * T_clk_div
```

With APB and RMT source clocks both at 80 MHz and channel divider 1, each level
must therefore exceed 11 source ticks. The implementation used four ticks per
level. The TRM also gives a stricter timing bound for the period immediately
before a zero end marker. Translator guard changes cannot correct either RMT
memory-reader violation.

The first replacement retained 10 MHz and used the RMT carrier unit rather than
50 ns RAM entries:

- TX0 is the only RMT channel used;
- one long high RAM envelope gates a 10 MHz carrier configured for four 80 MHz
  ticks high and four ticks low;
- host SWDIO remains in the dedicated output bundle and software updates it
  after each observed SWCLK falling edge;
- target SWDIO is sampled from the dedicated input bundle on each observed
  SWCLK rising edge;
- the input bundle contains GPIO18 SWDIO first and GPIO6 SWCLK second;
- one-to-three standalone clocks use slower software high/low writes because
  their RMT envelope would violate the zero-end-marker timing bound;
- all RMT interrupts remain disabled and completion is polled from raw status.

This carrier-envelope implementation compiled and ran on 2026-07-14. It still
failed at the first IDCODE read. The captured files were:

```text
/home/jackson/Downloads/decoder--260714-115244.csv
/home/jackson/Downloads/DSLogic U3Pro32-la-260714-115244.csv
```

Unlike the direct-RAM attempt, the decoder recognized line reset, JTAG-to-SWD,
the second line reset, and the IDCODE request before reporting `NOREPLY`. The
target-side clock was a stable 10 MHz within bursts, but envelope boundaries
were not phase-aligned to the free-running carrier:

- the intended 16-clock `0xe79e` selection contained 17 rising edges;
- the following eight-zero sequence contained nine rising edges;
- the IDCODE request had eight clocks, but rising-edge data was
  `11010010` instead of expected `10100101`;
- the same request sampled on falling edges was correct, proving host SWDIO was
  one target-visible clock late;
- the target left SWDIO high during the four ACK clocks, consistent with the
  malformed request never receiving a reply.

The first pulse of an envelope could begin part-way through a carrier high
phase and the final pulse could be truncated. Starting the RMT envelope and
then waiting for software-observed edges therefore could neither guarantee the
requested number of target-visible clocks nor associate bit zero with the first
rising edge. Turnaround guard changes cannot correct this framing error.

The next implementation keeps the carrier running continuously and uses the
SWCLK translator `/OE` as a hardware clock gate:

- TX0's 10 MHz carrier is enabled in all RMT states with its idle level high;
- GPIO4 `SWCLK_nOE` joins the dedicated output bundle;
- every phase starts hidden, observes a complete carrier high phase, and
  enables the clock translator immediately after the following falling edge;
- software counts the requested internal rising/falling edges, then disables
  the clock translator after the final falling edge;
- the first host data bit is preloaded before the clock translator is enabled;
- no finite RMT RAM envelope, RMT TX start, end marker, or manual short-clock
  fallback remains in the hot path.

The clock translator shares `/OE` between SWCLK and BOOT. This experiment
consequently isolates the BOOT channel between every SWD phase while leaving
HOST_SWBOOT low. There is no target reset during normal transfer phases, but
the target-side BOOT behavior while isolated has not been electrically
verified. Treat this as an explicit hardware experiment, not a proven-safe
production design.

The clock-gated replacement requires a fixed 240 MHz CPU. Both host-driven and
target-sampled carrier phases mask interrupts because software must service
every SWDIO edge within a 100 ns clock. It no longer fills or reuses TX1/TX2
waveforms during WAIT periods. This is a functional 10 MHz waveform experiment,
not yet evidence of a throughput improvement.

The full protocol still requires CPU barriers because RMT cannot interpret ACK
or select the next phase, dynamically tri-state GPIO8, or change translator
ownership as a conditional per-symbol side effect. RMT RX records transitions
and durations rather than sampling SWDIO on each externally defined SWCLK edge.

The discarded backend reset and exclusively owned the entire RMT peripheral.
It could not coexist with another RMT user and deliberately bypassed the public
RMT driver, encoders, queues, callbacks, and ISR path.

The discarded RMT working tree had to declare its LL header dependencies
unconditionally in `CMakeLists.txt`. ESP-IDF expands component requirements
before project Kconfig values are reliably available; making `esp_hal_rmt`
conditional caused the RMT source to compile without the target-specific
`hal/rmt_ll.h` include directory.

That working tree left `IRAM_ATTR` off function prototypes and put it only on
definitions. ESP-IDF 6 implements `IRAM_ATTR` with a
`__COUNTER__`-suffixed section name, so placing it on both a prototype and its
definition generates conflicting section attributes with GCC 15. The new SPI
file follows the same declaration pattern and includes `DAP_config.h` before
`DAP.h`; `DAP_SWD` controls whether `DAP_Data_t.swd_conf` is present.

The 10 MHz carrier frequency and 50 ns target-side half-periods are confirmed
for the failed envelope implementation. No RMT backend remains in the current
component working tree. Retain these captures as evidence of why that approach
was abandoned rather than as instructions for another immediate RMT test.

Official ESP-IDF references:

- https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/rmt.html
- https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/rmt.html#start-transmission-simultaneously
- https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/rmt.html#initiate-tx-transaction
- https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf#rmt

### SPI

The current `CONFIG_ESP_SWD_USE_SPI` backend exclusively owns SPI2 and bypasses
`spi_master`. It uses ESP32-S3 HAL/LL operations in mode 0, LSB-first, polling
mode, with no DMA or dummy clocks. GPIO6 is SCLK, GPIO8 is MOSI, GPIO18 is MISO,
and SPI2 CS0 drives the SWDIO translator's active-low `/OE` on GPIO15. GPIO17
`DIR1` and GPIO16 `DIR2` remain in a two-line dedicated-GPIO bundle.

It cannot be one ordinary half-duplex SPI transaction. SWD requires at least
these CPU-separated phases:

```text
TX request -> CS high/SWCLK low -> isolate and turn targetward
RX ACK and optional read data -> CS high/SWCLK low -> isolate and turn hostward
optional TX write data -> host idle
```

The target ACK determines the next phase. The implementation therefore uses
separate request, turnaround/ACK, read-data, turnaround, and write-data SPI
transactions. Hardware CS isolates the translator between phases. A CPU-cycle
guard runs after CS has gone inactive and before `DIR1` changes. CS setup timing
then delays the first SWCLK edge after `/OE` becomes active; the configured
guard is rounded up to whole 10 MHz SPI clocks, so 250 ns becomes 300 ns.

ESP32-S3 CPU-buffer receive accepts arbitrary lengths. CPU-buffer transmit does
not accept lengths congruent to one modulo eight. The backend handles this
without extra SWCLK edges:

- a 33-bit SWD write is split into exact 2-bit and 31-bit transactions;
- an isolated one-bit output sequence uses the SPI command phase;
- automatic dummy bits must remain off because every extra SCLK edge is an SWD
  clock and changes protocol state;
- disabling dummy compensation means the real translator, trace, and MISO
  input delay still limits the usable SPI clock.

The first SPI build ran on hardware on 2026-07-14 but failed IDCODE host-side
validation. The target recognized the request and returned an OK ACK. The
capture files were:

```text
/home/jackson/Downloads/decoder--260714-123542.csv
/home/jackson/Downloads/DSLogic U3Pro32-la-260714-123542.csv
```

The raw 100 MHz capture showed exact bursts of 51, 16, 51, and 8 clocks for
entry, then 8 request clocks and 4 turnaround-plus-ACK clocks. The intended
33-clock read-data phase emitted no clocks; only the later one-clock read
turnaround appeared. The decoder consequently consumed clocks from later retry
bursts to manufacture misleading IDCODE values. The missing burst established
that software did not enter the successful-read branch, but by itself did not
identify why.

An initial hypothesis was that the direct LL path could clear `trans_done`,
observe a stale asserted completion bit, and overwrite the shared data-length
register before a new transaction was latched. The transaction helper was
therefore changed to wait for command idle, wait for `trans_done` to clear,
start the transaction, wait for completion, and then wait for command idle
again. This is a defensive transaction boundary, not a confirmed root cause.

That completion-barrier version compiled and ran, but IDCODE still failed. All
four attempts reported:

```text
SPI RX debug: ACK raw=0x00000009 decoded=0x4,
read requested/programmed=0/0 bits, FIFO=00000000:00000000,
post-done busy=0
```

The zero post-done count and unchanged failure do not support the completion-
race hypothesis. The skipped data branch is instead explained by the decoded
FAULT value below.

No new capture accompanied this run, but the earlier 100 MHz raw capture
resolves `0x9`. During the four turnaround-plus-ACK clocks, target-side SWDIO
was `1,1,0,0` at rising edges and `1,0,0,1` at falling edges. The latter is
exactly the SPI FIFO value `0x9` in LSB-first order. The target therefore sent
OK; SPI2 sampled the response half a cycle later and software incorrectly
shifted the raw value as though it contained rising-edge samples.

This behavior matches ESP-IDF's definition of
`SPI_SAMPLING_POINT_PHASE_0`: master RX is delayed by half an SPI cycle from
standard sampling. In the ESP32-S3 LL source,
`spi_ll_master_set_rx_timing_mode()` is a no-op and
`spi_ll_master_is_rx_std_sample_supported()` returns false. This is a
chip-specific receive constraint, not an unverified translator timing theory.

The current uncompiled working tree keeps the exact turnaround-plus-three-ACK
clock window and realigns its delayed samples. For turnaround count `N`, ACK is
decoded starting at FIFO bit `N-1`, and the following FIFO bit is retained as
read data bit zero. A successful read then emits the full 33 data-plus-parity
clocks; its delayed FIFO stream supplies data bits 1 through 31 and parity.
The extra captured post-parity bit is ignored, so no SWD clock is removed or
added.

For the captured DP IDCODE `0x6ba02477`, the next failure diagnostic should
show `TA+ACK raw=0x00000009 decoded=0x1`, receive length `33/33`, and read FIFO
word zero `0xb5d0123b`. FIFO word one bit zero is deliberately ignored. A
different result should be compared with a new raw capture before changing
sampling logic again.

Official ESP-IDF references:

- https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/spi_master.html#spi-transactions
- https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/spi_master.html#transactions-with-integers-other-than-uint8-t
- https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/spi_master.html#transfer-speed-considerations
- https://github.com/espressif/esp-idf/blob/v6.0.2/components/esp_hal_gpspi/include/hal/spi_types.h
- https://github.com/espressif/esp-idf/blob/v6.0.2/components/esp_hal_gpspi/esp32s3/include/hal/spi_ll.h

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
`/home/jackson/esp/esp-idf`. Its subsequent hardware runs at idle 0, 2, 4, and
8 and the four-NOP fast-path run are recorded above. The dedicated translator-
control variant compiled with the generated configuration recorded above, but
it has not been run on hardware.

The first dual-channel direct-RMT backend compiled on ESP-IDF 6.0.2 and ran on
hardware, but its four-tick RAM pulse entries violated the ESP32-S3 RMT timing
bound and produced malformed clock counts before IDCODE. The carrier-envelope
replacement also compiled and ran, but carrier phase at each envelope boundary
produced extra/truncated clocks and one-clock-late host data. A later
continuous-carrier, GPIO4-gated replacement still failed and was discarded.
The first direct SPI2 backend compiled and produced the framing evidence above;
the completion-barrier fix also compiled and ran but exposed the fixed
half-cycle-delayed ESP32-S3 RX sample. The current receive realignment has not
been compiled.

Typical setup on the original machine:

```sh
source /Users/hu/esp/esp-idf/export.sh
idf.py build
```

Use the ESP-IDF path installed on the new machine. Prefer a fresh build
directory when comparing configurations, and verify the generated config:

```sh
rg 'ESP_SWD_(PHY_AXC2T245|USE_DEDICATED_GPIO|DEDICATED_TRANSLATOR_CONTROLS|USE_SPI|DEFAULT_CLOCK_HZ|FAST_DELAY_NOPS|TURNAROUND_DELAY_(US|NS)|IDLE_CYCLES|PERF_INSTRUMENTATION)|ESP_MAIN_TASK_AFFINITY' build/config/sdkconfig.h sdkconfig
```

Useful object-code check for the dedicated backend:

```sh
xtensa-esp32s3-elf-objdump -dr \
  build/esp-idf/swd_esp/CMakeFiles/__idf_swd_esp.dir/cmsis_dap/SW_DP.c.obj \
  | rg 'ee\.wr_mask_gpio_out|ee\.get_gpio_in|rsr\.ccount'
```

The exact build paths can differ by ESP-IDF/CMake version.

## Next hardware-debugging steps

The immediate test is the uncompiled SPI receive-realignment fix with the same
10 MHz clock, hardware-CS SWDIO translator enable, dedicated-GPIO direction
controls, profiling, 250 ns guard, one idle cycle, and fixed 240 MHz CPU. Record
the startup line and inspect the generated configuration rather than assuming
the defaults were applied.

First make the shortest useful hardware run: initialization through DP IDCODE.
The failure-only log should decode the already observed raw `0x9` as ACK OK,
then report `read requested/programmed=33/33`. The key waveform change is a
33-clock burst immediately after the four-clock turnaround/ACK burst. On the
target side, confirm:

1. A 10 MHz clock with approximately 50 ns low and 50 ns high times while a
   transaction is active.
2. Exactly 51 line-reset clocks with no partial first or final pulse.
3. Exactly 16 JTAG-to-SWD selection clocks and the expected `0xe79e` LSB-first
   data.
4. Eight request clocks followed by one target-owned turnaround clock and
   three ACK clocks.
5. SWCLK returns low between SPI phases with no extra rising edge.
6. SPI2 CS0 on GPIO15 is high between phases, goes low before the first SWCLK
   edge, and provides at least the rounded 300 ns setup at a 250 ns setting.
7. `DIR1` changes only while GPIO15 is high; GPIO8 is high impedance for every
   target-owned phase.
8. Host SWDIO is stable before each rising edge, including both sides of the
   2-bit plus 31-bit write split.
9. GPIO18 and target SWDIO agree during all three ACK samples and all read bits.
10. GPIO4 `SWCLK_nOE` remains low and BOOT remains at the intended target-side
    level throughout the session.

If IDCODE works, run the full 100-iteration test and retain the complete
profiler output. The direct comparison should keep the guard, idle cycles,
profiling, CPU frequency, RAM block, and iteration count fixed. Compare the
10 MHz SPI2 backend with the 10 MHz dedicated bit-bang backend by changing only
the backend selection while retaining a valid clock setting:

```text
CONFIG_ESP_SWD_USE_SPI
```

Compare transfer cycles, ACK OK/WAIT costs, WAIT counts and streaks, and final
KB/s. SPI can alter target request cadence, so a lower physical phase cost does
not guarantee fewer WAITs or higher throughput. Only after selecting the best
instrumented variant should profiling be disabled for final throughput and
stability runs.

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

Scope or logic-analyzer checks should be made on the target side of both
translators, not only at ESP32 pins:

1. Confirm VPP is present and between 0.65 V and 3.6 V; normally use 3.3 V.
2. Confirm the SWCLK/BOOT translator `/OE` goes low and target SWCLK toggles.
3. Confirm BOOT remains low.
4. Confirm request bits appear at target SWDIO.
5. In SPI mode, confirm the SWDIO translator `/OE` is high between every SPI
   phase and low only for the active phase.
6. Confirm DIR1 is high for host request/data and low for target ACK/read data.
7. Confirm target ACK bits appear after the turnaround clock.
8. Confirm GPIO18/A2 sees the same target-side level.
9. Confirm host and target never visibly drive opposing levels.
10. Confirm GPIO7 high pulls target reset low and GPIO7 low releases it.

If regular translated GPIO works but dedicated GPIO still fails with identical
electrical translator control, focus next on:

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
8. Zero-NOP fast mode is confirmed broken. Four-NOP fast mode is stable but
   produced 210.03 KB/s writes and 190.10 KB/s reads under instrumentation;
   its higher WAIT rate erased the lower physical-transfer cost.

## Working rules for the next Codex instance

- Do not invent a target MCU memory map, voltage, ACK value, or measured speed.
- Do not claim hardware success from a successful build.
- In software, logs, and documentation, call the SN74AXC2T245 devices
  "translators" or "level shifters"; do not use schematic reference designators.
- Preserve the direct-GPIO legacy backend while changing the translated Rev 6
  backend.
- Keep regular GPIO available for A/B diagnosis.
- Keep all dedicated-GPIO SWD calls on one explicitly pinned task/core.
- Keep the SWDIO translator isolated while changing DIR1 or output ownership.
- Keep SWCLK low during translator settling guards.
- Keep paced SWD clocks derived from `CONFIG_ESP_SWD_DEFAULT_CLOCK_HZ`. Treat
  `-1` as the fixed-NOP fast transfer sentinel, and keep the separate
  translator-settling delay limited to ownership transitions.
- Use `rg` for source searches and inspect the generated `sdkconfig` before
  diagnosing the selected backend.
- Use official Espressif source/docs for dedicated GPIO behavior and the TI
  SN74AXC2T245 data sheet for translator behavior.
- When the board is available, prefer raw ACK evidence and target-side waveforms
  over further speculative code changes.
