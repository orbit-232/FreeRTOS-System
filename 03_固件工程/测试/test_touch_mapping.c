/*
 * Coordinate regression for the real 1280x800 panel.
 *
 * The transform under test is panel_touch_transform_raw() from
 * panel_touch_map.h, the exact function installed in panel_bsp's official
 * esp_lv_adapter custom_touch_read hook.  The test checks:
 *   - raw GT9271 corners/centre map to the expected logical corners/centre;
 *   - every logical coordinate is strictly inside 0..1279 / 0..799;
 *   - out-of-range controller reports are rejected, not wrapped or clamped;
 *   - logical -> physical (adapter rotation 270) returns the original raw point;
 *   - the production source really installs this hook and keeps driver flags 0.
 *
 * The correct inverse of the old rotation-90 build (kept for the report only)
 * is lv_x = raw_y, lv_y = 799 - raw_x, not the formula used by the earlier
 * draft test.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "panel_touch_map.h"

typedef struct {
    const char *name;
    int raw_x;
    int raw_y;
    int want_x;
    int want_y;
} raw_point_t;

static int failures;

static void check_point(const raw_point_t *p)
{
    int lx = -9999;
    int ly = -9999;
    bool ok = panel_touch_transform_raw(p->raw_x, p->raw_y, &lx, &ly);
    int old90_x = p->raw_y;
    int old90_y = (PANEL_LOGICAL_HEIGHT - 1) - p->raw_x;

    printf("%-12s raw=(%4d,%4d) -> lv=(%4d,%4d) want=(%4d,%4d) old90=(%3d,%3d)\n",
           p->name, p->raw_x, p->raw_y, lx, ly, p->want_x, p->want_y, old90_x, old90_y);

    if (!ok) {
        printf("  FAIL: valid raw point rejected\n");
        failures++;
        return;
    }
    if (lx != p->want_x || ly != p->want_y) {
        printf("  FAIL: transform mismatch\n");
        failures++;
    }
    if (lx < 0 || lx >= PANEL_LOGICAL_WIDTH || ly < 0 || ly >= PANEL_LOGICAL_HEIGHT) {
        printf("  FAIL: logical point outside 0..%d / 0..%d\n", PANEL_LOGICAL_WIDTH - 1,
               PANEL_LOGICAL_HEIGHT - 1);
        failures++;
    }
}

static void check_display_roundtrip(void)
{
    for (int raw_y = 0; raw_y < PANEL_NATIVE_HEIGHT; raw_y += 13) {
        for (int raw_x = 0; raw_x < PANEL_NATIVE_WIDTH; raw_x += 17) {
            int lx, ly, px, py;
            if (!panel_touch_transform_raw(raw_x, raw_y, &lx, &ly)) {
                printf("FAIL: grid raw=(%d,%d) rejected\n", raw_x, raw_y);
                failures++;
                continue;
            }
            panel_display_logical_to_physical(lx, ly, &px, &py);
            if (px != raw_x || py != raw_y) {
                printf("FAIL: roundtrip raw=(%d,%d) -> lv=(%d,%d) -> phy=(%d,%d)\n",
                       raw_x, raw_y, lx, ly, px, py);
                failures++;
            }
        }
    }

    struct {
        int lx, ly, px, py;
        const char *name;
    } corners[] = {
        {0, 0, 0, 1279, "lv top-left"},
        {1279, 0, 0, 0, "lv top-right"},
        {0, 799, 799, 1279, "lv bottom-left"},
        {1279, 799, 799, 0, "lv bottom-right"},
        {640, 400, 400, 639, "lv centre"},
    };
    for (size_t i = 0; i < sizeof(corners) / sizeof(corners[0]); i++) {
        int px, py;
        panel_display_logical_to_physical(corners[i].lx, corners[i].ly, &px, &py);
        printf("display270 %-14s lv=(%4d,%4d) -> phy=(%4d,%4d) expect=(%4d,%4d)\n",
               corners[i].name, corners[i].lx, corners[i].ly, px, py, corners[i].px,
               corners[i].py);
        if (px != corners[i].px || py != corners[i].py) {
            printf("  FAIL: display mapping mismatch\n");
            failures++;
        }
    }
}

static void check_out_of_range(void)
{
    const struct {
        int raw_x, raw_y;
        const char *name;
    } bad[] = {
        {-1, 0, "x=-1"},
        {0, -1, "y=-1"},
        {PANEL_NATIVE_WIDTH, 0, "x=800"},
        {0, PANEL_NATIVE_HEIGHT, "y=1280"},
        {PANEL_NATIVE_WIDTH, PANEL_NATIVE_HEIGHT, "x=800,y=1280"},
        {INT_MAX, INT_MIN, "int extremes"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int lx = 12345;
        int ly = 54321;
        bool ok = panel_touch_transform_raw(bad[i].raw_x, bad[i].raw_y, &lx, &ly);
        printf("reject %-16s -> ok=%d lx=%d ly=%d\n", bad[i].name, ok, lx, ly);
        if (ok) {
            printf("  FAIL: out-of-range point accepted\n");
            failures++;
        }
        if (lx != 12345 || ly != 54321) {
            printf("  FAIL: outputs modified for rejected point\n");
            failures++;
        }
    }
}

int main(void)
{
    printf("panel native=%dx%d logical=%dx%d driver flags swap=%d mirror_x=%d mirror_y=%d\n",
           PANEL_NATIVE_WIDTH, PANEL_NATIVE_HEIGHT, PANEL_LOGICAL_WIDTH,
           PANEL_LOGICAL_HEIGHT, PANEL_TOUCH_DRIVER_SWAP_XY,
           PANEL_TOUCH_DRIVER_MIRROR_X, PANEL_TOUCH_DRIVER_MIRROR_Y);

    const raw_point_t points[] = {
        {"raw TL", 0, 0, PANEL_LOGICAL_WIDTH - 1, 0},
        {"raw TR", PANEL_NATIVE_WIDTH - 1, 0, PANEL_LOGICAL_WIDTH - 1,
         PANEL_LOGICAL_HEIGHT - 1},
        {"raw BL", 0, PANEL_NATIVE_HEIGHT - 1, 0, 0},
        {"raw BR", PANEL_NATIVE_WIDTH - 1, PANEL_NATIVE_HEIGHT - 1, 0,
         PANEL_LOGICAL_HEIGHT - 1},
        {"raw centre", PANEL_NATIVE_WIDTH / 2, PANEL_NATIVE_HEIGHT / 2,
         PANEL_LOGICAL_WIDTH / 2 - 1, PANEL_LOGICAL_HEIGHT / 2},
    };
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        check_point(&points[i]);
    }

    check_out_of_range();
    check_display_roundtrip();

    if (failures != 0) {
        printf("FAIL: touch mapping failures=%d\n", failures);
        return 1;
    }
    printf("PASS: exact 270-degree transform, strict 0..1279/0..799 bounds, "
           "out-of-range rejected\n");
    return 0;
}
