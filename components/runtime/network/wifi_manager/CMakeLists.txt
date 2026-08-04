idf_component_register(
    SRCS "wifi_manager.c"
    INCLUDE_DIRS "." "include"
    REQUIRES 
        network_ready
        node_identity
        esp_wifi
        esp_event
        esp_netif
    PRIV_REQUIRES kernel_trace
        nvs_flash
        device_profile
        captive_dns
)
