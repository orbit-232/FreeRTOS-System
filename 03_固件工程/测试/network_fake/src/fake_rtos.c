#include "fake_idf.h"

#include <stdlib.h>
#include <windows.h>

struct fake_queue {
    CRITICAL_SECTION cs;
    unsigned len;
    unsigned item_size;
    unsigned head;
    unsigned count;
    unsigned char *buf;
};

struct fake_sem {
    CRITICAL_SECTION cs;
    HANDLE event;
    bool binary;
    unsigned count;
};

static CRITICAL_SECTION g_time_lock;
static bool g_time_init;
static uint32_t g_fake_ms;
static uint64_t g_base_tick;
static bool g_task_create_fail;

static uint64_t tick_ms(void)
{
    return (uint64_t)GetTickCount();
}

static void time_lock(void)
{
    if (!g_time_init) {
        InitializeCriticalSection(&g_time_lock);
        g_time_init = true;
    }
    EnterCriticalSection(&g_time_lock);
}
static void time_unlock(void) { LeaveCriticalSection(&g_time_lock); }

void fake_advance_ms(uint32_t ms)
{
    time_lock();
    g_fake_ms += ms;
    time_unlock();
}

TickType_t xTaskGetTickCount(void)
{
    if (g_base_tick == 0) g_base_tick = tick_ms();
    time_lock();
    uint32_t v = g_fake_ms + (uint32_t)(tick_ms() - g_base_tick);
    time_unlock();
    return v;
}

QueueHandle_t xQueueCreate(unsigned length, unsigned item_size)
{
    struct fake_queue *q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->len = length;
    q->item_size = item_size;
    q->buf = calloc(length, item_size);
    if (!q->buf) {
        free(q);
        return NULL;
    }
    InitializeCriticalSection(&q->cs);
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    (void)wait;
    if (!q || !item) return pdFALSE;
    EnterCriticalSection(&q->cs);
    if (q->count >= q->len) {
        LeaveCriticalSection(&q->cs);
        return pdFALSE;
    }
    unsigned tail = (q->head + q->count) % q->len;
    memcpy(q->buf + tail * q->item_size, item, q->item_size);
    q->count++;
    LeaveCriticalSection(&q->cs);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    if (!q || !item) return pdFALSE;
    uint32_t elapsed = 0;
    for (;;) {
        EnterCriticalSection(&q->cs);
        if (q->count > 0) {
            memcpy(item, q->buf + q->head * q->item_size, q->item_size);
            q->head = (q->head + 1) % q->len;
            q->count--;
            LeaveCriticalSection(&q->cs);
            return pdTRUE;
        }
        LeaveCriticalSection(&q->cs);
        if (wait == 0 || (wait != portMAX_DELAY && elapsed >= wait)) return pdFALSE;
        Sleep(1);
        elapsed++;
    }
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct fake_sem *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    InitializeCriticalSection(&s->cs);
    s->binary = false;
    return s;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    struct fake_sem *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    InitializeCriticalSection(&s->cs);
    s->binary = true;
    s->count = 0;
    s->event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!s->event) {
        DeleteCriticalSection(&s->cs);
        free(s);
        return NULL;
    }
    return s;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait)
{
    if (!s) return pdFALSE;
    if (!s->binary) {
        EnterCriticalSection(&s->cs);
        return pdTRUE;
    }
    for (;;) {
        EnterCriticalSection(&s->cs);
        if (s->count > 0) {
            s->count--;
            LeaveCriticalSection(&s->cs);
            return pdTRUE;
        }
        LeaveCriticalSection(&s->cs);
        if (wait == 0) return pdFALSE;
        DWORD timeout = (wait == portMAX_DELAY) ? INFINITE : (DWORD)wait;
        if (WaitForSingleObject(s->event, timeout) != WAIT_OBJECT_0) {
            return pdFALSE;
        }
    }
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    if (!s) return pdFALSE;
    if (s->binary) {
        EnterCriticalSection(&s->cs);
        s->count = 1;
        SetEvent(s->event);
        LeaveCriticalSection(&s->cs);
        return pdTRUE;
    }
    LeaveCriticalSection(&s->cs);
    return pdTRUE;
}

BaseType_t xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                       unsigned prio, TaskHandle_t *out)
{
    (void)name; (void)stack; (void)prio;
    if (g_task_create_fail) {
        return pdFALSE;
    }
    HANDLE th = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)fn, arg, 0, NULL);
    if (!th) return pdFALSE;
    if (out) *out = (TaskHandle_t)th;
    return pdPASS;
}

void fake_sleep_ms(unsigned ms) { Sleep(ms); }

void vTaskDelay(TickType_t ticks) { Sleep((DWORD)ticks); }

void fake_set_task_create_fail(bool fail) { g_task_create_fail = fail; }
