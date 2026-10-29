cmake_minimum_required(VERSION 3.16)
project(waveshare_font_stress C)

# Equivalent invocation outside the harness pipe sandbox (the explicit
# CMAKE_MAKE_PROGRAM is required because an old STM32CubeCLT ninja is on PATH):
#   cmake -S C:/wsp/WavesharePanel/03_Firmware/test/font_stress \
#         -B C:/wsp/WavesharePanel/03_Firmware/test/font_stress/build -G Ninja \
#         -DCMAKE_C_COMPILER=D:/BaiduNetdiskDownload/MinGW/bin/gcc.exe \
#         -DCMAKE_MAKE_PROGRAM=C:/Espressif/tools/ninja/1.12.1/ninja.exe
# The checked-in runner uses direct GCC because CMake's Ninja try_compile
# deadlocks under the current sandbox's named-pipe restriction.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Use the ASCII junction so MinGW/CMake do not see the Chinese workspace path.
set(PROJECT_ROOT "C:/wsp/WavesharePanel")
set(FIRMWARE_DIR "${PROJECT_ROOT}/03_Firmware")
set(LVGL_DIR "${FIRMWARE_DIR}/managed_components/lvgl__lvgl")
set(SIM_CONF "${PROJECT_ROOT}/04_Tools/simulator/include/lv_conf.h")

# The firmware uses thread-safe C-library allocation (CONFIG_LV_USE_CLIB_MALLOC).
# The Windows simulator uses LVGL's built-in pool, which is not thread-safe; using
# it here would confound an intentional multi-threaded font decoder test.  Copy
# the simulator config and switch only the allocator to CLIB.
file(READ "${SIM_CONF}" LV_CONF_TEXT)
string(REPLACE
    "#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN"
    "#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB"
    LV_CONF_TEXT "${LV_CONF_TEXT}")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/include")
file(WRITE "${CMAKE_BINARY_DIR}/include/lv_conf.h" "${LV_CONF_TEXT}")

file(GLOB_RECURSE LVGL_SOURCES "${LVGL_DIR}/src/*.c")

add_executable(font_stress
    test_font_stress.c
    ${LVGL_SOURCES}
    "${FIRMWARE_DIR}/components/panel_ui/fonts/ui_font_16.c"
    "${FIRMWARE_DIR}/components/panel_ui/fonts/ui_font_20.c"
    "${FIRMWARE_DIR}/components/panel_ui/fonts/ui_font_24.c"
    "${FIRMWARE_DIR}/components/panel_ui/fonts/ui_font_32.c"
    "${FIRMWARE_DIR}/components/panel_ui/fonts/ui_font_num_128.c"
)

target_include_directories(font_stress PRIVATE
    "${CMAKE_BINARY_DIR}/include"
    "${LVGL_DIR}"
    "${LVGL_DIR}/src"
    "${FIRMWARE_DIR}/components/panel_ui/include"
)
target_compile_definitions(font_stress PRIVATE LV_CONF_INCLUDE_SIMPLE)
target_compile_options(font_stress PRIVATE -w)
