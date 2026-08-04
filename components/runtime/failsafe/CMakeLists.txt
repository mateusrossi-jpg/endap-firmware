idf_component_register(
    SRCS "failsafe.c"
    INCLUDE_DIRS "include"
    REQUIRES kernel_trace
        device_profile
        nvs_flash
        state
)
