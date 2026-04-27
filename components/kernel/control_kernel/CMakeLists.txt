idf_component_register(
    SRCS "control_kernel.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        runtime_monitor
        deterministic_snapshot
)
