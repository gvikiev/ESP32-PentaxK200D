#pragma once
#include "freertos/task.h"

void fake_clock_reset(void);
void fake_clock_advance(TickType_t ticks);
