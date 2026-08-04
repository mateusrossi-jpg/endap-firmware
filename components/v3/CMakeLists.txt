idf_component_register(SRCS "v3_catalog.c" "v3_events.c" "v3_discovery.c" "v3_api.c" "v3_automation.c" "v3_persistence.c"
                    INCLUDE_DIRS "include"
                    REQUIRES nvs_flash freertos kernel_trace)
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--undefined=v3_discovery_report_node")

