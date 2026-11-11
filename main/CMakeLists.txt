idf_component_register(SRCS "demo_main.c" "power_profiler.cpp" "target_power_test.c"
                       REQUIRES swd_esp esp_timer ina226 esp_driver_gpio esp_driver_i2c
                       INCLUDE_DIRS "")
