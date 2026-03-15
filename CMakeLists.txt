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

    components/data/io_image
    components/data/state

    components/comm/parser
    components/comm/protocol

    components/hal/hal_layer
    components/hal/hal_led
    components/hal/rs485

    components/util/logger
    
    components/observability/determinism_probe
    components/observability/kernel_trace
    components/observability/kernel_metrics
    components/observability/phase_monitor
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ENDAP)
