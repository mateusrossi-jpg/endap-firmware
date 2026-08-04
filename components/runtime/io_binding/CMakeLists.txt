idf_component_register(
    SRCS "io_binding.c" "io_mapping_persistence.c"
    INCLUDE_DIRS "include"
    REQUIRES kernel_trace
        nvs_flash
        device_profile
        json
        littlefs
)
