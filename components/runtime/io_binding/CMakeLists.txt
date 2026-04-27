idf_component_register(
    SRCS "io_binding.c"
    INCLUDE_DIRS "include"
    REQUIRES
        nvs_flash
        device_profile
)
