idf_component_register(
    SRCS "src/panel_clock.c" "src/panel_clock_platform.c"
    INCLUDE_DIRS "include"
    REQUIRES panel_online nvs_flash esp_timer
)
