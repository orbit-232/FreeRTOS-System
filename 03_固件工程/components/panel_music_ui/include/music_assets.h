#pragma once

#include "lvgl.h"

/* 预烘焙音乐播放器素材；全部数据为 const，驻留 Flash。 */
extern const lv_image_dsc_t music_bg;
extern const lv_image_dsc_t music_vinyl;
extern const lv_image_dsc_t music_cover_0;
extern const lv_image_dsc_t music_card_0;
extern const lv_image_dsc_t music_cover_1;
extern const lv_image_dsc_t music_card_1;
extern const lv_image_dsc_t music_cover_2;
extern const lv_image_dsc_t music_card_2;
extern const lv_image_dsc_t music_cover_3;
extern const lv_image_dsc_t music_card_3;

typedef enum {
    MUSIC_ICON_CHEVRON_DOWN,
    MUSIC_ICON_MORE,
    MUSIC_ICON_PLAY,
    MUSIC_ICON_PAUSE,
    MUSIC_ICON_PREV,
    MUSIC_ICON_NEXT,
    MUSIC_ICON_SHUFFLE,
    MUSIC_ICON_REPEAT,
    MUSIC_ICON_HEART,
    MUSIC_ICON_HEART_FILLED,
    MUSIC_ICON_LIST,
    MUSIC_ICON_VOLUME,
    MUSIC_ICON_VOLUME_MUTE,
    MUSIC_ICON_QUEUE,
    MUSIC_ICON_DOWNLOAD,
    MUSIC_ICON_COUNT,
} music_icon_t;

const lv_image_dsc_t *music_asset_icon(music_icon_t icon, bool large);
