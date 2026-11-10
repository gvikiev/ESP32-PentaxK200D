/* Backs freertos/task.h's stub declarations with a plain counter so tests
 * control simulated time instead of waiting on a real clock. */
#include "freertos/task.h"

static volatile TickType_t s_fake_ticks;

TickType_t xTaskGetTickCount(void)
{
    return s_fake_ticks;
}

void vTaskDelay(TickType_t ticks)
{
    s_fake_ticks += ticks;
}

/* Test-only helpers (not part of the FreeRTOS API being stubbed). */
void fake_clock_reset(void)
{
    s_fake_ticks = 0;
}

void fake_clock_advance(TickType_t ticks)
{
    s_fake_ticks += ticks;
}
