#define _WIN32_WINNT 0x0601
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "freertos/FreeRTOS.h"
#include "panel_audio.h"
#include "panel_files.h"

typedef struct {
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    int count;
} test_sem_t;

typedef struct {
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    unsigned capacity;
    unsigned item_size;
    unsigned head;
    unsigned tail;
    unsigned count;
    uint8_t *items;
} test_queue_t;

typedef struct {
    void (*fn)(void *);
    void *arg;
} task_start_t;

static CRITICAL_SECTION s_critical;
static CRITICAL_SECTION s_io_cs;
static DWORD s_io_owner;

void test_runtime_init(void)
{
    InitializeCriticalSection(&s_critical);
    InitializeCriticalSection(&s_io_cs);
}

void test_enter_critical(portMUX_TYPE *mux)
{
    (void)mux;
    EnterCriticalSection(&s_critical);
}

void test_exit_critical(portMUX_TYPE *mux)
{
    (void)mux;
    LeaveCriticalSection(&s_critical);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    test_sem_t *s = calloc(1, sizeof(*s));
    if (s == NULL) return NULL;
    InitializeCriticalSection(&s->cs);
    InitializeConditionVariable(&s->cv);
    s->count = 1;
    return s;
}

int xSemaphoreTake(SemaphoreHandle_t handle, TickType_t timeout)
{
    test_sem_t *s = handle;
    if (s == NULL) return pdFALSE;
    DWORD wait_ms = timeout == portMAX_DELAY ? INFINITE : (DWORD)timeout;
    EnterCriticalSection(&s->cs);
    while (s->count == 0) {
        if (!SleepConditionVariableCS(&s->cv, &s->cs, wait_ms)) break;
    }
    int ok = s->count > 0;
    if (ok) s->count--;
    LeaveCriticalSection(&s->cs);
    return ok ? pdTRUE : pdFALSE;
}

int xSemaphoreGive(SemaphoreHandle_t handle)
{
    test_sem_t *s = handle;
    if (s == NULL) return pdFALSE;
    EnterCriticalSection(&s->cs);
    s->count++;
    WakeConditionVariable(&s->cv);
    LeaveCriticalSection(&s->cs);
    return pdTRUE;
}

QueueHandle_t xQueueCreate(unsigned len, unsigned item_size)
{
    test_queue_t *q = calloc(1, sizeof(*q));
    if (q == NULL) return NULL;
    q->items = calloc(len, item_size);
    if (q->items == NULL) {
        free(q);
        return NULL;
    }
    InitializeCriticalSection(&q->cs);
    InitializeConditionVariable(&q->cv);
    q->capacity = len;
    q->item_size = item_size;
    return q;
}

int xQueueSend(QueueHandle_t handle, const void *item, TickType_t timeout)
{
    (void)timeout;
    test_queue_t *q = handle;
    if (q == NULL) return pdFALSE;
    EnterCriticalSection(&q->cs);
    int ok = q->count < q->capacity;
    if (ok) {
        memcpy(q->items + q->tail * q->item_size, item, q->item_size);
        q->tail = (q->tail + 1U) % q->capacity;
        q->count++;
        WakeConditionVariable(&q->cv);
    }
    LeaveCriticalSection(&q->cs);
    return ok ? pdTRUE : pdFALSE;
}

int xQueueReceive(QueueHandle_t handle, void *item, TickType_t timeout)
{
    test_queue_t *q = handle;
    if (q == NULL) return pdFALSE;
    DWORD wait_ms = timeout == portMAX_DELAY ? INFINITE : (DWORD)timeout;
    EnterCriticalSection(&q->cs);
    while (q->count == 0) {
        if (!SleepConditionVariableCS(&q->cv, &q->cs, wait_ms)) break;
    }
    int ok = q->count > 0;
    if (ok) {
        memcpy(item, q->items + q->head * q->item_size, q->item_size);
        q->head = (q->head + 1U) % q->capacity;
        q->count--;
    }
    LeaveCriticalSection(&q->cs);
    return ok ? pdTRUE : pdFALSE;
}

static DWORD WINAPI task_entry(LPVOID opaque)
{
    task_start_t *start = opaque;
    void (*fn)(void *) = start->fn;
    void *arg = start->arg;
    free(start);
    fn(arg);
    return 0;
}

static int s_fail_task_create_at = -1;
static int s_task_create_count;

void worker_test_fail_task_create_at(int index)
{
    s_fail_task_create_at = index;
    s_task_create_count = 0;
}

int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                unsigned prio, TaskHandle_t *out)
{
    (void)name;
    (void)stack;
    (void)prio;
    s_task_create_count++;
    if (s_task_create_count == s_fail_task_create_at) return pdFALSE;
    task_start_t *start = calloc(1, sizeof(*start));
    if (start == NULL) return pdFALSE;
    start->fn = fn;
    start->arg = arg;
    HANDLE thread = CreateThread(NULL, 0, task_entry, start, 0, NULL);
    if (thread == NULL) {
        free(start);
        return pdFALSE;
    }
    CloseHandle(thread);
    if (out != NULL) *out = NULL;
    return pdPASS;
}

void vTaskDelay(TickType_t ticks)
{
    Sleep((DWORD)ticks);
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)GetTickCount();
}

unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    (void)task;
    return 4096;
}

void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    return malloc(size);
}

void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    (void)caps;
    return calloc(count, size);
}

void heap_caps_free(void *ptr)
{
    free(ptr);
}

size_t heap_caps_get_free_size(unsigned caps)
{
    (void)caps;
    return 64U * 1024U * 1024U;
}

size_t heap_caps_get_largest_free_block(unsigned caps)
{
    (void)caps;
    return 32U * 1024U * 1024U;
}

size_t heap_caps_get_minimum_free_size(unsigned caps)
{
    (void)caps;
    return 32U * 1024U * 1024U;
}

const char *panel_files_root_path(void)
{
    const char *root = getenv("MUSIC_TEST_ROOT");
    return root != NULL ? root : ".";
}

volatile unsigned g_io_acquires;
volatile unsigned g_io_releases;
volatile unsigned g_pin_acquires;
volatile unsigned g_pin_releases;
volatile unsigned g_io_recursive;

bool panel_files_card_ready(void) { return true; }
bool panel_files_io_acquire(void)
{
    DWORD tid = GetCurrentThreadId();
    EnterCriticalSection(&s_io_cs);
    if (s_io_owner == tid) {
        g_io_recursive++;
        LeaveCriticalSection(&s_io_cs);
        return false;
    }
    s_io_owner = tid;
    g_io_acquires++;
    return true;
}
void panel_files_io_release(void)
{
    if (s_io_owner == GetCurrentThreadId()) {
        s_io_owner = 0;
        g_io_releases++;
    }
    LeaveCriticalSection(&s_io_cs);
}
bool panel_files_mount_pin_acquire(void)
{
    g_pin_acquires++;
    return true;
}
void panel_files_mount_pin_release(void)
{
    g_pin_releases++;
}
bool panel_files_mount_pin_held(void) { return false; }

volatile uint64_t g_audio_frames;
volatile unsigned g_audio_generation;
volatile unsigned g_audio_writes;
static volatile bool s_audio_active;
static volatile int s_audio_fail_after = -1;

void worker_test_fail_audio_after(int writes)
{
    s_audio_fail_after = writes;
}

bool panel_audio_stream_begin(void)
{
    s_audio_active = true;
    g_audio_generation++;
    return true;
}

bool panel_audio_stream_write(const int16_t *pcm, size_t frames)
{
    (void)pcm;
    if (!s_audio_active) return false;
    if (s_audio_fail_after == 0) return false;
    if (s_audio_fail_after > 0) s_audio_fail_after--;
    int ms = (int)((frames * 1000U) / 22050U);
    if (ms < 1) ms = 1;
    Sleep((DWORD)ms);
    g_audio_frames += frames;
    g_audio_writes++;
    return true;
}

void panel_audio_stream_set_volume(int volume) { (void)volume; }
void panel_audio_stream_end(void) { s_audio_active = false; }
bool panel_audio_stream_busy(void) { return false; }

bool panel_music_import_is_active(void) { return false; }
