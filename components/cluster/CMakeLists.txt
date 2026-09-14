idf_component_register(
    SRCS
        "cluster_manager.c"
        "cluster_discovery.c"
        "cluster_events.c"
        "cluster_failover.c"
        "cluster_io.c"
        "node_registry.c"
        "cluster_self_test.c"
        "cluster_transport.c"
        "cluster_transport_now.c"
        "cluster_transport_mesh.c"

    INCLUDE_DIRS
        "."
        "include"

    REQUIRES
        core
        system_diag
        device_profile
        esp_timer
        failsafe
        network_ready
        nvs_flash
        node_identity
        onboarding
        protocol
        state
    PRIV_REQUIRES kernel_trace
        rs485_engine
        esp_wifi
)
