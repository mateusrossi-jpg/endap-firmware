idf_component_register(
    SRCS
        "hal_gpio.c"
        "hal_time.c"
        "hal_uart.c"
        "hal_init.c"

    INCLUDE_DIRS
        "include"

    PRIV_REQUIRES kernel_trace
        driver
        esp_timer
        esp_driver_gpio
        esp_driver_uart
)

