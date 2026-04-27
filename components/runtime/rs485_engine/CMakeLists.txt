idf_component_register(
    SRCS "rs485_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES
        rs485
        parser
        
    PRIV_REQUIRES
    	esp_timer
    	node_identity
)
