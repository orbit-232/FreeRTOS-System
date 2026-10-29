#include <stdio.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

int64_t esp_timer_get_time(void)
{
    return (int64_t)xTaskGetTickCount() * 1000;
}

const char *esp_err_to_name(esp_err_t err)
{
    static char text[24];
    snprintf(text, sizeof(text), "err %d", (int)err);
    return text;
}
