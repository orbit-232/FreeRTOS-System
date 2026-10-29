#include "panel_music_import_policy.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    CHECK(panel_music_import_token_valid("0123456789abcdef"));
    CHECK(panel_music_import_token_valid("ABCDEFGHIJKLMNOPQRSTUVWXYZ012345"));
    CHECK(!panel_music_import_token_valid("short"));
    CHECK(!panel_music_import_token_valid("0123456789abcde-"));
    CHECK(!panel_music_import_token_valid("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdefg"));
    CHECK(!panel_music_import_token_valid(NULL));

    CHECK(!panel_music_import_size_valid(0));
    CHECK(panel_music_import_size_valid(1));
    CHECK(panel_music_import_size_valid(32U * 1024U * 1024U));
    CHECK(!panel_music_import_size_valid(32U * 1024U * 1024U + 1U));

    CHECK(panel_music_import_name_valid(""));
    CHECK(panel_music_import_name_valid("The Weeknd - After Hours.mp3"));
    CHECK(!panel_music_import_name_valid("../x"));
    CHECK(!panel_music_import_name_valid("a/b"));
    CHECK(!panel_music_import_name_valid("a\\b"));
    CHECK(!panel_music_import_name_valid("bad\nname"));
    char long_name[PANEL_IMPORT_NAME_MAX + 2];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    CHECK(!panel_music_import_name_valid(long_name));
    printf("IMPORT_POLICY_TESTS_OK\n");
    return 0;
}
