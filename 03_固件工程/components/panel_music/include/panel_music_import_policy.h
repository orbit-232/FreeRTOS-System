#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PANEL_IMPORT_MAX_BYTES (32U * 1024U * 1024U)
#define PANEL_IMPORT_TOKEN_MIN 16U
#define PANEL_IMPORT_TOKEN_MAX 64U
#define PANEL_IMPORT_NAME_MAX 128U

bool panel_music_import_token_valid(const char *token);
bool panel_music_import_size_valid(uint32_t len);
bool panel_music_import_name_valid(const char *name);
