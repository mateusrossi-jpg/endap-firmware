idf_component_register(
    SRCS "src/dashboard.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "index.html.gz"
    REQUIRES esp_http_server kernel_trace
             state
             kernel_metrics
             io_command
)
