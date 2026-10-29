file(GLOB MUSIC_UI_ASSETS "assets/*.c")
file(GLOB MUSIC_UI_FONTS "fonts/*.c")

idf_component_register(
    SRCS "src/panel_music_ui.c" "src/panel_music_ui_diag.c" ${MUSIC_UI_ASSETS} ${MUSIC_UI_FONTS}
    INCLUDE_DIRS "include"
    REQUIRES panel_music panel_bsp lvgl__lvgl
)
