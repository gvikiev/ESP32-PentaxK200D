/*
 * Shared ownership of the ESP-IDF USB Host Library (components/usb).
 *
 * usb_host_install() may only be called once per process; running both the
 * Pentax SCSI backend (usb_msc_link.c) and the PTP backend (ptp_transport.c)
 * at the same time -- so the camera's actual USB mode ([PC]/MSC vs
 * [PictBridge]/PTP) decides which one claims it, instead of a Kconfig
 * choice picked at build time -- means they can't each install the library
 * themselves. Both call usb_host_shared_acquire() instead; each backend
 * still registers its own USB Host client and keeps enumerating/claiming
 * devices exactly as before (the USB Host Library explicitly supports
 * multiple independent clients opening the same device, e.g. for composite
 * devices), which is what lets exactly one of them end up claiming the
 * interface that matches its own protocol while the other reports its own
 * "wrong mode" state.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the USB Host Library and starts its library-event daemon task
 * the first time this is called; a no-op (returns ESP_OK) on every call
 * after that. Call this before usb_host_client_register(). */
esp_err_t usb_host_shared_acquire(void);

#ifdef __cplusplus
}
#endif
