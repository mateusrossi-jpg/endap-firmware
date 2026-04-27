idf_component_register(
    SRCS "automation_engine.c"
    INCLUDE_DIRS "." "include"
    REQUIRES 
    	event_bus 
    	state
        cluster
        device_profile
        failsafe
        nvs_flash
        protocol
    PRIV_REQUIRES
        esp_timer
)
