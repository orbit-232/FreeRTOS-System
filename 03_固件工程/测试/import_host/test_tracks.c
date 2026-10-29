#include "panel_music_tracks.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    CHECK(PANEL_MUSIC_TRACK_COUNT == 4U);
    const char *ids[4];
    const char *files[4];
    for (unsigned i = 0; i < PANEL_MUSIC_TRACK_COUNT; i++) {
        const panel_music_track_t *t = panel_music_track_at(i);
        CHECK(t != NULL && t->id != NULL && t->file != NULL && t->title != NULL);
        CHECK(strchr(t->file, '/') == NULL && strchr(t->file, '\\') == NULL);
        CHECK(panel_music_track_find(t->id) == (int)i);
        for (unsigned j = 0; j < i; j++) {
            CHECK(strcmp(t->id, ids[j]) != 0);
            CHECK(strcmp(t->file, files[j]) != 0);
        }
        ids[i] = t->id;
        files[i] = t->file;
    }
    CHECK(panel_music_track_at(4) == NULL);
    CHECK(panel_music_track_find("nope") < 0);
    printf("TRACK_TABLE_TESTS_OK\n");
    return 0;
}
