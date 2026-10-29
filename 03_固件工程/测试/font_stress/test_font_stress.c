/*
 * Host regression: compressed LVGL font decoder under concurrent use, plus a
 * full single-thread coverage decode of every UI glyph in every Chinese font.
 *
 * The firmware renders with CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2 in the defective
 * build.  lv_font_fmt_txt.c keeps the RLE decoder state in the single global
 * LV_GLOBAL_DEFAULT()->font_fmt_rle object and decompress() takes no lock.
 * Two draw units decoding different glyphs at the same time can therefore
 * consume each other's RLE runs and produce corrupted glyph bitmaps ("mosaic"
 * text on the panel).  The Windows simulator uses one draw unit, so its
 * screenshots cannot expose this.
 *
 * Phase 1 decodes every codepoint in 04_Tools/fonts/ui_glyphs.txt with all four
 * Chinese fonts and checks for missing/zero-size/non-deterministic glyphs.
 * Phase 2 decodes a timing subset with N Win32 threads, each with its own draw
 * buffer, and compares every bitmap with a single-thread baseline.
 *
 * Host-only: does not prove the panel is correct, only whether the decoder is
 * safe to call from more than one draw unit and whether coverage is real.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

LV_FONT_DECLARE(ui_font_16);
LV_FONT_DECLARE(ui_font_20);
LV_FONT_DECLARE(ui_font_24);
LV_FONT_DECLARE(ui_font_32);
LV_FONT_DECLARE(ui_font_num_128);

#define MAX_GLYPHS 256
#define MAX_THREADS 8
#define MAX_UI_GLYPHS 512
#define GLYPH_BUF_W 200
#define GLYPH_BUF_H 200
#define UI_GLYPHS_PATH "C:/wsp/WavesharePanel/04_Tools/fonts/ui_glyphs.txt"

typedef struct {
    const lv_font_t *font;
    uint32_t cp;
    uint16_t w;
    uint16_t h;
    uint64_t hash;
} glyph_rec_t;

static glyph_rec_t g_glyphs[MAX_GLYPHS];
static int g_glyph_count;
static volatile LONG g_go;
static volatile LONG g_ready;

static const uint32_t g_cjk[] = {
    0x6587, /* 文 */
    0x5B57, /* 字 */
    0x89E6, /* 触 */
    0x6478, /* 摸 */
    0x65B9, /* 方 */
    0x5411, /* 向 */
    0x6821, /* 校 */
    0x51C6, /* 准 */
    0x663E, /* 显 */
    0x793A, /* 示 */
    0x754C, /* 界 */
    0x9762, /* 面 */
    0x8BBE, /* 设 */
    0x7F6E, /* 置 */
    0x5929, /* 天 */
    0x6C14, /* 气 */
    0x8BA1, /* 计 */
    0x7B97, /* 算 */
    0x5668, /* 器 */
    0x76D1, /* 监 */
    0x89C6, /* 视 */
    0x8BCA, /* 诊 */
    0x65AD, /* 断 */
    0x9000, /* 退 */
    0x4E32, /* 串 */
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

static uint64_t glyph_hash(const lv_draw_buf_t *buf)
{
    size_t stride = lv_draw_buf_width_to_stride(buf->header.w, LV_COLOR_FORMAT_A8);
    return hash_bytes(buf->data, stride * buf->header.h);
}

static int decode_hash(const lv_font_t *font, uint32_t cp, lv_draw_buf_t *buf,
                       uint64_t *out_hash, uint16_t *out_w, uint16_t *out_h)
{
    lv_font_glyph_dsc_t dsc;
    memset(&dsc, 0, sizeof(dsc));
    if (!lv_font_get_glyph_dsc(font, &dsc, cp, 0)) {
        return 0;
    }
    if (dsc.box_w == 0 || dsc.box_h == 0) {
        return 0;
    }
    if (lv_draw_buf_reshape(buf, LV_COLOR_FORMAT_A8, dsc.box_w, dsc.box_h, 0) == NULL) {
        fprintf(stderr, "reshape failed cp=U+%04X w=%u h=%u\n", cp, dsc.box_w, dsc.box_h);
        return -1;
    }
    const void *ret = lv_font_get_glyph_bitmap(&dsc, buf);
    if (ret == NULL) {
        fprintf(stderr, "decode returned NULL cp=U+%04X\n", cp);
        return -1;
    }
    const lv_draw_buf_t *used = (const lv_draw_buf_t *)ret;
    if (used->data == NULL || used->header.w != dsc.box_w || used->header.h != dsc.box_h) {
        fprintf(stderr, "unexpected draw buf cp=U+%04X\n", cp);
        return -1;
    }
    *out_hash = glyph_hash(used);
    *out_w = dsc.box_w;
    *out_h = dsc.box_h;
    return 1;
}

static int load_ui_codepoints(uint32_t *out, int cap)
{
    FILE *fp = fopen(UI_GLYPHS_PATH, "rb");
    if (fp == NULL) {
        fprintf(stderr, "cannot open %s\n", UI_GLYPHS_PATH);
        return -1;
    }
    char line[64];
    int count = 0;
    while (fgets(line, sizeof(line), fp) != NULL && count < cap) {
        unsigned cp = 0;
        if (sscanf(line, "%x", &cp) == 1 && cp > 0x7F) {
            out[count++] = cp;
        }
    }
    fclose(fp);
    return count;
}

/* Phase 1: every UI codepoint, every Chinese font, decoded twice. */
static int verify_ui_glyphs(void)
{
    uint32_t cps[MAX_UI_GLYPHS];
    int cp_count = load_ui_codepoints(cps, MAX_UI_GLYPHS);
    if (cp_count <= 0) {
        fprintf(stderr, "FAIL: could not load UI glyph list\n");
        return 1;
    }
    printf("ui_coverage: codepoints=%d fonts=16,20,24,32\n", cp_count);

    const lv_font_t *fonts[] = {&ui_font_16, &ui_font_20, &ui_font_24, &ui_font_32};
    const int sizes[] = {16, 20, 24, 32};
    lv_draw_buf_t *buf = lv_draw_buf_create(GLYPH_BUF_W, GLYPH_BUF_H, LV_COLOR_FORMAT_A8, 0);
    if (buf == NULL) {
        fprintf(stderr, "FAIL: could not allocate coverage draw buffer\n");
        return 1;
    }

    int failures = 0;
    for (size_t f = 0; f < sizeof(fonts) / sizeof(fonts[0]); f++) {
        int ok_count = 0;
        int missing = 0;
        int zero = 0;
        int nondet = 0;
        for (int i = 0; i < cp_count; i++) {
            lv_font_glyph_dsc_t dsc;
            memset(&dsc, 0, sizeof(dsc));
            if (!lv_font_get_glyph_dsc(fonts[f], &dsc, cps[i], 0)) {
                if (missing < 5) {
                    printf("  font%d missing U+%04X\n", sizes[f], cps[i]);
                }
                missing++;
                continue;
            }
            if (dsc.box_w == 0 || dsc.box_h == 0) {
                zero++;
                continue;
            }
            uint64_t h1 = 0;
            uint64_t h2 = 0;
            uint16_t w = 0;
            uint16_t h = 0;
            if (decode_hash(fonts[f], cps[i], buf, &h1, &w, &h) <= 0 ||
                    decode_hash(fonts[f], cps[i], buf, &h2, &w, &h) <= 0) {
                missing++;
                continue;
            }
            if (h1 != h2) {
                nondet++;
            }
            ok_count++;
        }
        printf("ui_coverage font=%d ok=%d missing=%d zero_size=%d nondeterministic=%d\n",
               sizes[f], ok_count, missing, zero, nondet);
        failures += missing + zero + nondet;
    }

    lv_draw_buf_destroy(buf);
    if (failures != 0) {
        fprintf(stderr, "FAIL: UI glyph coverage failures=%d\n", failures);
        return 1;
    }
    printf("PASS: every UI glyph decodes in all four Chinese fonts\n");
    return 0;
}

static int build_baseline(void)
{
    lv_draw_buf_t *buf = lv_draw_buf_create(GLYPH_BUF_W, GLYPH_BUF_H, LV_COLOR_FORMAT_A8, 0);
    if (buf == NULL) {
        fprintf(stderr, "could not allocate baseline draw buffer\n");
        return -1;
    }

    int font32_count = 0;
    int num128_count = 0;
    int missing = 0;
    const lv_font_t *fonts[] = {&ui_font_32, &ui_font_num_128};
    for (size_t f = 0; f < sizeof(fonts) / sizeof(fonts[0]); f++) {
        if (fonts[f] == &ui_font_32) {
            for (size_t i = 0; i < sizeof(g_cjk) / sizeof(g_cjk[0]); i++) {
                uint64_t hash;
                uint16_t w, h;
                int ok = decode_hash(fonts[f], g_cjk[i], buf, &hash, &w, &h);
                if (ok < 0) {
                    lv_draw_buf_destroy(buf);
                    return -1;
                }
                if (ok == 0 || g_glyph_count >= MAX_GLYPHS) {
                    missing++;
                    printf("missing ui_font_32 U+%04X\n", g_cjk[i]);
                    continue;
                }
                g_glyphs[g_glyph_count++] = (glyph_rec_t){fonts[f], g_cjk[i], w, h, hash};
                font32_count++;
            }
            for (uint32_t cp = 0x21; cp <= 0x7E && g_glyph_count < MAX_GLYPHS; cp++) {
                uint64_t hash;
                uint16_t w, h;
                int ok = decode_hash(fonts[f], cp, buf, &hash, &w, &h);
                if (ok < 0) {
                    lv_draw_buf_destroy(buf);
                    return -1;
                }
                if (ok == 0) {
                    missing++;
                    printf("missing ui_font_32 U+%04X\n", cp);
                    continue;
                }
                g_glyphs[g_glyph_count++] = (glyph_rec_t){fonts[f], cp, w, h, hash};
                font32_count++;
            }
        } else {
            const uint32_t digits[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                                       '8', '9', 0x00B0, 0x00B7, '%', '.', ',',
                                       ':', '+', '-'};
            for (size_t i = 0; i < sizeof(digits) / sizeof(digits[0]); i++) {
                uint64_t hash;
                uint16_t w, h;
                int ok = decode_hash(fonts[f], digits[i], buf, &hash, &w, &h);
                if (ok < 0) {
                    lv_draw_buf_destroy(buf);
                    return -1;
                }
                if (ok == 0 || g_glyph_count >= MAX_GLYPHS) {
                    missing++;
                    printf("missing ui_font_num_128 U+%04X\n", digits[i]);
                    continue;
                }
                g_glyphs[g_glyph_count++] = (glyph_rec_t){fonts[f], digits[i], w, h, hash};
                num128_count++;
            }
        }
    }

    printf("stress_coverage: ui_font_32=%d ui_font_num_128=%d missing=%d total=%d\n",
           font32_count, num128_count, missing, g_glyph_count);
    if (font32_count < 32 || num128_count < 12 || missing != 0) {
        fprintf(stderr, "FAIL: stress glyph set incomplete (32=%d num128=%d missing=%d)\n",
                font32_count, num128_count, missing);
        lv_draw_buf_destroy(buf);
        return -1;
    }

    lv_draw_buf_destroy(buf);
    return g_glyph_count;
}

typedef struct {
    int id;
    int threads;
    int iterations;
    unsigned long long calls;
    unsigned long long mismatches;
    uint32_t first_bad_cp;
} worker_ctx_t;

static DWORD WINAPI worker_main(LPVOID arg)
{
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    lv_draw_buf_t *buf = lv_draw_buf_create(GLYPH_BUF_W, GLYPH_BUF_H, LV_COLOR_FORMAT_A8, 0);
    if (buf == NULL) {
        return 1;
    }

    InterlockedIncrement(&g_ready);
    while (g_go == 0) {
        SwitchToThread();
    }

    for (int iter = 0; iter < ctx->iterations; iter++) {
        for (int i = ctx->id; i < g_glyph_count; i += ctx->threads) {
            lv_font_glyph_dsc_t dsc;
            memset(&dsc, 0, sizeof(dsc));
            const glyph_rec_t *g = &g_glyphs[i];
            if (!lv_font_get_glyph_dsc(g->font, &dsc, g->cp, 0)) {
                continue;
            }
            if (lv_draw_buf_reshape(buf, LV_COLOR_FORMAT_A8, dsc.box_w, dsc.box_h, 0) == NULL) {
                continue;
            }
            const void *ret = lv_font_get_glyph_bitmap(&dsc, buf);
            ctx->calls++;
            if (ret == NULL) {
                ctx->mismatches++;
                continue;
            }
            const lv_draw_buf_t *used = (const lv_draw_buf_t *)ret;
            if (used->data == NULL || glyph_hash(used) != g->hash) {
                ctx->mismatches++;
                if (ctx->first_bad_cp == 0) {
                    ctx->first_bad_cp = g->cp;
                }
            }
        }
    }

    lv_draw_buf_destroy(buf);
    return 0;
}

static int run_workers(int threads, int iterations, unsigned long long *calls_out,
                       unsigned long long *mismatch_out, uint32_t *first_bad_cp)
{
    if (threads < 1 || threads > MAX_THREADS) {
        return -1;
    }
    worker_ctx_t ctx[MAX_THREADS];
    HANDLE handles[MAX_THREADS];
    memset(ctx, 0, sizeof(ctx));
    memset(handles, 0, sizeof(handles));
    g_go = 0;
    g_ready = 0;

    for (int i = 0; i < threads; i++) {
        ctx[i].id = i;
        ctx[i].threads = threads;
        ctx[i].iterations = iterations;
        handles[i] = CreateThread(NULL, 0, worker_main, &ctx[i], 0, NULL);
        if (handles[i] == NULL) {
            fprintf(stderr, "CreateThread %d failed: %lu\n", i, (unsigned long)GetLastError());
            return -1;
        }
    }

    while (g_ready < threads) {
        SwitchToThread();
    }
    InterlockedExchange(&g_go, 1);

    DWORD wait = WaitForMultipleObjects((DWORD)threads, handles, TRUE, 120000);
    if (wait != WAIT_OBJECT_0) {
        fprintf(stderr, "worker wait failed/timeout: %lu\n", (unsigned long)wait);
        for (int i = 0; i < threads; i++) {
            TerminateThread(handles[i], 1);
        }
        return -1;
    }

    unsigned long long calls = 0;
    unsigned long long mismatches = 0;
    uint32_t first_bad = 0;
    for (int i = 0; i < threads; i++) {
        calls += ctx[i].calls;
        mismatches += ctx[i].mismatches;
        if (first_bad == 0 && ctx[i].first_bad_cp != 0) {
            first_bad = ctx[i].first_bad_cp;
        }
        CloseHandle(handles[i]);
    }
    *calls_out = calls;
    *mismatch_out = mismatches;
    *first_bad_cp = first_bad;
    return 0;
}

int main(void)
{
    lv_init();

    if (verify_ui_glyphs() != 0) {
        return 1;
    }

    int count = build_baseline();
    if (count <= 0) {
        fprintf(stderr, "FAIL: baseline glyph collection failed (%d)\n", count);
        return 1;
    }
    printf("font_stress: glyphs=%d fonts=ui_font_32,ui_font_num_128\n", count);

    unsigned long long calls = 0;
    unsigned long long mismatches = 0;
    uint32_t first_bad = 0;
    if (run_workers(1, 200, &calls, &mismatches, &first_bad) != 0) {
        fprintf(stderr, "FAIL: single-thread control run failed\n");
        return 1;
    }
    printf("single_thread: calls=%llu mismatches=%llu\n", calls, mismatches);
    if (mismatches != 0) {
        fprintf(stderr, "FAIL: single-thread decode is not deterministic (first U+%04X)\n",
                first_bad);
        return 1;
    }

    /* Four threads on a two-core host gives the same kind of overlap as two
     * LVGL draw units: two decoder calls are in flight at the same time. */
    if (run_workers(4, 1500, &calls, &mismatches, &first_bad) != 0) {
        fprintf(stderr, "FAIL: multi-thread run failed\n");
        return 1;
    }
    printf("multi_thread: threads=4 calls=%llu mismatches=%llu first_bad=U+%04X\n",
           calls, mismatches, first_bad);

    if (mismatches == 0) {
        printf("RESULT: NOT_REPRODUCED\n");
        return 3;
    }

    printf("RESULT: RACE_REPRODUCED\n");
    return 0;
}
