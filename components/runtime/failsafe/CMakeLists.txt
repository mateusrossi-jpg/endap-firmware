idf_component_register(
    SRCS "failsafe.c"
    INCLUDE_DIRS "include"
    REQUIRES
        device_profile
        io_binding
        nvs_flash
        state
)
