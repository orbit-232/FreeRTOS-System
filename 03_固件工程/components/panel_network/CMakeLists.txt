idf_component_register(
    SRCS "src/panel_network.c" "src/panel_ble.c" "src/panel_network_model.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_timer esp_wifi esp_netif esp_event lwip bt espressif__esp_hosted
)
