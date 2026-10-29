# music worker: 12 KiB internal stack. Target -fstack-usage:
# mp3dec_decode_frame_with_scratch=1136 B, worker=1504 B; the old stack-scratch
# entry point was 16256 B (scratch 16236 B). The player now allocates that
# scratch once in PSRAM and reuses it for every frame. Recheck target highwater.
idf_component_register(
    SRCS "src/panel_music.c" "src/panel_music_tracks.c" "src/panel_music_import.c" "src/panel_music_import_policy.c" "src/panel_music_resample.c"
    INCLUDE_DIRS "include"
    REQUIRES panel_files panel_audio panel_network minimp3 esp_http_server
)
target_compile_options(${COMPONENT_LIB} PRIVATE "-fstack-usage")
