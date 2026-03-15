 idf_component_register(

    SRCS "control_kernel.c"
    INCLUDE_DIRS "include"
    REQUIRES
    PRIV_REQUIRES
    	runtime_monitor
    	snapshot
    	io_image
	deterministic_snapshot
	watchdog
	hal
)
