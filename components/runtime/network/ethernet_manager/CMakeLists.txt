idf_component_register(
    SRCS "ethernet_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES
        device_profile
        network_ready
        driver
        esp_event
        esp_netif
        esp_eth
        esp_hw_support
)
