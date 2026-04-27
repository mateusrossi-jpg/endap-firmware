idf_component_register(
    SRCS "wifi_manager.c"
    INCLUDE_DIRS "." "include"
    REQUIRES network_ready
    node_identity
    esp_wifi
    esp_event
    esp_netif
    nvs_flash
    captive_dns
)
