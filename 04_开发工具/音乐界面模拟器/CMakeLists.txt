cmake_minimum_required(VERSION 3.16)
project(music_ui_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(PROJECT_ROOT "C:/wsp/WavesharePanel")
set(FIRMWARE_DIR "${PROJECT_ROOT}/03_Firmware")
set(LVGL_DIR "${FIRMWARE_DIR}/managed_components/lvgl__lvgl")
set(MUSIC_UI_DIR "${FIRMWARE_DIR}/components/panel_music_ui")

file(GLOB_RECURSE LVGL_SOURCES "${LVGL_DIR}/src/*.c")
add_library(lvgl_host STATIC ${LVGL_SOURCES})
target_include_directories(lvgl_host PUBLIC
    "${LVGL_DIR}"
    "${LVGL_DIR}/src"
    "${PROJECT_ROOT}/04_Tools/simulator/include"
)
target_compile_definitions(lvgl_host PUBLIC LV_CONF_INCLUDE_SIMPLE)

file(GLOB MUSIC_ASSETS "${MUSIC_UI_DIR}/assets/*.c")
file(GLOB MUSIC_FONTS "${MUSIC_UI_DIR}/fonts/*.c")
add_library(music_app STATIC
    "${MUSIC_UI_DIR}/src/panel_music_ui.c"
    "${FIRMWARE_DIR}/components/panel_music/src/panel_music_tracks.c"
    src/fake_backend.c
    src/bsp_stub.c
    ${MUSIC_ASSETS}
    ${MUSIC_FONTS}
)
target_include_directories(music_app PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${MUSIC_UI_DIR}/include"
    "${FIRMWARE_DIR}/components/panel_music/include"
    "${FIRMWARE_DIR}/components/panel_bsp/include"
    "${PROJECT_ROOT}/04_Tools/simulator/include"
)
target_compile_options(music_app PRIVATE -w)
target_compile_definitions(music_app PUBLIC LV_CONF_INCLUDE_SIMPLE
    WAVESHARE_HOST_SIM PANEL_MUSIC_UI_HOST_TEST)
target_link_libraries(music_app PUBLIC lvgl_host m)

add_executable(render_music src/render_music.c)
target_link_libraries(render_music PRIVATE music_app lvgl_host m)
target_compile_options(render_music PRIVATE -w)

add_executable(test_music_ui src/test_music_ui.c)
target_link_libraries(test_music_ui PRIVATE music_app lvgl_host m)
target_compile_options(test_music_ui PRIVATE -w)
