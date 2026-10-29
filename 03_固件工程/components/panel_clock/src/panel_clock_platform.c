#include "panel_clock_platform.h"

#include <time.h>

#include "esp_timer.h"

int64_t panel_clock_platform_epoch(void)
{
    return (int64_t)time(NULL);
}

int64_t panel_clock_platform_monotonic_us(void)
{
    return esp_timer_get_time();
}
