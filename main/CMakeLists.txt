idf_component_register(SRCS "demo_main.c" "power_profiler.cpp"
                       REQUIRES swd_esp esp_timer ina226 esp_driver_gpio esp_driver_i2c
                       INCLUDE_DIRS "")
