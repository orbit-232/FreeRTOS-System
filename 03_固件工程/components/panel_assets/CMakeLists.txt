idf_component_register(
    SRCS "src/panel_assets.c"
    INCLUDE_DIRS "include"
    REQUIRES panel_files lvgl__lvgl espressif__cjson mbedtls
)
