idf_component_register(
    SRCS "scheduler.c"
    INCLUDE_DIRS "include"
    WHOLE_ARCHIVE
    REQUIRES
    PRIV_REQUIRES kernel_trace
    	io_image
    	driver
	fieldbus
	automation
	control_kernel
	event_bus
	io_driver
	event_set
	io_command
	state
)
