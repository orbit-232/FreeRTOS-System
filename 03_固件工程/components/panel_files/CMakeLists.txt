idf_component_register(
    SRCS "src/panel_files.c" "src/panel_files_platform_sd.c"
    INCLUDE_DIRS "include"
    REQUIRES panel_bsp esp_driver_sdmmc fatfs sdmmc
)
