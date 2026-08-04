idf_component_register(
    SRCS "hal_led.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_gpio kernel_trace
)
