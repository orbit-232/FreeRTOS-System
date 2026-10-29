#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "panel_music.h"
#include "panel_music_prebuilt.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

extern volatile uint64_t g_audio_frames;
extern volatile unsigned g_audio_generation;
extern volatile unsigned g_audio_writes;
extern volatile unsigned g_io_acquires;
extern volatile unsigned g_io_releases;
extern volatile unsigned g_pin_acquires;
extern volatile unsigned g_pin_releases;
extern volatile unsigned g_io_recursive;
void test_runtime_init(void);
void worker_test_fail_audio_after(int writes);

static panel_music_status_t status(void)
{
    panel_music_status_t st;
    panel_music_get_status(&st);
    return st;
}

static bool wait_state(panel_music_state_t wanted, unsigned timeout_ms)
{
    uint32_t start = GetTickCount();
    while (GetTickCount() - start < timeout_ms) {
        if (status().state == wanted) return true;
        Sleep(10);
    }
    return false;
}

static bool wait_position(uint32_t minimum_ms, unsigned timeout_ms)
{
    uint32_t start = GetTickCount();
    while (GetTickCount() - start < timeout_ms) {
        panel_music_status_t st = status();
        if (st.state == PANEL_MUSIC_PLAYING && st.position_ms >= minimum_ms) return true;
        Sleep(10);
    }
    return false;
}

static bool wait_seek_done(uint32_t target, unsigned timeout_ms)
{
    uint32_t start = GetTickCount();
    while (GetTickCount() - start < timeout_ms) {
        panel_music_status_t st = status();
        if (!st.seek_pending && st.state != PANEL_MUSIC_LOADING) {
            if (st.position_ms + 1500U >= target && st.position_ms <= target + 5000U) {
                return true;
            }
        }
        Sleep(5);
    }
    return false;
}

static bool wait_index_ready(unsigned timeout_ms)
{
    uint32_t start = GetTickCount();
    while (GetTickCount() - start < timeout_ms) {
        panel_music_status_t st = status();
        if (st.index_ready) return true;
        if (st.index_failed) return false;
        Sleep(10);
    }
    return false;
}

int main(void)
{
    test_runtime_init();
    CHECK(panel_music_init() == ESP_OK);
    CHECK(panel_music_play() == ESP_OK);
    CHECK(wait_state(PANEL_MUSIC_PLAYING, 8000));
    CHECK(wait_position(200, 4000));

    panel_music_status_t st = status();
    CHECK(st.track == 0);
    CHECK(st.index_ready);
    CHECK(!st.index_building);
    CHECK(st.duration_ms > 300000U);
    CHECK(panel_music_test_index_track() == 0);
    CHECK(panel_music_test_index_source() == 1U);
    CHECK(strcmp(st.title, "After Hours") == 0);
    printf("WORKER_PREBUILT_OK track=%u pos=%u duration=%u audio_frames=%llu\n",
           (unsigned)st.track, (unsigned)st.position_ms, (unsigned)st.duration_ms,
           (unsigned long long)g_audio_frames);

    /* A bad warmup-frame header must clear the seek pending state without
     * cancelling any later request generation. */
    panel_music_test_set_seek_header_mismatch(true);
    CHECK(panel_music_seek(10000U) == ESP_OK);
    uint32_t wait_start = GetTickCount();
    while (status().seek_pending && GetTickCount() - wait_start < 2000U) Sleep(10);
    st = status();
    CHECK(!st.seek_pending);
    CHECK(strstr(st.detail, "跳转帧校验失败") != NULL);
    panel_music_test_set_seek_header_mismatch(false);
    printf("WORKER_SEEK_HEADER_REJECT_OK detail=%s\n", st.detail);

    /* Real temporary copy: last valid byte and a sampled byte must both make
     * the prebuilt probe fall back to full scan. */
    const char *media_root = getenv("MUSIC_TEST_ROOT");
    CHECK(media_root != NULL);
    char src_path[512], copy_path[512];
    snprintf(src_path, sizeof(src_path), "%s/panel/music/after_hours.mp3", media_root);
    snprintf(copy_path, sizeof(copy_path), "%s/panel/music/fallback_copy.mp3", media_root);
    CHECK(CopyFileA(src_path, copy_path, FALSE));
    CHECK(panel_music_test_probe_prebuilt(copy_path, 0) == 1);
    FILE *copy = fopen(copy_path, "r+b");
    CHECK(copy != NULL);
    fseek(copy, 0, SEEK_END);
    long copy_size = ftell(copy);
    fseek(copy, copy_size - 1, SEEK_SET);
    int ch = fgetc(copy);
    fseek(copy, copy_size - 1, SEEK_SET);
    fputc((ch ^ 0xFF) & 0xFF, copy);
    fclose(copy);
    CHECK(panel_music_test_probe_prebuilt(copy_path, 0) == 0);
    CHECK(CopyFileA(src_path, copy_path, FALSE));
    copy = fopen(copy_path, "r+b");
    CHECK(copy != NULL);
    fseek(copy, (long)panel_music_prebuilt_tracks[0].fingerprints[8].offset, SEEK_SET);
    ch = fgetc(copy);
    fseek(copy, (long)panel_music_prebuilt_tracks[0].fingerprints[8].offset, SEEK_SET);
    fputc((ch ^ 0xFF) & 0xFF, copy);
    fclose(copy);
    CHECK(panel_music_test_probe_prebuilt(copy_path, 0) == 0);
    DeleteFileA(copy_path);
    printf("WORKER_PREBUILT_TAMPER_FALLBACK_OK\n");

    uint32_t target = st.duration_ms / 4U;
    unsigned generation = g_audio_generation;
    CHECK(panel_music_seek(target) == ESP_OK);
    CHECK(wait_seek_done(target, 5000));
    CHECK(g_audio_generation > generation);
    st = status();
    printf("WORKER_SEEK_OK target=%u pos=%u generation=%u\n", (unsigned)target,
           (unsigned)st.position_ms, (unsigned)g_audio_generation);

    panel_music_pause();
    CHECK(wait_state(PANEL_MUSIC_PAUSED, 2000));
    Sleep(150);
    st = status();
    uint32_t paused_pos = st.position_ms;
    uint64_t paused_frames = g_audio_frames;

    uint32_t targets[3] = {
        (uint32_t)((uint64_t)st.duration_ms * 30U / 100U),
        (uint32_t)((uint64_t)st.duration_ms * 35U / 100U),
        (uint32_t)((uint64_t)st.duration_ms * 40U / 100U),
    };
    CHECK(panel_music_seek(targets[0]) == ESP_OK);
    Sleep(30);
    CHECK(panel_music_seek(targets[1]) == ESP_OK);
    Sleep(30);
    CHECK(panel_music_seek(targets[2]) == ESP_OK);
    Sleep(250);
    st = status();
    CHECK(st.state == PANEL_MUSIC_PAUSED);
    CHECK(st.seek_pending);
    CHECK(st.position_ms <= paused_pos + 80U);
    CHECK(st.position_ms + 80U >= paused_pos);
    CHECK(g_audio_frames <= paused_frames + 512U);
    unsigned loops_before = panel_music_test_loop_iterations();
    Sleep(500);
    unsigned loops_after = panel_music_test_loop_iterations();
    CHECK(loops_after - loops_before < 100U);
    printf("WORKER_PAUSED_MULTI_SEEK_OK pos=%u pending=%d audio_frames=%llu loop_delta=%u\n",
           (unsigned)st.position_ms, st.seek_pending ? 1 : 0,
           (unsigned long long)g_audio_frames, loops_after - loops_before);

    panel_music_resume();
    CHECK(wait_state(PANEL_MUSIC_PLAYING, 2000));
    CHECK(wait_seek_done(targets[2], 5000));
    st = status();
    printf("WORKER_PAUSED_RESUME_OK target=%u pos=%u\n", (unsigned)targets[2],
           (unsigned)st.position_ms);

    /* Playing-state rapid drags: the last generation must win. */
    uint32_t rapid[3] = {
        (uint32_t)((uint64_t)st.duration_ms * 55U / 100U),
        (uint32_t)((uint64_t)st.duration_ms * 60U / 100U),
        (uint32_t)((uint64_t)st.duration_ms * 65U / 100U),
    };
    for (unsigned i = 0; i < 3; i++) {
        CHECK(panel_music_seek(rapid[i]) == ESP_OK);
        Sleep(2);
    }
    CHECK(wait_seek_done(rapid[2], 6000));
    st = status();
    CHECK(!st.seek_pending);
    CHECK(st.position_ms + 1500U >= rapid[2]);
    CHECK(st.position_ms <= rapid[2] + 5000U);
    printf("WORKER_PLAYING_RAPID_SEEK_OK latest=%u pos=%u pending=%d\n",
           (unsigned)rapid[2], (unsigned)st.position_ms, st.seek_pending ? 1 : 0);

    panel_music_pause();
    CHECK(wait_state(PANEL_MUSIC_PAUSED, 2000));
    Sleep(100);
    CHECK(panel_music_seek(st.duration_ms + 5000U) == ESP_OK);
    CHECK(panel_music_test_last_seek_target() == st.duration_ms - 1000U);
    printf("WORKER_SEEK_CLAMP_OK target=%u\n", (unsigned)panel_music_test_last_seek_target());
    panel_music_resume();
    CHECK(wait_state(PANEL_MUSIC_PLAYING, 2000));

    panel_music_next();
    CHECK(wait_state(PANEL_MUSIC_PLAYING, 8000));
    st = status();
    CHECK(st.track == 1);
    CHECK(strcmp(st.title, "Starboy") == 0);
    printf("WORKER_NEXT_OK track=%u title=%s duration=%u\n", (unsigned)st.track, st.title,
           (unsigned)st.duration_ms);

    panel_music_prev();
    CHECK(wait_state(PANEL_MUSIC_PLAYING, 8000));
    st = status();
    CHECK(st.track == 0);

    const uint32_t expected_ms[4] = {361090U, 230530U, 269244U, 200124U};
    for (uint8_t t = 1; t < 4; t++) {
        panel_music_next();
        CHECK(wait_state(PANEL_MUSIC_PLAYING, 8000));
        st = status();
        CHECK(st.track == t);
        CHECK(st.index_ready);
        CHECK(panel_music_test_index_source() == 1U);
        CHECK(st.duration_ms + 80U >= expected_ms[t]);
        CHECK(st.duration_ms <= expected_ms[t] + 80U);
    }
    printf("WORKER_PREBUILT_ALL_TRACKS_OK\n");
    panel_music_select(0);
    CHECK(wait_state(PANEL_MUSIC_PLAYING, 8000));
    st = status();
    CHECK(st.track == 0);

    /* Publish-point race: a completed index for the old generation must not
     * become READY after a next; the newer track must win. */
    panel_music_test_set_index_publish_delay(600);
    panel_music_next();   /* -> track 1, waits at publish */
    Sleep(100);
    panel_music_next();   /* -> track 2, invalidates track 1 */
    CHECK(wait_state(PANEL_MUSIC_PLAYING, 8000));
    st = status();
    CHECK(st.track == 2);
    CHECK(st.index_ready);
    CHECK(panel_music_test_index_source() == 1U);
    printf("WORKER_PUBLISH_RACE_OK track=%u duration=%u\n", (unsigned)st.track,
           (unsigned)st.duration_ms);
    panel_music_select(0);
    CHECK(wait_state(PANEL_MUSIC_PLAYING, 8000));

    /* Interrupt a fresh playback open and its background index by switching. */
    panel_music_stop();
    CHECK(wait_state(PANEL_MUSIC_STOPPED, 3000));
    CHECK(panel_music_play() == ESP_OK);
    Sleep(40);
    panel_music_next();
    CHECK(wait_state(PANEL_MUSIC_PLAYING, 8000));
    if (!wait_index_ready(5000)) {
        panel_music_status_t bad = status();
        printf("NEXT_INDEX_FAIL state=%d building=%d ready=%d failed=%d duration=%u detail=%s index_track=%u\n",
               (int)bad.state, bad.index_building ? 1 : 0, bad.index_ready ? 1 : 0,
               bad.index_failed ? 1 : 0, (unsigned)bad.duration_ms, bad.detail,
               (unsigned)panel_music_test_index_track());
        return 1;
    }
    st = status();
    CHECK(st.track == 1);
    CHECK(panel_music_test_index_track() == 1);
    printf("WORKER_SCAN_INTERRUPT_OK track=%u pos=%u\n", (unsigned)st.track,
           (unsigned)st.position_ms);

    /* Controlled slow file-open: generation captured at play start must win;
     * a second next during the old open must restart with the latest track. */
    panel_music_test_set_open_delay(600);
    panel_music_next();   /* -> track 2, worker enters slow open */
    Sleep(100);
    panel_music_next();   /* -> track 3 while old open is still blocked */
    CHECK(wait_state(PANEL_MUSIC_PLAYING, 8000));
    CHECK(wait_index_ready(5000));
    st = status();
    CHECK(st.track == 3);
    CHECK(panel_music_test_index_track() == 3);
    printf("WORKER_OPEN_RACE_NEXT_OK track=%u index_track=%u\n", (unsigned)st.track,
           (unsigned)panel_music_test_index_track());

    /* A stale file_open error must not overwrite the newer selection; the
     * injected error is consumed once, so the restarted track must play. */
    panel_music_test_set_open_error_once(true);
    panel_music_test_set_open_delay(600);
    panel_music_next();   /* -> track 0, slow open will fail */
    Sleep(100);
    panel_music_next();   /* -> track 1 while old open is blocked */
    CHECK(wait_state(PANEL_MUSIC_PLAYING, 8000));
    CHECK(wait_index_ready(5000));
    st = status();
    CHECK(st.track == 1);
    CHECK(st.state == PANEL_MUSIC_PLAYING);
    CHECK(panel_music_test_index_track() == 1);
    printf("WORKER_STALE_OPEN_ERROR_OK track=%u state=%d\n", (unsigned)st.track,
           (int)st.state);

    /* Stop while a stale file-open is in flight: no old index publication. */
    panel_music_next();   /* -> track 2, slow open */
    Sleep(100);
    panel_music_stop();
    CHECK(wait_state(PANEL_MUSIC_STOPPED, 3000));
    Sleep(800);
    st = status();
    CHECK(st.state == PANEL_MUSIC_STOPPED);
    CHECK(!st.index_ready);
    CHECK(!st.index_building);
    panel_music_test_set_open_delay(0);
    printf("WORKER_OPEN_RACE_STOP_OK state=%d\n", (int)st.state);

    /* Forced prebuilt mismatch must fall back to the real 16 KiB scan. */
    panel_music_test_set_prebuilt_mismatch(true);
    CHECK(panel_music_play() == ESP_OK);
    CHECK(wait_state(PANEL_MUSIC_PLAYING, 8000));
    st = status();
    CHECK(st.index_ready);
    CHECK(st.duration_ms > 200000U);
    CHECK(panel_music_test_index_source() == 2U);
    panel_music_test_set_prebuilt_mismatch(false);
    printf("WORKER_PREBUILT_FALLBACK_OK duration=%u source=%u\n",
           (unsigned)st.duration_ms, (unsigned)panel_music_test_index_source());
    panel_music_stop();
    CHECK(wait_state(PANEL_MUSIC_STOPPED, 3000));

    /* A prebuilt probe read failure is a real error, not a silent full scan. */
    panel_music_test_set_prebuilt_io_error(true);
    CHECK(panel_music_play() == ESP_OK);
    CHECK(wait_state(PANEL_MUSIC_ERROR, 3000));
    st = status();
    CHECK(strstr(st.detail, "测试TF读取失败") != NULL);
    panel_music_test_set_prebuilt_io_error(false);
    panel_music_stop();
    CHECK(wait_state(PANEL_MUSIC_STOPPED, 3000));
    printf("WORKER_PREBUILT_IO_ERROR_OK detail=%s\n", st.detail);

    /* Audio failure then stop must still clean up index flags. */
    CHECK(panel_music_play() == ESP_OK);
    CHECK(wait_state(PANEL_MUSIC_PLAYING, 8000));
    worker_test_fail_audio_after(0);
    CHECK(wait_state(PANEL_MUSIC_ERROR, 3000));
    panel_music_stop();
    CHECK(wait_state(PANEL_MUSIC_STOPPED, 3000));
    Sleep(300);
    st = status();
    CHECK(st.state == PANEL_MUSIC_STOPPED);
    CHECK(!st.index_building);
    CHECK(!st.index_ready);
    CHECK(g_io_acquires == g_io_releases);
    CHECK(g_pin_acquires == g_pin_releases);
    worker_test_fail_audio_after(-1);
    printf("WORKER_AUDIO_ERROR_STOP_CANCEL_OK state=%d building=%d ready=%d\n",
           (int)st.state, st.index_building ? 1 : 0, st.index_ready ? 1 : 0);

    Sleep(400);
    CHECK(g_io_acquires == g_io_releases);
    CHECK(g_pin_acquires == g_pin_releases);
    CHECK(g_io_recursive == 0U);
    printf("WORKER_LIFECYCLE_OK io=%u/%u pin=%u/%u recursive=%u\n",
           (unsigned)g_io_acquires, (unsigned)g_io_releases,
           (unsigned)g_pin_acquires, (unsigned)g_pin_releases,
           (unsigned)g_io_recursive);

    uint64_t stopped_frames = g_audio_frames;
    Sleep(200);
    CHECK(g_audio_frames == stopped_frames);
    printf("WORKER_STOP_OK frames=%llu writes=%u\n", (unsigned long long)g_audio_frames,
           (unsigned)g_audio_writes);
    printf("MUSIC_WORKER_PRODUCTION_TESTS_OK\n");
    return 0;
}
