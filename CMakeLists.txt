cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    components/kernel/core
    components/kernel/control_loop
    components/kernel/control_kernel
    components/kernel/scheduler
    components/kernel/watchdog

    components/runtime/deterministic_snapshot
    components/runtime/fieldbus
    components/runtime/rs485_engine
    components/runtime/rs485_master
    components/runtime/runtime_monitor
    components/runtime/snapshot
    components/runtime/latency_histogram
    components/runtime/event_bus
    components/runtime/automation
    components/runtime/failsafe
    components/runtime/network/network_ready
    components/runtime/network/wifi_manager
    components/runtime/network/ethernet_manager
    components/runtime/network/runtime_network
    components/runtime/network/captive_dns
    components/runtime/http_server
    components/runtime/input_learning
    components/runtime/io_binding
    components/runtime/io_driver
    components/runtime/event_set
    components/runtime/io_command
     
    components/data/io_image
    components/data/state

    components/comm/parser
    components/comm/protocol

    components/hal/hal_layer
    components/hal/hal_led
    components/hal/rs485

    components/util/logger
    
    components/cluster
    components/cluster/node_identity
    
    components/observability/determinism_probe
    components/observability/kernel_trace
    components/observability/kernel_metrics
    components/observability/phase_load_test
    components/observability/phase_monitor
    
    components/dashboard
    components/security/auth
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ENDAP)
