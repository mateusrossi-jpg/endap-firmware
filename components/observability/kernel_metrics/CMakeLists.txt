idf_component_register(
    PRIV_REQUIRES kernel_trace
    SRCS
        "kernel_metrics.c"
        "kernel_phase_metrics.c"
    INCLUDE_DIRS "." "include"
)
