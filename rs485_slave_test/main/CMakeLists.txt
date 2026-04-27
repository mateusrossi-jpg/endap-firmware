idf_component_register(
    SRCS "rs485_slave_test.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        parser
        rs485
        esp_timer
)
