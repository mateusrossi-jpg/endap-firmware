idf_component_register(
    SRCS "failsafe.c"
    INCLUDE_DIRS "include"
    REQUIRES
        device_profile
        nvs_flash
        state
)
