/*
 * ESP-IDF USB Host binding for a Mass Storage (class 0x08 / subclass 0x06 /
 * protocol 0x50 "Bulk-Only") interface. Provides the bot_io_t pipes that
 * bot.h runs on. A dedicated client task pumps USB events, so the
 * bot_io_t callbacks may be used from any single task at a time.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "bot.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USB_MSC_DISCONNECTED = 0,
    USB_MSC_CONNECTED,   /* BOT interface claimed and ready */
    USB_MSC_NOT_MSC,     /* device present but no BOT interface (wrong camera USB mode) */
} usb_msc_state_t;

typedef void (*usb_msc_event_cb_t)(usb_msc_state_t state, void *ctx);

esp_err_t usb_msc_link_init(usb_msc_event_cb_t cb, void *ctx);
usb_msc_state_t usb_msc_link_state(void);

/* Fills `io` with pipes for the currently connected device. */
esp_err_t usb_msc_link_get_io(bot_io_t *io);

#ifdef __cplusplus
}
#endif
