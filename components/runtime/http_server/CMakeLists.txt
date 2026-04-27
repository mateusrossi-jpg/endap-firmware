idf_component_register(
    SRCS "http_server.c"
    INCLUDE_DIRS "include"
    REQUIRES 
    esp_http_server
    state
    event_set
    io_command
    automation
    dashboard
    wifi_manager
    network_ready
    cluster
    device_profile
    failsafe
    fieldbus
    io_driver
    input_learning
    io_binding
    auth
    nvs_flash
    kernel_metrics
    phase_load_test
    phase_monitor
    rs485
    rs485_engine
    rs485_master
)
