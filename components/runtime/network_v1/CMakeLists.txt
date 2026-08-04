idf_component_register(
    SRCS "endap_network_v1.c"
    INCLUDE_DIRS "include"
    WHOLE_ARCHIVE
    REQUIRES device_profile cluster nvs_flash
    PRIV_REQUIRES esp_timer esp_driver_gpio
)
