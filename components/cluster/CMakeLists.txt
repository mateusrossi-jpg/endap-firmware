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

    INCLUDE_DIRS
        "."
        "include"

    REQUIRES
        device_profile
        esp_timer
        failsafe
        network_ready
        nvs_flash
        node_identity
        protocol
        state
    PRIV_REQUIRES
        rs485_engine
)
