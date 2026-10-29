#pragma once
#include <stdint.h>
typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef uint32_t TickType_t;
typedef int portMUX_TYPE;
#define portMAX_DELAY 0xffffffffu
#define portTICK_PERIOD_MS 10
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define pdFALSE 0
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(m)
#define taskEXIT_CRITICAL(m)
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
QueueHandle_t xQueueCreate(unsigned, unsigned);
int xQueueSend(QueueHandle_t, const void *, TickType_t);
int xQueueReceive(QueueHandle_t, void *, TickType_t);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
void vTaskDelay(TickType_t);
void vTaskDelete(TaskHandle_t);
TickType_t xTaskGetTickCount(void);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t);

