idf_component_register(
    SRCS "determinism_probe.c"
    INCLUDE_DIRS "." "include"
    REQUIRES kernel_trace
	driver
	esp_timer
)
