# SWD Host Demo on ESP32

This is the minimal demo and benchmark assessment for https://github.com/huming2207/swd-esp/tree/feature/direction-ctrl-swd

To build, run：

```
idf.py -B build-parlio \
  -D IDF_TARGET=esp32s31 \
  -D SDKCONFIG=sdkconfig.parlio \
  -D SDKCONFIG_DEFAULTS=sdkconfig.defaults \
  build
```
```
idf.py -B build-spi \
  -D IDF_TARGET=esp32s31 \
  -D SDKCONFIG=sdkconfig.spi \
  -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.spi \
  build
```

Then flash with either:

- `idf.py -B build-parlio flash monitor`
- `idf.py -B build-spi flash monitor`
