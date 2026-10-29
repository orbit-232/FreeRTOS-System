#pragma once

#include <stdbool.h>
#include <stdint.h>

void fake_clock_set_epoch(int64_t epoch);
void fake_clock_advance_monotonic_us(int64_t delta_us);
int64_t fake_clock_epoch(void);
int64_t fake_clock_monotonic_us(void);
void fake_clock_set_calibrated(bool calibrated);
bool fake_clock_is_calibrated(void);
void fake_clock_reset(void);
