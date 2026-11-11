# Soul Injector Rev 8 hardware test

Tests the Rev 8 adaptor's INA226 power profiler and SWD circuits, using
https://github.com/huming2207/swd-esp/tree/feature/direction-ctrl-swd

Wire PROG pin 11 (`VPP_INPUT`) to pin 7 (`+3V3`) or a 1.8 to 3.6 V bench supply,
and power the target from pin 9 (`VPP_OUTPUT`).

To build and run:

```
mkdir -p build-rev8
idf.py -B build-rev8 -D IDF_TARGET=esp32s31 -D SDKCONFIG="$PWD/build-rev8/sdkconfig" build
idf.py -B build-rev8 -D SDKCONFIG="$PWD/build-rev8/sdkconfig" flash monitor
```
