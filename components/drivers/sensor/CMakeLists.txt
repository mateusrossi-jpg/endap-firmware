idf_component_register(
    SRCS "aht10.c" "dht11.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_gpio
)
