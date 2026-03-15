idf_component_register(
    SRCS "determinism_probe.c"
    INCLUDE_DIRS "." "include"
    REQUIRES
	driver
	esp_timer
)
