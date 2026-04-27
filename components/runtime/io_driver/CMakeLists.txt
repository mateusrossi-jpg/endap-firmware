idf_component_register(
    SRCS "io_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES 
    	driver
        state
        device_profile
        failsafe
        io_image
        cluster
    PRIV_REQUIRES
        http_server
)
