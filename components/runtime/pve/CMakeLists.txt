idf_component_register(
    SRCS "pve.c"
         "pve_self_test.c"
         "telemetry.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash event_bus kernel_trace driver esp_driver_gpio state sensor device_profile
)
