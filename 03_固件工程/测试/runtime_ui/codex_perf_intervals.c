#include <assert.h>
#include <stdio.h>
#include "lvgl.h"
#include "ui_perf.h"
#include "panel_services.h"
static void frame(lv_display_t *d, uint32_t delta) {
    lv_tick_inc(delta);
    lv_display_send_event(d, LV_EVENT_RENDER_START, NULL);
    lv_display_send_event(d, LV_EVENT_RENDER_READY, NULL);
}
int main(void) {
    lv_init();
    lv_display_t *d=lv_display_create(1280,800);
    ui_perf_attach(d);
    panel_settings_t settings;
    panel_services_get_settings(&settings);
    ui_perf_bench_reset(1, UI_PAGE_HOME, &settings);
    ui_perf_action_begin(UI_PERF_ACT_APP_ENTER);
    ui_perf_action_stage_start();
    frame(d,10); frame(d,20); frame(d,30);
    ui_perf_snapshot_t s;
    ui_perf_get_snapshot(&s);
    assert(s.frames==3);
    assert(s.frame_samples==2);
    assert(s.frame_p50_us==20000);
    assert(s.frame_overflow==0);
    ui_perf_bench_reset(1, UI_PAGE_HOME, &settings);
    ui_perf_action_begin(UI_PERF_ACT_APP_ENTER);
    ui_perf_action_stage_start();
    frame(d,10);
    ui_perf_get_snapshot(&s);
    assert(s.frames==1 && s.frame_samples==0);
    puts("PASS: actual perf event hooks record N-1 non-fabricated intervals; single frame has no interval");
    return 0;
}
