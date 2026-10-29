#include "panel_assets.h"
#include "panel_files.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "lvgl.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static char s_root[1024];
static wchar_t s_out_w[1024];

bool panel_files_card_ready(void) { return true; }
const char *panel_files_root_path(void) { return s_root; }
bool panel_files_io_acquire(void) { return true; }
void panel_files_io_release(void) { }

static bool wide_join(wchar_t *out, size_t cap, const wchar_t *a, const wchar_t *b)
{
    int n = _snwprintf(out, cap, L"%ls\\%ls", a, b);
    return n > 0 && (size_t)n < cap;
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

static bool wait_ready(unsigned timeout_ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        panel_assets_status_t st;
        panel_assets_get_status(&st);
        if (st.state == PANEL_ASSETS_STATE_READY) return true;
        if (st.state == PANEL_ASSETS_STATE_CORRUPT || st.state == PANEL_ASSETS_STATE_INVALID ||
            st.state == PANEL_ASSETS_STATE_MISSING) {
            printf("ASSET_LOAD_FAILED state=%d detail=%s\n", st.state, st.detail);
            return false;
        }
        if (GetTickCount() - start > timeout_ms) {
            printf("ASSET_TIMEOUT state=%d detail=%s\n", st.state, st.detail);
            return false;
        }
        Sleep(5);
    }
}

#define WIDTH 360
#define HEIGHT 140
static uint16_t s_fb[WIDTH * HEIGHT];
static uint8_t s_draw[WIDTH * HEIGHT * 2];

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t w = lv_area_get_width(area);
    int32_t h = lv_area_get_height(area);
    const uint16_t *src = (const uint16_t *)px_map;
    for (int32_t y = 0; y < h; y++) {
        int32_t dy = area->y1 + y;
        if (dy < 0 || dy >= HEIGHT) continue;
        for (int32_t x = 0; x < w; x++) {
            int32_t dx = area->x1 + x;
            if (dx >= 0 && dx < WIDTH) s_fb[dy * WIDTH + dx] = src[y * w + x];
        }
    }
    lv_display_flush_ready(disp);
}

static uint32_t expected_gid(const lv_font_fmt_txt_dsc_t *dsc, uint32_t code)
{
    const lv_font_fmt_txt_cmap_t *cmap = &dsc->cmaps[0];
    const uint16_t *ofs = (const uint16_t *)cmap->glyph_id_ofs_list;
    for (uint16_t i = 0; i < cmap->list_length; i++) {
        if (cmap->unicode_list[i] == code) return cmap->glyph_id_start + ofs[i];
    }
    return 0;
}

static int render_font(const lv_font_t *font)
{
    lv_init();
    lv_tick_inc(16);
    lv_display_t *disp = lv_display_create(WIDTH, HEIGHT);
    CHECK(disp != NULL);
    lv_display_set_default(disp);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_draw, NULL, sizeof(s_draw), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "\xE4\xB8\xAD\xE6\x96\x87" "A");
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_pos(label, 12, 20);
    lv_obj_update_layout(screen);
    lv_obj_invalidate(screen);
    lv_refr_now(disp);

    unsigned lit = 0;
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        if (s_fb[i] != 0) lit++;
    }
    CHECK(lit > 200);

    FILE *f = _wfopen(s_out_w, L"wb");
    CHECK(f != NULL);
    fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        uint16_t c = s_fb[i];
        unsigned char rgb[3] = {
            (unsigned char)(((c >> 11) & 0x1F) * 255 / 31),
            (unsigned char)(((c >> 5) & 0x3F) * 255 / 63),
            (unsigned char)((c & 0x1F) * 255 / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("RENDER lit=%u\n", lit);
    return 0;
}

int main(void)
{
    wchar_t cwd[1024];
    CHECK(GetCurrentDirectoryW(1024, cwd) != 0);
    CHECK(wide_join(s_out_w, 1024, cwd, L"test\\b3_assets_host\\real_lvgl_pfnt.ppm"));
    wchar_t package[1024];
    CHECK(find_package(cwd, package, 1024));
    CHECK(SetCurrentDirectoryW(package) != 0);
    snprintf(s_root, sizeof(s_root), "%s", ".");
    CHECK(sizeof(lv_font_fmt_txt_glyph_dsc_t) == 16);
    CHECK(panel_assets_init() == ESP_OK);
    CHECK(panel_assets_request_font() == ESP_OK);
    CHECK(wait_ready(10000));
    const lv_font_t *font = panel_assets_acquire_font();
    CHECK(font != NULL);

    lv_font_glyph_dsc_t g;
    memset(&g, 0, sizeof(g));
    const lv_font_fmt_txt_dsc_t *dbg = (const lv_font_fmt_txt_dsc_t *)font->dsc;
    printf("CMAP start=%u len=%u list=%u\n", (unsigned)dbg->cmaps[0].range_start,
           (unsigned)dbg->cmaps[0].range_length, (unsigned)dbg->cmaps[0].list_length);
    uint32_t gid_zhong = expected_gid(dbg, 0x4E2D);
    uint32_t gid_wen = expected_gid(dbg, 0x6587);
    uint32_t gid_a = expected_gid(dbg, 'A');
    CHECK(gid_zhong > 0 && gid_wen > 0 && gid_a > 0);
    CHECK(lv_font_get_glyph_dsc_fmt_txt(font, &g, 0x4E2D, 0));
    CHECK(g.gid.index == gid_zhong);
    CHECK(g.box_w > 0 && g.box_h > 0);
    CHECK(lv_font_get_glyph_dsc_fmt_txt(font, &g, 0x6587, 0));
    CHECK(g.gid.index == gid_wen);
    CHECK(g.box_w > 0);
    CHECK(lv_font_get_glyph_dsc_fmt_txt(font, &g, 'A', 0));
    CHECK(g.gid.index == gid_a);
    CHECK(g.box_w > 0);
    CHECK(!lv_font_get_glyph_dsc_fmt_txt(font, &g, 0x1F600, 0));
    CHECK(render_font(font) == 0);
    panel_assets_release_font();
    printf("B3_ASSETS_REAL_LVGL failures=0 large=1\n");
    return 0;
}
