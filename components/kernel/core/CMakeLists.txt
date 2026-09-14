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
        fieldbus
        state
        nvs_flash
        driver
        automation ladder
        failsafe
        cluster
        system_diag
        device_profile
        esp_event
        esp_netif
        network_ready
        wifi_manager
        ethernet_manager
        node_identity
        http_server
        io_driver
        io_command
        input_learning
        io_binding
        pve
        snapshot
        network_v1
    PRIV_REQUIRES esp_driver_uart app_update kernel_trace
)
