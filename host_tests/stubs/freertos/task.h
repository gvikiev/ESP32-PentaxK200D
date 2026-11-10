/*
 * Host-test stand-in for freertos/task.h. Only the handful of symbols
 * pentax_scsi.c actually uses (TickType_t, pdMS_TO_TICKS, xTaskGetTickCount,
 * vTaskDelay). Definitions live in host_tests/fake_clock.c, which backs them
 * with a plain counter the tests can advance deterministically instead of
 * sleeping in real time -- that's what lets the ready/timeout fault-
 * injection tests run instantly.
 */
#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
