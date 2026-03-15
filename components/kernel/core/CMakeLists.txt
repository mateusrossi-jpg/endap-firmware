idf_component_register(
    SRCS "core_boot.c"
    INCLUDE_DIRS "include"
    REQUIRES
        rs485
        rs485_master
        rs485_engine
        control_loop
        control_kernel
        scheduler
        watchdog
        runtime_monitor
        fieldbus
        state
        nvs_flash
        driver
        automation
)
