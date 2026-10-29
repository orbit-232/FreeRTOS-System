file(GLOB UI_SOURCES "src/*.c" "fonts/*.c")

idf_component_register(
    SRCS ${UI_SOURCES}
    INCLUDE_DIRS "include"
    REQUIRES panel_model panel_services panel_online panel_clock panel_files panel_wallpaper panel_audio panel_music panel_bsp panel_network lvgl__lvgl
)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/validate_parallel_fonts.cmake")
panel_assert_parallel_fonts("${CMAKE_CURRENT_LIST_DIR}/fonts"
                            "${CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT}"
                            "${CONFIG_LV_DRAW_SW_SHADOW_CACHE_SIZE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_LIST_DIR}/fonts/ui_font_meta.h)
file(GLOB parallel_font_inputs "${CMAKE_CURRENT_LIST_DIR}/fonts/ui_font_*.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${parallel_font_inputs})
