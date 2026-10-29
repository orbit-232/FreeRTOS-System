idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES panel_bsp panel_model panel_services panel_ui nvs_flash esp_timer esp_app_format
)
