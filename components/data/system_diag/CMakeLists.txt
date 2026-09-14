idf_component_register(
    SRCS "system_diag.c"
    INCLUDE_DIRS "include"
    REQUIRES
        nvs_flash
        esp_system
        log
)
