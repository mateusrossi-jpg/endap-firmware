idf_component_register(
    SRCS
        "hal_gpio.c"
        "hal_time.c"
        "hal_uart.c"
        "hal_init.c"

    INCLUDE_DIRS
        "include"

    PRIV_REQUIRES
        driver
        esp_timer
)

