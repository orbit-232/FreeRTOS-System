#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 音乐播放器唯一启动界面。创建和加载屏幕只在调用线程持有一次 LVGL 锁，
 * 不创建第二个 LVGL 任务。 */
void panel_music_ui_start(void);

/* 串口诊断，复用现有 UART stdin；不操作 COM 口、不烧录。 */
void panel_music_ui_start_console(void);

/* 运行时封面旋转开关与 UI 心跳（供诊断任务读取，不阻塞 LVGL）。 */
void panel_music_ui_set_disc_spin(bool enabled);
bool panel_music_ui_disc_spin_enabled(void);
void panel_music_ui_heartbeat_snapshot(uint32_t *count, uint32_t *age_ms);

#ifdef PANEL_MUSIC_UI_HOST_TEST
#include "lvgl.h"
lv_obj_t *panel_music_ui_page_create(void);
bool panel_music_ui_test_click(const char *action);
const char *panel_music_ui_test_status_text(void);
void panel_music_ui_test_refresh(void);
#endif

#ifdef __cplusplus
}
#endif
