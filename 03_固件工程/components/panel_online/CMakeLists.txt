idf_component_register(
    SRCS "src/panel_online.c" "src/panel_online_model.c" "src/panel_online_http.c"
    INCLUDE_DIRS "include"
    REQUIRES panel_network esp_http_client mbedtls lwip nvs_flash esp_timer
    PRIV_REQUIRES espressif__cjson
)
