#include "fake_rtos.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

struct fake_queue {
    CRITICAL_SECTION cs;
    HANDLE event;
    unsigned length;
    unsigned item_size;
    unsigned head;
    unsigned count;
    unsigned char *items;
};

struct fake_sem {
    CRITICAL_SECTION cs;
};

QueueHandle_t xQueueCreate(unsigned length, unsigned item_size)
{
    if (length == 0 || item_size == 0) return NULL;
    struct fake_queue *q = calloc(1, sizeof(*q));
    if (q == NULL) return NULL;
    q->items = calloc(length, item_size);
    if (q->items == NULL) {
        free(q);
        return NULL;
    }
    q->length = length;
    q->item_size = item_size;
    q->event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (q->event == NULL) {
        free(q->items);
        free(q);
        return NULL;
    }
    InitializeCriticalSection(&q->cs);
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    if (q == NULL || item == NULL) return pdFALSE;
    DWORD start = GetTickCount();
    for (;;) {
        EnterCriticalSection(&q->cs);
        if (q->count < q->length) {
            unsigned tail = (q->head + q->count) % q->length;
            memcpy(q->items + tail * q->item_size, item, q->item_size);
            q->count++;
            LeaveCriticalSection(&q->cs);
            SetEvent(q->event);
            return pdTRUE;
        }
        LeaveCriticalSection(&q->cs);
        if (wait == 0) return pdFALSE;
        if (wait != portMAX_DELAY && GetTickCount() - start >= wait) return pdFALSE;
        Sleep(1);
    }
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    if (q == NULL || item == NULL) return pdFALSE;
    for (;;) {
        EnterCriticalSection(&q->cs);
        if (q->count > 0) {
            memcpy(item, q->items + q->head * q->item_size, q->item_size);
            q->head = (q->head + 1) % q->length;
            q->count--;
            LeaveCriticalSection(&q->cs);
            return pdTRUE;
        }
        LeaveCriticalSection(&q->cs);
        if (wait == 0) return pdFALSE;
        DWORD timeout = wait == portMAX_DELAY ? INFINITE : (DWORD)wait;
        WaitForSingleObject(q->event, timeout);
        if (wait != portMAX_DELAY) return pdFALSE;
    }
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct fake_sem *s = calloc(1, sizeof(*s));
    if (s == NULL) return NULL;
    InitializeCriticalSection(&s->cs);
    return s;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait)
{
    (void)wait;
    if (s == NULL) return pdFALSE;
    EnterCriticalSection(&s->cs);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    if (s == NULL) return pdFALSE;
    LeaveCriticalSection(&s->cs);
    return pdTRUE;
}

BaseType_t xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                       unsigned prio, TaskHandle_t *out)
{
    (void)name;
    (void)stack;
    (void)prio;
    HANDLE thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)fn, arg, 0, NULL);
    if (thread == NULL) return pdFALSE;
    if (out != NULL) *out = thread;
    return pdPASS;
}

TickType_t xTaskGetTickCount(void) { return (TickType_t)GetTickCount(); }

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    (void)task;
    /* Host thread stacks are not ESP task stacks; just a non-zero sentinel. */
    return 6144;
}

void vTaskDelay(TickType_t ticks) { Sleep((DWORD)ticks); }
