idf_component_register(
    SRCS "state.c"
    INCLUDE_DIRS "include"
    REQUIRES
        io_image
    PRIV_REQUIRES snapshot
)
