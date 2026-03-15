idf_component_register(

    SRCS "rs485_master.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES protocol rs485_engine

)
