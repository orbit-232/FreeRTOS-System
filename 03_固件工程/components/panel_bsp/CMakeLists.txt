set(PANEL_BSP_SRCS
    "src/panel_bsp.c"
    "src/panel_boot_key.c"
    "src/ppa_guard.c"
    "src/ppa_guard_pixels.c"
)
if(PANEL_NETWORK_ALLOC_DIAGNOSTICS)
    list(APPEND PANEL_BSP_SRCS "src/panel_alloc_diag.c")
endif()

idf_component_register(
    SRCS ${PANEL_BSP_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl waveshare__esp32_p4_wifi6_touch_lcd_x espressif__esp_lvgl_adapter espressif__esp_lcd_touch esp_driver_ppa esp_driver_gpio esp_timer esp_mm
)

target_link_options(${COMPONENT_LIB} INTERFACE
    "LINKER:--undefined=__wrap_ppa_do_scale_rotate_mirror"
    "LINKER:--wrap=ppa_do_scale_rotate_mirror"
)

if(PANEL_NETWORK_ALLOC_DIAGNOSTICS)
    target_link_options(${COMPONENT_LIB} INTERFACE
        "LINKER:--undefined=__wrap_heap_caps_aligned_alloc"
        "LINKER:--wrap=heap_caps_aligned_alloc"
    )
endif()
