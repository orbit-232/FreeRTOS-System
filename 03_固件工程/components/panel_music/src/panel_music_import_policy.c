#include "panel_music_import_policy.h"

#include <ctype.h>
#include <string.h>

bool panel_music_import_token_valid(const char *token)
{
    if (token == NULL) return false;
    size_t n = strlen(token);
    if (n < PANEL_IMPORT_TOKEN_MIN || n > PANEL_IMPORT_TOKEN_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)token[i])) return false;
    }
    return true;
}

bool panel_music_import_size_valid(uint32_t len)
{
    return len > 0 && len <= PANEL_IMPORT_MAX_BYTES;
}

bool panel_music_import_name_valid(const char *name)
{
    if (name == NULL) return true;
    size_t n = strlen(name);
    if (n > PANEL_IMPORT_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '/' || c == '\\' || !isprint(c)) return false;
    }
    return true;
}
