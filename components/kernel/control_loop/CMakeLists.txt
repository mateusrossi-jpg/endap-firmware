idf_component_register(
    SRCS "control_loop.c"
    INCLUDE_DIRS "include"
    REQUIRES 
    	scheduler
    	watchdog
    	determinism_probe
    	kernel_trace
    	kernel_metrics
    	phase_monitor
        phase_load_test
)
