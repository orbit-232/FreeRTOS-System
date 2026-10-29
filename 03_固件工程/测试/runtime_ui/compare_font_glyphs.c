/*
 * Compare every glyph decoded through the installed LVGL fmt_txt path:
 * frozen compressed baseline symbols vs the raw candidate symbols.
 *
 * The compressed font C files are compiled with -Dui_font_16=ui_font_16_ref
 * etc. so both sets can coexist in one process.  No cmap-only shortcut is used.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"

LV_FONT_DECLARE(ui_font_16);
LV_FONT_DECLARE(ui_font_20);
LV_FONT_DECLARE(ui_font_24);
LV_FONT_DECLARE(ui_font_32);
LV_FONT_DECLARE(ui_font_num_48);
LV_FONT_DECLARE(ui_font_num_80);
LV_FONT_DECLARE(ui_font_num_128);

LV_FONT_DECLARE(ui_font_16_ref);
LV_FONT_DECLARE(ui_font_20_ref);
LV_FONT_DECLARE(ui_font_24_ref);
LV_FONT_DECLARE(ui_font_32_ref);
LV_FONT_DECLARE(ui_font_num_48_ref);
LV_FONT_DECLARE(ui_font_num_80_ref);
LV_FONT_DECLARE(ui_font_num_128_ref);

typedef struct {
    const char *name;
    const lv_font_t *raw;
    const lv_font_t *compressed;
} font_pair_t;

static unsigned mismatches;
static unsigned compared_glyphs;

static bool dsc_equal(const lv_font_glyph_dsc_t *a, const lv_font_glyph_dsc_t *b)
{
    return a->adv_w == b->adv_w && a->box_w == b->box_w && a->box_h == b->box_h &&
           a->ofs_x == b->ofs_x && a->ofs_y == b->ofs_y && a->stride == b->stride &&
           a->format == b->format && a->is_placeholder == b->is_placeholder;
}

static void report(uint32_t cp, const char *name, const char *what)
{
    if (mismatches < 20) {
        printf("MISMATCH font=%s cp=U+%04X %s\n", name, (unsigned)cp, what);
    }
    mismatches++;
}

static void compare_bitmap(uint32_t cp, const font_pair_t *pair,
                           lv_font_glyph_dsc_t *raw_dsc,
                           lv_font_glyph_dsc_t *cmp_dsc)
{
    uint32_t w = raw_dsc->box_w;
    uint32_t h = raw_dsc->box_h;
    if (w == 0 || h == 0) {
        return;
    }

    lv_draw_buf_t *raw_buf = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_A8, 0);
    lv_draw_buf_t *cmp_buf = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_A8, 0);
    if (raw_buf == NULL || cmp_buf == NULL) {
        report(cp, pair->name, "draw buffer allocation failed");
        if (raw_buf) {
            lv_draw_buf_destroy(raw_buf);
        }
        if (cmp_buf) {
            lv_draw_buf_destroy(cmp_buf);
        }
        return;
    }

    const void *raw_ptr = lv_font_get_glyph_bitmap(raw_dsc, raw_buf);
    const void *cmp_ptr = lv_font_get_glyph_bitmap(cmp_dsc, cmp_buf);
    if (raw_ptr == NULL || cmp_ptr == NULL) {
        report(cp, pair->name, "decoded bitmap is NULL");
    } else {
        size_t bytes = (size_t)w * h;
        if (memcmp(raw_buf->data, cmp_buf->data, bytes) != 0) {
            size_t first = 0;
            while (first < bytes && raw_buf->data[first] == cmp_buf->data[first]) {
                first++;
            }
            char detail[96];
            snprintf(detail, sizeof(detail), "A8 bytes differ at %u (%02X vs %02X)",
                     (unsigned)first, raw_buf->data[first], cmp_buf->data[first]);
            report(cp, pair->name, detail);
        } else {
            compared_glyphs++;
        }
    }

    lv_font_glyph_release_draw_data(raw_dsc);
    lv_font_glyph_release_draw_data(cmp_dsc);
    lv_draw_buf_destroy(raw_buf);
    lv_draw_buf_destroy(cmp_buf);
}

static void compare_font(const font_pair_t *pair)
{
    unsigned present = 0;
    for (uint32_t cp = 0x20; cp <= 0xFFFF; cp++) {
        lv_font_glyph_dsc_t raw_dsc;
        lv_font_glyph_dsc_t cmp_dsc;
        memset(&raw_dsc, 0, sizeof(raw_dsc));
        memset(&cmp_dsc, 0, sizeof(cmp_dsc));

        bool raw_ok = lv_font_get_glyph_dsc(pair->raw, &raw_dsc, cp, 0);
        bool cmp_ok = lv_font_get_glyph_dsc(pair->compressed, &cmp_dsc, cp, 0);
        if (raw_ok != cmp_ok) {
            report(cp, pair->name, raw_ok ? "raw has glyph, compressed missing"
                                          : "compressed has glyph, raw missing");
            continue;
        }
        if (!raw_ok) {
            continue;
        }
        present++;
        if (!dsc_equal(&raw_dsc, &cmp_dsc)) {
            report(cp, pair->name, "glyph descriptor differs");
            continue;
        }
        compare_bitmap(cp, pair, &raw_dsc, &cmp_dsc);
    }
    printf("FONT %s glyphs=%u compared_bitmaps=%u\n", pair->name, present, compared_glyphs);
}

int main(void)
{
    lv_init();

    const font_pair_t pairs[] = {
        {"ui_font_16", &ui_font_16, &ui_font_16_ref},
        {"ui_font_20", &ui_font_20, &ui_font_20_ref},
        {"ui_font_24", &ui_font_24, &ui_font_24_ref},
        {"ui_font_32", &ui_font_32, &ui_font_32_ref},
        {"ui_font_num_48", &ui_font_num_48, &ui_font_num_48_ref},
        {"ui_font_num_80", &ui_font_num_80, &ui_font_num_80_ref},
        {"ui_font_num_128", &ui_font_num_128, &ui_font_num_128_ref},
    };

    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        compare_font(&pairs[i]);
    }

    if (mismatches != 0) {
        fprintf(stderr, "FAIL: raw/compressed glyph mismatches=%u\n", mismatches);
        return 1;
    }
    printf("PASS: all decoded glyphs identical (%u bitmaps)\n", compared_glyphs);
    return 0;
}
