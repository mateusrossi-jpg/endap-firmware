idf_component_register(
    SRCS "rs485.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES kernel_trace
        driver
        esp_driver_gpio
        esp_driver_uart
)
