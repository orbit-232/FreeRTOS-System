#pragma once

#include <stdint.h>
#include <stddef.h>

typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef uint32_t TickType_t;
typedef int portMUX_TYPE;

#define portMAX_DELAY 0xffffffffu
#define portTICK_PERIOD_MS 1
#define pdTRUE 1
#define pdPASS 1
#define pdFALSE 0
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
#define portMUX_INITIALIZER_UNLOCKED 0

void test_enter_critical(portMUX_TYPE *mux);
void test_exit_critical(portMUX_TYPE *mux);
#define taskENTER_CRITICAL(m) test_enter_critical(m)
#define taskEXIT_CRITICAL(m) test_exit_critical(m)

SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
QueueHandle_t xQueueCreate(unsigned len, unsigned item_size);
int xQueueSend(QueueHandle_t, const void *, TickType_t);
int xQueueReceive(QueueHandle_t, void *, TickType_t);
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                unsigned prio, TaskHandle_t *out);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
TickType_t xTaskGetTickCount(void);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t);
