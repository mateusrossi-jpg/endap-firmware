idf_component_register(
    SRCS "src/dashboard.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "index.html"
    REQUIRES esp_http_server
             state
             kernel_metrics
             io_command
)
