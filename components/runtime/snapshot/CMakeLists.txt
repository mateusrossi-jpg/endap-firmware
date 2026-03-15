idf_component_register(
    SRCS "snapshot.c"
    INCLUDE_DIRS "include"
    REQUIRES
    PRIV_REQUIRES
        state
        logger
)
