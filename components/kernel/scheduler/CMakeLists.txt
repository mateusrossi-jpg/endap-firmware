idf_component_register(

    SRCS "scheduler.c"
    INCLUDE_DIRS "include"
    REQUIRES
    PRIV_REQUIRES io_image driver fieldbus control_kernel

)
