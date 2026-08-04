idf_component_register(
    SRCS "http_server.c"
    INCLUDE_DIRS "include"
    REQUIRES  kernel_trace
    esp_http_server
    state
    event_set
    io_command
    automation ladder
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
    pve
    auth
    onboarding
    nvs_flash
    kernel_metrics
    phase_load_test
    phase_monitor
    rs485
    rs485_engine
    rs485_master
    ethernet_manager
    network_v1
)
