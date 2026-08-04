idf_component_register(
    SRCS "input_learning.c"
    INCLUDE_DIRS "include"
    REQUIRES kernel_trace
        device_profile
        event_bus
        io_binding
)
