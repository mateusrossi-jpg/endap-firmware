idf_component_register(
    SRCS "input_learning.c"
    INCLUDE_DIRS "include"
    REQUIRES
        device_profile
        event_bus
        io_binding
)
