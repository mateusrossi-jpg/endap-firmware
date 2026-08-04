idf_component_register(
    SRCS "endap_onboarding.c"
    INCLUDE_DIRS "include"
    REQUIRES device_profile nvs_flash
    PRIV_REQUIRES esp_timer kernel_trace
)
