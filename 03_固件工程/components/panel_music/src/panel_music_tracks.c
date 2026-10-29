#include "panel_music_tracks.h"

#include <string.h>

static const panel_music_track_t s_tracks[PANEL_MUSIC_TRACK_COUNT] = {
    {"after_hours", "after_hours.mp3", "After Hours", "The Weeknd"},
    {"starboy", "starboy.mp3", "Starboy", "The Weeknd, Daft Punk"},
    {"i_feel_it_coming", "i_feel_it_coming.mp3", "I Feel It Coming",
     "The Weeknd, Daft Punk"},
    {"blinding_lights", "blinding_lights.mp3", "Blinding Lights", "The Weeknd"},
};

const panel_music_track_t *panel_music_track_at(unsigned index)
{
    return index < PANEL_MUSIC_TRACK_COUNT ? &s_tracks[index] : NULL;
}

int panel_music_track_find(const char *id)
{
    if (id == NULL) return -1;
    for (unsigned i = 0; i < PANEL_MUSIC_TRACK_COUNT; i++) {
        if (strcmp(id, s_tracks[i].id) == 0) return (int)i;
    }
    return -1;
}
