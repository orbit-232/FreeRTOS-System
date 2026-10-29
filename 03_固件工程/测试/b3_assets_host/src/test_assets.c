#include "panel_assets.h"
#include "panel_files.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "psa/crypto.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static const char *s_root = ".";
static bool s_card_ready = true;

bool panel_files_card_ready(void) { return s_card_ready; }
const char *panel_files_root_path(void) { return s_root; }
bool panel_files_io_acquire(void) { return true; }
void panel_files_io_release(void) { }

static bool wide_join(wchar_t *out, size_t cap, const wchar_t *a, const wchar_t *b)
{
    int n = _snwprintf(out, cap, L"%ls\\%ls", a, b);
    return n > 0 && (size_t)n < cap;
}

static bool make_dir_w(const wchar_t *path)
{
    return CreateDirectoryW(path, NULL) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool copy_file_w(const wchar_t *src, const wchar_t *dst)
{
    return CopyFileW(src, dst, FALSE) != 0;
}

static bool read_all_w(const wchar_t *path, char *buf, size_t cap, size_t *out_len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    bool ok = ReadFile(f, buf, (DWORD)cap, &got, NULL) && got < cap;
    CloseHandle(f);
    if (!ok) return false;
    buf[got] = '\0';
    *out_len = got;
    return true;
}

static bool write_all_w(const wchar_t *path, const char *buf, size_t len)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(f, buf, (DWORD)len, &wrote, NULL) && wrote == len;
    CloseHandle(f);
    return ok;
}

static bool read_bytes_w(const wchar_t *path, unsigned char **out, size_t *out_len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024) {
        CloseHandle(f);
        return false;
    }
    unsigned char *data = malloc((size_t)size.QuadPart);
    if (data == NULL) {
        CloseHandle(f);
        return false;
    }
    DWORD got = 0;
    bool ok = ReadFile(f, data, (DWORD)size.QuadPart, &got, NULL) && got == size.QuadPart;
    CloseHandle(f);
    if (!ok) {
        free(data);
        return false;
    }
    *out = data;
    *out_len = (size_t)size.QuadPart;
    return true;
}

static bool wait_state(panel_assets_state_t state, unsigned timeout_ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        panel_assets_status_t st;
        panel_assets_get_status(&st);
        if (st.state == state) return true;
        if (GetTickCount() - start > timeout_ms) {
            printf("TIMEOUT state=%d expected=%d detail=%s\n", st.state, state, st.detail);
            return false;
        }
        Sleep(5);
    }
}

bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t *font, lv_font_glyph_dsc_t *out,
                                   uint32_t letter, uint32_t next)
{
    (void)next;
    if (font == NULL || out == NULL || font->dsc == NULL) return false;
    const lv_font_fmt_txt_dsc_t *dsc = (const lv_font_fmt_txt_dsc_t *)font->dsc;
    const lv_font_fmt_txt_cmap_t *cmap = &dsc->cmaps[0];
    if (letter < cmap->range_start) return false;
    uint32_t rcp = letter - cmap->range_start;
    if (rcp >= cmap->range_length) return false;
    uint32_t gid = 0;
    if (cmap->type == LV_FONT_FMT_TXT_CMAP_SPARSE_FULL) {
        const uint16_t *unicode = cmap->unicode_list;
        const uint16_t *ofs = (const uint16_t *)cmap->glyph_id_ofs_list;
        for (uint16_t i = 0; i < cmap->list_length; i++) {
            if (unicode[i] == (uint16_t)letter) {
                gid = cmap->glyph_id_start + ofs[i];
                break;
            }
        }
    }
    if (gid == 0) return false;
    const lv_font_fmt_txt_glyph_dsc_t *g = &dsc->glyph_dsc[gid];
    out->gid = gid;
    out->adv_w = g->adv_w;
    out->box_w = g->box_w;
    out->box_h = g->box_h;
    out->ofs_x = g->ofs_x;
    out->ofs_y = g->ofs_y;
    return true;
}

const void *lv_font_get_bitmap_fmt_txt(lv_font_glyph_dsc_t *g_dsc, lv_draw_buf_t *draw_buf)
{
    (void)draw_buf;
    return g_dsc != NULL ? (const void *)(uintptr_t)g_dsc->gid : NULL;
}

static int prepare_variants(const wchar_t *package, const wchar_t *cwd)
{
    wchar_t work[1024];
    if (!wide_join(work, 1024, cwd, L"test\\b3_assets_host\\work")) return 1;
    make_dir_w(work);
    wchar_t missing[1024];
    if (!wide_join(missing, 1024, work, L"missing")) return 1;
    make_dir_w(missing);

    const wchar_t *cases[] = {L"corrupt", L"mismatch", L"deep", L"lowcount"};
    for (int c = 0; c < 4; c++) {
        wchar_t root[1024], panel[1024], assets[1024], fonts[1024];
        if (!wide_join(root, 1024, work, cases[c])) return 1;
        if (!wide_join(panel, 1024, root, L"panel")) return 1;
        if (!wide_join(assets, 1024, panel, L"assets")) return 1;
        if (!wide_join(fonts, 1024, assets, L"fonts")) return 1;
        make_dir_w(root);
        make_dir_w(panel);
        make_dir_w(assets);
        make_dir_w(fonts);

        wchar_t src_manifest[1024], dst_manifest[1024];
        wchar_t src_font[1024], dst_font[1024];
        if (!wide_join(src_manifest, 1024, package, L"panel\\assets\\manifest.json"))
            return 2;
        if (!wide_join(dst_manifest, 1024, assets, L"manifest.json")) return 2;
        if (!wide_join(src_font, 1024, package,
                       L"panel\\assets\\fonts\\file_cjk_20.pfnt")) return 2;
        if (!wide_join(dst_font, 1024, fonts, L"file_cjk_20.pfnt")) return 2;
        if (!copy_file_w(src_manifest, dst_manifest)) return 3;
        if (!copy_file_w(src_font, dst_font)) return 4;

        if (c == 0) {
            HANDLE f = CreateFileW(dst_font, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
            if (f == INVALID_HANDLE_VALUE) return 5;
            LARGE_INTEGER pos;
            pos.QuadPart = 100;
            SetFilePointerEx(f, pos, NULL, FILE_BEGIN);
            unsigned char bad = 0xFF;
            DWORD wrote = 0;
            WriteFile(f, &bad, 1, &wrote, NULL);
            FlushFileBuffers(f);
            CloseHandle(f);
        } else if (c == 1) {
            char manifest[32768];
            size_t len = 0;
            if (!read_all_w(dst_manifest, manifest, sizeof(manifest), &len)) return 6;
            char *hit = strstr(manifest, "1.2.0-apps-b3");
            if (hit == NULL) return 7;
            memcpy(hit, "9.9.9-apps-b3", 13);
            if (!write_all_w(dst_manifest, manifest, len)) return 8;
        } else {
            char manifest[32768];
            size_t len = 0;
            if (!read_all_w(dst_manifest, manifest, sizeof(manifest), &len)) return 9;
            if (c == 2) {
                char *brace = strchr(manifest, '{');
                if (brace == NULL) return 10;
                char field[128];
                int n = snprintf(field, sizeof(field), "\"deep\":");
                for (int i = 0; i < 20; i++) field[n++] = '[';
                for (int i = 0; i < 20; i++) field[n++] = ']';
                field[n++] = ',';
                size_t insert_at = (size_t)(brace - manifest) + 1;
                if (len + (size_t)n + 1 >= sizeof(manifest)) return 11;
                memmove(manifest + insert_at + n, manifest + insert_at, len - insert_at + 1);
                memcpy(manifest + insert_at, field, (size_t)n);
                len += (size_t)n;
                if (!write_all_w(dst_manifest, manifest, len)) return 12;
            } else {
                /* glyph_count=1: must be rejected before glyph_count-2 underflow. */
                unsigned char *font = NULL;
                size_t font_len = 0;
                if (!read_bytes_w(dst_font, &font, &font_len) || font_len < 20) return 13;
                font[16] = 1;
                font[17] = 0;
                font[18] = 0;
                font[19] = 0;
                if (!write_all_w(dst_font, (const char *)font, font_len)) {
                    free(font);
                    return 14;
                }
                unsigned char digest[32];
                size_t digest_len = 0;
                if (psa_hash_compute(PSA_ALG_SHA_256, font, font_len, digest, sizeof(digest),
                                     &digest_len) != PSA_SUCCESS || digest_len != 32) {
                    free(font);
                    return 15;
                }
                free(font);
                char hex[65];
                for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
                hex[64] = '\0';

                char *glyph_key = strstr(manifest, "\"glyphs\"");
                char *glyph_colon = glyph_key ? strchr(glyph_key, ':') : NULL;
                char *glyph_val = glyph_colon ? glyph_colon + 1 : NULL;
                while (glyph_val && *glyph_val == ' ') glyph_val++;
                if (glyph_val == NULL) return 16;
                size_t tail = len - (size_t)(glyph_val - manifest);
                memmove(glyph_val + 1, glyph_val + 4, tail - 4 + 1);
                glyph_val[0] = '1';
                len -= 3;

                char *sha_key = strstr(manifest, "\"sha256\"");
                char *sha_colon = sha_key ? strchr(sha_key, ':') : NULL;
                char *sha_val = sha_colon ? sha_colon + 1 : NULL;
                while (sha_val && *sha_val != '"') sha_val++;
                if (sha_val == NULL) return 17;
                sha_val++;
                memcpy(sha_val, hex, 64);
                if (!write_all_w(dst_manifest, manifest, len)) return 18;
            }
        }
    }
    return 0;
}

static bool find_package(const wchar_t *cwd, wchar_t *package, size_t cap)
{
    wchar_t pattern[1024];
    if (!wide_join(pattern, 1024, cwd, L"..\\05_*")) return false;
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileW(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        wchar_t candidate[1024];
        if (!_snwprintf(candidate, 1024,
                        L"%ls\\..\\%ls\\v1.2.0-apps-b3-tf-assets-pending",
                        cwd, data.cFileName)) continue;
        wchar_t manifest[1200];
        if (!_snwprintf(manifest, 1200, L"%ls\\panel\\assets\\manifest.json", candidate)) continue;
        if (GetFileAttributesW(manifest) == INVALID_FILE_ATTRIBUTES) continue;
        if (wcslen(candidate) >= cap) continue;
        wcscpy(package, candidate);
        found = true;
        break;
    } while (FindNextFileW(h, &data));
    FindClose(h);
    return found;
}

int main(void)
{
    wchar_t cwd[1024];
    if (GetCurrentDirectoryW(1024, cwd) == 0) return 1;
    wchar_t package[1024];
    if (!find_package(cwd, package, 1024)) {
        printf("FAIL cannot locate asset package\n");
        return 1;
    }
    int prep = prepare_variants(package, cwd);
    if (prep != 0) {
        printf("FAIL prepare variants step=%d\n", prep);
        return 1;
    }
    CHECK(SetCurrentDirectoryW(package) != 0);

    CHECK(panel_assets_init() == ESP_OK);
    s_root = ".";
    s_card_ready = true;
    CHECK(panel_assets_request_font() == ESP_OK);
    CHECK(wait_state(PANEL_ASSETS_STATE_READY, 10000));
    panel_assets_status_t status;
    panel_assets_get_status(&status);
    CHECK(status.pack_version == 1);
    CHECK(status.hash_checked);
    CHECK(status.font_file_bytes == 1487920);
    CHECK(status.font_ram_bytes > status.font_file_bytes);
    CHECK(strcmp(status.font_name, "file_cjk_20") == 0);
    unsigned font_file_bytes = (unsigned)status.font_file_bytes;
    unsigned font_ram_bytes = (unsigned)status.font_ram_bytes;

    const lv_font_t *font = panel_assets_acquire_font();
    CHECK(font != NULL);
    CHECK(font->line_height == 25);
    CHECK(font->base_line == 6);
    const lv_font_fmt_txt_dsc_t *dsc = (const lv_font_fmt_txt_dsc_t *)font->dsc;
    CHECK(dsc != NULL);
    CHECK(dsc->cmaps[0].range_length > 0);
    CHECK(dsc->cmaps[0].range_start <= 0x4E2D);
    CHECK(dsc->cmaps[0].list_length == 7539);
    unsigned unicode_count = dsc->cmaps[0].list_length;
    lv_font_glyph_dsc_t glyph;
    memset(&glyph, 0, sizeof(glyph));
    CHECK(lv_font_get_glyph_dsc_fmt_txt(font, &glyph, 0x4E2D, 0));
    CHECK(glyph.box_w > 0 && glyph.box_h > 0);
    CHECK(glyph.gid > 0);
    CHECK(glyph.adv_w > 0);
    CHECK(lv_font_get_glyph_dsc_fmt_txt(font, &glyph, 'A', 0));
    CHECK(glyph.box_w > 0);
    CHECK(lv_font_get_glyph_dsc_fmt_txt(font, &glyph, 0x6587, 0));
    CHECK(glyph.box_w > 0);
    CHECK(!lv_font_get_glyph_dsc_fmt_txt(font, &glyph, 0x1F600, 0));
    panel_assets_get_status(&status);
    CHECK(status.refs == 1);
    panel_assets_release_font();
    panel_assets_get_status(&status);
    CHECK(status.state == PANEL_ASSETS_STATE_IDLE);
    CHECK(status.font_ram_bytes == 0);
    CHECK(panel_assets_acquire_font() == NULL);

    CHECK(SetCurrentDirectoryW(cwd) != 0);

    s_root = "test/b3_assets_host/work/deep";
    CHECK(panel_assets_request_font() == ESP_OK);
    CHECK(wait_state(PANEL_ASSETS_STATE_INVALID, 10000));

    s_root = "test/b3_assets_host/work/corrupt";
    CHECK(panel_assets_request_font() == ESP_OK);
    CHECK(wait_state(PANEL_ASSETS_STATE_CORRUPT, 10000));
    panel_assets_get_status(&status);
    CHECK(!status.hash_checked);

    s_root = "test/b3_assets_host/work/lowcount";
    CHECK(panel_assets_request_font() == ESP_OK);
    CHECK(wait_state(PANEL_ASSETS_STATE_CORRUPT, 10000));

    s_root = "test/b3_assets_host/work/mismatch";
    CHECK(panel_assets_request_font() == ESP_OK);
    CHECK(wait_state(PANEL_ASSETS_STATE_INVALID, 10000));

    s_root = "test/b3_assets_host/work/missing";
    CHECK(panel_assets_request_font() == ESP_OK);
    CHECK(wait_state(PANEL_ASSETS_STATE_MISSING, 5000));

    s_card_ready = false;
    s_root = ".";
    CHECK(panel_assets_request_font() == ESP_OK);
    CHECK(wait_state(PANEL_ASSETS_STATE_NO_CARD, 5000));

    printf("B3_ASSETS failures=0 file=%u ram=%u glyphs=%u\n",
           font_file_bytes, font_ram_bytes, unicode_count);
    return 0;
}
