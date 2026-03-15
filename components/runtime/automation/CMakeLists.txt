idf_component_register(
    SRCS "automation_engine.c"
    INCLUDE_DIRS "." "include"
    REQUIRES event_bus state
)
