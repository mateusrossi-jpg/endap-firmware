idf_component_register(
    SRCS "device_profile.c"
    INCLUDE_DIRS "include"
    REQUIRES driver state nvs_flash
)
