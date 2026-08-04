idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES core
             control_loop
             scheduler
             http_server
             onboarding
             network_v1
             cluster
)
