idf_component_register(
    SRCS "rs485_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES 
    PRIV_REQUIRES esp_timer
)
