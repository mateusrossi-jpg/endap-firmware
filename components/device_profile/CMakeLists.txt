idf_component_register(
    SRCS "device_profile.c" "device_profile_sensors.c" "device_profile_templates.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_gpio state nvs_flash
    PRIV_REQUIRES kernel_trace
)
