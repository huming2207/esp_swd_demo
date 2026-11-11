# Soul Injector Rev 8 hardware test handoff

Read this before changing the demo. It records what the demo does, what has
been measured on hardware, and what is still open. Snapshot date: 2026-11-11.

Check local source, the KiCad design, ESP-IDF source or vendor documentation
before making a hardware or ESP32-specific claim. Label anything not measured
on the board as unverified. A successful build is not hardware success.

## Repositories

- Demo: this repository, branch `feature/si-rev8`. Rev 8 is the only supported
  board. The older Rev 6 handoff is in git history under `AGENTS.md`.
- Firmware: `soul-injector`, branch `next`. Its Rev 8 notes are in
  `docs/en/rev8-hardware.md`.
- Hardware: `soulinjector-hw`. The adaptor board is `SoulInjector/swd_adaptor`,
  and its power profiler sheet (`power_profiler.kicad_sch`) has the design notes.
- STM32WL registers come from RM0461 and ST's `stm32wle5xx.h`.

## Build and run

```sh
mkdir -p build-rev8
idf.py -B build-rev8 -D IDF_TARGET=esp32s31 -D SDKCONFIG="$PWD/build-rev8/sdkconfig" build
idf.py -B build-rev8 -D SDKCONFIG="$PWD/build-rev8/sdkconfig" flash monitor
```

Uses ESP-IDF v6.1. Changing `sdkconfig.defaults` does not update an existing
`build-rev8/sdkconfig`, so delete that file first. Inspect the generated config
before diagnosing which SWD backend or clock was built.

Wire PROG pin 11 (`VPP_INPUT`) to pin 7 (`+3V3`) or a 1.8 to 3.6 V supply, and
power the target from pin 9 (`VPP_OUTPUT`).

## Files

- `main/demo_main.c`: test sequence and the original SWD RAM stress test.
- `main/power_profiler.cpp`: range latches, auto-range state machine, profiler
  checks, INA226 bias test and the background sampler.
- `main/target_power_test.c`: STM32WLE5 current test.
- `components/ina226/`: copy of the firmware component with two changes: the
  `gpio_config_t` initializer fix needed under default ESP-IDF 6.1 warnings, and
  `set_operate_mode()`.

## Test sequence

1. Profiler test with debug lines isolated.
2. SWD RAM stress test: 100 iterations of 8 KB at `0x20000000`.
3. STM32WLE5 current test, only if the SWD session is good. It refuses any
   target whose `DBGMCU_IDCODE` device ID is not `0x497`.
4. Profiler test again with debug lines isolated.

The profiler test runs the sensor and `/ALERT` checks, the range ladder and the
auto-range state machine for 5 seconds. It then runs the bias test, which needs
pin 9 open and is skipped if LOW range does not hold.

The profiler results table is cumulative. NOT EXERCISED means no evidence either
way, not a pass.

The current test resets the target repeatedly and restarts it at the end. It
turns off the RTC and LSE, which stay off until firmware re-enables them. It does
not perform a backup-domain reset, so backup registers are kept.

## Power profiler facts

The shunts are in series from `VPP_INPUT` to `VPP_OUTPUT`:

| Range | Bypass state | Shunt | INA226 | LSB |
| --- | --- | --- | --- | --- |
| HIGH | HIGH latch set | 50 mohm | U6, 0x40 | 50 uA |
| MID | MID latch set only | 4.02 ohm | U7, 0x41 | 622 nA |
| LOW | Neither set | 162 ohm | U8, 0x44 | 15.4 nA |

- The latches are a 2-bit shift register: GPIO19 (D) to HIGH to MID, with a shared
  rising-edge clock on GPIO37. Readback is GPIO38 (HIGH) and GPIO5 (MID).
- Step down one range with D low and one clock. Force both bypasses on with D
  high and two clocks.
- Up-ranging is done by comparators. Down-ranging is done by firmware, using a
  limit derived from the measured bus voltage.
- After a range change, wait for settling and discard the conversion that spans
  the switch.
- GPIO57 is `DBG_EN`, active high and pulled low. Without it, SWD, reset, BOOT
  and UART do not reach the target connector.
- I2C is GPIO17/18. `/ALERT` is shared on GPIO4.
- Keep host-side SWDIO translator inputs from floating while isolated. The demo
  enables pull-downs on them.

## Measured results

Target: STM32WLE5, `DBGMCU_IDCODE` `0x10016497`, DP IDCODE `0x6ba02477`, about
1.4 mA when running its own firmware.

SWD uses the direct SPI2 backend, 100 of 100 iterations verified in each case:

| Clock | Guard | Write | Read |
| --- | --- | --- | --- |
| 500 kHz | 1 us | 32.8 KB/s | 33.0 KB/s |
| 13 MHz requested, 13.33 MHz actual | 10 ns | 251 KB/s | 262 KB/s |

Keep 13 MHz for now. Faster clocks are untested on Rev 8. The driver changes RX
alignment at 16 MHz and above.

Target current at 3.31 V:

| Phase | Average | Evidence |
| --- | --- | --- |
| Halted, SWD idle | 610 uA | |
| Halted, SWD writing | 694 uA | 263 KB/s while sampling |
| Halted, SWD reading | 767 uA | 262 KB/s while sampling |
| Stop 2, debug on | 556 uA | `C1STOP2F=1`, `S_SLEEP=1`, SWD stayed connected |
| Standby, debug and RTC off | 6.73 uA | `C1SBF=1`, read before firmware ran |

The SWD phases run at the reset clock (MSI, 4 MHz). Stop 2 with `DBG_STOP` keeps
active clocks running, so it is not a true Stop 2 figure. The Standby figure is
almost entirely the INA226 bias error below.

## INA226 input bias

With pin 9 open, each INA226 shunt input pin draws about 6.6 uA, against 0.1 nA
in datasheet SBOS547C. All three parts report die ID `0x2260`, the value that
datasheet lists, so the die ID does not identify low-bias parts.

- R23 (LOW) carries one pin and reads about 6.6 uA.
- R22 (MID) carries three pins and reads about 19.6 uA.
- R21 (HIGH) should carry five, about 33 uA, below its resolution. Not measured.

Bias test, first pass: powering down U8 took U7 from 19.62 to 6.43 uA, a drop of
13.20 uA against 13.33 uA predicted. U7 returned to 19.65 uA. Powering down U7
left U8 at 6.54 uA, compared with 6.66 uA.

So the STM32's real Standby current is still unmeasured. `MID and LOW sensors
agree: FAIL` appears with pin 9 open because those shunts carry different bias
currents. It is a limitation of the check, not a hardware fault.

## Open items

- Zero calibration is deliberately not implemented. The plan was a per-range
  open-output zero scaled with `Vin`, stored per board. Repeat U8 readings ranged
  from 6.49 to 6.93 uA, so expect a limit of a few hundred nA with these parts.
- The HIGH comparator has not been tested with a real load above about 13 mA. A
  150 ohm resistor from pin 9 to GND draws about 22 mA at 3.3 V.
- With the target attached, the step to LOW ended with both latches set in 4 of 6
  ladder runs at about 1.4 mA, when only MID should trip. One unconfirmed
  explanation is that C17 pulls `PROF_B` to the HIGH comparator threshold when
  Q5 turns on. Scope `PROF_B` and comparator output A to check.
- LOW range noise with pin 9 open is about +/-3 uA before SWD init and about
  +/-11 uA after. It is not from the target. Rail ripple on `VPP_INPUT` is a guess
  and has not been tested. A bench supply on pin 11 would test it.

## `ina226` component issues

- `init()` logs an `gpio_install_isr_service` error for the second and third
  sensor. It is harmless.
- `gpio_isr_handler_add()` keeps one handler per pin, so only the last sensor gets
  alert events on the shared pin. The tests poll the pin instead.
- `set_shunt_coefficient()` is declared but not defined, and there is no deinit.

## Working rules

- Avoid C++ STL unless it is constexpr or has very small performance and heap
  impact. Allocate heap only at init or deinit.
- In software, logs and documentation, call the SN74AVC2T245 devices translators
  or level shifters.
- Keep all dedicated-GPIO SWD calls on one pinned task and core. The sampler runs
  on the other core and owns the I2C interrupt.
- Keep the SWDIO translator isolated while changing direction or output ownership,
  and keep SWCLK low during translator settling guards.
- Target reset is a MOSFET, so a high GPIO asserts reset.
- Prefer raw measurements and waveforms over speculative changes.
