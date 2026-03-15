idf_component_register(

    SRCS "fieldbus.c"
    	 "bus_health_monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES
    PRIV_REQUIRES
        rs485_master
        rs485_engine
        parser
        latency_histogram
)
