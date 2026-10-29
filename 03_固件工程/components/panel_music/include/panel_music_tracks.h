#pragma once

#include <stdint.h>

#define PANEL_MUSIC_TRACK_COUNT 4U

typedef struct {
    const char *id;
    const char *file;
    const char *title;
    const char *artist;
} panel_music_track_t;

const panel_music_track_t *panel_music_track_at(unsigned index);
int panel_music_track_find(const char *id);
