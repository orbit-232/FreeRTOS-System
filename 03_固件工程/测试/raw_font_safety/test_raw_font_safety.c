/*
 * Independent raw-font safety test.  Does not replace or modify the existing
 * font_stress compressed-font reproduction.
 *
 * - actual LVGL font decode
 * - CLIB allocator (separate lv_conf.h)
 * - every non-empty glyph of all 7 shipped fonts, single-thread baseline
 * - 4 Win32 threads, each with its own lv_draw_buf_t, >=200000 decodes
 * - 0 mismatch required; compressed font descriptors are rejected up front.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "font/fmt_txt/lv_font_fmt_txt.h"

LV_FONT_DECLARE(ui_font_16);
LV_FONT_DECLARE(ui_font_20);
LV_FONT_DECLARE(ui_font_24);
LV_FONT_DECLARE(ui_font_32);
LV_FONT_DECLARE(ui_font_num_48);
LV_FONT_DECLARE(ui_font_num_80);
LV_FONT_DECLARE(ui_font_num_128);

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_24);
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_32);
LV_FONT_DECLARE(lv_font_montserrat_40);

#define MAX_GLYPHS 4096
#define THREADS 4
#define CONCURRENT_CALLS 200000
#define GLYPH_BUF_W 200
#define GLYPH_BUF_H 200

typedef struct {
    const lv_font_t *font;
    uint32_t cp;
    uint16_t w;
    uint16_t h;
    uint64_t hash;
} glyph_rec_t;

typedef struct {
    int index;
} worker_ctx_t;

static glyph_rec_t g_glyphs[MAX_GLYPHS];
static int g_glyph_count;
static volatile LONG g_go;
static volatile LONG g_ready;
static volatile LONG g_calls;
static volatile LONG g_mismatches;
static volatile LONG g_first_bad;

static const lv_font_t *g_ui_fonts[] = {
    &ui_font_16, &ui_font_20, &ui_font_24, &ui_font_32,
    &ui_font_num_48, &ui_font_num_80, &ui_font_num_128,
};
static const char *g_ui_names[] = {
    "ui_font_16", "ui_font_20", "ui_font_24", "ui_font_32",
    "ui_font_num_48", "ui_font_num_80", "ui_font_num_128",
};

static uint64_t hash_bytes(const uint8_t *data, size_t len)
{
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t glyph_hash(const lv_draw_buf_t *buf, uint16_t w, uint16_t h)
{
    size_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_A8);
    return hash_bytes(buf->data, stride * h);
}

static int decode_hash(const lv_font_t *font, uint32_t cp, lv_draw_buf_t *buf,
                       uint64_t *out_hash, uint16_t *out_w, uint16_t *out_h)
{
    lv_font_glyph_dsc_t dsc;
    memset(&dsc, 0, sizeof(dsc));
    if (!lv_font_get_glyph_dsc(font, &dsc, cp, 0)) {
        return 0;
    }
    if (dsc.is_placeholder || dsc.box_w == 0 || dsc.box_h == 0) {
        return 0;
    }
    const void *ret = lv_font_get_glyph_bitmap(&dsc, buf);
    if (ret == NULL) {
        fprintf(stderr, "decode returned NULL font=%p cp=U+%04X\n", (const void *)font, cp);
        return -1;
    }
    if (buf->data == NULL) {
        fprintf(stderr, "unexpected null bitmap font=%p cp=U+%04X\n", (const void *)font, cp);
        return -1;
    }
    *out_hash = glyph_hash(buf, dsc.box_w, dsc.box_h);
    *out_w = dsc.box_w;
    *out_h = dsc.box_h;
    return 1;
}

/* Every descriptor reachable from a font, including fallback chains, must be
 * plain.  This is the automatic no-compressed condition for the parallel
 * candidate. */
static int check_font_plain(const lv_font_t *font, const char *name)
{
    int checked = 0;
    for (const lv_font_t *f = font; f != NULL; f = f->fallback) {
        if (f->get_glyph_dsc == lv_font_get_glyph_dsc_fmt_txt) {
            const lv_font_fmt_txt_dsc_t *d = (const lv_font_fmt_txt_dsc_t *)f->dsc;
            if (d == NULL || d->bitmap_format != LV_FONT_FMT_TXT_PLAIN) {
                fprintf(stderr, "FAIL: compressed/non-plain descriptor in %s\n", name);
                return 0;
            }
            checked++;
        }
    }
    if (checked == 0) {
        fprintf(stderr, "FAIL: no fmt_txt descriptor checked for %s\n", name);
        return 0;
    }
    return 1;
}

static int check_all_fonts_plain(void)
{
    for (size_t i = 0; i < sizeof(g_ui_fonts) / sizeof(g_ui_fonts[0]); i++) {
        if (!check_font_plain(g_ui_fonts[i], g_ui_names[i])) {
            return 0;
        }
    }
    if (!check_font_plain(lv_font_get_default(), "lv_font_get_default()")) {
        return 0;
    }
    if (!check_font_plain(&lv_font_montserrat_14, "lv_font_montserrat_14")) return 0;
    if (!check_font_plain(&lv_font_montserrat_20, "lv_font_montserrat_20")) return 0;
    if (!check_font_plain(&lv_font_montserrat_24, "lv_font_montserrat_24")) return 0;
    if (!check_font_plain(&lv_font_montserrat_28, "lv_font_montserrat_28")) return 0;
    if (!check_font_plain(&lv_font_montserrat_32, "lv_font_montserrat_32")) return 0;
    if (!check_font_plain(&lv_font_montserrat_40, "lv_font_montserrat_40")) return 0;
    return 1;
}

static int build_baseline(void)
{
    lv_draw_buf_t *buf = lv_draw_buf_create(GLYPH_BUF_W, GLYPH_BUF_H, LV_COLOR_FORMAT_A8, 0);
    if (buf == NULL) {
        fprintf(stderr, "FAIL: baseline draw buffer allocation\n");
        return 1;
    }

    for (size_t f = 0; f < sizeof(g_ui_fonts) / sizeof(g_ui_fonts[0]); f++) {
        int font_count = 0;
        for (uint32_t cp = 0; cp <= 0xFFFF; cp++) {
            uint64_t h1 = 0, h2 = 0;
            uint16_t w1 = 0, hh1 = 0, w2 = 0, hh2 = 0;
            int a = decode_hash(g_ui_fonts[f], cp, buf, &h1, &w1, &hh1);
            if (a < 0) {
                lv_draw_buf_destroy(buf);
                return 1;
            }
            if (a == 0) {
                continue;
            }
            int b = decode_hash(g_ui_fonts[f], cp, buf, &h2, &w2, &hh2);
            if (b <= 0 || h1 != h2 || w1 != w2 || hh1 != hh2) {
                fprintf(stderr, "FAIL: non-deterministic baseline %s cp=U+%04X\n",
                        g_ui_names[f], cp);
                lv_draw_buf_destroy(buf);
                return 1;
            }
            if (g_glyph_count >= MAX_GLYPHS) {
                fprintf(stderr, "FAIL: baseline glyph capacity\n");
                lv_draw_buf_destroy(buf);
                return 1;
            }
            g_glyphs[g_glyph_count++] = (glyph_rec_t){
                g_ui_fonts[f], cp, w1, hh1, h1
            };
            font_count++;
        }
        printf("baseline %s: %d non-empty glyphs\n", g_ui_names[f], font_count);
    }

    lv_draw_buf_destroy(buf);
    printf("baseline total: %d non-empty glyphs (>=2196)\n", g_glyph_count);
    if (g_glyph_count < 2196) {
        fprintf(stderr, "FAIL: expected at least 2196 non-empty glyphs, got %d\n", g_glyph_count);
        return 1;
    }
    return 0;
}

static DWORD WINAPI worker(LPVOID arg)
{
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    lv_draw_buf_t *buf = lv_draw_buf_create(GLYPH_BUF_W, GLYPH_BUF_H, LV_COLOR_FORMAT_A8, 0);
    if (buf == NULL) {
        InterlockedIncrement(&g_mismatches);
        return 1;
    }
    InterlockedIncrement(&g_ready);
    while (g_go == 0) {
        SwitchToThread();
    }

    const int per_thread = CONCURRENT_CALLS / THREADS;
    for (int i = 0; i < per_thread; i++) {
        int idx = (ctx->index * 1009 + i * 7919) % g_glyph_count;
        const glyph_rec_t *g = &g_glyphs[idx];
        lv_font_glyph_dsc_t dsc;
        memset(&dsc, 0, sizeof(dsc));
        if (!lv_font_get_glyph_dsc(g->font, &dsc, g->cp, 0)) {
            InterlockedIncrement(&g_mismatches);
            InterlockedCompareExchange(&g_first_bad, (LONG)g->cp, 0);
            InterlockedIncrement(&g_calls);
            continue;
        }
        const void *ret = lv_font_get_glyph_bitmap(&dsc, buf);
        InterlockedIncrement(&g_calls);
        if (ret == NULL) {
            InterlockedIncrement(&g_mismatches);
            InterlockedCompareExchange(&g_first_bad, (LONG)g->cp, 0);
            continue;
        }
        const lv_draw_buf_t *used = (const lv_draw_buf_t *)ret;
        if (used->data == NULL || used->data != buf->data ||
            glyph_hash(buf, g->w, g->h) != g->hash) {
            InterlockedIncrement(&g_mismatches);
            InterlockedCompareExchange(&g_first_bad, (LONG)g->cp, 0);
        }
    }

    lv_draw_buf_destroy(buf);
    return 0;
}

int main(void)
{
    lv_init();
    if (!check_all_fonts_plain()) {
        return 1;
    }
    printf("plain-font condition: PASS (7 UI fonts + default/fallback + all used built-ins)\n");

    if (build_baseline() != 0) {
        return 1;
    }

    HANDLE threads[THREADS];
    worker_ctx_t ctx[THREADS];
    for (int i = 0; i < THREADS; i++) {
        ctx[i].index = i;
        threads[i] = CreateThread(NULL, 0, worker, &ctx[i], 0, NULL);
        if (threads[i] == NULL) {
            fprintf(stderr, "FAIL: CreateThread %d\n", i);
            return 1;
        }
    }
    while (g_ready < THREADS) {
        SwitchToThread();
    }
    g_go = 1;
    DWORD wait = WaitForMultipleObjects(THREADS, threads, TRUE, 120000);
    for (int i = 0; i < THREADS; i++) {
        CloseHandle(threads[i]);
    }
    if (wait != WAIT_OBJECT_0) {
        fprintf(stderr, "FAIL: worker timeout\n");
        return 1;
    }

    printf("concurrent: threads=%d calls=%ld mismatches=%ld first_bad=U+%04lX\n",
           THREADS, g_calls, g_mismatches, (unsigned long)g_first_bad);
    if (g_calls < CONCURRENT_CALLS || g_mismatches != 0) {
        return 1;
    }
    printf("RAW_FONT_SAFETY PASS\n");
    return 0;
}
