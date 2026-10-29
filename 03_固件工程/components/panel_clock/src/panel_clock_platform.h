#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Platform seam: production uses time(NULL)/esp_timer_get_time(); host tests
 * provide a fake implementation while still linking production panel_clock.c. */
int64_t panel_clock_platform_epoch(void);
int64_t panel_clock_platform_monotonic_us(void);

#ifdef __cplusplus
}
#endif
