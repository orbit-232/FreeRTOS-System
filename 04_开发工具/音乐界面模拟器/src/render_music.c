#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "panel_music_ui.h"

#define WIDTH 1280
#define HEIGHT 800

static uint16_t framebuffer[WIDTH * HEIGHT];
static uint8_t draw_buffer[WIDTH * HEIGHT * 2] __attribute__((aligned(4)));

void music_fake_init_from_env(void);

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
            if (dx >= 0 && dx < WIDTH) framebuffer[dy * WIDTH + dx] = src[y * w + x];
        }
    }
    lv_display_flush_ready(disp);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: render_music <out.ppm> <ignored>\n");
        return 2;
    }
    music_fake_init_from_env();
    lv_init();
    lv_tick_inc(100);
    lv_display_t *disp = lv_display_create(WIDTH, HEIGHT);
    lv_display_set_default(disp);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, draw_buffer, NULL, sizeof(draw_buffer),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_obj_t *screen = panel_music_ui_page_create();
    if (screen == NULL) {
        fprintf(stderr, "page create failed\n");
        return 3;
    }
    lv_screen_load(screen);
    lv_obj_update_layout(screen);
    lv_obj_invalidate(screen);
    lv_refr_now(disp);

    FILE *file = fopen(argv[1], "wb");
    if (file == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 4;
    }
    fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        uint16_t c = framebuffer[i];
        uint8_t rgb[3];
        rgb[0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
        rgb[1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
        rgb[2] = (uint8_t)((c & 0x1F) * 255 / 31);
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
    printf("rendered music page -> %s\n", argv[1]);
    return 0;
}
