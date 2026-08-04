idf_component_register(
    SRCS "snapshot.c"
    INCLUDE_DIRS "include"
    REQUIRES
        nvs_flash
    PRIV_REQUIRES kernel_trace
        state
        logger
        event_bus
)
