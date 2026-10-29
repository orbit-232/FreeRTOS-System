idf_component_register(
    SRCS "src/panel_audio.c" "src/panel_audio_tone.c" "src/panel_wav.c"
    INCLUDE_DIRS "include"
    REQUIRES waveshare__esp32_p4_wifi6_touch_lcd_x espressif__esp_codec_dev panel_files
)
