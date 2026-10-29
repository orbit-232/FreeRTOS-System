idf_component_register(
    SRCS "panel_services.c"
    INCLUDE_DIRS "include"
    REQUIRES panel_bsp panel_model panel_network panel_online panel_clock panel_files panel_assets panel_wallpaper panel_audio panel_music nvs_flash esp_timer
    PRIV_REQUIRES esp_psram
)
