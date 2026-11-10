/*
 * Minimal stand-in for ESP-IDF's esp_err.h, just enough to host-compile the
 * hardware-independent modules under test (bot.c, pentax_scsi.c,
 * pentax_status.c) without pulling in the real ESP-IDF. Values match the
 * real esp_err.h so error codes printed by a failing test mean the same
 * thing they would in a device log.
 */
#pragma once

typedef int esp_err_t;

#define ESP_OK                  0
#define ESP_FAIL                -1

#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERR_TIMEOUT         0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
